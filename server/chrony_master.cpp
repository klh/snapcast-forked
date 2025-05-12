/***
    This file is part of snapcast
    Copyright (C) 2014-2025  Johannes Pohl

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
***/

#include "chrony_master.hpp"
#include "common/aixlog.hpp"
#include <array>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <signal.h>

namespace fs = std::filesystem;

static constexpr auto LOG_TAG = "ChronyMaster";

// Helper function to safely execute a command and capture its output
static std::string execCommand(const std::string& cmd) {
    std::string result;
    std::array<char, 128> buffer;
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);

    if (!pipe) {
        LOG(WARNING, LOG_TAG) << "Failed to execute command: " << cmd << "\n";
        return "";
    }

    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
        result += buffer.data();
    }
    return result;
}

namespace snapserver {

ChronyMaster::~ChronyMaster() {
    stop();
}

void ChronyMaster::init(const std::string& config_dir, uint16_t port) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (running_) {
        throw std::runtime_error("Cannot initialize chrony master while it is already running");
    }
    
    // Check if chrony is installed - this is now a hard requirement
    if (!isChronyInstalled()) {
        throw std::runtime_error("Chrony is not installed. It is required for time synchronization.");
    }
    
    // Store configuration parameters
    port_ = port;
    
    // Get server address (hostname or IP)
    server_address_ = execCommand("hostname -f 2>/dev/null");
    if (server_address_.empty()) {
        server_address_ = execCommand("hostname 2>/dev/null");
    }
    if (server_address_.empty()) {
        server_address_ = "127.0.0.1";
    }
    // Trim newlines
    server_address_.erase(server_address_.find_last_not_of("\n\r") + 1);
    
    LOG(NOTICE, LOG_TAG) << "Chrony present, setting up " << server_address_ << " as chrony MASTER\n";
    LOG(INFO, LOG_TAG) << "Initialized chrony master with server address: " << server_address_ << ":" << port_ << "\n";
}

void ChronyMaster::start()
{
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (running_) {
        LOG(INFO, LOG_TAG) << "Chrony master already running\n";
        return;
    }
    
    // Check if chronyd is already running
    std::string status = execCommand("chronyc -c tracking 2>/dev/null");
    if (!status.empty()) {
        LOG(INFO, LOG_TAG) << "Chronyd already running, configuring as master\n";
    } else {
        // Start chronyd if not running
        std::string cmd = "chronyd";
        LOG(INFO, LOG_TAG) << "Starting chronyd: " << cmd << "\n";
        
        // Start chronyd as a background process
        std::string result = execCommand(cmd + " & echo $!");
        if (result.empty()) {
            throw std::runtime_error("Failed to start chronyd. Time synchronization cannot function.");
        }
        
        // Wait for chronyd to start
        std::this_thread::sleep_for(std::chrono::seconds(1));
        
        // Verify chronyd started successfully
        status = execCommand("chronyc -c tracking 2>/dev/null");
        if (status.empty()) {
            throw std::runtime_error("Chronyd started but is not responding. Time synchronization cannot function.");
        }
    }
    
    // Configure as master using chronyc commands
    LOG(INFO, LOG_TAG) << "Configuring chronyd as master\n";
    
    // Use the server address that was discovered during initialization
    // The server_address_ is set in the init() method using hostname -f or hostname
    std::string server_ip = server_address_;
    
    // If the server address is a hostname, try to get its IP address
    if (server_ip.find_first_not_of("0123456789.") != std::string::npos && server_ip != "localhost" && server_ip != "127.0.0.1") {
        std::string ip_cmd = "getent hosts " + server_ip + " | awk '{print $1}'";
        std::string resolved_ip = execCommand(ip_cmd);
        if (!resolved_ip.empty()) {
            // Remove any trailing whitespace
            resolved_ip.erase(resolved_ip.find_last_not_of("\n\r\t ") + 1);
            server_ip = resolved_ip;
            LOG(INFO, LOG_TAG) << "Resolved server hostname to IP: " << server_ip << "\n";
        }
    }
    
    // Determine the network class for the allow directive
    std::string network_allow;
    if (server_ip == "localhost" || server_ip == "127.0.0.1") {
        // If local, allow all
        network_allow = "0.0.0.0/0";
        LOG(INFO, LOG_TAG) << "Using localhost, allowing all connections\n";
    } else {
        // Extract network part from IP and use /24 subnet
        size_t last_dot = server_ip.find_last_of(".");
        if (last_dot != std::string::npos) {
            network_allow = server_ip.substr(0, last_dot) + ".0/24";
            LOG(INFO, LOG_TAG) << "Using network: " << network_allow << " based on server IP: " << server_ip << "\n";
        } else {
            // Fallback to all if IP format is unexpected
            network_allow = "0.0.0.0/0";
            LOG(WARNING, LOG_TAG) << "Could not determine network from IP: " << server_ip << ", allowing all connections\n";
        }
    }
    
    // Configure chrony with all required settings in a single command
    bool config_success = false;
    for (int retry = 0; retry < 3; retry++) {
        // Combined command to configure chrony master with public NTP servers as fallback
        std::string cmd = "chronyc -a \
"
                          "'add server time.cloudflare.com iburst' \
"
                          "'add server time.google.com iburst' \
"
                          "'local stratum 1 orphan' \
"
                          "'allow " + network_allow + "' \
"
                          "'cmdallow " + network_allow + "'";
        
        LOG(INFO, LOG_TAG) << "Running command: " << cmd << "\n";
        std::string result = execCommand(cmd);
        
        // Check if commands succeeded (should see multiple 200 OK responses)
        size_t pos = 0;
        int ok_count = 0;
        while ((pos = result.find("200 OK", pos)) != std::string::npos) {
            ok_count++;
            pos += 6; // length of "200 OK"
        }
        
        if (ok_count >= 5) { // We expect 5 OK responses for our 5 commands
            config_success = true;
            LOG(INFO, LOG_TAG) << "Successfully configured chrony master\n";
            break;
        }
        
        LOG(WARNING, LOG_TAG) << "Failed to configure chrony (attempt " << retry+1 << "/3): " << result << "\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    
    if (!config_success) {
        LOG(ERROR, LOG_TAG) << "Failed to configure chrony master after multiple attempts\n";
        return false;
    }
    
    LOG(NOTICE, LOG_TAG) << "Note: For this configuration to work properly, chronyd must be run as root or the chrony user\n";
    
    // Verify configuration was applied
    std::string tracking = execCommand("chronyc -c tracking 2>/dev/null");
    if (tracking.empty()) {
        LOG(ERROR, LOG_TAG) << "Failed to verify chrony configuration\n";
        return false;
    }
    
    // Mark as running
    running_ = true;
    
    LOG(NOTICE, LOG_TAG) << "Chrony master configured successfully on " << server_address_ << "\n";
    LOG(INFO, LOG_TAG) << "Chrony tracking information:\n" << tracking;
    return true;
}

void ChronyMaster::stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!running_) {
        return;
    }
    
    // We don't actually need to stop chronyd - it can continue running
    // Just mark our service as stopped
    running_ = false;
    LOG(INFO, LOG_TAG) << "Chrony master stopped\n";
}

bool ChronyMaster::isRunning() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return running_;
}

std::string ChronyMaster::getStatus() const {
    if (!isRunning()) {
        return "Chrony master is not running";
    }
    
    // Get tracking information
    std::string tracking = execCommand("chronyc -n tracking 2>/dev/null");
    if (tracking.empty()) {
        return "Chrony master is running but tracking information is not available";
    }
    
    return "Chrony master is running\n" + tracking;
}

std::vector<std::string> ChronyMaster::getClients() const {
    std::vector<std::string> clients;
    
    if (!isRunning()) {
        return clients;
    }
    
    // Get clients from chronyc
    std::string clients_output = execCommand("chronyc -n clients 2>/dev/null");
    if (clients_output.empty()) {
        return clients;
    }
    
    // Parse client list
    std::istringstream iss(clients_output);
    std::string line;
    
    // Skip header lines
    for (int i = 0; i < 3; ++i) {
        std::getline(iss, line);
    }
    
    // Parse client entries
    while (std::getline(iss, line)) {
        if (line.empty()) {
            continue;
        }
        
        // Extract client IP address (first column)
        std::istringstream line_iss(line);
        std::string ip;
        line_iss >> ip;
        
        if (!ip.empty()) {
            clients.push_back(ip);
        }
    }
    
    return clients;
}

std::string ChronyMaster::getConfig() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (config_file_.empty()) {
        return "Configuration not available";
    }
    
    try {
        std::ifstream file(config_file_);
        if (!file.is_open()) {
            return "Failed to open configuration file";
        }
        
        std::stringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    } catch (const std::exception& e) {
        return std::string("Error reading configuration: ") + e.what();
    }
}

std::string ChronyMaster::getServerAddress() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return server_address_;
}

uint16_t ChronyMaster::getPort() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return port_;
}

std::optional<time_sync::TimeSyncInfo> ChronyMaster::getTrackingInfo() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!running_) {
        return std::nullopt;
    }
    
    // Get raw output from chronyc
    std::string tracking = execCommand("chronyc -c tracking 2>/dev/null");
    if (tracking.empty()) {
        return std::nullopt;
    }
    
    // Create time sync info
    time_sync::TimeSyncInfo info;
    info.source = time_sync::TimeSyncSource::CHRONY;
    info.available = true;
    info.quality = 0.8f;
    info.estimated_error_ms = 1.0f;
    info.last_update = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    
    return info;
}

// No configuration file generation - using direct chronyc commands

bool ChronyMaster::isChronyInstalled() const {
    std::string result = execCommand("which chronyd 2>/dev/null");
    return !result.empty();
}

// No monitor thread implementation - assuming chrony works if configured properly

} // namespace snapserver
