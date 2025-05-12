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

// standard headers
#include <cstdint>
#include <map>
#include <string>
#include <vector>

// local headers
#include "time_defs.hpp"
#include "message/message.hpp"

// Forward declaration
namespace msg {
    class Time;
}

namespace time_sync {

/**
 * Protocol version for time synchronization
 * Version 1: Legacy protocol (no version field)
 * Version 2: Adds time source capabilities and selection
 */
enum class ProtocolVersion : uint8_t
{
    V1 = 1, ///< Legacy protocol
    V2 = 2  ///< Enhanced protocol with time source selection
};

/**
 * Time synchronization source types
 */
enum class TimeSyncSource : uint8_t
{
    NONE = 255,     ///< No specific time source
    CHRONY = 0,     ///< Chrony time source
    PTP = 1,        ///< Precision Time Protocol
    NTP = 2,        ///< Network Time Protocol
    MONOTONIC = 3,  ///< Monotonic clock
    SYSTEM = 4      ///< System time
};

/**
 * Time synchronization information structure
 */
struct TimeSyncInfo
{
    TimeSyncSource source = TimeSyncSource::NONE;  ///< The time source
    bool available = false;                        ///< Whether the time source is available
    float quality = 0.5f;                          ///< Quality metric (0.0-1.0, higher is better)
    float estimated_error_ms = 50.0f;              ///< Estimated error in milliseconds
    int64_t last_update = 0;                       ///< Last update timestamp (microseconds since epoch)
};

/**
 * Time synchronization mode
 */
enum class SyncMode
{
    auto_select,    ///< Automatically select best available source
    fixed,          ///< Use only the preferred source
    client_guided,  ///< Let clients decide based on their capabilities
    server_guided,  ///< Let server guide source selection
    disabled        ///< Disable time synchronization
};

/**
 * Convert a string mode to SyncMode enum
 * @param mode_str The mode string
 * @return The corresponding SyncMode enum value
 */
SyncMode stringToSyncMode(const std::string& mode_str);

/**
 * Convert a SyncMode enum to string
 * @param mode The SyncMode enum value
 * @return The corresponding string representation
 */
std::string syncModeToString(SyncMode mode);

/**
 * Check if a time source is available on the system
 * @param source The time source to check
 * @return True if the time source is available
 */
bool isTimeSourceAvailable(TimeSyncSource source);

/**
 * Select the best available time source
 * @param sources Map of available time sources and their information
 * @param preferred Preferred time source (if any)
 * @param min_quality Minimum quality threshold (0.0-1.0)
 * @return The selected time source
 */
TimeSyncSource selectBestTimeSource(
    const std::map<TimeSyncSource, TimeSyncInfo>& sources,
    TimeSyncSource preferred = TimeSyncSource::NONE,
    double min_quality = 0.3);

/**
 * Get a string representation of a time source
 * @param source The time source
 * @return String representation
 */
std::string timeSourceToString(TimeSyncSource source);

/**
 * Convert an integer to a TimeSyncSource
 * @param source_int Integer representation of the time source
 * @return The corresponding TimeSyncSource enum value
 */
TimeSyncSource intToTimeSource(int source_int);

/**
 * Structure to hold time information from a specific source
 */
struct TimeValue {
    TimeSyncSource source;              ///< The time source used
    chronos::time_point_clk timestamp; ///< The timestamp using the chronos clock
    std::string raw;                   ///< Raw output from the time source
    float quality = 0.5f;              ///< Quality metric (0.0-1.0, higher is better)
    float estimated_error_ms = 50.0f;  ///< Estimated error in milliseconds
};

/**
 * Get default quality metrics for a time source
 * @param source The time source
 * @return TimeSyncInfo with default quality metrics for the source
 */
TimeSyncInfo getDefaultQualityMetrics(TimeSyncSource source);

/**
 * Ensure a valid protocol version
 * @param version The protocol version to check
 * @return A valid protocol version (defaults to V1 if invalid)
 */
ProtocolVersion ensureValidProtocolVersion(uint8_t version);

/**
 * Create a complete time source info map with availability and quality metrics
 * @return Map of all time sources with their availability and quality metrics
 */
std::map<TimeSyncSource, TimeSyncInfo> getAllTimeSourcesInfo();

/**
 * Structure containing comprehensive time status information
 */
struct TimeStatus
{
    TimeSyncSource active_source;       // Currently active time source
    TimeSyncInfo active_source_info;    // Info about the active source
    TimeValue current_time;             // Current time value
    std::map<TimeSyncSource, TimeSyncInfo> available_sources; // All available sources
    ProtocolVersion protocol_version;   // Protocol version in use
    double diff_ms{0};                  // Time difference in ms (for client)
};

/**
 * Get comprehensive time status information
 * @param diff_ms Optional time difference to server in microseconds (for client)
 * @return TimeStatus object with all time-related information
 */
TimeStatus getTimeStatus(double diff_ms = 0);

/**
 * Generate log messages for time status
 * @param status The TimeStatus object containing time information
 * @param log_tag The tag to use for logging
 */
void logTimeStatus(const TimeStatus& status, const std::string& log_tag);

/**
 * Initialize and log time synchronization status in a standardized way
 * This function should be used by both client and server for consistent logging
 * @param log_tag The tag to use for logging
 * @param diff_ms Optional time difference to server in ms (for client only)
 * @param protocol_version Optional protocol version (defaults to V2)
 * @param preferred_source Optional preferred time source
 * @return The initialized TimeStatus object
 */
TimeStatus initAndLogTimeSync(const std::string& log_tag, 
                              double diff_ms = 0, 
                              ProtocolVersion protocol_version = ProtocolVersion::V2,
                              TimeSyncSource preferred_source = TimeSyncSource::NONE);

/**
 * Get current time from the best available or specified time source
 * @param specific Specific time source to use (NONE for auto-selection)
 * @param preferred Ordered list of preferred time sources to try
 * @return TimeValue containing the time information
 * @throws std::runtime_error if no valid time source is available
 */
TimeValue getTime(
    TimeSyncSource specific = TimeSyncSource::NONE,
    const std::vector<TimeSyncSource>& preferred = {
        TimeSyncSource::CHRONY,
        TimeSyncSource::PTP,
        TimeSyncSource::NTP,
        TimeSyncSource::MONOTONIC,
        TimeSyncSource::SYSTEM
    });

/**
 * Populate a time message with standardized information
 * @param timeMsg Pointer to the time message to populate
 * @param source Time source to use (NONE for auto-selection)
 * @param version Protocol version to use
 */
void populateTimeMessage(msg::Time* timeMsg, 
                         TimeSyncSource source = TimeSyncSource::NONE,
                         ProtocolVersion version = ProtocolVersion::V2);

/**
 * Process a time response message and create a TimeStatus object
 * @param response Pointer to the time response message
 * @param diff_ms Time difference to server in milliseconds
 * @return TimeStatus object with processed information
 * @throws std::invalid_argument if response is null
 */
TimeStatus processTimeResponse(const msg::Time* response, double diff_ms = 0);

} // namespace time_sync
