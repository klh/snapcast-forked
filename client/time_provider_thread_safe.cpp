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

// Thread-safe implementation of time source methods

TimeSyncInfo TimeProvider::getSyncInfo() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (time_sources_.find(current_source_) != time_sources_.end()) {
        return time_sources_.at(current_source_);
    }
    
    // Return default info if current source not found
    TimeSyncInfo info;
    info.source = current_source_;
    return info;
}

void TimeProvider::setPreferredSyncSource(TimeSyncSource source)
{
    std::lock_guard<std::mutex> lock(mutex_);
    
    preferred_source_ = source;
    LOG(INFO, LOG_TAG) << "Set preferred time source to: " << static_cast<int>(source) << "\n";
    
    // Re-select the best time source based on the new preference
    selectBestTimeSource();
}

void TimeProvider::selectBestTimeSource()
{
    // Note: This method is called from other methods that already hold the mutex
    // No need to lock again here
    
    // If we have a preferred source and it's available, use it
    if (preferred_source_ != TimeSyncSource::NONE && 
        time_sources_.find(preferred_source_) != time_sources_.end() && 
        time_sources_[preferred_source_].available) {
        current_source_ = preferred_source_;
        LOG(INFO, LOG_TAG) << "Using preferred time source: " << static_cast<int>(current_source_) << "\n";
        return;
    }
    
    // Try sources in order of precision
    if (time_sources_.find(TimeSyncSource::CHRONY) != time_sources_.end() && 
        time_sources_[TimeSyncSource::CHRONY].available) {
        current_source_ = TimeSyncSource::CHRONY;
    } else if (time_sources_.find(TimeSyncSource::PTP) != time_sources_.end() && 
               time_sources_[TimeSyncSource::PTP].available) {
        current_source_ = TimeSyncSource::PTP;
    } else if (time_sources_.find(TimeSyncSource::NTP) != time_sources_.end() && 
               time_sources_[TimeSyncSource::NTP].available) {
        current_source_ = TimeSyncSource::NTP;
    } else {
        // Fall back to monotonic clock
        current_source_ = TimeSyncSource::MONOTONIC;
        
        // Ensure we have the monotonic source in our map
        if (time_sources_.find(TimeSyncSource::MONOTONIC) == time_sources_.end()) {
            TimeSyncInfo info;
            info.source = TimeSyncSource::MONOTONIC;
            info.quality = 0.5;
            info.estimated_error_ms = 50.0;
            info.available = true;
            time_sources_[TimeSyncSource::MONOTONIC] = info;
        }
    }
    
    LOG(INFO, LOG_TAG) << "Selected time source: " << static_cast<int>(current_source_) 
                       << ", quality: " << time_sources_[current_source_].quality << "\n";
}

void TimeProvider::configure(const ClientSettings::TimeSync& settings)
{
    std::lock_guard<std::mutex> lock(mutex_);
    
    LOG(INFO, LOG_TAG) << "Configuring time provider with client settings\n";
    
    // Set preferred time source from settings
    if (settings.preferred_source >= 0 && settings.preferred_source < 4) {
        preferred_source_ = static_cast<TimeSyncSource>(settings.preferred_source);
        LOG(INFO, LOG_TAG) << "Set preferred time source to: " << settings.preferred_source << "\n";
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

void TimeProvider::negotiateSyncSource(const TimeSyncInfo& server_info)
{
    std::lock_guard<std::mutex> lock(mutex_);
    
    // If we haven't detected our available time sources yet, do it now
    if (time_sources_.empty()) {
        detectAvailableTimeSources();
    }
    
    // If we already have a high-quality time source, stick with it
    if (current_source_ != TimeSyncSource::NONE && 
        current_source_ != TimeSyncSource::MONOTONIC && 
        time_sources_[current_source_].quality > 0.7) {
        LOG(DEBUG, LOG_TAG) << "Keeping current high-quality time source: " << static_cast<int>(current_source_) 
                           << ", quality: " << time_sources_[current_source_].quality << "\n";
        return;
    }
    
    // Store server's time source information for reference
    TimeSyncSource serverSource = static_cast<TimeSyncSource>(server_info.source);
    if (time_sources_.find(serverSource) == time_sources_.end()) {
        time_sources_[serverSource] = TimeSyncInfo();
    }
    
    // Update server time source info but keep our local availability flag
    bool was_available = time_sources_[serverSource].available;
    time_sources_[serverSource].source = serverSource;
    time_sources_[serverSource].quality = server_info.quality;
    time_sources_[serverSource].estimated_error_ms = server_info.estimated_error_ms;
    time_sources_[serverSource].available = was_available;
    time_sources_[serverSource].last_update = 
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    
    // Always prioritize using our own best available time source
    // This ensures that clients with good clocks don't degrade to match clients with poor clocks
    selectBestTimeSource();
    
    LOG(INFO, LOG_TAG) << "Time source negotiation: Server using " << static_cast<int>(server_info.source) 
                       << " (quality: " << server_info.quality << "), client using " 
                       << static_cast<int>(current_source_) << " (quality: " 
                       << time_sources_[current_source_].quality << ")\n";
}
