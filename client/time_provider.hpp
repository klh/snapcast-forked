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

// local headers
#include "client_settings.hpp"
#include "common/message/message.hpp"
#include "common/time_defs.hpp"
#include "common/time_sync.hpp"
#include "double_buffer.hpp"

// standard headers
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>

/// Provides local and server time
/**
 * Stores time difference to the server
 * Returns server's local system time.
 * Clients are using the server time to play audio in sync, independent of the client's system time
 */
class TimeProvider
{
public:
    static TimeProvider& getInstance()
    {
        static TimeProvider instance;
        return instance;
    }

    void setDiffToServer(double ms);
    void setDiff(const tv& c2s, const tv& s2c);
    
    /// Negotiate the best time synchronization source with the server
    void negotiateSyncSource(const time_sync::TimeSyncInfo& server_info);
    
    /// Set fallback mode for backward compatibility with older servers
    void setFallbackMode(const time_sync::TimeSyncInfo& fallback_info);

    /// Set the preferred time synchronization source
    void setPreferredSyncSource(time_sync::TimeSyncSource source);

    /// Configure the time provider with client settings
    void configure(const ClientSettings::TimeSync& settings);
    
    /// Get current time sync information
    time_sync::TimeSyncInfo getSyncInfo() const;
    
    /// Get protocol version to use with server
    time_sync::ProtocolVersion getProtocolVersion() const;
    
    /// Set protocol version based on server capabilities
    void setProtocolVersion(time_sync::ProtocolVersion version);

    template <typename T>
    inline T getDiffToServer() const
    {
        return std::chrono::duration_cast<T>(chronos::usec(diffToServer_));
    }

    template <typename T>
    static T sinceEpoche(const chronos::time_point_clk& point)
    {
        return std::chrono::duration_cast<T>(point.time_since_epoch());
    }

    static chronos::time_point_clk toTimePoint(const tv& timeval)
    {
        return chronos::time_point_clk(chronos::usec(timeval.usec) + chronos::sec(timeval.sec));
    }

    inline static chronos::time_point_clk now()
    {
        return chronos::clk::now();
    }

    inline static chronos::time_point_clk serverNow()
    {
        return chronos::clk::now() + TimeProvider::getInstance().getDiffToServer<chronos::usec>();
    }

private:
    TimeProvider();
    TimeProvider(TimeProvider const&);   // Don't Implement
    void operator=(TimeProvider const&); // Don't implement

    // Detect available time sources on the system
    void detectAvailableTimeSources();
    
    // Select the best available time source
    void selectBestTimeSource();
    
    // Get current time using the selected time source
    chronos::time_point_clk getCurrentTime();

    DoubleBuffer<chronos::usec::rep> diffBuffer_;
    std::atomic<chronos::usec::rep> diffToServer_;
    
    // Thread safety
    mutable std::mutex mutex_;
    
    // Protocol version
    std::atomic<time_sync::ProtocolVersion> protocol_version_;
    
    // Time source management
    time_sync::TimeSyncSource preferred_source_;
    time_sync::TimeSyncSource current_source_;
    std::map<time_sync::TimeSyncSource, time_sync::TimeSyncInfo> time_sources_;
    
    // Configuration
    ClientSettings::TimeSync settings_;
};
