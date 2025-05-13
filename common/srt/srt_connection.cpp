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
      running_(false),
      connection_monitor_(nullptr)
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

    LOG(INFO, LOG_TAG) << "SRT socket created successfully: " << socket_;

    // Apply options before connecting
    applySrtOptions(socket_);

    // Set connection timeout
    int timeout = options_.connection_timeout;
    if (srt_setsockopt(socket_, 0, SRTO_CONNTIMEO, &timeout, sizeof(timeout)) == SRT_ERROR) {
        LOG(ERROR, LOG_TAG) << "Failed to set connection timeout: " << srt_getlasterror_str();
    }
    
    // Always bind to an ephemeral port for client connections
    // This ensures it works properly when both client and server are on the same machine
    sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = INADDR_ANY;
    bind_addr.sin_port = htons(0); // Use port 0 to let OS assign an ephemeral port
    
    if (srt_bind(socket_, reinterpret_cast<sockaddr*>(&bind_addr), sizeof(bind_addr)) == SRT_ERROR) {
        LOG(ERROR, LOG_TAG) << "Failed to bind client socket to ephemeral port: " << srt_getlasterror_str();
        srt_close(socket_);
        socket_ = SRT_INVALID_SOCK;
        boost::asio::post(io_context_, [handler]() {
            handler(boost::asio::error::address_in_use);
        });
        return;
    }
    
    LOG(INFO, LOG_TAG) << "SRT socket bound to ephemeral port successfully";

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
    
    // Set non-blocking mode for better scalability with multiple clients
    int blocking = 0; // 0 = non-blocking, 1 = blocking
    if (srt_setsockopt(socket_, 0, SRTO_RCVSYN, &blocking, sizeof(blocking)) == SRT_ERROR) {
        LOG(ERROR, LOG_TAG) << "Failed to set non-blocking receive mode: " << srt_getlasterror_str();
    }
    if (srt_setsockopt(socket_, 0, SRTO_SNDSYN, &blocking, sizeof(blocking)) == SRT_ERROR) {
        LOG(ERROR, LOG_TAG) << "Failed to set non-blocking send mode: " << srt_getlasterror_str();
    }
    
    // Set connection timeout explicitly
    int connect_timeout_ms = options_.connection_timeout;
    if (srt_setsockopt(socket_, 0, SRTO_CONNTIMEO, &connect_timeout_ms, sizeof(connect_timeout_ms)) == SRT_ERROR) {
        LOG(ERROR, LOG_TAG) << "Failed to set connection timeout: " << srt_getlasterror_str();
    }
    LOG(INFO, LOG_TAG) << "Using non-blocking mode with " << connect_timeout_ms << "ms connection timeout";
    
    // Try to connect
    LOG(INFO, LOG_TAG) << "Attempting SRT connection to " << host << ":" << port;
    int connect_result = srt_connect(socket_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    
    if (connect_result == SRT_ERROR) {
        int error_code = srt_getlasterror(nullptr);
        SRT_SOCKSTATUS error_status = srt_getsockstate(socket_);
        
        // In non-blocking mode, EAGAIN is expected and not an error
        // SRT uses EAGAIN for non-blocking operations that would block
        if (error_code == SRT_EAGAIN) {
            LOG(INFO, LOG_TAG) << "SRT connection in progress to " << host << ":" << port;
            
            // Start polling for connection status
            startPolling();
            
            // Use a shared_ptr to track the connection state across threads
            auto connection_state = std::make_shared<bool>(true);
            
            // Store in class member for cleanup
            connection_monitor_ = connection_state;
            
            // Create a connection monitor to check for connection completion
            // Use io_context for thread management instead of raw thread
            boost::asio::post(io_context_, [this, host, port, handler, connection_state]() {
                const int MAX_WAIT_MS = options_.connection_timeout;
                const int POLL_INTERVAL_MS = 100;
                int elapsed_ms = 0;
                
                // Use a timer for clean shutdown
                auto timer = std::make_shared<boost::asio::steady_timer>(io_context_);
                
                // Define the polling function
                std::function<void(const boost::system::error_code&)> check_connection;
                
                check_connection = [this, host, port, handler, connection_state, timer, &check_connection, &elapsed_ms, MAX_WAIT_MS, POLL_INTERVAL_MS]
                    (const boost::system::error_code&) { // Unused parameter
                    // Check if we've been asked to stop
                    if (!*connection_state) {
                        LOG(INFO, LOG_TAG) << "SRT connection monitor stopped for " << host << ":" << port;
                        return;
                    }
                    
                    // Check socket state
                    SRT_SOCKSTATUS status = srt_getsockstate(socket_);
                    
                    if (status == SRTS_CONNECTED) {
                        // Connection successful
                        LOG(INFO, LOG_TAG) << "SRT connection established to " << host << ":" << port;
                        remote_endpoint_ = host + ":" + std::to_string(port);
                        connected_ = true;
                        *connection_state = false; // Stop monitoring
                        
                        boost::asio::post(io_context_, [handler]() {
                            handler(boost::system::error_code());
                        });
                        return;
                    } else if (status == SRTS_BROKEN || status == SRTS_NONEXIST || status == SRTS_CLOSED) {
                        // Connection failed
                        LOG(ERROR, LOG_TAG) << "SRT connection failed to " << host << ":" << port 
                                          << ", socket state: " << getSockStateStr(status);
                        
                        srt_close(socket_);
                        socket_ = SRT_INVALID_SOCK;
                        connected_ = false;
                        *connection_state = false; // Stop monitoring
                        
                        boost::asio::post(io_context_, [handler]() {
                            handler(boost::asio::error::connection_refused);
                        });
                        return;
                    }
                    
                    // Check for timeout
                    elapsed_ms += POLL_INTERVAL_MS;
                    if (elapsed_ms >= MAX_WAIT_MS) {
                        // Timeout occurred
                        LOG(ERROR, LOG_TAG) << "SRT connection timeout to " << host << ":" << port;
                        
                        srt_close(socket_);
                        socket_ = SRT_INVALID_SOCK;
                        connected_ = false;
                        *connection_state = false; // Stop monitoring
                        
                        boost::asio::post(io_context_, [handler]() {
                            handler(boost::asio::error::timed_out);
                        });
                        return;
                    }
                    
                    // Schedule next check
                    timer->expires_after(std::chrono::milliseconds(POLL_INTERVAL_MS));
                    timer->async_wait(check_connection);
                };
                
                // Start the polling
                timer->expires_after(std::chrono::milliseconds(POLL_INTERVAL_MS));
                timer->async_wait(check_connection);
            });
            
            return;
        } else {
            // Real error occurred
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
    }
    
    // For blocking mode, we'd reach here only if connection was successful immediately
    LOG(INFO, LOG_TAG) << "SRT connection established immediately to " << host << ":" << port;
    remote_endpoint_ = host + ":" + std::to_string(port);
    connected_ = true;
    
    // Start polling for data
    startPolling();
    
    // Log successful connection
    LOG(INFO, LOG_TAG) << "Successfully connected to " << host << ":" << port << " with SRT";
    LOG(INFO, LOG_TAG) << "SRT socket state: " << getSockStateStr(srt_getsockstate(socket_));

    boost::asio::post(io_context_, [handler]() {
        handler(boost::system::error_code());
    });
}

void SrtConnection::disconnect()
{
    // Stop the connection monitor if it exists
    if (connection_monitor_) {
        *connection_monitor_ = false;
        connection_monitor_.reset();
        LOG(INFO, LOG_TAG) << "Connection monitor stopped";
    }
    
    // Stop the polling thread
    stopPolling();

    // Close the socket
    if (socket_ != SRT_INVALID_SOCK) {
        LOG(INFO, LOG_TAG) << "Closing SRT socket: " << socket_;
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
    // Log the socket we're configuring
    LOG(INFO, LOG_TAG) << "Applying SRT options to socket: " << socket;
    
    // === Basic Configuration ===
    
    // Set latency - this is the most important parameter for audio streaming
    int latency = options_.latency;
    if (srt_setsockopt(socket, 0, SRTO_LATENCY, &latency, sizeof(latency)) == SRT_ERROR) {
        LOG(ERROR, LOG_TAG) << "Failed to set SRTO_LATENCY: " << srt_getlasterror_str();
    } else {
        LOG(INFO, LOG_TAG) << "Set SRT latency to " << latency << " ms";
    }

    // Set message API mode (for datagram-based transmission)
    int messageapi = 1;
    if (srt_setsockopt(socket, 0, SRTO_MESSAGEAPI, &messageapi, sizeof(messageapi)) == SRT_ERROR) {
        LOG(ERROR, LOG_TAG) << "Failed to set SRTO_MESSAGEAPI: " << srt_getlasterror_str();
    }
    
    // === Connection Configuration ===
    
    // Set connection reuse for faster reconnection
    int reuse = 1;
    if (srt_setsockopt(socket, 0, SRTO_REUSEADDR, &reuse, sizeof(reuse)) == SRT_ERROR) {
        LOG(ERROR, LOG_TAG) << "Failed to set SRTO_REUSEADDR: " << srt_getlasterror_str();
    }
    
    // === Performance Tuning ===
    
    // Use live congestion control algorithm optimized for real-time audio
    if (srt_setsockopt(socket, 0, SRTO_CONGESTION, "live", 4) == SRT_ERROR) {
        LOG(ERROR, LOG_TAG) << "Failed to set SRTO_CONGESTION: " << srt_getlasterror_str();
    }
    
    // Enable timestamp-based packet dropping for late packets
    int too_late_ms = 1000; // Drop packets that are 1000ms too late
    if (srt_setsockopt(socket, 0, SRTO_TLPKTDROP, &too_late_ms, sizeof(too_late_ms)) == SRT_ERROR) {
        LOG(ERROR, LOG_TAG) << "Failed to set SRTO_TLPKTDROP: " << srt_getlasterror_str();
    }
    
    // Set buffer sizes appropriate for audio streaming
    int rcvbuf = 8192 * 16; // 128KB receive buffer
    if (srt_setsockopt(socket, 0, SRTO_RCVBUF, &rcvbuf, sizeof(rcvbuf)) == SRT_ERROR) {
        LOG(ERROR, LOG_TAG) << "Failed to set SRTO_RCVBUF: " << srt_getlasterror_str();
    }
    
    int sndbuf = 8192 * 16; // 128KB send buffer
    if (srt_setsockopt(socket, 0, SRTO_SNDBUF, &sndbuf, sizeof(sndbuf)) == SRT_ERROR) {
        LOG(ERROR, LOG_TAG) << "Failed to set SRTO_SNDBUF: " << srt_getlasterror_str();
    }
    
    // Set stream ID to indicate audio content
    std::string stream_id = "m=audio,snapcast";
    if (srt_setsockopt(socket, 0, SRTO_STREAMID, stream_id.c_str(), static_cast<int>(stream_id.size())) == SRT_ERROR) {
        LOG(ERROR, LOG_TAG) << "Failed to set SRTO_STREAMID: " << srt_getlasterror_str();
    }
    
    // === Bandwidth Control ===
    
    // Set maximum bandwidth if specified
    if (options_.max_bandwidth > 0) {
        int maxbw = options_.max_bandwidth;
        if (srt_setsockopt(socket, 0, SRTO_MAXBW, &maxbw, sizeof(maxbw)) == SRT_ERROR) {
            LOG(ERROR, LOG_TAG) << "Failed to set SRTO_MAXBW: " << srt_getlasterror_str();
        } else {
            LOG(INFO, LOG_TAG) << "Set SRT max bandwidth to " << maxbw << " bytes/sec";
        }
    }
    
    // === Security ===
    
    // Set encryption if enabled
    if (options_.encryption && !options_.passphrase.empty()) {
        if (srt_setsockopt(socket, 0, SRTO_PASSPHRASE, options_.passphrase.c_str(), 
                         static_cast<int>(options_.passphrase.size())) == SRT_ERROR) {
            LOG(ERROR, LOG_TAG) << "Failed to set SRTO_PASSPHRASE: " << srt_getlasterror_str();
        } else {
            LOG(INFO, LOG_TAG) << "SRT encryption enabled with passphrase";
        }
    }
    
    LOG(INFO, LOG_TAG) << "SRT socket options applied successfully";
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
