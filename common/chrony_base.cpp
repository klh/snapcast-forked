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

#include "chrony_base.hpp"
#include <array>
#include <sstream>
#include <system_error>
#include <unistd.h>

namespace chrony {

std::string ChronyBase::execCommand(const std::string& cmd) const {
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

bool ChronyBase::isChronyInstalled() const {
    std::string result = execCommand("which chronyd 2>/dev/null");
    return !result.empty();
}

void ChronyBase::verifyChronoInstalled() {
    if (!isChronyInstalled()) {
        throw std::runtime_error("Chrony is not installed. It is required for time synchronization.");
    }
    LOG(INFO, LOG_TAG) << "Chrony is installed\n";
}

bool ChronyBase::isSynchronized() const {
    // Get tracking information from chronyc
    std::string tracking = execCommand("chronyc -c tracking 2>/dev/null");
    if (tracking.empty()) {
        LOG(WARNING, LOG_TAG) << "Failed to get chrony tracking information\n";
        return false;
    }
    
    // Parse the tracking information
    // The 5th field in CSV output contains the stratum (lower is better)
    // Stratum 0 = reference clock, 1 = primary server, 2+ = secondary servers
    std::istringstream iss(tracking);
    std::string field;
    int field_count = 0;
    int stratum = 16; // Default to highest (worst) stratum
    
    // Parse CSV format
    while (std::getline(iss, field, ',')) {
        field_count++;
        if (field_count == 5) {
            try {
                stratum = std::stoi(field);
            } catch (...) {
                // Failed to parse stratum
            }
            break;
        }
    }
    
    // Consider synchronized if stratum is 0-10 (0-2 is good, 3-10 is acceptable)
    bool synchronized = (stratum >= 0 && stratum <= 10);
    
    if (synchronized) {
        LOG(DEBUG, LOG_TAG) << "Chrony is synchronized with stratum " << stratum << "\n";
    } else {
        LOG(WARNING, LOG_TAG) << "Chrony is not properly synchronized (stratum " << stratum << ")\n";
    }
    
    return synchronized;
}

void ChronyBase::checkSynchronization() {
    if (!isSynchronized()) {
        throw std::runtime_error("Chrony is not properly synchronized. Accurate time synchronization is required.");
    }
    LOG(DEBUG, LOG_TAG) << "Chrony synchronization verified\n";
}

std::optional<time_sync::ChronyTrackingInfo> ChronyBase::getTrackingInfo() const {
    std::string tracking = execCommand("chronyc tracking 2>/dev/null");
    if (tracking.empty()) {
        return std::nullopt;
    }
    
    return time_sync::ChronyTrackingInfo::parse(tracking);
}

std::string ChronyBase::getStatus() const {
    std::string status = execCommand("chronyc -c tracking 2>/dev/null");
    if (status.empty()) {
        return "Chrony is not running or not responding";
    }
    
    // Get more detailed status
    std::string sources = execCommand("chronyc -c sources 2>/dev/null");
    std::string sourcestats = execCommand("chronyc -c sourcestats 2>/dev/null");
    
    std::ostringstream oss;
    oss << "Chrony tracking:\n" << status << "\n";
    
    if (!sources.empty()) {
        oss << "Chrony sources:\n" << sources << "\n";
    }
    
    if (!sourcestats.empty()) {
        oss << "Chrony source stats:\n" << sourcestats << "\n";
    }
    
    return oss.str();
}

} // namespace chrony
