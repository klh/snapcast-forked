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

#include "time_manager.hpp"
#include "aixlog.hpp"

namespace snapcast {

static constexpr auto LOG_TAG = "TimeManager";

TimeManager::TimeManager() : 
    current_source_(time_sync::TimeSyncSource::NONE),
    protocol_version_(time_sync::ProtocolVersion::V1)
{
    // Initialize available time sources
    detectAvailableTimeSources();
}

void TimeManager::detectAvailableTimeSources()
{
    std::lock_guard<std::mutex> lock(mutex_);
    time_sources_ = time_sync::getAllTimeSourcesInfo();
}

time_sync::TimeSyncSource TimeManager::selectBestTimeSource(
    time_sync::TimeSyncSource preferred, double min_quality)
{
    std::lock_guard<std::mutex> lock(mutex_);
    time_sync::TimeSyncSource selected = time_sync::selectBestTimeSource(
        time_sources_, preferred, min_quality);
    current_source_ = selected;
    return selected;
}

chronos::time_point_clk TimeManager::getCurrentTime(time_sync::TimeSyncSource specific)
{
    try {
        time_sync::TimeValue timeValue;
        if (specific != time_sync::TimeSyncSource::NONE) {
            timeValue = time_sync::getTime(specific);
        } else {
            timeValue = time_sync::getTime(current_source_);
        }
        return timeValue.timestamp;
    } catch (const std::exception& e) {
        LOG(WARNING, LOG_TAG) << "Error getting time: " << e.what() << ", using monotonic clock\n";
        return chronos::clk::now();
    }
}

time_sync::ProtocolVersion TimeManager::getProtocolVersion() const
{
    return protocol_version_;
}

void TimeManager::setProtocolVersion(time_sync::ProtocolVersion version)
{
    protocol_version_ = version;
}

time_sync::TimeSyncInfo TimeManager::getCurrentSourceInfo() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = time_sources_.find(current_source_);
    if (it != time_sources_.end()) {
        return it->second;
    }
    return time_sync::getDefaultQualityMetrics(current_source_);
}

} // namespace snapcast
