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

void TimeProvider::setPreferredSyncSource(TimeSyncSource source)
{
    preferred_source_ = source;
    LOG(INFO, LOG_TAG) << "Set preferred time source to: " << static_cast<int>(source) << "\n";
    
    // Re-select the best time source based on the new preference
    selectBestTimeSource();
}

void TimeProvider::configure(const ClientSettings::TimeSync& settings)
{
    LOG(INFO, LOG_TAG) << "Configuring time provider with client settings\n";
    
    // Set preferred time source from settings
    if (settings.preferred_source >= 0 && settings.preferred_source < 4) {
        setPreferredSyncSource(static_cast<TimeSyncSource>(settings.preferred_source));
    } else {
        // Auto mode - no specific preference
        preferred_source_ = TimeSyncSource::NONE;
    }
    
    // Apply minimum quality threshold
    double min_quality = settings.min_quality;
    if (min_quality < 0.0) min_quality = 0.0;
    if (min_quality > 1.0) min_quality = 1.0;
    
    // Apply time sync mode
    switch (settings.mode) {
        case ClientSettings::TimeSync::Mode::fixed:
            // In fixed mode, only use the preferred source
            if (preferred_source_ != TimeSyncSource::NONE) {
                // Force the source to be available
                if (time_sources_.find(preferred_source_) == time_sources_.end()) {
                    time_sources_[preferred_source_] = TimeSyncInfo();
                }
                time_sources_[preferred_source_].available = true;
                
                // Make other sources unavailable
                for (auto& source : time_sources_) {
                    if (source.first != preferred_source_) {
                        source.second.available = false;
                    }
                }
                
                LOG(INFO, LOG_TAG) << "Fixed mode: Using only time source " 
                                   << static_cast<int>(preferred_source_) << "\n";
            }
            break;
            
        case ClientSettings::TimeSync::Mode::server_guided:
            // Server will guide source selection, we'll respect its choice
            LOG(INFO, LOG_TAG) << "Server-guided mode: Server will select time source\n";
            break;
            
        case ClientSettings::TimeSync::Mode::disabled:
            // Disable time synchronization, use only monotonic clock
            for (auto& source : time_sources_) {
                if (source.first != TimeSyncSource::MONOTONIC) {
                    source.second.available = false;
                }
            }
            
            // Ensure monotonic source is available
            if (time_sources_.find(TimeSyncSource::MONOTONIC) == time_sources_.end()) {
                time_sources_[TimeSyncSource::MONOTONIC] = TimeSyncInfo();
            }
            time_sources_[TimeSyncSource::MONOTONIC].available = true;
            preferred_source_ = TimeSyncSource::MONOTONIC;
            
            LOG(INFO, LOG_TAG) << "Time sync disabled: Using only monotonic clock\n";
            break;
            
        case ClientSettings::TimeSync::Mode::auto_select:
        default:
            // Auto-select the best available source
            detectAvailableTimeSources();
            LOG(INFO, LOG_TAG) << "Auto mode: Detecting available time sources\n";
            break;
    }
    
    // Re-select the best time source based on the new settings
    selectBestTimeSource();
}
