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

#include "common/chrony_base.hpp"
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace snapserver {

/**
 * Manages a chrony master server for time synchronization
 * 
 * This class configures and manages a chrony server instance that acts as a 
 * master clock for Snapcast clients. It provides methods to:
 * 1. Start/stop the chrony server
 * 2. Configure the server for optimal audio synchronization
 * 3. Monitor the server status
 * 4. Allow clients to connect and synchronize
 */
class ChronyMaster : public chrony::ChronyBase {
public:
    // Get the singleton instance
    static ChronyMaster& getInstance() {
        static ChronyMaster instance;
        return instance;
    }
    
    /**
     * Initialize the chrony master server
     * @param config_dir Directory to store configuration files
     * @param port Port to use for chrony server (default: 323)
     * @return true if initialization was successful, false otherwise
     */
    bool init(const std::string& config_dir, uint16_t port = 323);
    
    /**
     * Start the chrony master server
     * @return true if server started successfully, false otherwise
     */
    bool start();
    
    /**
     * Stop the chrony master server
     */
    void stop();
    
    /**
     * Check if the chrony master server is running
     * @return true if the server is running, false otherwise
     */
    bool isRunning() const;
    
    // Methods inherited from ChronyBase:
    // - verifyChronoInstalled()
    // - checkSynchronization()
    // - getTrackingInfo()
    // - getStatus()
    
    /**
     * Get the current server status
     * @return A string containing server status information
     */
    std::string getStatus() const;
    
    /**
     * Get the list of connected clients
     * @return Vector of client IP addresses
     */
    std::vector<std::string> getClients() const;
    
    /**
     * Get the server configuration
     * @return Configuration as a string
     */
    std::string getConfig() const;
    
    /**
     * Get the server address for clients to connect to
     * @return Server address string
     */
    std::string getServerAddress() const;
    
    /**
     * Get the port the server is listening on
     * @return Server port
     */
    uint16_t getPort() const;
    
    /**
     * Get detailed tracking information
     * @return Chrony tracking information as TimeSyncInfo
     */
    std::optional<time_sync::TimeSyncInfo> getTrackingInfo();
    
private:
    ChronyMaster() = default;
    ~ChronyMaster();
    
    ChronyMaster(const ChronyMaster&) = delete;
    ChronyMaster& operator=(const ChronyMaster&) = delete;
    
    // Check if chrony is installed
    bool isChronyInstalled() const;
    
    // No configuration file or monitor thread - using direct chronyc commands
    
    std::string config_dir_;
    std::string config_file_;
    uint16_t port_{323};
    std::string server_address_;
    
    std::atomic<bool> running_{false};
    mutable std::mutex mutex_;
    
    // Process ID of the chrony server
    pid_t chrony_pid_{-1};
};

} // namespace snapserver
