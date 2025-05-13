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

#include "stream_session_srt.hpp"
#include "common/aixlog.hpp"
#include "common/message/message.hpp"
#include "common/snap_exception.hpp"
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>

using namespace std;

StreamSessionSrt::StreamSessionSrt(StreamMessageReceiver* messageReceiver, SRTSOCKET socket)
    : StreamSession(boost::asio::any_io_executor(), messageReceiver), 
      socket_(socket), 
      buffer_(1000000), // Using max_size from message.hpp
      running_(false)
{
    // Get client info for logging
    sockaddr_in client_addr;
    int addr_len = sizeof(client_addr);
    if (srt_getpeername(socket_, reinterpret_cast<sockaddr*>(&client_addr), &addr_len) == SRT_ERROR)
    {
        LOG(ERROR, LOG_TAG) << "Failed to get peer name: " << srt_getlasterror_str() << "\n";
    }
    else
    {
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
        uint16_t client_port = ntohs(client_addr.sin_port);
        LOG(INFO, LOG_TAG) << "New SRT session from " << client_ip << ":" << client_port << "\n";
    }
}

StreamSessionSrt::~StreamSessionSrt()
{
    stop();
}

void StreamSessionSrt::start()
{
    if (running_)
        return;

    // Get client info for logging
    sockaddr_in client_addr;
    int addr_len = sizeof(client_addr);
    if (srt_getpeername(socket_, reinterpret_cast<sockaddr*>(&client_addr), &addr_len) == SRT_ERROR)
    {
        LOG(ERROR, LOG_TAG) << "Failed to get peer name: " << srt_getlasterror_str() << "\n";
    }
    else
    {
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
        uint16_t client_port = ntohs(client_addr.sin_port);
        LOG(INFO, LOG_TAG) << "Starting SRT stream session from " << client_ip << ":" << client_port << " (SRT protocol)\n";
    }

    running_ = true;
    read_thread_ = std::thread(&StreamSessionSrt::read, this);
}

void StreamSessionSrt::stop()
{
    running_ = false;
    
    if (read_thread_.joinable())
        read_thread_.join();
    
    if (socket_ != SRT_INVALID_SOCK)
    {
        srt_close(socket_);
        socket_ = SRT_INVALID_SOCK;
    }
}

void StreamSessionSrt::send(shared_const_buffer const_buf)
{
    if (socket_ == SRT_INVALID_SOCK)
        return;

    std::lock_guard<std::mutex> lock(mutex_);
    
    // Send data using SRT
    const auto& message = const_buf.message();
    int result = srt_send(socket_, message.data.data(), static_cast<int>(message.data.size()));
    if (result == SRT_ERROR)
    {
        int error = srt_getlasterror(nullptr);
        if (error == SRT_ECONNLOST)
        {
            LOG(INFO, LOG_TAG) << "Connection lost\n";
            messageReceiver_->onDisconnect(this);
            return;
        }
        
        LOG(ERROR, LOG_TAG) << "Failed to send data: " << srt_getlasterror_str() << "\n";
    }
}

void StreamSessionSrt::read()
{
    // Create epoll container for efficient waiting
    int epoll_id = srt_epoll_create();
    if (epoll_id < 0)
    {
        LOG(ERROR, LOG_TAG) << "Failed to create epoll: " << srt_getlasterror_str() << "\n";
        return;
    }
    
    // Add socket to epoll
    int events = SRT_EPOLL_IN | SRT_EPOLL_ERR;
    if (srt_epoll_add_usock(epoll_id, socket_, &events) < 0)
    {
        LOG(ERROR, LOG_TAG) << "Failed to add socket to epoll: " << srt_getlasterror_str() << "\n";
        srt_epoll_release(epoll_id);
        return;
    }
    
    // Read loop
    while (running_ && socket_ != SRT_INVALID_SOCK)
    {
        // Wait for events with timeout
        const int TIMEOUT_MS = 100;
        SRTSOCKET ready_sockets[1];
        int ready_count = 1;
        
        int result = srt_epoll_wait(epoll_id, ready_sockets, &ready_count, nullptr, nullptr, TIMEOUT_MS, nullptr, nullptr, nullptr, nullptr);
        
        if (!running_)
            break;
        
        if (result < 0)
        {
            int error = srt_getlasterror(nullptr);
            if (error == SRT_ETIMEOUT)
            {
                // Timeout is normal, continue polling
                continue;
            }
            
            LOG(ERROR, LOG_TAG) << "Epoll wait failed: " << srt_getlasterror_str() << "\n";
            break;
        }
        
        if (ready_count <= 0)
            continue;
        
        // Receive data
        int recv_size = srt_recv(socket_, buffer_.data(), static_cast<int>(buffer_.size()));
        if (recv_size == SRT_ERROR)
        {
            int error = srt_getlasterror(nullptr);
            if (error == SRT_ECONNLOST)
            {
                LOG(INFO, LOG_TAG) << "Connection lost\n";
                break;
            }
            
            LOG(ERROR, LOG_TAG) << "Failed to receive data: " << srt_getlasterror_str() << "\n";
            continue;
        }
        
        if (recv_size > 0)
        {
            // Process received data
            processReceived(buffer_.data(), static_cast<size_t>(recv_size));
        }
    }
    
    // Clean up
    srt_epoll_remove_usock(epoll_id, socket_);
    srt_epoll_release(epoll_id);
    
    // Notify about disconnection
    messageReceiver_->onDisconnect(this);
}

void StreamSessionSrt::processReceived(const char* data, size_t size)
{
    try
    {
        // Create a copy of the data since deserialize expects non-const char*
        std::vector<char> dataCopy(data, data + size);
        baseMessage_.deserialize(dataCopy.data());
        if (baseMessage_.type != message_type::kTime)
            LOG(DEBUG, LOG_TAG) << "Received message: " << baseMessage_.type << ", size: " << baseMessage_.size << ", id: " << baseMessage_.id << ", refers: " << baseMessage_.refersTo << "\n";
        if (messageReceiver_ != nullptr)
            messageReceiver_->onMessageReceived(this, baseMessage_, dataCopy.data());
    }
    catch (const std::exception& e)
    {
        LOG(ERROR, LOG_TAG) << "Error processing message: " << e.what() << "\n";
    }
}
