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

#include "time_sync.hpp"
#include "aixlog.hpp"
#include "time_defs.hpp"
#include "common/chrony_tracker.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>

static constexpr auto LOG_TAG = "TimeSync";

// Helper function to safely execute a command and capture its output
static std::string execCommand(const std::string& cmd) {
    std::string result;
    std::array<char, 128> buffer;
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);

    if (!pipe) {
        LOG(WARNING, LOG_TAG) << "Failed to execute command: " << cmd << "\n";
        return "";
    }

    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
        result += buffer.data();
    }
    return result;
}

namespace time_sync {

SyncMode stringToSyncMode(const std::string& mode_str)
{
    if (mode_str == "auto_select")
        return SyncMode::auto_select;
    else if (mode_str == "fixed")
        return SyncMode::fixed;
    else if (mode_str == "client_guided")
        return SyncMode::client_guided;
    else if (mode_str == "server_guided")
        return SyncMode::server_guided;
    else if (mode_str == "disabled")
        return SyncMode::disabled;
    
    LOG(WARNING, LOG_TAG) << "Unknown sync mode: " << mode_str << ", using auto_select\n";
    return SyncMode::auto_select;
}

std::string syncModeToString(SyncMode mode)
{
    switch (mode)
    {
        case SyncMode::auto_select:
            return "auto_select";
        case SyncMode::fixed:
            return "fixed";
        case SyncMode::client_guided:
            return "client_guided";
        case SyncMode::server_guided:
            return "server_guided";
        case SyncMode::disabled:
            return "disabled";
        default:
            return "unknown";
    }
}

bool isTimeSourceAvailable(TimeSyncSource source)
{
    switch (source)
    {
        case TimeSyncSource::CHRONY:
            // Use ChronyTracker to check if chrony is available
            if (snapcast::ChronyTracker::getInstance().isAvailable()) {
                LOG(DEBUG, LOG_TAG) << "Chrony available\n";
                return true;
            }
            return false;
            
        case TimeSyncSource::PTP:
            // Check if PTP is available
            {
                // Try checking for PTP devices
                std::string result = execCommand("ls /dev/ptp* 2>/dev/null");
                if (!result.empty() && result.find("/dev/ptp") != std::string::npos)
                {
                    LOG(DEBUG, LOG_TAG) << "PTP devices available\n";
                    return true;
                }
                
                // Check if PTP daemon is running
                result = execCommand("ps -ef | grep ptp4[l] 2>/dev/null");
                if (!result.empty())
                {
                    LOG(DEBUG, LOG_TAG) << "PTP daemon detected running\n";
                    return true;
                }
                
                // Check if linuxptp is installed
                result = execCommand("which ptp4l 2>/dev/null");
                if (!result.empty())
                {
                    LOG(DEBUG, LOG_TAG) << "PTP tools installed\n";
                    return true;
                }
            }
            return false;
            
        case TimeSyncSource::NTP:
            // Check if NTP is available
            {
                std::string result = execCommand("ntpq -p 2>/dev/null");
                if (!result.empty() && result.find("remote") != std::string::npos)
                {
                    LOG(DEBUG, LOG_TAG) << "NTP available via ntpq\n";
                    return true;
                }
                
                // Check if ntpd is running
                result = execCommand("ps -ef | grep ntp[d] 2>/dev/null");
                if (!result.empty())
                {
                    LOG(DEBUG, LOG_TAG) << "NTP daemon detected running\n";
                    return true;
                }
                
                // Check if systemd-timesyncd is running (used on some systems instead of ntpd)
                result = execCommand("systemctl status systemd-timesyncd 2>/dev/null | grep active");
                if (!result.empty() && result.find("active") != std::string::npos)
                {
                    LOG(DEBUG, LOG_TAG) << "systemd-timesyncd active\n";
                    return true;
                }
            }
            return false;
            
        case TimeSyncSource::MONOTONIC:
        case TimeSyncSource::SYSTEM:
            // Monotonic and system clocks are always available
            return true;
            
        default:
            return false;
    }
}

TimeSyncSource selectBestTimeSource(
    const std::map<TimeSyncSource, TimeSyncInfo>& sources,
    TimeSyncSource preferred,
    double min_quality)
{
    // If preferred source is specified and available with sufficient quality, use it
    if (preferred != TimeSyncSource::NONE)
    {
        auto it = sources.find(preferred);
        if (it != sources.end() && it->second.available && it->second.quality >= min_quality)
        {
            LOG(INFO, LOG_TAG) << "Using preferred time source: " 
                              << timeSourceToString(preferred) << "\n";
            return preferred;
        }
    }
    
    // Try sources in order of precision
    std::vector<TimeSyncSource> ordered_sources = {
        TimeSyncSource::CHRONY,
        TimeSyncSource::PTP,
        TimeSyncSource::NTP,
        TimeSyncSource::MONOTONIC,
        TimeSyncSource::SYSTEM
    };
    
    for (const auto& source : ordered_sources)
    {
        auto it = sources.find(source);
        if (it != sources.end() && it->second.available && it->second.quality >= min_quality)
        {
            LOG(INFO, LOG_TAG) << "Selected time source: " << timeSourceToString(source) << "\n";
            return source;
        }
    }
    
    // If no source meets the quality threshold, use monotonic clock as fallback
    LOG(WARNING, LOG_TAG) << "No time source meets quality threshold, using monotonic clock\n";
    return TimeSyncSource::MONOTONIC;
}

std::string timeSourceToString(TimeSyncSource source)
{
    switch (source)
    {
        case TimeSyncSource::CHRONY:
            return "Chrony";
        case TimeSyncSource::PTP:
            return "PTP";
        case TimeSyncSource::NTP:
            return "NTP";
        case TimeSyncSource::MONOTONIC:
            return "Monotonic";
        case TimeSyncSource::SYSTEM:
            return "System";
        case TimeSyncSource::NONE:
            return "None";
        default:
            return "Unknown";
    }
}

TimeSyncSource intToTimeSource(int source_int)
{
    switch (source_int)
    {
        case 0:
            return TimeSyncSource::CHRONY;
        case 1:
            return TimeSyncSource::PTP;
        case 2:
            return TimeSyncSource::NTP;
        case 3:
            return TimeSyncSource::MONOTONIC;
        case 4:
            return TimeSyncSource::SYSTEM;
        case 255:
            return TimeSyncSource::NONE;
        default:
            LOG(WARNING, LOG_TAG) << "Unknown time source: " << source_int << ", using NONE\n";
            return TimeSyncSource::NONE;
    }
}

/**
 * Query a specific time source and return its time value
 * @param source The time source to query
 * @return TimeValue containing the time information
 * @throws std::runtime_error if the time source is not available
 */
static TimeValue queryTimeSource(TimeSyncSource source) {
    std::string raw;
    chronos::time_point_clk now;
    
    // Use the appropriate chronos time function based on the source
    if (source == TimeSyncSource::MONOTONIC) {
        now = chronos::clk::now(); // Use monotonic clock
        raw = "monotonic";
    } else if (source == TimeSyncSource::SYSTEM) {
        // Convert system time to chronos time_point_clk
        struct timeval tv;
        chronos::systemtimeofday(&tv);
        auto duration = chronos::usec(tv.tv_sec * 1000000LL + tv.tv_usec);
        now = chronos::time_point_clk(duration);
        raw = "system_clock";
    } else {
        // For external time sources, use the system time but get the raw output
        struct timeval tv;
        chronos::systemtimeofday(&tv);
        auto duration = chronos::usec(tv.tv_sec * 1000000LL + tv.tv_usec);
        now = chronos::time_point_clk(duration);
        
        switch (source) {
            case TimeSyncSource::CHRONY:
                // Get raw tracking output directly from chronyc
                raw = snapcast::ChronyTracker::getInstance().getRawTracking();
                break;
            case TimeSyncSource::PTP:
                raw = execCommand("pmc -u -b 0 'GET TIME_STATUS_NP' 2>/dev/null");
                break;
            case TimeSyncSource::NTP:
                raw = execCommand("ntpq -c rl 2>/dev/null");
                break;
            default:
                throw std::runtime_error("Invalid time source.");
        }
    }

    if (raw.empty() && source != TimeSyncSource::MONOTONIC && source != TimeSyncSource::SYSTEM) {
        throw std::runtime_error("Failed to query source.");
    }

    return TimeValue{source, now, raw};
}

TimeValue getTime(TimeSyncSource specific, const std::vector<TimeSyncSource>& preferred) {
    // If a specific time source is requested, try to use it
    if (specific != TimeSyncSource::NONE) {
        if (!isTimeSourceAvailable(specific)) {
            LOG(WARNING, LOG_TAG) << "Specified time source " << timeSourceToString(specific) << " not available\n";
            throw std::runtime_error("Specified time source not available.");
        }
        try {
            return queryTimeSource(specific);
        } catch (const std::exception& e) {
            LOG(ERROR, LOG_TAG) << "Error querying time source " << timeSourceToString(specific) << ": " << e.what() << "\n";
            throw;
        }
    }

    // Try each preferred time source in order
    for (auto& src : preferred) {
        if (isTimeSourceAvailable(src)) {
            try {
                LOG(DEBUG, LOG_TAG) << "Trying time source: " << timeSourceToString(src) << "\n";
                return queryTimeSource(src);
            } catch (const std::exception& e) {
                LOG(WARNING, LOG_TAG) << "Failed to get time from " << timeSourceToString(src) << ": " << e.what() << "\n";
                continue;
            }
        }
    }

    // If all else fails, fall back to system time
    LOG(WARNING, LOG_TAG) << "No valid time sources available, falling back to system time\n";
    try {
        // Use chronos utilities to get system time
        struct timeval tv;
        chronos::systemtimeofday(&tv);
        auto duration = chronos::usec(tv.tv_sec * 1000000LL + tv.tv_usec);
        chronos::time_point_clk now(duration);
        
        return TimeValue{TimeSyncSource::SYSTEM, now, "fallback_system_time"};
    } catch (const std::exception& e) {
        LOG(ERROR, LOG_TAG) << "Critical error: Failed to get system time: " << e.what() << "\n";
        throw std::runtime_error("No valid time sources available.");
    }
}

// Define LOG_TAG for the new functions
#define LOG_TAG "TimeSync"

TimeSyncInfo getDefaultQualityMetrics(TimeSyncSource source)
{
    TimeSyncInfo info;
    info.source = source;
    
    // Set default quality metrics based on source type
    switch (source) {
        case TimeSyncSource::CHRONY:
            {
                // Use ChronyTracker to get quality metrics
                auto tracking_info = snapcast::ChronyTracker::getInstance().getTrackingInfo();
                info.quality = static_cast<float>(tracking_info.quality);
                info.estimated_error_ms = static_cast<float>(tracking_info.estimated_error_ms);
                LOG(DEBUG, LOG_TAG) << "Using chrony quality metrics: quality=" << info.quality 
                                   << ", error=" << info.estimated_error_ms << "ms\n";
            }
            break;
        case TimeSyncSource::PTP:
            info.quality = 0.9f;
            info.estimated_error_ms = 0.5f;
            break;
        case TimeSyncSource::NTP:
            info.quality = 0.7f;
            info.estimated_error_ms = 5.0f;
            break;
        case TimeSyncSource::MONOTONIC:
            info.quality = 0.5f;
            info.estimated_error_ms = 50.0f;
            break;
        case TimeSyncSource::SYSTEM:
            info.quality = 0.4f;
            info.estimated_error_ms = 100.0f;
            break;
        default:
            info.quality = 0.0f;
            info.estimated_error_ms = 1000.0f;
            break;
    }
    
    return info;
}

ProtocolVersion ensureValidProtocolVersion(uint8_t version)
{
    // If no version is specified (version = 0), assume V1
    if (version == 0 || version == static_cast<uint8_t>(ProtocolVersion::V1)) {
        return ProtocolVersion::V1;
    } else if (version >= static_cast<uint8_t>(ProtocolVersion::V2)) {
        return ProtocolVersion::V2;
    } else {
        // Default to V1 for any unknown version
        LOG(WARNING, LOG_TAG) << "Unknown protocol version: " << static_cast<int>(version) 
                             << ", defaulting to V1\n";
        return ProtocolVersion::V1;
    }
}

std::map<TimeSyncSource, TimeSyncInfo> getAllTimeSourcesInfo()
{
    std::map<TimeSyncSource, TimeSyncInfo> time_sources;
    
    // Check each time source
    for (auto source : {TimeSyncSource::CHRONY, 
                       TimeSyncSource::PTP, 
                       TimeSyncSource::NTP, 
                       TimeSyncSource::MONOTONIC, 
                       TimeSyncSource::SYSTEM}) {
        // Get default quality metrics
        TimeSyncInfo info = getDefaultQualityMetrics(source);
        
        // Check if the source is available
        info.available = isTimeSourceAvailable(source);
        
        // Store in map
        time_sources[source] = info;
    }
    
    return time_sources;
}

TimeStatus getTimeStatus(double diff_ms)
{
    TimeStatus status;
    
    // Get all available time sources
    status.available_sources = getAllTimeSourcesInfo();
    
    // Get current time from best available source
    status.current_time = getTime();
    status.active_source = status.current_time.source;
    
    // Set active source info
    auto it = status.available_sources.find(status.active_source);
    if (it != status.available_sources.end()) {
        status.active_source_info = it->second;
    } else {
        // Fallback if source not in available_sources
        status.active_source_info = getDefaultQualityMetrics(status.active_source);
    }
    
    // Set protocol version (default to V1)
    status.protocol_version = ProtocolVersion::V1;
    
    // Set time difference (for client)
    status.diff_ms = diff_ms;
    
    return status;
}

                        << timeSourceToString(status.active_source) 
                        << " (quality: " << status.active_source_info.quality 
                        << ", estimated error: " << status.active_source_info.estimated_error_ms << "ms)";
    
    // For chrony, directly show the raw chronyc output
    if (status.active_source == TimeSyncSource::CHRONY) {
        // Get raw output from chronyc
        std::string chrony_output = snapcast::ChronyTracker::getInstance().getRawTracking();
        if (!chrony_output.empty()) {
            LOG(INFO, log_tag) << "Chrony tracking information:\n" << chrony_output;
        }
    } else {
        // For other sources, log standard information
        LOG(INFO, log_tag) << "Current time source: " 
                          << timeSourceToString(status.current_time.source) 
                          << ", quality: " << status.current_time.quality 
                          << ", error: " << status.current_time.estimated_error_ms << "ms";
    }
    
    // Log protocol version as a separate log entry
    if (status.protocol_version > ProtocolVersion::V1) {
        LOG(INFO, log_tag) << "Time sync protocol version: V" 
                          << static_cast<int>(status.protocol_version);
        
        // Log time difference if available
        if (status.diff_ms != 0) {
            LOG(INFO, log_tag) << "Time difference to server: " 
                              << status.diff_ms << "ms";
        }
    } else {
        LOG(INFO, log_tag) << "Using legacy time sync protocol V1";
    }
TimeStatus initAndLogTimeSync(const std::string& log_tag, 
                              double diff_ms, 
                              ProtocolVersion protocol_version,
                              TimeSyncSource preferred_source)
{
    // Get all available time sources once to avoid race conditions
    auto time_sources = getAllTimeSourcesInfo();
    
    // Select the best time source
    TimeSyncSource selected_source;
    if (preferred_source != TimeSyncSource::NONE) {
        // Use preferred source if specified and available
        auto it = time_sources.find(preferred_source);
        if (it != time_sources.end() && it->second.available) {
            selected_source = preferred_source;
            LOG(DEBUG, log_tag) << "Using preferred time source: " 
                               << timeSourceToString(preferred_source);
        } else {
            // Fall back to best available
            selected_source = selectBestTimeSource(time_sources, preferred_source);
            LOG(DEBUG, log_tag) << "Preferred source " 
                               << timeSourceToString(preferred_source)
                               << " not available, using " 
                               << timeSourceToString(selected_source);
        }
    } else {
        // No preference, select best available
        selected_source = selectBestTimeSource(time_sources);
        LOG(DEBUG, log_tag) << "Selected best available time source: " 
                           << timeSourceToString(selected_source);
    }
    
    // Get the source info
    TimeSyncInfo sourceInfo;
    auto it = time_sources.find(selected_source);
    if (it != time_sources.end()) {
        sourceInfo = it->second;
    } else {
        sourceInfo = getDefaultQualityMetrics(selected_source);
    }
    
    // Get current time from the selected source
    TimeValue current_time = getTime(selected_source);
    
    // Create time status manually instead of calling getTimeStatus()
    // to avoid race condition with a second call to getAllTimeSourcesInfo()
    TimeStatus status;
    status.available_sources = time_sources;
    status.current_time = current_time;
    status.active_source = selected_source;
    status.active_source_info = sourceInfo;
    status.protocol_version = protocol_version;
    status.diff_ms = diff_ms;
    
    // Log the time status
    logTimeStatus(status, log_tag);
    
    return status;
}

void populateTimeMessage(msg::Time* timeMsg, 
                         TimeSyncSource source,
                         ProtocolVersion version)
{
    if (!timeMsg) return;
    
    // Set protocol version
    timeMsg->version = static_cast<uint8_t>(version);
    
    if (version >= ProtocolVersion::V2) {
        // Get time sources once to avoid race condition
        auto time_sources = getAllTimeSourcesInfo();
        
        // For V2+ protocol, include time source information
        if (source == TimeSyncSource::NONE) {
            // Auto-select best source
            source = selectBestTimeSource(time_sources);
        }
        
        // Get source info
        TimeSyncInfo sourceInfo = getDefaultQualityMetrics(source);
        auto it = time_sources.find(source);
        if (it != time_sources.end()) {
            sourceInfo = it->second;
        }
        
        // Set time source information
        timeMsg->source = static_cast<uint8_t>(source);
        timeMsg->quality = sourceInfo.quality;
        timeMsg->error_ms = sourceInfo.estimated_error_ms;
    }
}

TimeStatus processTimeResponse(const msg::Time* response, double diff_ms)
{
    if (!response) {
        throw std::invalid_argument("Null time response");
    }
    
    // Create time status
    TimeStatus status;
    
    // Set protocol version
    status.protocol_version = ensureValidProtocolVersion(response->version);
    
    // Set time difference
    status.diff_ms = diff_ms;
    
    // For V2+ protocol, extract time source information
    if (status.protocol_version >= ProtocolVersion::V2) {
        TimeSyncSource source = static_cast<TimeSyncSource>(response->source);
        status.active_source = source;
        
        // Create source info
        TimeSyncInfo sourceInfo = getDefaultQualityMetrics(source);
        sourceInfo.quality = response->quality;
        sourceInfo.estimated_error_ms = response->error_ms;
        sourceInfo.available = true;
        
        status.active_source_info = sourceInfo;
    } else {
        // For V1 protocol, use monotonic clock
        status.active_source = TimeSyncSource::MONOTONIC;
        status.active_source_info = getDefaultQualityMetrics(TimeSyncSource::MONOTONIC);
    }
    
    // Get available sources
    status.available_sources = getAllTimeSourcesInfo();
    
    // Get current time
    status.current_time = getTime(status.active_source);
    
    return status;
}

} // namespace time_sync
void logTimeStatus(const TimeStatus& status, const std::string& log_tag)
{
    // Log active time source and its quality
    LOG(NOTICE, log_tag) << "Time synchronization initialized using " 
                        << timeSourceToString(status.active_source) 
                        << " (quality: " << status.active_source_info.quality 
                        << ", estimated error: " << status.active_source_info.estimated_error_ms << "ms)";
    
    // For chrony, directly show the raw chronyc output
    if (status.active_source == TimeSyncSource::CHRONY) {
        // Get raw output from chronyc
        std::string chrony_output = snapcast::ChronyTracker::getInstance().getRawTracking();
        if (!chrony_output.empty()) {
            LOG(INFO, log_tag) << "Chrony tracking information:\n" << chrony_output;
        }
    } else {
        // For other sources, log standard information
        LOG(INFO, log_tag) << "Current time source: " 
                          << timeSourceToString(status.current_time.source) 
                          << ", quality: " << status.current_time.quality 
                          << ", error: " << status.current_time.estimated_error_ms << "ms";
    }
    
    // Log protocol version as a separate log entry
    if (status.protocol_version > ProtocolVersion::V1) {
        LOG(INFO, log_tag) << "Time sync protocol version: V" 
                          << static_cast<int>(status.protocol_version);
        
        // Log time difference if available
        if (status.diff_ms != 0) {
            LOG(INFO, log_tag) << "Time difference to server: " 
                              << status.diff_ms << "ms";
        }
    } else {
        LOG(INFO, log_tag) << "Using legacy time sync protocol V1";
    }
}
