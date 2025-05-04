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

// Thread-safe implementation of time synchronization methods

void TimeProvider::setDiffToServer(double ms)
{
    using namespace std::chrono_literals;
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Use steady_clock consistently for time synchronization to avoid timezone issues
    auto now = chronos::clk::now();
    static auto lastTimeSync = now;
    auto diff = chronos::abs(now - lastTimeSync);

    /// clear diffBuffer if last update is older than a minute
    if (!diffBuffer_.empty() && (diff > 60s))
    {
        LOG(INFO, LOG_TAG) << "Last time sync older than a minute. Clearing time buffer\n";
        diffToServer_ = static_cast<chronos::usec::rep>(ms * 1000);
        diffBuffer_.clear();
    }
    lastTimeSync = now;

    // Ensure we handle 64-bit values correctly
    int64_t diff_usec = static_cast<int64_t>(ms * 1000.);
    diffBuffer_.add(diff_usec);
    diffToServer_ = diffBuffer_.median();
    
    // Update the last sync time for the current source
    if (time_sources_.find(current_source_) != time_sources_.end()) {
        time_sources_[current_source_].last_update = 
            std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
    }
    
    LOG(DEBUG, LOG_TAG) << "setDiffToServer: " << ms << ", diff: " << diffToServer_ / 1000000 << " s, " 
                       << (diffToServer_ / 1000) % 1000 << "." << diffToServer_ % 1000 
                       << " ms, source: " << static_cast<int>(current_source_) << "\n";
}

void TimeProvider::setDiff(const tv& c2s, const tv& s2c)
{
    // Use 64-bit arithmetic to prevent overflow on large time differences
    int64_t c2s_usec = c2s.sec * INT64_C(1000000) + c2s.usec;
    int64_t s2c_usec = s2c.sec * INT64_C(1000000) + s2c.usec;
    
    // Calculate latency and time difference using 64-bit integers
    double latency = static_cast<double>(c2s_usec + s2c_usec) / 2.0;
    double diff = static_cast<double>(c2s_usec - s2c_usec);
    
    // Convert to milliseconds and set the difference
    setDiffToServer(diff / 1000.0 - latency / 1000.0);
}

// Thread-safe implementation of time source detection

void TimeProvider::detectAvailableTimeSources()
{
    std::lock_guard<std::mutex> lock(mutex_);
    
    LOG(INFO, LOG_TAG) << "Detecting available time sources\n";
    
    // Initialize time sources map with default values
    TimeSyncInfo chrony_info;
    chrony_info.source = TimeSyncSource::CHRONY;
    chrony_info.quality = 1.0;
    chrony_info.estimated_error_ms = 0.1;
    chrony_info.available = false;
    
    TimeSyncInfo ptp_info;
    ptp_info.source = TimeSyncSource::PTP;
    ptp_info.quality = 0.9;
    ptp_info.estimated_error_ms = 0.5;
    ptp_info.available = false;
    
    TimeSyncInfo ntp_info;
    ntp_info.source = TimeSyncSource::NTP;
    ntp_info.quality = 0.7;
    ntp_info.estimated_error_ms = 5.0;
    ntp_info.available = false;
    
    TimeSyncInfo monotonic_info;
    monotonic_info.source = TimeSyncSource::MONOTONIC;
    monotonic_info.quality = 0.5;
    monotonic_info.estimated_error_ms = 50.0;
    monotonic_info.available = true;  // Monotonic is always available
    
    // Store in map
    time_sources_[TimeSyncSource::CHRONY] = chrony_info;
    time_sources_[TimeSyncSource::PTP] = ptp_info;
    time_sources_[TimeSyncSource::NTP] = ntp_info;
    time_sources_[TimeSyncSource::MONOTONIC] = monotonic_info;
    
    // Check for Chrony availability
    FILE* pipe = popen("chronyc tracking 2>/dev/null", "r");
    if (pipe) {
        char buffer[1024];
        if (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            time_sources_[TimeSyncSource::CHRONY].available = true;
            LOG(INFO, LOG_TAG) << "Chrony is available\n";
        }
        pclose(pipe);
    }
    
    // Check for PTP availability
    pipe = popen("ptp4l --version 2>/dev/null || which ptp4l 2>/dev/null", "r");
    if (pipe) {
        char buffer[1024];
        if (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            time_sources_[TimeSyncSource::PTP].available = true;
            LOG(INFO, LOG_TAG) << "PTP is available\n";
        }
        pclose(pipe);
    }
    
    // Check for NTP availability
    pipe = popen("ntpq -p 2>/dev/null || which ntpd 2>/dev/null", "r");
    if (pipe) {
        char buffer[1024];
        if (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            time_sources_[TimeSyncSource::NTP].available = true;
            LOG(INFO, LOG_TAG) << "NTP is available\n";
        }
        pclose(pipe);
    }
    
    // Update timestamps
    auto now = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    
    for (auto& source : time_sources_) {
        source.second.last_update = now;
    }
}

// Constructor with proper initialization
TimeProvider::TimeProvider() : 
    diffBuffer_(100),
    diffToServer_(0),
    preferred_source_(TimeSyncSource::NONE),
    current_source_(TimeSyncSource::MONOTONIC)
{
    // Initialize time sources
    detectAvailableTimeSources();
    
    // Select the best available time source
    selectBestTimeSource();
}

chronos::time_point_clk TimeProvider::getCurrentTime()
{
    // This method doesn't need locking as it only reads the current_source_ value
    // which is updated atomically by other methods
    
    // Always use the steady clock for consistent time handling
    return chronos::clk::now();
}
