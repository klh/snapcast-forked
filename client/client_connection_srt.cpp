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

#include "client_connection_srt.hpp"
#include "common/aixlog.hpp"
#include "common/message/message.hpp"
#include "common/utils.hpp"
#include "common/snap_exception.hpp"
#include "common/utils.hpp"

using namespace std;

static constexpr auto LOG_TAG = "ClientConnSRT";

ClientConnectionSrt::ClientConnectionSrt(boost::asio::io_context& io_context, ClientSettings::Server server)
    : ClientConnection(io_context, server), buffer_(msg::max_size)
{
    // Create SRT options from server settings
    srt::SrtOptions options;
    options.latency = server.srt.latency;
    options.encryption = server.srt.encryption;
    options.passphrase = server.srt.passphrase;
    
    // Create SRT connection
    srt_connection_ = std::make_unique<srt::SrtConnection>(io_context, options);
}

ClientConnectionSrt::~ClientConnectionSrt()
{
    disconnect();
}

void ClientConnectionSrt::disconnect()
{
    if (srt_connection_)
    {
        srt_connection_->disconnect();
    }
}

std::string ClientConnectionSrt::getMacAddress()
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    std::string result = getMacAddress(sock);
    close(sock);
    return result;
}

boost::system::error_code ClientConnectionSrt::doConnect(boost::asio::ip::basic_endpoint<boost::asio::ip::tcp> endpoint)
{
    boost::system::error_code ec;
    
    try
    {
        // Connect to the server using SRT
        std::string host = endpoint.address().to_string();
        uint16_t port = endpoint.port();
        
        LOG(INFO, LOG_TAG) << "Connecting to " << host << ":" << port << "\n";
        
        // Use a promise to convert async to sync
        std::promise<boost::system::error_code> promise;
        std::future<boost::system::error_code> future = promise.get_future();
        
        srt_connection_->connect(host, port, [&promise](const boost::system::error_code& ec) {
            promise.set_value(ec);
        });
        
        // Wait for the connection result
        ec = future.get();
        
        if (ec)
        {
            LOG(ERROR, LOG_TAG) << "Failed to connect to " << host << ":" << port << ": " << ec.message() << "\n";
        }
        else
        {
            LOG(INFO, LOG_TAG) << "Connected to " << host << ":" << port << "\n";
        }
    }
    catch (const std::exception& e)
    {
        LOG(ERROR, LOG_TAG) << "Exception during connect: " << e.what() << "\n";
        ec = boost::asio::error::connection_aborted;
    }
    
    return ec;
}

void ClientConnectionSrt::write(boost::asio::streambuf& buffer, WriteHandler&& writeHandler)
{
    // Get data from streambuf
    std::string data(boost::asio::buffer_cast<const char*>(buffer.data()), buffer.size());
    
    // Send data using SRT
    srt_connection_->send(data, [this, writeHandler = std::move(writeHandler), size = buffer.size()](const boost::system::error_code& ec) {
        if (ec)
        {
            LOG(ERROR, LOG_TAG) << "Failed to send data: " << ec.message() << "\n";
        }
        
        writeHandler(ec, size);
    });
    
    // Clear the buffer
    buffer.consume(buffer.size());
}

void ClientConnectionSrt::getNextMessage(const MessageHandler<msg::BaseMessage>& handler)
{
    // Receive data from SRT
    srt_connection_->receive([this, handler](const boost::system::error_code& ec, const std::string& data) {
        if (ec)
        {
            LOG(ERROR, LOG_TAG) << "Error receiving data: " << ec.message() << "\n";
            handler(ec, nullptr);
            return;
        }
        
        try
        {
            // Copy data to buffer
            if (data.size() > buffer_.size())
            {
                LOG(ERROR, LOG_TAG) << "Received message too large: " << data.size() << " bytes\n";
                handler(boost::asio::error::message_size, nullptr);
                return;
            }
            
            std::copy(data.begin(), data.end(), buffer_.begin());
            
            // Deserialize the message
            msg::BaseMessage baseMessage;
            baseMessage.deserialize(buffer_.data());
            
            // Create message from type
            auto message = msg::factory::createMessage(baseMessage, buffer_.data());
            if (message == nullptr)
            {
                LOG(ERROR, LOG_TAG) << "Failed to create message of type: " << baseMessage.type << "\n";
                handler(boost::asio::error::invalid_argument, nullptr);
                return;
            }
            
            // Deserialize message
            message->deserialize(buffer_.data(), data.size());
            
            // Handle message
            messageReceived(std::move(message), handler);
        }
        catch (const std::exception& e)
        {
            LOG(ERROR, LOG_TAG) << "Exception while processing message: " << e.what() << "\n";
            handler(boost::asio::error::invalid_argument, nullptr);
        }
        
        // Continue receiving
        getNextMessage(handler);
    });
}
