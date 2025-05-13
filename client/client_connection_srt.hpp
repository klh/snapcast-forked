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

#include "client_connection.hpp"
#include "common/srt/srt_connection.hpp"

/**
 * SRT connection
 */
class ClientConnectionSrt : public ClientConnection
{
public:
    /// c'tor
    ClientConnectionSrt(boost::asio::io_context& io_context, ClientSettings::Server server);
    
    /// d'tor
    virtual ~ClientConnectionSrt();
    
    void disconnect() override;
    std::string getMacAddress() override;
    void getNextMessage(const MessageHandler<msg::BaseMessage>& handler) override;

private:
    boost::system::error_code doConnect(boost::asio::ip::basic_endpoint<boost::asio::ip::tcp> endpoint) override;
    void write(boost::asio::streambuf& buffer, WriteHandler&& write_handler) override;
    
    /// SRT connection
    std::unique_ptr<srt::SrtConnection> srt_connection_;
    
    /// Receive buffer
    std::vector<char> buffer_;
};
