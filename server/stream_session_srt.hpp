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

#include "stream_session.hpp"
#include <srt/srt.h>
#include <boost/asio.hpp>
#include <memory>
#include <string>

/**
 * Stream session using SRT protocol
 */
class StreamSessionSrt : public StreamSession
{
public:
    /**
     * Constructor
     * @param messageReceiver Receiver for stream messages
     * @param socket SRT socket for the connection
     */
    StreamSessionSrt(StreamMessageReceiver* messageReceiver, SRTSOCKET socket);
    
    /**
     * Destructor
     */
    virtual ~StreamSessionSrt();
    
    /**
     * Start the session
     */
    void start() override;
    
    /**
     * Stop the session
     */
    void stop() override;
    
    /**
     * Get the client IP address
     * @return The client IP address
     */
    std::string getIP() override;
    
    /**
     * Send a message to the client
     * @param buffer Message to send
     */
    void send(shared_const_buffer const_buf) override;

protected:
    /**
     * Send data asynchronously
     * @param buffer Data to send
     * @param handler Callback for completion
     */
    void sendAsync(const shared_const_buffer& buffer, const WriteHandler& handler) override;
    
private:
    /**
     * Read data from the socket
     */
    void read();
    
    /**
     * Process received data
     * @param data Received data
     * @param size Size of the data
     */
    void processReceived(const char* data, size_t size);
    
    /// SRT socket
    SRTSOCKET socket_;
    
    /// Buffer for receiving data
    std::vector<char> buffer_;
    
    /// Thread for reading data
    std::thread read_thread_;
    
    /// Flag to control the read thread
    std::atomic<bool> running_;
    
    /// Mutex for thread safety
    std::mutex mutex_;
    
    /// Base message for deserialization
    msg::BaseMessage baseMessage_;
    
    /// Client IP address for logging
    std::string client_ip_;
    
    /// Client port for logging
    uint16_t client_port_;
    
    static constexpr auto LOG_TAG = "StreamSessionSRT";
};
