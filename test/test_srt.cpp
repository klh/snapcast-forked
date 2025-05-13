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

#include <boost/asio.hpp>
#include <boost/test/unit_test.hpp>
#include <chrono>
#include <iostream>
#include <memory>
#include <thread>

#include "common/srt/srt_connection.hpp"
#include "common/srt/srt_options.hpp"

using namespace std;
using namespace std::chrono_literals;

BOOST_AUTO_TEST_SUITE(srt_test)

BOOST_AUTO_TEST_CASE(test_srt_init)
{
    // Test SRT library initialization
    SRTSOCKET socket = srt_create_socket();
    BOOST_CHECK(socket != SRT_INVALID_SOCK);
    srt_close(socket);
}

BOOST_AUTO_TEST_CASE(test_srt_options)
{
    // Test SRT options
    srt::SrtOptions options;
    BOOST_CHECK_EQUAL(options.latency, 120);
    BOOST_CHECK_EQUAL(options.encryption, false);
    BOOST_CHECK_EQUAL(options.passphrase, "");
    BOOST_CHECK_EQUAL(options.max_bandwidth, 0);
    BOOST_CHECK_EQUAL(options.connection_timeout, 3000);
    
    // Test custom options
    srt::SrtOptions custom_options;
    custom_options.latency = 200;
    custom_options.encryption = true;
    custom_options.passphrase = "test_passphrase";
    custom_options.max_bandwidth = 1000000;
    custom_options.connection_timeout = 5000;
    
    BOOST_CHECK_EQUAL(custom_options.latency, 200);
    BOOST_CHECK_EQUAL(custom_options.encryption, true);
    BOOST_CHECK_EQUAL(custom_options.passphrase, "test_passphrase");
    BOOST_CHECK_EQUAL(custom_options.max_bandwidth, 1000000);
    BOOST_CHECK_EQUAL(custom_options.connection_timeout, 5000);
}

BOOST_AUTO_TEST_CASE(test_srt_connection)
{
    // Test SRT connection creation
    boost::asio::io_context io_context;
    srt::SrtOptions options;
    
    // Create SRT connection
    auto connection = std::make_unique<srt::SrtConnection>(io_context, options);
    BOOST_CHECK(connection != nullptr);
    
    // Check initial state
    BOOST_CHECK_EQUAL(connection->isConnected(), false);
    BOOST_CHECK_EQUAL(connection->getRemoteEndpoint(), "");
}

BOOST_AUTO_TEST_SUITE_END()
