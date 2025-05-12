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

#include "time_sync.hpp"
#include "time_defs.hpp"
#include <map>
#include <mutex>
#include <atomic>

namespace snapcast {

/**
 * Base class for time management functionality
 * Provides common methods for time source selection and time retrieval
 */
class TimeManager {
protected:
    // Shared state
    std::map<time_sync::TimeSyncSource, time_sync::TimeSyncInfo> time_sources_;
    std::atomic<time_sync::TimeSyncSource> current_source_{time_sync::TimeSyncSource::NONE};
    std::atomic<time_sync::ProtocolVersion> protocol_version_{time_sync::ProtocolVersion::V1};
    mutable std::mutex mutex_;
    
    /**
     * Constructor initializes time sources
     */
    TimeManager();
    
    /**
     * Virtual destructor for proper cleanup in derived classes
     */
    virtual ~TimeManager() = default;
    
    /**
     * Detect available time sources on the system
     */
    void detectAvailableTimeSources();
    
    /**
     * Select the best available time source
     * @param preferred Preferred time source (if any)
     * @param min_quality Minimum quality threshold (0.0-1.0)
     * @return The selected time source
     */
    time_sync::TimeSyncSource selectBestTimeSource(
        time_sync::TimeSyncSource preferred = time_sync::TimeSyncSource::NONE,
        double min_quality = 0.3);
    
    /**
     * Get current time using the selected or specified time source
     * @param specific Specific time source to use (NONE for current selected source)
     * @return Current time point
     */
    chronos::time_point_clk getCurrentTime(
        time_sync::TimeSyncSource specific = time_sync::TimeSyncSource::NONE);
        
    /**
     * Get protocol version
     * @return Current protocol version
     */
    time_sync::ProtocolVersion getProtocolVersion() const;
    
    /**
     * Set protocol version
     * @param version Protocol version to set
     */
    void setProtocolVersion(time_sync::ProtocolVersion version);
    
    /**
     * Get current time source information
     * @return Time sync info for the current source
     */
    time_sync::TimeSyncInfo getCurrentSourceInfo() const;
};

} // namespace snapcast
