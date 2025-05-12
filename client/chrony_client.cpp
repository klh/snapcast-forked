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

#include "chrony_client.hpp"
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

static constexpr auto LOG_TAG = "ChronyClient";

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

namespace snapclient {

ChronyClient::~ChronyClient() {
    disconnect();
}

bool ChronyClient::init(const std::string& config_dir) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (connected_) {
        LOG(WARNING, LOG_TAG) << "Cannot initialize while connected\n";
        return false;
    }
    
    // Check if chrony is installed
    if (!isChronyInstalled()) {
        LOG(ERROR, LOG_TAG) << "Chrony is not installed. Please install chrony package\n";
        return false;
    }
    
    // Store configuration parameters
    config_dir_ = config_dir;
    
    // Create config directory if it doesn't exist
    try {
        if (!fs::exists(config_dir_)) {
            fs::create_directories(config_dir_);
        }
    } catch (const std::exception& e) {
        LOG(ERROR, LOG_TAG) << "Failed to create config directory: " << e.what() << "\n";
        return false;
    }
    
    // Set config file path
    config_file_ = config_dir_ + "/chrony.conf";
    
    LOG(INFO, LOG_TAG) << "Initialized chrony client with config directory: " << config_dir_ << "\n";
    return true;
}

bool ChronyClient::connectToServer(const std::string& server_address, uint16_t port)
{
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (connected_) {
        LOG(WARNING, LOG_TAG) << "Already connected to a server\n";
        return true;
    }
    
    // Store server information
    server_address_ = server_address;
    port_ = port;
    
    // Check if chronyd is already running
    std::string status = execCommand("chronyc tracking 2>/dev/null");
    if (status.empty()) {
        // Start chronyd if not running
        std::string cmd = "chronyd";
        LOG(INFO, LOG_TAG) << "Starting chronyd: " << cmd << "\n";
        
        // Start chronyd as a background process
        std::string result = execCommand(cmd + " & echo $!");
        if (result.empty()) {
            LOG(ERROR, LOG_TAG) << "Failed to start chronyd\n";
            return false;
        }
        
        // Wait for chronyd to start
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    
    // Configure client to use server
    LOG(INFO, LOG_TAG) << "Configuring chronyd to use server: " << server_address << "\n";
    
    // Try to resolve server address using mDNS/Avahi if it's a hostname
    resolved_address_ = ""; // Clear any previous resolved address
    std::string resolved_address = server_address;
    
    // First try to resolve using getent hosts (DNS lookup)
    if (server_address.find_first_not_of("0123456789.") != std::string::npos && 
        server_address != "localhost" && server_address != "127.0.0.1") {
        std::string ip_cmd = "getent hosts " + server_address + " | awk '{print $1}'";
        std::string resolved_ip = execCommand(ip_cmd);
        if (!resolved_ip.empty()) {
            // Remove any trailing whitespace
            resolved_ip.erase(resolved_ip.find_last_not_of("\n\r\t ") + 1);
            resolved_address = resolved_ip;
            resolved_address_ = resolved_ip; // Store for later use
            LOG(INFO, LOG_TAG) << "Resolved " << server_address << " to " << resolved_address << " via DNS\n";
        }
    }
    
    // If DNS didn't work, try mDNS/Avahi for local hostnames
    if (resolved_address == server_address && 
        server_address.find(".") == std::string::npos && 
        server_address.find(":") == std::string::npos) {
        // This looks like a hostname without domain, try to resolve via Avahi
        std::string avahi_cmd = "avahi-resolve-host-name " + server_address + ".local 2>/dev/null | awk '{print $2}'";
        std::string avahi_result = execCommand(avahi_cmd);
        
        if (!avahi_result.empty()) {
            // Remove any trailing whitespace
            avahi_result.erase(avahi_result.find_last_not_of("\n\r\t ") + 1);
            resolved_address = avahi_result;
            resolved_address_ = avahi_result; // Store for later use
            LOG(INFO, LOG_TAG) << "Resolved " << server_address << " to " << resolved_address << " via mDNS\n";
        } else {
            LOG(WARNING, LOG_TAG) << "Could not resolve " << server_address << " via mDNS, using as-is\n";
        }
    }
    
    // Configure chrony client with server and options in a single command
    bool server_added = false;
    for (int retry = 0; retry < 3; retry++) {
        // Combined command to add server with appropriate options and fallback pool
        std::string cmd = "chronyc -a \
"
                          "'add server " + resolved_address + " prefer iburst' \
"
                          "'burst' \
"
                          "'add pool pool.ntp.org iburst'";
        
        LOG(INFO, LOG_TAG) << "Running command: " << cmd << "\n";
        std::string result = execCommand(cmd);
        
        // Check if commands succeeded (should see multiple 200 OK responses)
        size_t pos = 0;
        int ok_count = 0;
        while ((pos = result.find("200 OK", pos)) != std::string::npos) {
            ok_count++;
            pos += 6; // length of "200 OK"
        }
        
        if (ok_count >= 2) { // We expect at least 2 OK responses
            server_added = true;
            LOG(INFO, LOG_TAG) << "Successfully configured chrony client with server and fallback pool\n";
            break;
        }
        
        LOG(WARNING, LOG_TAG) << "Failed to configure client (attempt " << retry+1 << "/3): " << result << "\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    
    if (!server_added) {
        LOG(ERROR, LOG_TAG) << "Failed to configure chrony client after multiple attempts\n";
        // Continue anyway, as it might still work or the server might already be added
    }
    
    // Verify server was added by checking sources
    bool server_verified = false;
    for (int retry = 0; retry < 5; retry++) {
        std::string sources = execCommand("chronyc sources 2>/dev/null");
        // Check for either the original server address or the resolved address
        if (sources.find(server_address) != std::string::npos || 
            (resolved_address != server_address && sources.find(resolved_address) != std::string::npos)) {
            server_verified = true;
            LOG(INFO, LOG_TAG) << "Server verified in sources list\n";
            break;
        }
        
        LOG(INFO, LOG_TAG) << "Waiting for server to appear in sources list (attempt " << retry+1 << "/5)\n";
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    
    if (!server_verified) {
        LOG(WARNING, LOG_TAG) << "Server not found in sources list, but continuing anyway\n";
    }
    
    // Mark as connected
    connected_ = true;
    stop_requested_ = false;
    
    // Start monitor thread
    monitor_thread_ = std::make_unique<std::thread>(&ChronyClient::monitorThread, this);
    
    LOG(NOTICE, LOG_TAG) << "Connected to chrony server at " << server_address_ << "\n";
    return true;
}

void ChronyClient::disconnect()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        
        if (!connected_) {
            return;
        }
        
        // Request stop
        stop_requested_ = true;
        
        // Remove server using chronyc command
        std::string cmd = "chronyc -a 'delete " + server_address_ + "'";
        std::string result = execCommand(cmd);
        
        if (result.find("200 OK") == std::string::npos) {
            LOG(WARNING, LOG_TAG) << "Failed to remove server: " << result << "\n";
            // Continue anyway
        }
    }
    
    // Wait for monitor thread to finish
    if (monitor_thread_ && monitor_thread_->joinable()) {
        monitor_thread_->join();
        monitor_thread_.reset();
    }
    
    {
        std::lock_guard<std::mutex> lock(mutex_);
        connected_ = false;
        LOG(INFO, LOG_TAG) << "Disconnected from chrony server\n";
    }
}

bool ChronyClient::isConnected() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return connected_;
}

std::string ChronyClient::getStatus() const
{
    if (!isConnected()) {
        return "Not connected to chrony server";
    }
    
    // Get tracking information directly from chronyc
    std::string tracking = execCommand("chronyc tracking 2>/dev/null");
    if (tracking.empty()) {
        return "Connected to chrony server but tracking information is not available";
    }
    
    // Get sources information directly from chronyc
    std::string sources = execCommand("chronyc sources 2>/dev/null");
    
    // Get sourcestats information directly from chronyc
    std::string sourcestats = execCommand("chronyc sourcestats 2>/dev/null");
    
    std::stringstream status;
    status << "Connected to chrony server at " << server_address_ << "\n\n";
    status << "Tracking:\n" << tracking << "\n";
    status << "Sources:\n" << sources << "\n";
    status << "Source Statistics:\n" << sourcestats;
    
    return status.str();
}

std::optional<snapcast::ChronyTrackingInfo> ChronyClient::getTrackingInfo() const {
    if (!isConnected()) {
        return std::nullopt;
    }
    
    std::string tracking = execCommand("chronyc -n tracking 2>/dev/null");
    return snapcast::ChronyTrackingInfo::parse(tracking);
}

std::string ChronyClient::getServerAddress() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return server_address_;
}

uint16_t ChronyClient::getPort() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return port_;
}

bool ChronyClient::generateConfig(const std::string& server_address, uint16_t port) {
    try {
        std::ofstream config(config_file_);
        if (!config.is_open()) {
            LOG(ERROR, LOG_TAG) << "Failed to open config file for writing: " << config_file_ << "\n";
            return false;
        }
        
        // Write chrony client configuration
        config << "# Snapcast chrony client configuration\n";
        config << "# Generated automatically - do not edit\n\n";
        
        // Server connection
        config << "# Connect to Snapcast server's chrony master\n";
        config << "server " << server_address << " port " << port << " iburst\n\n";
        
        // Make this server our preferred source
        config << "# Make Snapcast server our preferred time source\n";
        config << "prefer " << server_address << "\n\n";
        
        // Optimize for audio synchronization
        config << "# Optimize for audio synchronization\n";
        config << "maxupdateskew 100.0\n";
        config << "makestep 0.1 3\n";
        config << "driftfile " << config_dir_ << "/drift\n";
        config << "logdir " << config_dir_ << "\n";
        config << "log measurements statistics tracking\n\n";
        
        // Don't act as a server
        config << "# Don't act as a server\n";
        config << "port 0\n";
        
        config.close();
        LOG(INFO, LOG_TAG) << "Generated chrony client configuration at " << config_file_ << "\n";
        return true;
    } catch (const std::exception& e) {
        LOG(ERROR, LOG_TAG) << "Failed to generate config: " << e.what() << "\n";
        return false;
    }
}

bool ChronyClient::isChronyInstalled() const {
    std::string result = execCommand("which chronyd 2>/dev/null");
    return !result.empty();
}

bool ChronyClient::startClient() {
    // Stop any existing client
    stopClient();
    
    // Start chronyd with our configuration
    std::string cmd = "chronyd -f " + config_file_ + " -d";
    LOG(INFO, LOG_TAG) << "Starting chrony client: " << cmd << "\n";
    
    // Start chronyd as a background process
    std::string pid_output = execCommand(cmd + " & echo $!");
    try {
        chrony_pid_ = std::stoi(pid_output);
    } catch (const std::exception& e) {
        LOG(ERROR, LOG_TAG) << "Failed to start chronyd: " << e.what() << "\n";
        return false;
    }
    
    // Check if chronyd is running
    if (chrony_pid_ <= 0) {
        LOG(ERROR, LOG_TAG) << "Failed to start chronyd\n";
        return false;
    }
    
    // Wait for client to connect to server
    std::this_thread::sleep_for(std::chrono::seconds(2));
    
    // Check if client is connected to server
    std::string sources = execCommand("chronyc -n sources 2>/dev/null");
    if (sources.find(server_address_) == std::string::npos) {
        LOG(WARNING, LOG_TAG) << "Chrony client started but not connected to server yet\n";
        // Continue anyway, as it might connect later
    }
    
    LOG(INFO, LOG_TAG) << "Chrony client started successfully\n";
    return true;
}

void ChronyClient::stopClient() {
    if (chrony_pid_ > 0) {
        LOG(INFO, LOG_TAG) << "Stopping chrony client (PID: " << chrony_pid_ << ")\n";
        kill(chrony_pid_, SIGTERM);
        
        // Wait briefly for clean shutdown
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        
        // Check if still running
        if (kill(chrony_pid_, 0) == 0) {
            LOG(WARNING, LOG_TAG) << "Chrony client did not stop gracefully, forcing termination\n";
            kill(chrony_pid_, SIGKILL);
        }
        
        chrony_pid_ = -1;
    }
}

void ChronyClient::monitorThread() {
    LOG(INFO, LOG_TAG) << "Chrony client monitor thread started\n";
    
    while (!stop_requested_) {
        // Check if chronyd is still running
        std::string tracking = execCommand("chronyc tracking 2>/dev/null");
        if (tracking.empty()) {
            LOG(WARNING, LOG_TAG) << "Chrony daemon not running\n";
            
            std::lock_guard<std::mutex> lock(mutex_);
            connected_ = false;
            break;
        }
        
        // Check if still connected to server
        std::string sources = execCommand("chronyc -n sources 2>/dev/null");
        
        // Store the resolved address for use in the monitor thread
        if (resolved_address_.empty()) {
            // Try to resolve server address if not already done
            if (server_address_.find(".") == std::string::npos && server_address_.find(":") == std::string::npos) {
                std::string avahi_cmd = "avahi-resolve-host-name " + server_address_ + ".local 2>/dev/null | awk '{print $2}'";
                std::string avahi_result = execCommand(avahi_cmd);
                
                if (!avahi_result.empty()) {
                    // Remove any trailing whitespace
                    avahi_result.erase(avahi_result.find_last_not_of("\n\r\t ") + 1);
                    resolved_address_ = avahi_result;
                    LOG(INFO, LOG_TAG) << "Monitor thread resolved " << server_address_ << " to " << resolved_address_ << " via mDNS\n";
                }
            }
        }
        
        // Check if either the original or resolved address is in the sources list
        bool server_found = sources.find(server_address_) != std::string::npos || 
                           (!resolved_address_.empty() && sources.find(resolved_address_) != std::string::npos);
        
        if (!server_found) {
            LOG(WARNING, LOG_TAG) << "Lost connection to chrony server\n";
            
            // Try to reconnect
            std::lock_guard<std::mutex> lock(mutex_);
            if (connected_) {
                LOG(INFO, LOG_TAG) << "Attempting to reconnect to chrony server\n";
                
                // Use the resolved address if available
                std::string target_address = resolved_address_.empty() ? server_address_ : resolved_address_;
                
                // Add server using chronyc command
                std::string cmd = "chronyc -a 'add server " + target_address + " iburst prefer'";
                std::string result = execCommand(cmd);
                
                if (result.find("200 OK") == std::string::npos) {
                    LOG(ERROR, LOG_TAG) << "Failed to reconnect to chrony server\n";
                    connected_ = false;
                    break;
                }
            }
        }
        
        // Sleep for a bit
        std::this_thread::sleep_for(std::chrono::seconds(10));
    }
    
    LOG(INFO, LOG_TAG) << "Chrony client monitor thread stopped\n";
}

} // namespace snapclient
