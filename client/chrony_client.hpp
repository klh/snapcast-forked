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

#pragma once

#include "common/time_sync.hpp"
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace snapclient {

/**
 * Manages chrony client configuration and connection to Snapcast server's chrony master
 * 
 * This class configures the local chrony client to synchronize with the Snapcast server's
 * chrony master, ensuring precise time synchronization for audio playback.
 */
class ChronyClient {
public:
    // Get the singleton instance
    static ChronyClient& getInstance() {
        static ChronyClient instance;
        return instance;
    }
    
    /**
     * Initialize the chrony client
     * @param config_dir Directory to store configuration files
     * @return True if initialization was successful
     */
    bool init(const std::string& config_dir);
    
    /**
     * Connect to the Snapcast server's chrony master
     * @param server_address Server hostname or IP address
     * @param port Server port (default: 323)
     * @return True if connection was successful
     */
    bool connectToServer(const std::string& server_address, uint16_t port = 323);
    
    // No disconnect method - chrony configuration persists until system restart
    
    /**
     * Check if connected to the Snapcast server's chrony master
     * @return True if connected
     */
    bool isConnected() const;
    
    /**
     * Get the current synchronization status
     * @return A string containing synchronization status information
     */
    std::string getStatus() const;
    
    /**
     * Get detailed tracking information
     * @return Chrony tracking information
     */
    std::optional<time_sync::ChronyTrackingInfo> getTrackingInfo() const;
    
    /**
     * Get the server address we're connected to
     * @return Server address string
     */
    std::string getServerAddress() const;
    
    /**
     * Get the port we're connected to
     * @return Server port
     */
    uint16_t getPort() const;
    
private:
    ChronyClient() = default;
    ~ChronyClient();
    
    ChronyClient(const ChronyClient&) = delete;
    ChronyClient& operator=(const ChronyClient&) = delete;
    
    // Configure chrony client using chronyc -a commands
    bool configureClient(const std::string& server_address, uint16_t port);
    
    // Check if chrony is installed
    bool isChronyInstalled() const;
    
    // Start chrony client
    bool startClient();
    
    // Stop chrony client
    void stopClient();
    
    // No monitoring thread - assume chrony works if configured properly
    
    std::string config_dir_;
    std::string config_file_;
    std::string server_address_;
    std::string resolved_address_; // Stores the resolved IP address from hostname
    uint16_t port_{323};
    
    std::atomic<bool> connected_{false};
    mutable std::mutex mutex_;
    
    // Process ID of the chrony client
    pid_t chrony_pid_{-1};
};

} // namespace snapclient
