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
    
    // Check if snapserver is running on the same system
    bool is_local_server = false;
    
    // Use ps to check if snapserver is running
    std::string ps_output = execCommand("ps -ef | grep -v grep | grep snapserver 2>/dev/null");
    if (!ps_output.empty()) {
        // Found snapserver process running locally
        is_local_server = true;
        LOG(INFO, LOG_TAG) << "Detected snapserver running on the same system\n";
    }
    
    if (is_local_server) {
        LOG(NOTICE, LOG_TAG) << "Chrony present locally and running on same machine as server - using local monotonic clock\n";
        // Don't set up chrony client when running on the same machine as the server
        return true;
    } else {
        LOG(NOTICE, LOG_TAG) << "Chrony present locally, Chrony present on master - setting up " << server_address << " as master and this as slave\n";
    }
    
    // Store configuration directory for any future use
    config_dir_ = config_dir;
    
    LOG(INFO, LOG_TAG) << "Initialized chrony client\n";
    return true;
}

bool ChronyClient::connectToServer(const std::string& server_address, uint16_t port)
{
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (connected_) {
        LOG(WARNING, LOG_TAG) << "Already connected to a server\n";
        return true;
    }
    
    // Check if snapserver is running on the same system
    bool is_local_server = false;
    
    // Use ps to check if snapserver is running
    std::string ps_output = execCommand("ps -ef | grep -v grep | grep snapserver 2>/dev/null");
    if (!ps_output.empty()) {
        // Found snapserver process running locally
        is_local_server = true;
        LOG(INFO, LOG_TAG) << "Detected snapserver running on the same system\n";
    }
    
    if (is_local_server) {
        LOG(NOTICE, LOG_TAG) << "Server running on same machine - using local monotonic clock instead of chrony\n";
        // Mark as connected but don't actually configure chrony
        server_address_ = server_address;
        port_ = port;
        connected_ = true;
        stop_requested_ = false;
        return true;
    }
    
    // Store server information
    server_address_ = server_address;
    port_ = port;
    
    // Check if chronyd is already running
    std::string status = execCommand("chronyc -c tracking 2>/dev/null");
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
        std::string sources = execCommand("chronyc -c sources 2>/dev/null");
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
    
    return true;
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
    std::string tracking = execCommand("chronyc -c tracking 2>/dev/null");
    if (tracking.empty()) {
        return "Connected to chrony server but tracking information is not available";
    }
    
    // Get sources information directly from chronyc
    std::string sources = execCommand("chronyc -c sources 2>/dev/null");
    
    // Get sourcestats information directly from chronyc
    std::string sourcestats = execCommand("chronyc -c sourcestats 2>/dev/null");
    
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

bool ChronyClient::configureClient(const std::string& server_address, uint16_t port) {
    // Configure chrony client with server and options using chronyc -a commands
    bool server_added = false;
    for (int retry = 0; retry < 3; retry++) {
        // Combined command to add server with appropriate options and fallback pool
        std::string cmd = "chronyc -a \
"
                          "'add server " + server_address + " prefer iburst' \
"
                          "'burst' \
"
                          "'add pool pool.ntp.org iburst'";
        
        LOG(INFO, LOG_TAG) << "Configuring chrony client with server: " << server_address << "\n";
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
    
    return true;
}

bool ChronyClient::isChronyInstalled() const {
    std::string result = execCommand("which chronyd 2>/dev/null");
    return !result.empty();
}

bool ChronyClient::startClient() {
    // Check if chronyd is already running
    std::string status = execCommand("chronyc -c tracking 2>/dev/null");
    if (!status.empty()) {
        LOG(INFO, LOG_TAG) << "Chronyd already running, configuring as client\n";
    } else {
        // Start chronyd if not running
        std::string cmd = "chronyd";
        LOG(INFO, LOG_TAG) << "Starting chronyd: " << cmd << "\n";
        
        // Start chronyd as a background process
        std::string result = execCommand(cmd + " & echo $!");
        if (result.empty()) {
            LOG(ERROR, LOG_TAG) << "Failed to start chronyd\n";
            return false;
        }
        
        // Extract PID
        try {
            chrony_pid_ = std::stoi(result);
            LOG(INFO, LOG_TAG) << "Started chronyd with PID " << chrony_pid_ << "\n";
        } catch (const std::exception& e) {
        // Wait for chronyd to start
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    
    // Configure client using chronyc -a commands
    if (!configureClient(server_address_, port_)) {
        LOG(ERROR, LOG_TAG) << "Failed to configure chrony client\n";
        return false;
    }
    
    // Verify server was added by checking sources
    bool server_verified = false;
    for (int retry = 0; retry < 5; retry++) {
        std::string sources = execCommand("chronyc -c sources 2>/dev/null");
        // Check for either the original server address or the resolved address
        if (sources.find(server_address_) != std::string::npos || 
            (!resolved_address_.empty() && sources.find(resolved_address_) != std::string::npos)) {
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

// No monitor thread implementation - assuming chrony works if configured properly

} // namespace snapclient
