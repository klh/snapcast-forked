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

// prototype/interface header file
#include "time_provider.hpp"

// local headers
#include "common/aixlog.hpp"
#include "chrony_client.hpp"

// standard headers
#include <chrono>
#include <unistd.h>

static constexpr auto LOG_TAG = "TimeProvider";

TimeProvider::TimeProvider()
{
    verifyChrony();
}

time_sync::TimeSyncInfo TimeProvider::getSyncInfo() const
{
    time_sync::TimeSyncInfo info;
    info.version = protocol_version_;
    
    // Set available time sources
    if (chrony_available_)
        info.sources.push_back(time_sync::TimeSyncSource::CHRONY);
    else if (local_server_)
        info.sources.push_back(time_sync::TimeSyncSource::MONOTONIC);
    
    info.sources.push_back(time_sync::TimeSyncSource::SYSTEM);
    
    return info;
}

void TimeProvider::configure(const ClientSettings::TimeSync& settings)
{
    LOG(INFO, LOG_TAG) << "Configuring time provider with client settings\n";
    
    // Store settings
    settings_ = settings;
    
    // Re-check if chrony is available
    detectChrony();
}

void TimeProvider::verifyChrony()
{
    // Check if server is running on the same machine
    // If so, we can use the local clock directly
    local_server_ = false;
    
    // Check for snapserver process
    FILE* fp = popen("pgrep -x snapserver", "r");
    if (fp != nullptr) {
        char buffer[10];
        if (fgets(buffer, sizeof(buffer), fp) != nullptr) {
            local_server_ = true;
            LOG(INFO, LOG_TAG) << "Detected snapserver running on local machine, using local clock\n";
            pclose(fp);
            return; // Local server is fine, no need for chrony
        }
        pclose(fp);
    }
    
    // For remote server, chrony is required
    auto& chronyClient = snapclient::ChronyClient::getInstance();
    
    // Check if chrony is installed
    if (!chronyClient.isChronyInstalled()) {
        throw std::runtime_error("Chrony is not installed. It is required for time synchronization.");
    }
    
    // Mark chrony as available
    chrony_available_ = true;
    LOG(INFO, LOG_TAG) << "Chrony detected and available for time synchronization\n";
    
    // Check if chrony is synchronized
    checkSynchronization();
}

void TimeProvider::checkSynchronization()
{
    // Skip check if server is on the same machine
    if (local_server_) {
        return;
    }
    
    // Check if chrony is synchronized
    auto& chronyClient = snapclient::ChronyClient::getInstance();
    if (!chronyClient.isSynchronized()) {
        throw std::runtime_error("Chrony is not properly synchronized. Accurate time synchronization is required.");
    }
    
    LOG(DEBUG, LOG_TAG) << "Chrony synchronization verified\n";
}

chronos::time_point_clk TimeProvider::getCurrentTime()
{
    // Periodically check chrony synchronization status (every ~10 seconds)
    static auto last_check = std::chrono::steady_clock::now();
    auto now_check = std::chrono::steady_clock::now();
    
    if (std::chrono::duration_cast<std::chrono::seconds>(now_check - last_check).count() > 10) {
        try {
            // Verify chrony is still synchronized
            checkSynchronization();
            last_check = now_check;
        } catch (const std::exception& e) {
            LOG(ERROR, LOG_TAG) << "Time synchronization error: " << e.what() << "\n";
            throw; // Re-throw to halt playback if synchronization is lost
        }
    }
    
    // When chrony is properly configured, we can use system time directly
    // as it's already synchronized with the server
    auto now = chronos::clk::now();
    
    // Log the current time at trace level
    auto duration = now.time_since_epoch();
    auto microseconds = std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
    LOG(TRACE, LOG_TAG) << "getCurrentTime: " << microseconds / 1000000 << "." << microseconds % 1000000 << "\n";
    
    return now;
}
    

