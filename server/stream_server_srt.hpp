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

#include "common/srt/srt_options.hpp"
#include "stream_server.hpp"
#include <boost/asio.hpp>
#include <memory>
#include <vector>
#include <srt/srt.h>

/**
 * Server implementation for SRT (Secure Reliable Transport) protocol
 */
class StreamServerSrt : public StreamServer
{
public:
    /**
     * Constructor
     * @param io_context The boost::asio::io_context to use
     * @param port The port to listen on
     * @param options SRT connection options
     */
    StreamServerSrt(boost::asio::io_context& io_context, size_t port, const srt::SrtOptions& options);
    
    /**
     * Destructor
     */
    virtual ~StreamServerSrt();
    
    /**
     * Start the server
     */
    void start();
    
    /**
     * Stop the server
     */
    void stop();

private:
    /**
     * Initialize SRT socket
     */
    void initSocket();
    
    /**
     * Start accepting connections
     */
    void acceptConnection();
    
    /**
     * Handle a new connection
     * @param socket The SRT socket for the new connection
     */
    void handleConnection(SRTSOCKET socket);
    
    /**
     * Apply SRT options to socket
     * @param socket SRT socket to configure
     */
    void applySrtOptions(SRTSOCKET socket);
    
    /**
     * Polling thread function
     */
    void pollThread();
    
    /// The port to listen on
    size_t port_;
    
    /// SRT options
    srt::SrtOptions options_;
    
    /// SRT socket
    SRTSOCKET socket_;
    
    /// Thread for polling SRT events
    std::thread poll_thread_;
    
    /// Flag to control polling thread
    std::atomic<bool> running_;
    
    /// Mutex for thread safety
    std::mutex mutex_;
    
    /// Reference to the io_context
    boost::asio::io_context& io_context_;
    
    /// List of active connections
    std::vector<SRTSOCKET> connections_;
    
    static constexpr auto LOG_TAG = "StreamServerSrt";
};
