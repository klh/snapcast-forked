/***
    This file is part of snapcast
    Copyright (C) 2014-2025 Johannes Pohl

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

#include <string>

namespace srt {

/**
 * Configuration options for SRT transport
 */
struct SrtOptions
{
    /// Default latency in milliseconds
    int latency = 120;
    
    /// Enable encryption
    bool encryption = false;
    
    /// Encryption passphrase (only used if encryption is true)
    std::string passphrase = "";
    
    /// Maximum bandwidth in bytes per second (0 = unlimited)
    int max_bandwidth = 0;
    
    /// Connection timeout in milliseconds
    int connection_timeout = 3000;
};

} // namespace srt
