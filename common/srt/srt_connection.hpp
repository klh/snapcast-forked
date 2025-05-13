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

#include "srt_options.hpp"
#include "common/aixlog.hpp"

#include <boost/asio.hpp>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <mutex>
#include <atomic>
#include <srt/srt.h>

namespace srt {

/**
 * Base class for SRT connections
 */
class SrtConnection
{
public:
    /// Result callback with boost::error_code
    using ResultHandler = std::function<void(const boost::system::error_code&)>;
    
    /// Data callback with boost::error_code and data buffer
    using DataHandler = std::function<void(const boost::system::error_code&, const std::string&)>;

    /**
     * Constructor
     * @param io_context The boost::asio::io_context to use
     * @param options SRT connection options
     */
    SrtConnection(boost::asio::io_context& io_context, const SrtOptions& options);
    
    /**
     * Destructor
     */
    virtual ~SrtConnection();
    
    /**
     * Connect to a remote host
     * @param host Host to connect to
     * @param port Port to connect to
     * @param handler Callback for connection result
     */
    void connect(const std::string& host, uint16_t port, const ResultHandler& handler);
    
    /**
     * Disconnect from remote host
     */
    void disconnect();
    
    /**
     * Check if connected
     * @return true if connected
     */
    bool isConnected() const;
    
    /**
     * Send data to remote host
     * @param data Data to send
     * @param handler Callback for send result
     */
    void send(const std::string& data, const ResultHandler& handler);
    
    /**
     * Receive data from remote host
     * @param handler Callback for received data
     */
    void receive(const DataHandler& handler);
    
    /**
     * Get remote endpoint address
     * @return Remote endpoint as string
     */
    std::string getRemoteEndpoint() const;
    
    /**
     * Get SRT socket
     * @return SRT socket
     */
    SRTSOCKET getSocket() const;

protected:
    /**
     * Apply SRT options to socket
     * @param socket SRT socket to configure
     */
    void applySrtOptions(SRTSOCKET socket);
    
    /**
     * Start polling thread for SRT events
     */
    void startPolling();
    
    /**
     * Stop polling thread
     */
    void stopPolling();
    
    /**
     * Polling thread function
     */
    void pollThread();
    
    /**
     * Process received data
     * @param data Received data
     */
    void processReceivedData(const std::string& data);

    boost::asio::io_context& io_context_;
    SrtOptions options_;
    SRTSOCKET socket_;
    std::string remote_endpoint_;
    std::atomic<bool> connected_;
    std::atomic<bool> running_;
    std::thread poll_thread_;
    std::mutex mutex_;
    std::vector<DataHandler> data_handlers_;
    
    static constexpr auto LOG_TAG = "SrtConnection";
};

} // namespace srt
