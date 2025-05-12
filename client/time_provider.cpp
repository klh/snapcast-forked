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

// standard headers
#include <chrono>

static constexpr auto LOG_TAG = "TimeProvider";

TimeProvider::TimeProvider() : snapcast::TimeManager()
{
    diffBuffer_.setSize(200);
    diffToServer_ = 0;
}

void TimeProvider::setDiff(const tv& c2s, const tv& s2c)
{
    // Use 64-bit arithmetic to prevent overflow on large time differences
    int64_t c2s_usec = c2s.sec * INT64_C(1000000) + c2s.usec;
    int64_t s2c_usec = s2c.sec * INT64_C(1000000) + s2c.usec;
    
    // Calculate time difference using 64-bit integers
    double diff = static_cast<double>(c2s_usec - s2c_usec) / 2000.0;
    setDiffToServer(diff);
}

void TimeProvider::setDiffToServer(double ms)
{
    using namespace std::chrono_literals;
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Use steady_clock consistently for time synchronization to avoid timezone issues
    auto now = chronos::clk::now();
    static auto lastTimeSync = now;
    auto diff = chronos::abs(now - lastTimeSync);

    // Check if the time difference exceeds the maximum allowed threshold
    if (settings_.max_time_diff_ms > 0)
    {
        double current_diff_ms = static_cast<double>(diffToServer_) / 1000.0;
        if (std::abs(current_diff_ms - ms) > settings_.max_time_diff_ms)
        {
            LOG(WARNING, LOG_TAG) << "Time difference exceeds maximum threshold: " 
                                  << std::abs(current_diff_ms - ms) << " ms > " 
                                  << settings_.max_time_diff_ms << " ms. Forcing resync.\n";
            diffBuffer_.clear();
        }
    }

    /// clear diffBuffer if last update is older than a minute
    if (!diffBuffer_.empty() && (diff > 60s))
    {
        LOG(INFO, LOG_TAG) << "Last time sync older than a minute. Clearing time buffer\n";
        diffToServer_ = static_cast<chronos::usec::rep>(ms * 1000);
        diffBuffer_.clear();
    }
    lastTimeSync = now;

    // Ensure we handle 64-bit values correctly
    int64_t diff_usec = static_cast<int64_t>(ms * 1000.0);
    diffBuffer_.add(diff_usec);
    diffToServer_ = diffBuffer_.median();
    
    // Update the last sync time for the current source
    if (time_sources_.find(current_source_) != time_sources_.end()) {
        time_sources_[current_source_].last_update = 
            std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
    }
    
    LOG(DEBUG, LOG_TAG) << "setDiffToServer: " << ms << ", diff: " << diffToServer_ / 1000000 << " s, " 
                       << (diffToServer_ / 1000) % 1000 << "." << diffToServer_ % 1000 
                       << " ms, source: " << static_cast<int>(current_source_.load()) << "\n";
}

time_sync::TimeSyncInfo TimeProvider::getSyncInfo() const
{
    return getCurrentSourceInfo();
}

// These methods are now provided by the TimeManager base class

void TimeProvider::setPreferredSyncSource(time_sync::TimeSyncSource source)
{
    LOG(INFO, LOG_TAG) << "Set preferred time source to: " << time_sync::timeSourceToString(source) << "\n";
    
    // Use the base class method to select the best time source with the new preference
    selectBestTimeSource(source);
}

void TimeProvider::configure(const ClientSettings::TimeSync& settings)
{
    LOG(INFO, LOG_TAG) << "Configuring time provider with client settings\n";
    
    // Store settings
    settings_ = settings;
    
    // Set preferred time source from settings
    time_sync::TimeSyncSource preferred = time_sync::TimeSyncSource::NONE;
    if (settings.preferred_source >= 0 && settings.preferred_source < 5) {
        preferred = time_sync::intToTimeSource(settings.preferred_source);
        LOG(INFO, LOG_TAG) << "Set preferred time source to: " 
                          << time_sync::timeSourceToString(preferred) << "\n";
    } else {
        // Auto mode - no specific preference
        LOG(INFO, LOG_TAG) << "Auto mode: No specific time source preference\n";
    }
    
    // Apply time sync mode based on settings
    switch (settings.mode) {
        case time_sync::SyncMode::fixed:
            // In fixed mode, only use the preferred source if specified
            if (preferred != time_sync::TimeSyncSource::NONE) {
                LOG(INFO, LOG_TAG) << "Fixed mode: Using only time source " 
                                   << time_sync::timeSourceToString(preferred) << "\n";
                
                // Force selection of the preferred source
                detectAvailableTimeSources();
                selectBestTimeSource(preferred);
            } else {
                LOG(WARNING, LOG_TAG) << "Fixed mode specified but no preferred source set, falling back to auto mode\n";
                detectAvailableTimeSources();
                selectBestTimeSource();
            }
            break;
            
        case time_sync::SyncMode::server_guided:
            // Server will guide source selection, we'll respect its choice
            LOG(INFO, LOG_TAG) << "Server-guided mode: Server will select time source\n";
            // Just detect available sources, actual selection will happen during negotiation
            detectAvailableTimeSources();
            break;
            
        case time_sync::SyncMode::disabled:
            // Disable time synchronization, use only monotonic clock
            LOG(INFO, LOG_TAG) << "Time sync disabled: Using only monotonic clock\n";
            // Force monotonic clock selection
            selectBestTimeSource(time_sync::TimeSyncSource::MONOTONIC);
            break;
            
        case time_sync::SyncMode::auto_select:
        default:
            // Auto-select the best available source
            LOG(INFO, LOG_TAG) << "Auto mode: Detecting available time sources\n";
            detectAvailableTimeSources();
            selectBestTimeSource();
            break;
    }
}

void TimeProvider::negotiateSyncSource(const time_sync::TimeSyncInfo& server_info)
{
    std::lock_guard<std::mutex> lock(mutex_);
    
    // If we haven't detected our available time sources yet, do it now
    if (time_sources_.empty()) {
        detectAvailableTimeSources();
    }
    
    // If we're in fixed mode and have a preferred source, stick with it
    if (settings_.mode == time_sync::SyncMode::fixed && 
        preferred_source_ != time_sync::TimeSyncSource::NONE) {
        LOG(DEBUG, LOG_TAG) << "Fixed mode: Keeping preferred time source: " 
                           << time_sync::timeSourceToString(preferred_source_) << "\n";
        return;
    }
    
    // If we already have a high-quality time source, stick with it
    if (current_source_ != time_sync::TimeSyncSource::NONE && 
        current_source_ != time_sync::TimeSyncSource::MONOTONIC && 
        time_sources_[current_source_].quality > 0.7) {
        LOG(DEBUG, LOG_TAG) << "Keeping current high-quality time source: " 
                           << time_sync::timeSourceToString(current_source_) 
                           << ", quality: " << time_sources_[current_source_].quality << "\n";
        return;
    }
    
    // Store server's time source information for reference
    time_sync::TimeSyncSource serverSource = server_info.source;
    if (time_sources_.find(serverSource) == time_sources_.end()) {
        time_sources_[serverSource] = time_sync::TimeSyncInfo();
    }
    
    // Update server time source info but keep our local availability flag
    bool was_available = time_sources_[serverSource].available;
    time_sources_[serverSource] = server_info;
    time_sources_[serverSource].available = was_available;
    
    // If server suggests a source and we allow server override, try to use it
    if (settings_.allow_server_override && serverSource != time_sync::TimeSyncSource::NONE) {
        // Check if the source is available locally
        if (!time_sources_[serverSource].available) {
            // Try to detect if it's available
            bool available = time_sync::isTimeSourceAvailable(serverSource);
            time_sources_[serverSource].available = available;
            
            if (available) {
                LOG(INFO, LOG_TAG) << "Server suggested time source " 
                                  << time_sync::timeSourceToString(serverSource) 
                                  << " is available locally\n";
            } else {
                LOG(INFO, LOG_TAG) << "Server suggested time source " 
                                  << time_sync::timeSourceToString(serverSource) 
                                  << " is not available locally\n";
            }
        }
        
        // If the source is available and meets quality threshold, use it
        if (time_sources_[serverSource].available && 
            time_sources_[serverSource].quality >= settings_.min_quality) {
            current_source_ = serverSource;
            LOG(INFO, LOG_TAG) << "Using server-suggested time source: " 
                              << time_sync::timeSourceToString(current_source_) << "\n";
            return;
        }
    }
    
    // If we couldn't use the server's suggestion, select the best available source
    selectBestTimeSource();
}

void TimeProvider::setFallbackMode(const time_sync::TimeSyncInfo& fallback_info)
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
    
    LOG(INFO, LOG_TAG) << "Using fallback time source: " << time_sync::timeSourceToString(current_source_) 
                       << ", quality: " << fallback_info.quality << "\n";
}

void TimeProvider::detectAvailableTimeSources()
{
    // Note: This method is called from other methods that already hold the mutex
    // No need to lock again here
    
    LOG(INFO, LOG_TAG) << "Detecting available time sources\n";
    
    // Define the time sources to check
    std::vector<time_sync::TimeSyncSource> sources = {
        time_sync::TimeSyncSource::CHRONY,
        time_sync::TimeSyncSource::PTP,
        time_sync::TimeSyncSource::NTP,
        time_sync::TimeSyncSource::MONOTONIC,
        time_sync::TimeSyncSource::SYSTEM
    };
    
    // Clear existing sources
    time_sources_.clear();
    
    // Check each time source
    for (auto source : sources) {
        time_sync::TimeSyncInfo info;
        info.source = source;
        
        // Check if source is available using our new implementation
        info.available = time_sync::isTimeSourceAvailable(source);
        
        // Set quality and error estimates based on source type
        switch (source) {
            case time_sync::TimeSyncSource::CHRONY:
                info.quality = 1.0;
                info.estimated_error_ms = 0.1;
                break;
            case time_sync::TimeSyncSource::PTP:
                info.quality = 0.9;
                info.estimated_error_ms = 0.5;
                break;
            case time_sync::TimeSyncSource::NTP:
                info.quality = 0.7;
                info.estimated_error_ms = 5.0;
                break;
            case time_sync::TimeSyncSource::MONOTONIC:
                info.quality = 0.5;
                info.estimated_error_ms = 50.0;
                info.available = true;  // Monotonic is always available
                break;
            case time_sync::TimeSyncSource::SYSTEM:
                info.quality = 0.4;
                info.estimated_error_ms = 100.0;
                info.available = true;  // System time is always available
                break;
            default:
                info.quality = 0.0;
                info.estimated_error_ms = 1000.0;
                break;
        }
        
        // Try to get a time value from the source if it's available
        if (info.available) {
            try {
                // Try to get time from this source to verify it works
                time_sync::getTime(source);
                LOG(INFO, LOG_TAG) << time_sync::timeSourceToString(source) 
                                  << " is available and working, quality: " << info.quality << "\n";
            } catch (const std::exception& e) {
                // Source is not actually working
                LOG(WARNING, LOG_TAG) << time_sync::timeSourceToString(source) 
                                    << " reported as available but failed: " << e.what() << "\n";
                info.available = false;
                info.quality = 0.0;
            }
        }
        
        // Store in map
        time_sources_[source] = info;
    }
    
    // Update timestamps
    auto now = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    
    for (auto& source : time_sources_) {
        source.second.last_update = now;
    }
}

chronos::time_point_clk TimeProvider::getCurrentTime(time_sync::TimeSyncSource specific)
{
    // This method doesn't need locking as it only reads the current_source_ value
    // which is updated atomically by other methods
    
    // If a specific source is requested, use the base class implementation
    if (specific != time_sync::TimeSyncSource::NONE) {
        return TimeManager::getCurrentTime(specific);
    }
    
    // Get the current time from the system clock
    auto now = chronos::clk::now();
    
    // For client, we need to adjust by the diff to server
    // This ensures all clients are synchronized with the server's clock
    now += getDiffToServer<chronos::usec>();
    
    // Log the current time source and time
    auto duration = now.time_since_epoch();
    auto microseconds = std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
    LOG(TRACE, LOG_TAG) << "getCurrentTime: " << microseconds / 1000000 << "." << microseconds % 1000000
                       << ", source: " << time_sync::timeSourceToString(current_source_.load()) << "\n";
    
    return now;
}
