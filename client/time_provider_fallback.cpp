/**
    This file is part of snapcast
    Copyright (C) 2014-2024  Johannes Pohl

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
**/

#include "time_provider.hpp"
#include "common/utils/logging.hpp"

using namespace std;

static constexpr auto LOG_TAG = "TimeProvider";

void TimeProvider::setFallbackMode(const TimeSyncInfo& fallback_info)
{
    std::lock_guard<std::mutex> lock(mutex_);
    
    LOG(INFO, LOG_TAG) << "Setting fallback mode for backward compatibility with older server\n";
    
    // Clear any existing time sources except monotonic
    time_sources_.clear();
    
    // Set the fallback time source (usually monotonic)
    time_sources_[fallback_info.source] = fallback_info;
    
    // Force using only this time source
    current_source_ = fallback_info.source;
    preferred_source_ = fallback_info.source;
    
    LOG(INFO, LOG_TAG) << "Using fallback time source: " << static_cast<int>(current_source_) 
                       << ", quality: " << fallback_info.quality << "\n";
}
