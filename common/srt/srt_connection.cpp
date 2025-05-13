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

#include "srt_connection.hpp"
#include <arpa/inet.h>

using namespace std;

namespace srt {

// Helper function to convert SRT socket state to string
static std::string getSockStateStr(SRT_SOCKSTATUS state) {
    switch (state) {
        case SRTS_INIT: return "INIT";
        case SRTS_OPENED: return "OPENED";
        case SRTS_LISTENING: return "LISTENING";
        case SRTS_CONNECTING: return "CONNECTING";
        case SRTS_CONNECTED: return "CONNECTED";
        case SRTS_BROKEN: return "BROKEN";
        case SRTS_CLOSING: return "CLOSING";
        case SRTS_CLOSED: return "CLOSED";
        case SRTS_NONEXIST: return "NONEXIST";
        default: return "UNKNOWN(" + std::to_string(state) + ")";
    }
}

// Static initialization of SRT library
static struct SrtInit {
    SrtInit() {
        if (srt_startup() < 0) {
            LOG(ERROR, "SrtConnection") << "SRT startup failed: " << srt_getlasterror_str();
        }
    }
    ~SrtInit() {
        srt_cleanup();
    }
} srt_init;

SrtConnection::SrtConnection(boost::asio::io_context& io_context, const SrtOptions& options)
    : io_context_(io_context), 
      options_(options), 
      socket_(SRT_INVALID_SOCK), 
      connected_(false), 
      running_(false)
{
}

SrtConnection::~SrtConnection()
{
    disconnect();
}

void SrtConnection::connect(const std::string& host, uint16_t port, const ResultHandler& handler)
{
    if (connected_) {
        boost::asio::post(io_context_, [handler]() {
            handler(boost::system::error_code());
        });
        return;
    }

    // Create socket
    socket_ = srt_create_socket();
    if (socket_ == SRT_INVALID_SOCK) {
        LOG(ERROR, LOG_TAG) << "Failed to create SRT socket: " << srt_getlasterror_str();
        boost::asio::post(io_context_, [handler]() {
            handler(boost::asio::error::connection_aborted);
        });
        return;
    }

    // Apply options
    applySrtOptions(socket_);

    // Set connection timeout
    int timeout = options_.connection_timeout;
    srt_setsockopt(socket_, 0, SRTO_CONNTIMEO, &timeout, sizeof(timeout));

    // Prepare address
    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        LOG(ERROR, LOG_TAG) << "Invalid address: " << host;
        srt_close(socket_);
        socket_ = SRT_INVALID_SOCK;
        boost::asio::post(io_context_, [handler]() {
            handler(boost::asio::error::host_not_found);
        });
        return;
    }

    // Connect
    LOG(INFO, LOG_TAG) << "Connecting to " << host << ":" << port << " with SRT";
    
    // Get SRT socket state before connect
    SRT_SOCKSTATUS pre_status = srt_getsockstate(socket_);
    LOG(INFO, LOG_TAG) << "SRT socket state before connect: " << getSockStateStr(pre_status);
    
    // Set connection timeout
    int timeout_ms = 2000; // 2 seconds
    if (srt_setsockopt(socket_, 0, SRTO_CONNTIMEO, &timeout_ms, sizeof(timeout_ms)) == SRT_ERROR) {
        LOG(ERROR, LOG_TAG) << "Failed to set connection timeout: " << srt_getlasterror_str();
    }
    
    // Set non-blocking mode for socket
    int blocking = 0; // 0 = non-blocking, 1 = blocking
    if (srt_setsockopt(socket_, 0, SRTO_RCVSYN, &blocking, sizeof(blocking)) == SRT_ERROR) {
        LOG(ERROR, LOG_TAG) << "Failed to set non-blocking receive mode: " << srt_getlasterror_str();
    }
    if (srt_setsockopt(socket_, 0, SRTO_SNDSYN, &blocking, sizeof(blocking)) == SRT_ERROR) {
        LOG(ERROR, LOG_TAG) << "Failed to set non-blocking send mode: " << srt_getlasterror_str();
    }
    
    // Try to connect with timeout handling
    LOG(INFO, LOG_TAG) << "Attempting SRT connection to " << host << ":" << port;
    int connect_result = srt_connect(socket_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    
    if (connect_result == SRT_ERROR) {
        SRT_SOCKSTATUS error_status = srt_getsockstate(socket_);
        int error_code = srt_getlasterror(nullptr);
        LOG(ERROR, LOG_TAG) << "Failed to connect to " << host << ":" << port << " with SRT";
        LOG(ERROR, LOG_TAG) << "SRT error code: " << error_code << ", message: " << srt_getlasterror_str();
        LOG(ERROR, LOG_TAG) << "SRT socket state: " << getSockStateStr(error_status);
        
        srt_close(socket_);
        socket_ = SRT_INVALID_SOCK;
        boost::asio::post(io_context_, [handler]() {
            handler(boost::asio::error::connection_refused);
        });
        return;
    }
    
    // Check if connection was successful
    SRT_SOCKSTATUS status = srt_getsockstate(socket_);
    if (status != SRTS_CONNECTED) {
        LOG(ERROR, LOG_TAG) << "SRT connection failed - socket state: " << getSockStateStr(status);
        srt_close(socket_);
        socket_ = SRT_INVALID_SOCK;
        boost::asio::post(io_context_, [handler]() {
            handler(boost::asio::error::connection_refused);
        });
        return;
    }

    // Store remote endpoint for later use
    remote_endpoint_ = host + ":" + to_string(port);
    connected_ = true;
    
    // Log successful connection
    SRT_SOCKSTATUS connected_status = srt_getsockstate(socket_);
    LOG(INFO, LOG_TAG) << "Successfully connected to " << host << ":" << port << " with SRT";
    LOG(INFO, LOG_TAG) << "SRT socket state: " << getSockStateStr(connected_status);

    // Start polling thread
    startPolling();

    boost::asio::post(io_context_, [handler]() {
        handler(boost::system::error_code());
    });
}

void SrtConnection::disconnect()
{
    stopPolling();

    if (socket_ != SRT_INVALID_SOCK) {
        srt_close(socket_);
        socket_ = SRT_INVALID_SOCK;
    }

    connected_ = false;
}

bool SrtConnection::isConnected() const
{
    return connected_ && (socket_ != SRT_INVALID_SOCK);
}

void SrtConnection::send(const std::string& data, const ResultHandler& handler)
{
    if (!isConnected()) {
        boost::asio::post(io_context_, [handler]() {
            handler(boost::asio::error::not_connected);
        });
        return;
    }

    // Send data
    int result = srt_send(socket_, data.c_str(), static_cast<int>(data.size()));
    if (result == SRT_ERROR) {
        LOG(ERROR, LOG_TAG) << "Failed to send data: " << srt_getlasterror_str();
        boost::asio::post(io_context_, [handler]() {
            handler(boost::asio::error::connection_aborted);
        });
        return;
    }

    boost::asio::post(io_context_, [handler]() {
        handler(boost::system::error_code());
    });
}

void SrtConnection::receive(const DataHandler& handler)
{
    if (!isConnected()) {
        boost::asio::post(io_context_, [handler]() {
            handler(boost::asio::error::not_connected, "");
        });
        return;
    }

    // Add handler to the queue
    {
        std::lock_guard<std::mutex> lock(mutex_);
        data_handlers_.push_back(handler);
    }
}

std::string SrtConnection::getRemoteEndpoint() const
{
    return remote_endpoint_;
}

SRTSOCKET SrtConnection::getSocket() const
{
    return socket_;
}

void SrtConnection::applySrtOptions(SRTSOCKET socket)
{
    // Set latency
    int latency = options_.latency;
    srt_setsockopt(socket, 0, SRTO_LATENCY, &latency, sizeof(latency));

    // Set message API mode (for datagram-based transmission)
    int messageapi = 1;
    srt_setsockopt(socket, 0, SRTO_MESSAGEAPI, &messageapi, sizeof(messageapi));

    // Set maximum bandwidth if specified
    if (options_.max_bandwidth > 0) {
        int maxbw = options_.max_bandwidth;
        srt_setsockopt(socket, 0, SRTO_MAXBW, &maxbw, sizeof(maxbw));
    }
    
    // === Audio Streaming Optimizations ===
    
    // Use live congestion control algorithm optimized for real-time audio
    srt_setsockopt(socket, 0, SRTO_CONGESTION, "live", 4);
    
    // Enable timestamp-based packet dropping for late packets
    int too_late_ms = 100; // Drop packets that are 100ms too late
    srt_setsockopt(socket, 0, SRTO_TLPKTDROP, &too_late_ms, sizeof(too_late_ms));
    
    // Set receive buffer size appropriate for audio
    int rcvbuf = 8192 * 8; // 64KB receive buffer
    srt_setsockopt(socket, 0, SRTO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    
    // Set stream ID to indicate audio content
    std::string stream_id = "m=audio,snapcast";
    srt_setsockopt(socket, 0, SRTO_STREAMID, stream_id.c_str(), static_cast<int>(stream_id.size()));
    
    // Enable periodic NAK reports to improve loss recovery
    int nakrpt = 1;
    srt_setsockopt(socket, 0, SRTO_NAKREPORT, &nakrpt, sizeof(nakrpt));
    
    // Set recovery policy appropriate for audio
    int recovery_policy = 2; // SRTO_RETRANSMITALGO
    srt_setsockopt(socket, 0, SRTO_RETRANSMITALGO, &recovery_policy, sizeof(recovery_policy));
    
    // Set encryption if enabled
    if (options_.encryption && !options_.passphrase.empty()) {
        srt_setsockopt(socket, 0, SRTO_PASSPHRASE, options_.passphrase.c_str(), 
                      static_cast<int>(options_.passphrase.size()));
    }
}

void SrtConnection::startPolling()
{
    if (running_) {
        return;
    }

    running_ = true;
    poll_thread_ = std::thread(&SrtConnection::pollThread, this);
}

void SrtConnection::stopPolling()
{
    running_ = false;
    if (poll_thread_.joinable()) {
        poll_thread_.join();
    }
}

void SrtConnection::pollThread()
{
    const int BUFFER_SIZE = 64 * 1024; // 64KB buffer
    vector<char> buffer(BUFFER_SIZE);

    // Create epoll container
    int epoll_id = srt_epoll_create();
    if (epoll_id < 0) {
        LOG(ERROR, LOG_TAG) << "Failed to create epoll: " << srt_getlasterror_str();
        return;
    }

    // Add socket to epoll
    int events = SRT_EPOLL_IN | SRT_EPOLL_ERR;
    if (srt_epoll_add_usock(epoll_id, socket_, &events) < 0) {
        LOG(ERROR, LOG_TAG) << "Failed to add socket to epoll: " << srt_getlasterror_str();
        srt_epoll_release(epoll_id);
        return;
    }

    // Polling loop
    while (running_ && isConnected()) {
        // Wait for events with timeout
        const int TIMEOUT_MS = 100;
        SRTSOCKET ready_sockets[1];
        int ready_count = 1;
        int result = srt_epoll_wait(epoll_id, ready_sockets, &ready_count, nullptr, nullptr, TIMEOUT_MS, nullptr, nullptr, nullptr, nullptr);
        
        if (!running_) {
            break;
        }

        if (result < 0) {
            int error = srt_getlasterror(nullptr);
            if (error == SRT_ETIMEOUT) {
                // Timeout is normal, continue polling
                continue;
            }
            
            LOG(ERROR, LOG_TAG) << "Epoll wait failed: " << srt_getlasterror_str();
            break;
        }

        if (ready_count <= 0) {
            continue;
        }

        // Receive data
        int recv_size = srt_recv(socket_, buffer.data(), BUFFER_SIZE);
        if (recv_size == SRT_ERROR) {
            int error = srt_getlasterror(nullptr);
            if (error == SRT_ECONNLOST) {
                LOG(INFO, LOG_TAG) << "Connection lost";
                connected_ = false;
                break;
            }
            
            LOG(ERROR, LOG_TAG) << "Failed to receive data: " << srt_getlasterror_str();
            continue;
        }

        if (recv_size > 0) {
            // Process received data
            string data(buffer.data(), recv_size);
            processReceivedData(data);
        }
    }

    // Clean up
    srt_epoll_remove_usock(epoll_id, socket_);
    srt_epoll_release(epoll_id);
}

void SrtConnection::processReceivedData(const std::string& data)
{
    // Deliver data to handlers
    std::vector<DataHandler> handlers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        handlers.swap(data_handlers_);
    }

    for (const auto& handler : handlers) {
        boost::asio::post(io_context_, [handler, data]() {
            handler(boost::system::error_code(), data);
        });
    }
}

} // namespace srt
