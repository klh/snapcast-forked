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
#include "message.hpp"

namespace msg
{

/// Time sync message, send from client to server and back
class Time : public BaseMessage
{
public:
    /// Protocol version for time synchronization
    enum class ProtocolVersion : uint8_t
    {
        V1 = 1,  ///< Original protocol (32-bit time)
        V2 = 2   ///< Enhanced protocol (64-bit time, multiple sources)
    };

    /// c'tor
    Time() : BaseMessage(message_type::kTime), 
             protocol_version(ProtocolVersion::V2),
             time_source(TimeSyncSource::MONOTONIC), 
             time_quality(0.5), estimated_error_ms(50.0)
    {
    }

    /// d'tor
    ~Time() override = default;

    void read(std::istream& stream) override
    {
        // Read the protocol version first
        uint8_t version;
        readVal(stream, version);
        protocol_version = static_cast<ProtocolVersion>(version);
        
        // Read the latency values
        readVal(stream, latency.sec);
        readVal(stream, latency.usec);
        
        // For V1 protocol, we're done
        if (protocol_version == ProtocolVersion::V1) {
            // Set default values for V2 fields
            time_source = TimeSyncSource::MONOTONIC;
            time_quality = 0.5f;
            estimated_error_ms = 50.0f;
            return;
        }
        
        // For V2 protocol, read additional fields
        uint8_t source;
        readVal(stream, source);
        time_source = static_cast<TimeSyncSource>(source);
        readVal(stream, time_quality);
        readVal(stream, estimated_error_ms);
    }

    uint32_t getSize() const override
    {
        // Base size includes protocol version and latency
        uint32_t size = sizeof(tv) + sizeof(uint8_t);
        
        // For V2 protocol, add size of additional fields
        if (protocol_version == ProtocolVersion::V2) {
            size += sizeof(uint8_t) + sizeof(float) + sizeof(float);
        }
        
        return size;
    }
    
    /// The latency after round trip "client => server => client"
    tv latency;
    
    /// Protocol version being used
    ProtocolVersion protocol_version;
    
    /// Time source being used (CHRONY, PTP, NTP, MONOTONIC)
    TimeSyncSource time_source;
    
    /// Quality metric (0.0-1.0, higher is better)
    float time_quality;
    
    /// Estimated error in milliseconds
    float estimated_error_ms;

protected:
    void doserialize(std::ostream& stream) const override
    {
        // Write the protocol version first
        uint8_t version = static_cast<uint8_t>(protocol_version);
        writeVal(stream, version);
        
        // Write the latency values
        writeVal(stream, latency.sec);
        writeVal(stream, latency.usec);
        
        // For V2 protocol, write additional fields
        if (protocol_version == ProtocolVersion::V2) {
            uint8_t source = static_cast<uint8_t>(time_source);
            writeVal(stream, source);
            writeVal(stream, time_quality);
            writeVal(stream, estimated_error_ms);
        }
    }
};

} // namespace msg
