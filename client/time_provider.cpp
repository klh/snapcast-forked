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

void TimeProvider::setPreferredSource(time_sync::TimeSyncSource source)
{
    // Update the sync_info_ with the preferred source
    sync_info_.source = source;
    sync_info_.available = true;
    
    // Set quality and error based on source
    if (source == time_sync::TimeSyncSource::MONOTONIC) {
        sync_info_.quality = 0.95f; // Highest quality for monotonic clock
        sync_info_.estimated_error_ms = 0.1f; // Lowest error for monotonic clock
        LOG(INFO, LOG_TAG) << "Setting preferred time source to MONOTONIC";
    } else if (source == time_sync::TimeSyncSource::CHRONY) {
        sync_info_.quality = 0.9f; // High quality for chrony
        sync_info_.estimated_error_ms = 1.0f; // Low error for chrony
        LOG(INFO, LOG_TAG) << "Setting preferred time source to CHRONY";
    } else {
        // Default to system time
        sync_info_.quality = 0.5f; // Medium quality for system time
        sync_info_.estimated_error_ms = 10.0f; // Higher error for system time
        LOG(INFO, LOG_TAG) << "Setting preferred time source to SYSTEM";
    }
    
    // Update timestamp
    sync_info_.last_update = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

time_sync::TimeSyncInfo TimeProvider::getSyncInfo() const
{
    // If we have a preferred source set, return that
    if (sync_info_.available) {
        return sync_info_;
    }
    
    // Otherwise, determine the best source based on configuration
    time_sync::TimeSyncInfo info;
    
    // Set source and availability based on configuration
    if (local_server_) {
        // Always prioritize local server detection
        info.source = time_sync::TimeSyncSource::MONOTONIC;
        info.available = true;
        info.quality = 0.95f; // Highest quality for local server
        info.estimated_error_ms = 0.1f; // Lowest error for local server
        LOG(TRACE, LOG_TAG) << "Using MONOTONIC time source (local server)";
    } else if (chrony_available_) {
        info.source = time_sync::TimeSyncSource::CHRONY;
        info.available = true;
        info.quality = 0.9f; // High quality for chrony
        info.estimated_error_ms = 0.5f; // Very low error for chrony
        LOG(TRACE, LOG_TAG) << "Using CHRONY time source";
    } else {
        info.source = time_sync::TimeSyncSource::NONE;
        info.available = true;
        info.quality = 0.5f; // Medium quality for system time
        info.estimated_error_ms = 10.0f; // Higher error for system time
        LOG(TRACE, LOG_TAG) << "Using NONE time source (fallback)";
    }
    
    // Set last update timestamp
    info.last_update = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    
    return info;
}

void TimeProvider::configure(const ClientSettings::TimeSync& settings)
{
    LOG(INFO, LOG_TAG) << "Configuring time provider with client settings";
    
    // Store settings
    settings_ = settings;
    
    // Verify chrony is available and properly configured
    verifyChrony();
    
    // After verifyChrony, check if we're on the same machine as the server
    // If so, force the time source to be MONOTONIC regardless of other settings
    if (local_server_) {
        // Make sure we're using the monotonic clock when running on the same machine as the server
        if (sync_info_.source != time_sync::TimeSyncSource::MONOTONIC) {
            LOG(INFO, LOG_TAG) << "Forcing time source to MONOTONIC due to local server detection";
            setPreferredSource(time_sync::TimeSyncSource::MONOTONIC);
        }
    }
}

void TimeProvider::verifyChrony()
{
    // Check if server is running on the same machine
    // If so, we can use the local clock directly
    local_server_ = false;
    chrony_available_ = false; // Start with chrony disabled by default
    
    // If on_server flag is set, use that directly and skip detection
    if (settings_.on_server) {
        local_server_ = true;
        LOG(INFO, LOG_TAG) << "Using local clock as specified by --on-server flag";
        // Force time source to MONOTONIC when running on the same machine as server
        setPreferredSource(time_sync::TimeSyncSource::MONOTONIC);
        return; // Local server is fine, no need for chrony
    }
    
    // Check for snapserver process using multiple methods
    // Method 1: Check using pgrep with a more flexible pattern
    FILE* fp = popen("pgrep snapserver", "r");
    if (fp != nullptr) {
        char buffer[10];
        if (fgets(buffer, sizeof(buffer), fp) != nullptr) {
            local_server_ = true;
            LOG(INFO, LOG_TAG) << "Detected snapserver running on local machine (pgrep), using local clock";
            // Force time source to MONOTONIC when running on the same machine as server
            setPreferredSource(time_sync::TimeSyncSource::MONOTONIC);
            pclose(fp);
            return; // Local server is fine, no need for chrony
        }
        pclose(fp);
    }
    
    // Method 2: Check using ps
    fp = popen("ps aux | grep -v grep | grep snapserver", "r");
    if (fp != nullptr) {
        char buffer[128];
        if (fgets(buffer, sizeof(buffer), fp) != nullptr) {
            local_server_ = true;
            LOG(INFO, LOG_TAG) << "Detected snapserver running on local machine (ps), using local clock";
            // Force time source to MONOTONIC when running on the same machine as server
            setPreferredSource(time_sync::TimeSyncSource::MONOTONIC);
            pclose(fp);
            return; // Local server is fine, no need for chrony
        }
        pclose(fp);
    }
    
    // Note: We can't check server host here because TimeSync doesn't have access to it
    // This check is done in Controller::initChronyClient instead
    
    // If we're using a fixed time source and it's set to MONOTONIC, honor that setting
    if (settings_.mode == time_sync::SyncMode::fixed && 
        settings_.preferred_source == static_cast<int>(time_sync::TimeSyncSource::MONOTONIC)) {
        LOG(INFO, LOG_TAG) << "Using monotonic clock as specified by time source preference";
        local_server_ = true; // Treat as local server to use monotonic clock
        // Force time source to MONOTONIC
        setPreferredSource(time_sync::TimeSyncSource::MONOTONIC);
        return;
    }
    
    LOG(INFO, LOG_TAG) << "No local snapserver detected, will use chrony for time synchronization";
    
    // For remote server, chrony is required
    auto& chronyClient = snapclient::ChronyClient::getInstance();
    
    try {
        // Verify chrony is installed using the base class method
        chronyClient.verifyChronoInstalled();
        
        // Mark chrony as available
        chrony_available_ = true;
        LOG(INFO, LOG_TAG) << "Chrony detected and available for time synchronization";
        
        // Check if chrony is synchronized
        checkSynchronization();
    } catch (const std::exception& e) {
        LOG(ERROR, LOG_TAG) << "Chrony initialization failed: " << e.what();
        LOG(WARNING, LOG_TAG) << "Falling back to system time";
        chrony_available_ = false;
    }
}

void TimeProvider::checkSynchronization()
{
    // Skip check if server is on the same machine
    if (local_server_) {
        return;
    }
    
    // Use the ChronyBase method to check synchronization
    auto& chronyClient = snapclient::ChronyClient::getInstance();
    chronyClient.checkSynchronization();
    
    LOG(DEBUG, LOG_TAG) << "Chrony synchronization verified";
}

chronos::time_point_clk TimeProvider::getCurrentTime()
{
    // If we're on the same machine as the server, always use the local clock
    if (local_server_) {
        auto now = chronos::clk::now();
        
        // Log the current time at trace level
        auto duration = now.time_since_epoch();
        auto microseconds = std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
        LOG(TRACE, LOG_TAG) << "getCurrentTime (local): " << microseconds / 1000000 << "." << microseconds % 1000000;
        
        return now;
    }
    
    // For remote servers, use chrony and periodically check synchronization
    static auto last_check = std::chrono::steady_clock::now();
    auto now_check = std::chrono::steady_clock::now();
    
    if (chrony_available_ && std::chrono::duration_cast<std::chrono::seconds>(now_check - last_check).count() > 10) {
        try {
            // Verify chrony is still synchronized
            checkSynchronization();
            last_check = now_check;
        } catch (const std::exception& e) {
            LOG(ERROR, LOG_TAG) << "Time synchronization error: " << e.what();

            throw; // Re-throw to halt playback if synchronization is lost
        }
    }
    
    // When chrony is properly configured, we can use system time directly
    // as it's already synchronized with the server
    auto now = chronos::clk::now();
    
    // Log the current time at trace level
    auto duration = now.time_since_epoch();
    auto microseconds = std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
    LOG(TRACE, LOG_TAG) << "getCurrentTime (chrony): " << microseconds / 1000000 << "." << microseconds % 1000000 << "\n";
    
    return now;
}
    

