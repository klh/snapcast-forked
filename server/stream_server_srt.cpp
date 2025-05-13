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

#include "stream_server_srt.hpp"
#include "stream_session_srt.hpp"
#include "common/aixlog.hpp"
#include "common/message/message.hpp"
#include "common/snap_exception.hpp"
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>

using namespace std;

StreamServerSrt::StreamServerSrt(boost::asio::io_context& io_context, size_t port, const srt::SrtOptions& options,
                           StreamMessageReceiver* messageReceiver, PcmStream* /*stream*/)
    : StreamServer(io_context, ServerSettings(), nullptr), port_(port), options_(options), socket_(SRT_INVALID_SOCK), 
      running_(false), io_context_(io_context), messageReceiver_(messageReceiver)
{
}

StreamServerSrt::~StreamServerSrt()
{
    stop();
    
    // Clear all sessions
    sessions_.clear();
}

void StreamServerSrt::start()
{
    initSocket();
    acceptConnection();
}

void StreamServerSrt::stop()
{
    running_ = false;
    
    if (poll_thread_.joinable())
    {
        poll_thread_.join();
    }
    
    // Stop all sessions
    for (auto& session : sessions_)
    {
        if (session)
        {
            try
            {
                session->stop();
            }
            catch (const std::exception& e)
            {
                LOG(ERROR, LOG_TAG) << "Error stopping session: " << e.what() << "\n";
            }
        }
    }
    
    // Close all connections
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto socket : connections_)
        {
            if (socket != SRT_INVALID_SOCK)
            {
                srt_close(socket);
            }
        }
        connections_.clear();
    }
    
    // Close listening socket
    if (socket_ != SRT_INVALID_SOCK)
    {
        srt_close(socket_);
        socket_ = SRT_INVALID_SOCK;
    }
}

void StreamServerSrt::initSocket()
{
    // Initialize SRT if not already initialized
    if (srt_startup() == -1)
    {
        throw SnapException("SRT startup failed: " + string(srt_getlasterror_str()));
    }
    
    LOG(INFO, LOG_TAG) << "SRT library initialized successfully";
    
    // Create socket
    socket_ = srt_create_socket();
    if (socket_ == SRT_INVALID_SOCK)
    {
        throw SnapException("Failed to create SRT socket: " + string(srt_getlasterror_str()));
    }
    
    LOG(INFO, LOG_TAG) << "SRT socket created successfully";
    
    // Apply options
    applySrtOptions(socket_);
    LOG(INFO, LOG_TAG) << "SRT options applied to socket";
    
    // Set reuse address
    int reuse = 1;
    if (srt_setsockopt(socket_, 0, SRTO_REUSEADDR, &reuse, sizeof(reuse)) == SRT_ERROR)
    {
        LOG(WARNING, LOG_TAG) << "Failed to set SRTO_REUSEADDR: " << srt_getlasterror_str();
    }
    
    // Prepare address
    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port_));
    addr.sin_addr.s_addr = INADDR_ANY;
    
    // Bind
    LOG(INFO, LOG_TAG) << "Binding SRT socket to port " << port_ << "\n";
    if (srt_bind(socket_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SRT_ERROR)
    {
        throw SnapException("Failed to bind SRT socket: " + string(srt_getlasterror_str()));
    }
    
    LOG(INFO, LOG_TAG) << "SRT socket bound to port " << port_ << " successfully";
    
    // Listen
    LOG(INFO, LOG_TAG) << "Starting to listen on SRT socket";
    if (srt_listen(socket_, 10) == SRT_ERROR)
    {
        throw SnapException("Failed to listen on SRT socket: " + string(srt_getlasterror_str()));
    }
    
    LOG(INFO, LOG_TAG) << "SRT server listening on port " << port_ << " (SRT protocol)\n";
}

void StreamServerSrt::acceptConnection()
{
    if (!running_)
    {
        running_ = true;
        poll_thread_ = std::thread(&StreamServerSrt::pollThread, this);
    }
}

void StreamServerSrt::applySrtOptions(SRTSOCKET socket)
{
    // Set latency
    int latency = options_.latency;
    srt_setsockopt(socket, 0, SRTO_LATENCY, &latency, sizeof(latency));
    
    // Set message API mode (for datagram-based transmission)
    int messageapi = 1;
    srt_setsockopt(socket, 0, SRTO_MESSAGEAPI, &messageapi, sizeof(messageapi));
    
    // Set maximum bandwidth if specified
    if (options_.max_bandwidth > 0)
    {
        int maxbw = options_.max_bandwidth;
        srt_setsockopt(socket, 0, SRTO_MAXBW, &maxbw, sizeof(maxbw));
    }
    
    // Set encryption if enabled
    if (options_.encryption && !options_.passphrase.empty())
    {
        srt_setsockopt(socket, 0, SRTO_PASSPHRASE, options_.passphrase.c_str(), 
                       static_cast<int>(options_.passphrase.size()));
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
    // This option might not be available in all SRT versions, so handle errors gracefully
    #ifdef SRTO_RETRANSMITALGO
    int recovery_policy = 2; // SRTO_RETRANSMITALGO
    if (srt_setsockopt(socket, 0, SRTO_RETRANSMITALGO, &recovery_policy, sizeof(recovery_policy)) == SRT_ERROR) {
        LOG(WARNING, LOG_TAG) << "SRTO_RETRANSMITALGO not supported in this SRT version, skipping";
    }
    #endif
}

void StreamServerSrt::pollThread()
{
    LOG(INFO, LOG_TAG) << "Starting SRT polling thread";
    
    // Create epoll container
    int epoll_id = srt_epoll_create();
    if (epoll_id < 0)
    {
        LOG(ERROR, LOG_TAG) << "Failed to create epoll: " << srt_getlasterror_str() << "\n";
        return;
    }
    
    LOG(INFO, LOG_TAG) << "SRT epoll container created successfully";
    
    // Add listening socket to epoll
    int events = SRT_EPOLL_IN | SRT_EPOLL_ERR;
    if (srt_epoll_add_usock(epoll_id, socket_, &events) < 0)
    {
        LOG(ERROR, LOG_TAG) << "Failed to add socket to epoll: " << srt_getlasterror_str() << "\n";
        srt_epoll_release(epoll_id);
        return;
    }
    
    LOG(INFO, LOG_TAG) << "SRT listening socket added to epoll, ready to accept connections";
    LOG(INFO, LOG_TAG) << "SRT server ready on port " << port_;
    
    // Polling loop
    while (running_)
    {
        // Wait for events with timeout
        const int MAX_SOCKETS = 100;
        const int TIMEOUT_MS = 100;
        SRTSOCKET ready_sockets[MAX_SOCKETS];
        int ready_count = MAX_SOCKETS;
        
        int result = srt_epoll_wait(epoll_id, ready_sockets, &ready_count, nullptr, nullptr, TIMEOUT_MS, nullptr, nullptr, nullptr, nullptr);
        
        if (!running_)
        {
            LOG(INFO, LOG_TAG) << "SRT polling thread stopping";
            break;
        }
        
        if (result < 0)
        {
            int error = srt_getlasterror(nullptr);
            if (error == SRT_ETIMEOUT)
            {
                // Timeout is normal, continue polling
                continue;
            }
            
            LOG(ERROR, LOG_TAG) << "SRT epoll wait failed: " << srt_getlasterror_str() << "\n";
            break;
        }
        
        if (ready_count <= 0)
        {
            continue;
        }
        
        LOG(INFO, LOG_TAG) << "SRT epoll detected " << ready_count << " ready socket(s)";

        
        // Process ready sockets
        for (int i = 0; i < ready_count; ++i)
        {
            SRTSOCKET ready_socket = ready_sockets[i];
            
            if (ready_socket == socket_)
            {
                LOG(INFO, LOG_TAG) << "SRT server detected incoming connection";
                
                // Accept new connection
                sockaddr_in client_addr;
                int addr_len = sizeof(client_addr);
                SRTSOCKET client_socket = srt_accept(socket_, reinterpret_cast<sockaddr*>(&client_addr), &addr_len);
                
                if (client_socket == SRT_INVALID_SOCK)
                {
                    int error = srt_getlasterror(nullptr);
                    LOG(ERROR, LOG_TAG) << "Failed to accept SRT connection: " << srt_getlasterror_str() << ", error code: " << error << "\n";
                    continue;
                }
                
                // Get client info
                char client_ip[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
                uint16_t client_port = ntohs(client_addr.sin_port);
                
                LOG(INFO, LOG_TAG) << "Accepted new SRT connection from " << client_ip << ":" << client_port << "\n";
                
                // Apply options to client socket
                applySrtOptions(client_socket);
                
                // Add client socket to epoll
                int client_events = SRT_EPOLL_IN | SRT_EPOLL_ERR;
                if (srt_epoll_add_usock(epoll_id, client_socket, &client_events) < 0)
                {
                    LOG(ERROR, LOG_TAG) << "Failed to add client socket to epoll: " << srt_getlasterror_str() << "\n";
                    srt_close(client_socket);
                    continue;
                }
                
                // Add to connections list
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    connections_.push_back(client_socket);
                }
                
                // Handle connection in a separate thread
                boost::asio::post(io_context_, [this, client_socket]() {
                    handleConnection(client_socket);
                });
            }
            else
            {
                // Handle client socket event
                // This is handled in handleConnection
            }
        }
    }
    
    // Clean up
    srt_epoll_release(epoll_id);
}

void StreamServerSrt::handleConnection(SRTSOCKET socket)
{
    try
    {
        // Create a session for this connection
        LOG(INFO, LOG_TAG) << "Creating SRT stream session for socket " << socket << "\n";
        
        // Create a new StreamSessionSrt and add it to the sessions
        auto session = std::make_shared<StreamSessionSrt>(messageReceiver_, socket);
        
        // Start the session
        session->start();
        
        // Add session to the sessions list
        // Note: We can't directly add the session to the message receiver
        // because StreamMessageReceiver doesn't have an addSession method.
        // Instead, we'll let the session handle its own lifecycle.
        LOG(INFO, LOG_TAG) << "SRT stream session created\n";
        
        // Store the session to keep it alive
        sessions_.push_back(std::move(session));
        
        // Clean up expired sessions
        sessions_.erase(
            std::remove_if(sessions_.begin(), sessions_.end(),
                          [](const std::shared_ptr<StreamSession>& s) { return s.use_count() <= 1; }),
            sessions_.end());
        
        LOG(INFO, LOG_TAG) << "SRT stream session started successfully\n";
        
        // Remove from connections tracking list since the session now owns the socket
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = std::find(connections_.begin(), connections_.end(), socket);
        if (it != connections_.end())
        {
            connections_.erase(it);
        }
    }
    catch (const std::exception& e)
    {
        LOG(ERROR, LOG_TAG) << "Error creating SRT session: " << e.what() << "\n";
        
        // Close the socket only in case of error
        srt_close(socket);
        
        // Remove from connections list
        std::lock_guard<std::mutex> lock(mutex_);
        connections_.erase(std::remove(connections_.begin(), connections_.end(), socket), connections_.end());
    }
    
    // DO NOT close the socket here - the session now owns it and will close it when done
}
