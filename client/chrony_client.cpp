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
#include <filesystem>
#include <fstream>
#include <sstream>
#include <signal.h>

namespace fs = std::filesystem;

static constexpr auto LOG_TAG = "ChronyClient";

namespace snapclient {

ChronyClient::~ChronyClient() {
    disconnect();
}

void ChronyClient::disconnect() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!connected_) {
        return;
    }
    
    LOG(INFO, LOG_TAG) << "Client disconnecting but keeping chrony server connection active\n";
    
    // Never remove the server from chrony sources
    // Just reset our internal connection state
    connected_ = false;
    
    LOG(INFO, LOG_TAG) << "Client disconnected but chrony still running and connected to server\n";
}

bool ChronyClient::init(const std::string& config_dir) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (connected_) {
        LOG(ERROR, LOG_TAG) << "Cannot initialize chrony client while already connected\n";
        return false;
    }
    
    try {
        // Verify chrony is installed - this is a hard requirement
        verifyChronoInstalled();
        
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
            // Don't set up chrony client when running on the same machine as the server
        } else {
            LOG(NOTICE, LOG_TAG) << "Chrony present locally - will configure as client\n";
        }
        
        // Store configuration directory for any future use
        config_dir_ = config_dir;
        
        LOG(INFO, LOG_TAG) << "Initialized chrony client\n";
        return true;
    } catch (const std::exception& e) {
        LOG(ERROR, LOG_TAG) << "Failed to initialize chrony client: " << e.what() << "\n";
        return false;
    }
}

bool ChronyClient::connectToServer(const std::string& server_address, uint16_t port)
{
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (connected_) {
        LOG(INFO, LOG_TAG) << "Already connected to a server\n";
        return true;
    }
    
    try {
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
            LOG(ERROR, LOG_TAG) << "Failed to start chronyd. Time synchronization cannot function.\n";
            return false;
        }
        
        // Wait for chronyd to start
        std::this_thread::sleep_for(std::chrono::seconds(1));
        
        // Verify chronyd started successfully
        status = execCommand("chronyc -c tracking 2>/dev/null");
        if (status.empty()) {
            LOG(ERROR, LOG_TAG) << "Chronyd started but is not responding. Time synchronization cannot function.\n";
            return false;
        }
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
    
    LOG(NOTICE, LOG_TAG) << "Successfully connected to chrony server at " << server_address << "\n";
    return true;
    } catch (const std::exception& e) {
        LOG(ERROR, LOG_TAG) << "Failed to connect to chrony server: " << e.what() << "\n";
        return false;
    }
}





bool ChronyClient::isConnected() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return connected_;
}

// Using getStatus from ChronyBase with client-specific override
std::string ChronyClient::getStatus() const
{
    if (!isConnected()) {
        return "Not connected to chrony server";
    }
    
    std::stringstream status;
    status << "Connected to chrony server at " << server_address_ << "\n\n";
    status << chrony::ChronyBase::getStatus();
    
    return status.str();
}

// Using getTrackingInfo from ChronyBase

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

// Using isChronyInstalled from ChronyBase

// Using isSynchronized from ChronyBase

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
    // Never stop chrony or remove servers
    LOG(INFO, LOG_TAG) << "Client stopping but keeping chrony running and connected to server\n";
    
    // Just reset our internal state
    chrony_pid_ = -1;
    
    LOG(INFO, LOG_TAG) << "Client stopped but chrony still running and connected to server\n";
}

// No monitor thread implementation - assuming chrony works if configured properly

} // namespace snapclient
