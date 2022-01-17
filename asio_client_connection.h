#ifndef ASIO_HTTP2_CLIENT_CONNECTION_H
#define ASIO_HTTP2_CLIENT_CONNECTION_H

/*
// This was written based on the original code which has the
// following license:
//
*/
//
// async_client.cpp
// ~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2008 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#include <iostream>
#include <istream>
#include <ostream>
#include <string>
#include <array>
#include <cstring>

#include <boost/asio.hpp>
#include <boost/bind.hpp>
#include <boost/noncopyable.hpp>

#include "h2load_http2_session.h"
#include "h2load_utils.h"
#include "h2load_Client.h"
#include "h2load_Config.h"
#include "h2load_stats.h"
#include "config_schema.h"


template <typename socket_type>
class asio_client_connection
    : public std::enable_shared_from_this<asio_client_connection<socket_type>>, h2load::Client_Interface,
      private boost::noncopyable
{
public:
    asio_client_connection(boost::asio::io_service& io_service, h2load::Config* conf, h2load::Stats& st)
        : dns_resolver(io_service),
          client_socket(io_service),
          connect_timeout(boost::posix_time::seconds(2)),
          config(conf),
          stats(st),
          input_buffer(8 * 1024, 0),
          output_buffers(2, std::vector<uint8_t>(64 * 1024, 0))
    {
    }
    virtual ~asio_client_connection() {}

    virtual std::map<int32_t, h2load::Request_Data>& requests_waiting_for_response() = 0;
    virtual size_t send_out_data(const uint8_t* data, size_t length)
    {
        if (output_buffers[output_buffer_index].capacity() - output_data_length < length)
        {
            output_buffers[output_buffer_index].reserve(output_data_length + length);
        }
        std::memcpy(output_buffers[output_buffer_index].data() + output_data_length, data, length);
        output_data_length += length;
        return output_data_length;
    }
    virtual void signal_write()
    {
        if (!is_write_in_progress)
        {
            do_write();
        }
    }
    virtual bool any_pending_data_to_write() = 0;
    virtual void try_new_connection() = 0;
    virtual std::unique_ptr<Client_Interface> create_dest_client(const std::string& dst_sch,
                                                                 const std::string& dest_authority) = 0;
    virtual void setup_connect_with_async_fqdn_lookup() = 0;
    virtual void connect_to_host(const std::string& dest_schema, const std::string& dest_authority)
    {
        std::string port;
        auto vec = tokenize_string(authority, ":");
        if (vec.size() == 1)
        {
            if (schema == "https")
            {
                port = "443";
            }
            else
            {
                port = "80";
            }
        }
        else
        {
            port = vec[1];
        }

        boost::asio::ip::tcp::resolver::query query(vec[0], port);
        dns_resolver.async_resolve(query,
                                   boost::bind(&asio_client_connection::on_resolve_result_event, this,
                                               boost::asio::placeholders::error,
                                               boost::asio::placeholders::iterator));
        connect_timer.expires_from_now(connect_timeout);
        connect_timer.async_wait(
            std::bind(&asio_client_connection::handle_connect_timeout, this->shared_from_this()));
        schema = dest_schema;
        authority = dest_authority;

    }

    virtual int connect()
    {
        connect_to_host(schema, authority);
        return 0;
    }

private:

    bool timer_common_check(boost::asio::deadline_timer& timer, void (asio_client_connection::*handler)())
    {
        if (is_client_stopped)
        {
            return false;
        }

        if (timer.expires_at() >
            boost::asio::deadline_timer::traits_type::now())
        {
            timer.async_wait(
                std::bind(handler, this->shared_from_this()));
            return false;
        }
        return true;
    }
    virtual void start_rps_timer()
    {
        rps_timer.expires_from_now(boost::posix_time::seconds(std::max(0.01, 1. / rps)));
        rps_timer.async_wait(
            std::bind(&asio_client_connection::handle_rps_timer_timeout, this->shared_from_this()));
    }

    void handle_rps_timer_timeout()
    {
        if (!timer_common_check(rps_timer, &asio_client_connection::handle_rps_timer_timeout))
        {
            return;
        }
        start_rps_timer();
        on_rps_timer();
    }


    virtual void start_request_delay_execution_timer()
    {
        delay_request_execution_timer.expires_from_now(boost::posix_time::seconds(0.01));
        delay_request_execution_timer.async_wait(
            std::bind(&asio_client_connection::handle_request_execution_timer_timeout, this->shared_from_this()));

    }

    void handle_request_execution_timer_timeout()
    {
        if (!timer_common_check(delay_request_execution_timer, &asio_client_connection::handle_request_execution_timer_timeout))
        {
            return;
        }
        resume_delayed_request_execution();
        start_request_delay_execution_timer();
    }

    void on_connected_event(const boost::system::error_code& err,
                            boost::asio::ip::tcp::resolver::iterator endpoint_iterator)
    {
        static thread_local boost::asio::ip::tcp::resolver::iterator end_of_resolve_result;
        if (!err)
        {
            connection_made();
        }
        else if (endpoint_iterator != end_of_resolve_result)
        {
            // The connection failed. Try the next endpoint in the list.
            client_socket.close();
            boost::asio::ip::tcp::endpoint endpoint = *endpoint_iterator;
            client_socket.async_connect(endpoint,
                                        boost::bind(&asio_client_connection::on_connected_event, this,
                                                    boost::asio::placeholders::error, ++endpoint_iterator));
        }
        else
        {
            std::cout << "Error: " << err.message() << "\n";
        }
    }

    socket_type& socket()
    {
        return client_socket;
    }

    void handle_connect_timeout()
    {
        if (is_client_stopped)
        {
            return;
        }

        if (connect_timer.expires_at() >
            boost::asio::deadline_timer::traits_type::now())
        {
            connect_timer.async_wait(
                std::bind(&asio_client_connection::handle_connect_timeout, this->shared_from_this()));
            return;
        }

        stop();
    }

    void do_read()
    {
        auto self = this->shared_from_this();

        client_socket.async_read_some(
            boost::asio::buffer(input_buffer),
            [this, self](const boost::system::error_code & e,
                         std::size_t bytes_transferred)
        {
            if (e)
            {
                stop();
                return;
            }

            if (session->on_read(input_buffer.data(), bytes_transferred) != 0)
            {
                stop();
                return;
            }

            do_read();
        });
    }

    void do_write()
    {
        auto self = this->shared_from_this();

        if (is_write_in_progress)
        {
            return;
        }

        session->on_write();
        if (output_data_length <= 0)
        {
            return;
        }

        auto& buffer = output_buffers[output_buffer_index];
        auto length = output_data_length;

        output_data_length = 0;
        output_buffer_index = ((++output_buffer_index) % output_buffers.size());

        boost::asio::async_write(
            client_socket, boost::asio::buffer(buffer, length),
            [this, self](const boost::system::error_code & e, std::size_t)
        {
            if (e)
            {
                stop();
                return;
            }

            is_write_in_progress = false;

            do_write();
        });
        is_write_in_progress = true;
    }

    void stop()
    {
        if (is_client_stopped)
        {
            return;
        }

        is_client_stopped = true;
        boost::system::error_code ignored_ec;
        client_socket.lowest_layer().close(ignored_ec);
        connect_timer.cancel();

    }

    void on_resolve_result_event(const boost::system::error_code& err,
                                 boost::asio::ip::tcp::resolver::iterator endpoint_iterator)
    {
        if (!err)
        {
            // Attempt a connection to the first endpoint in the list. Each endpoint
            // will be tried until we successfully establish a connection.
            boost::asio::ip::tcp::endpoint endpoint = *endpoint_iterator;
            client_socket.async_connect(endpoint,
                                        boost::bind(&asio_client_connection::on_connected_event, this,
                                                    boost::asio::placeholders::error, ++endpoint_iterator));
        }
        else
        {
            std::cerr << "Error: " << err.message() << "\n";
        }
    }

    boost::asio::ip::tcp::resolver dns_resolver;
    boost::asio::ip::tcp::socket client_socket;
    bool is_write_in_progress = false;
    bool is_client_stopped = false;
    std::shared_ptr<h2load::Http2Session> session;

    /// Buffer for incoming data.
    std::vector<uint8_t> input_buffer;
    std::vector<std::vector<uint8_t>> output_buffers;
    size_t output_data_length = 0;
    size_t output_buffer_index = 0;

    boost::asio::deadline_timer connect_timer;
    boost::posix_time::time_duration connect_timeout;
    h2load::Config* config;
    h2load::Stats& stats;
    std::string schema;
    std::string authority;
    std::string original_authority;
    boost::asio::deadline_timer delay_request_execution_timer;
    boost::asio::deadline_timer rps_timer;
};

#endif
