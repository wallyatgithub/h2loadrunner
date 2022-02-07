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
#include <boost/asio/ssl.hpp>

#include "h2load_http2_session.h"
#include "h2load_http1_session.h"
#include "h2load_utils.h"
#include "h2load_Client.h"
#include "h2load_Config.h"
#include "h2load_stats.h"
#include "config_schema.h"

namespace h2load
{

class asio_client_connection
    : public std::enable_shared_from_this<asio_client_connection>, public h2load::Client_Interface,
      private boost::noncopyable
{
public:
    asio_client_connection
    (
        boost::asio::io_context& io_ctx,
        uint32_t id,
        Worker_Interface* wrker,
        size_t req_todo,
        Config* conf,
        Client_Interface* parent = nullptr,
        const std::string& dest_schema = "",
        const std::string& dest_authority = ""
    )
        : Client_Interface(id, wrker, req_todo, conf, parent, dest_schema, dest_authority),
          io_context(io_ctx),
          dns_resolver(io_ctx),
          client_socket(io_ctx),
          client_probe_socket(io_ctx),
          output_buffers(2, std::vector<uint8_t>(16 * 1024, 0)),
          connect_timer(io_ctx),
          delay_request_execution_timer(io_ctx),
          rps_timer(io_ctx),
          conn_activity_timer(io_ctx),
          ping_timer(io_ctx),
          conn_inactivity_timer(io_ctx),
          stream_timeout_timer(io_ctx),
          timing_script_request_timeout_timer(io_ctx),
          connect_back_to_preferred_host_timer(io_ctx),
          delayed_reconnect_timer(io_ctx)
    {
        init_connection_targert();
    }
    virtual ~asio_client_connection()
    {
        std::cerr << "asio_client_connection deallocated: " << schema << "://" << authority << std::endl;
        disconnect();
        final_cleanup();
    }

    virtual void start_conn_active_watcher()
    {
        if (!config->conn_active_timeout > 0.)
        {
            return;
        }
        auto self = this->shared_from_this();
        conn_activity_timer.expires_from_now(boost::posix_time::millisec((size_t)(1000 * config->conn_active_timeout)));
        conn_activity_timer.async_wait
        (
            [this, self](const boost::system::error_code & ec)
        {
            handle_con_activity_timer_timeout(ec);
        });
    }

    virtual void start_conn_inactivity_watcher()
    {
        if (!config->conn_inactivity_timeout > 0.)
        {
            return;
        }

        auto self = this->shared_from_this();
        conn_inactivity_timer.expires_from_now(boost::posix_time::millisec((size_t)(1000 * config->conn_inactivity_timeout)));
        conn_inactivity_timer.async_wait
        (
            [this, self](const boost::system::error_code & ec)
        {
            handle_con_activity_timer_timeout(ec);
        });
    }

    virtual void start_stream_timeout_timer()
    {
        stream_timeout_timer.expires_from_now(boost::posix_time::millisec(10));
        auto self = this->shared_from_this();
        stream_timeout_timer.async_wait
        (
            [this, self](const boost::system::error_code & ec)
        {
            handle_stream_timeout_timer_timeout(ec);
        });
    }

    virtual void restart_timeout_timer()
    {
        start_conn_inactivity_watcher();
        start_ping_watcher();
    }

    virtual void stop_rps_timer()
    {
        rps_timer.cancel();
    }

    virtual void start_timing_script_request_timeout_timer(double duration)
    {
        timing_script_request_timeout_timer.expires_from_now(boost::posix_time::millisec((size_t)(duration * 1000)));
        auto self = this->shared_from_this();
        timing_script_request_timeout_timer.async_wait
        (
            [this, self](const boost::system::error_code & ec)
        {
            handle_timing_script_request_timeout(ec);
        });
    }

    virtual void stop_timing_script_request_timeout_timer()
    {
        timing_script_request_timeout_timer.cancel();
    }

    virtual void start_connect_timeout_timer()
    {
        connect_timer.expires_from_now(boost::posix_time::seconds(2));
        auto self = this->shared_from_this();
        connect_timer.async_wait
        (
            [this, self](const boost::system::error_code & ec)
        {
            handle_connect_timeout(ec);
        });
    }

    virtual void stop_connect_timeout_timer()
    {
        connect_timer.cancel();
    }

    virtual void start_connect_to_preferred_host_timer()
    {
        connect_back_to_preferred_host_timer.expires_from_now(boost::posix_time::millisec(1000));
        auto self = this->shared_from_this();
        connect_back_to_preferred_host_timer.async_wait
        (
            [this, self](const boost::system::error_code & ec)
        {
            connect_to_prefered_host_timer_handler(ec);
        });
    }

    virtual void start_delayed_reconnect_timer()
    {
        delayed_reconnect_timer.expires_from_now(boost::posix_time::millisec(1000));
        auto self = this->shared_from_this();
        delayed_reconnect_timer.async_wait
        (
            [this, self](const boost::system::error_code & ec)
        {
            handle_delayed_reconnect_timer_timeout(ec);
        });
    }

    virtual void stop_conn_inactivity_timer()
    {
        conn_inactivity_timer.cancel();
    }

    virtual int make_async_connection()
    {
        return connect_to_host(schema, authority);
    }

    virtual int do_connect()
    {
        return connect();
    }

    virtual void disconnect()
    {
        stop();
        cleanup_due_to_disconnect();
    }

    virtual void start_warmup_timer()
    {
        worker->start_warmup_timer();
    }

    virtual void stop_warmup_timer()
    {
        worker->stop_warmup_timer();
    }

    virtual void clear_default_addr_info()
    {
    }

    virtual int connected()
    {
        is_client_stopped = false;
        do_read();
        if (connection_made() != 0)
        {
            return -1;
        }
        return 0;
    }

    virtual void feed_timing_script_request_timeout_timer()
    {
        auto self = this->shared_from_this();
        auto task = [this, self]()
        {
            handle_con_activity_timer_timeout(boost::asio::error::timed_out);
        };
        io_context.post(task);
    }

    virtual int select_protocol_and_allocate_session()
    {
        // TODO:
        //if (ssl)
        //{
        //}
        //else
        {
            switch (config->no_tls_proto)
            {
                case Config::PROTO_HTTP2:
                    session = std::make_unique<Http2Session>(this);
                    selected_proto = NGHTTP2_CLEARTEXT_PROTO_VERSION_ID;
                    break;
                case Config::PROTO_HTTP1_1:
                    session = std::make_unique<Http1Session>(this);
                    selected_proto = NGHTTP2_H1_1.str();
                    break;
                default:
                    // unreachable
                    assert(0);
            }
            print_app_info();
        }

        return 0;
    }

    virtual size_t push_data_to_output_buffer(const uint8_t* data, size_t length)
    {
        if (output_buffers[output_buffer_index].capacity() - output_data_length < length)
        {
            output_buffers[output_buffer_index].reserve(output_data_length + length);
            //return NGHTTP2_ERR_WOULDBLOCK;
        }
        std::memcpy(output_buffers[output_buffer_index].data() + output_data_length, data, length);
        output_data_length += length;
        return output_data_length;
    }
    virtual void signal_write()
    {
        auto self = this->shared_from_this();
        io_context.post([this, self]()
        {
            handle_write_signal();
        });
        //handle_write_signal();
    }
    virtual bool any_pending_data_to_write()
    {
        return (output_data_length > 0);
    }

    virtual std::shared_ptr<Client_Interface> create_dest_client(const std::string& dst_sch,
                                                                 const std::string& dest_authority)
    {
        // TODO:
        /*
        if (dst_sch == "https")
        {
            auto new_client =
                std::make_unique<asio_client_connection<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>>(io_context,
                                                                                                                 ssl_context, this->id, worker, req_todo, config, this, dst_sch, dest_authority);
            return new_client;
        }
        else
        */
        {
            auto new_client =
                std::make_shared<asio_client_connection>(io_context, this->id, worker,
                                                         req_todo, config, this, dst_sch, dest_authority);
            return new_client;
        }
    }

    virtual void setup_connect_with_async_fqdn_lookup()
    {
        return;
    }

    virtual int connect_to_host(const std::string& dest_schema, const std::string& dest_authority)
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
        auto self = this->shared_from_this();
        dns_resolver.async_resolve(query,
                                   [this, self](const boost::system::error_code & err, boost::asio::ip::tcp::resolver::iterator endpoint_iterator)
        {
            on_resolve_result_event(err, endpoint_iterator);
        });
        start_connect_timeout_timer();
        schema = dest_schema;
        authority = dest_authority;

        return 0;

    }

    virtual void probe_and_connect_to(const std::string& schema, const std::string& authority)
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
        auto self = this->shared_from_this();
        dns_resolver.async_resolve(query,
                                   [this, self](const boost::system::error_code & err, boost::asio::ip::tcp::resolver::iterator endpoint_iterator)
        {
            on_probe_resolve_result_event(err, endpoint_iterator);
        });
    }

    virtual void setup_graceful_shutdown()
    {
        write_clear_callback = [this]()
        {
            disconnect();
            worker->free_client(this);
            return false;
        };
    }

private:

    void start_ping_watcher()
    {
        if (!config->json_config_schema.interval_to_send_ping > 0.)
        {
            return;
        }
        auto self = this->shared_from_this();
        ping_timer.expires_from_now(boost::posix_time::millisec((size_t)(1000 *
                                                                         config->json_config_schema.interval_to_send_ping)));
        ping_timer.async_wait
        (
            [this, self](const boost::system::error_code & ec)
        {
            handle_ping_timeout(ec);
        });
    }

    void restart_rps_timer()
    {
        rps_timer.expires_from_now(boost::posix_time::millisec(std::max(100, 1000 / (int)rps)));
        auto self = this->shared_from_this();
        rps_timer.async_wait
        (
            [this, self](const boost::system::error_code & ec)
        {
            handle_rps_timer_timeout(ec);
        });
    }

    bool timer_common_check(boost::asio::deadline_timer& timer, const boost::system::error_code& ec,
                            void (asio_client_connection::*handler)(const boost::system::error_code&))
    {
        if (boost::asio::error::operation_aborted == ec)
        {
            return false;
        }

        if (is_client_stopped)
        {
            return false;
        }

        if (timer.expires_at() >
            boost::asio::deadline_timer::traits_type::now())
        {
            auto self = this->shared_from_this();
            timer.async_wait
            (
                [this, handler, self](const boost::system::error_code & ec)
            {
                (this->*handler)(ec);
            });
            return false;
        }
        return true;
    }
    virtual void start_rps_timer()
    {
        restart_rps_timer();
    }

    virtual void conn_activity_timeout_handler()
    {
        stop();
    }

    virtual void handle_delayed_reconnect_timer_timeout(const boost::system::error_code& ec)
    {
        if (!timer_common_check(delayed_reconnect_timer, ec, &asio_client_connection::handle_delayed_reconnect_timer_timeout))
        {
            return;
        }
        reconnect_to_used_host();
    }

    virtual void connect_to_prefered_host_timer_handler(const boost::system::error_code& ec)
    {
        if (!timer_common_check(connect_back_to_preferred_host_timer, ec,
                                &asio_client_connection::connect_to_prefered_host_timer_handler))
        {
            return;
        }
        if (CLIENT_CONNECTED != state)
        {
            return;
        }
        else if (authority == preferred_authority && CLIENT_CONNECTED == state)
        {
            return;
        }
        else
        {
            probe_and_connect_to(schema, preferred_authority);
            start_connect_to_preferred_host_timer();
        }
    }

    void handle_rps_timer_timeout(const boost::system::error_code& ec)
    {
        if (!timer_common_check(rps_timer, ec, &asio_client_connection::handle_rps_timer_timeout))
        {
            return;
        }
        restart_rps_timer();
        on_rps_timer();
    }

    void handle_timing_script_request_timeout(const boost::system::error_code& ec)
    {
        if (!timer_common_check(timing_script_request_timeout_timer,
                                ec, &asio_client_connection::handle_timing_script_request_timeout))
        {
            return;
        }
        timing_script_timeout_handler();
    }

    void handle_stream_timeout_timer_timeout(const boost::system::error_code& ec)
    {
        if (!timer_common_check(stream_timeout_timer, ec, &asio_client_connection::handle_stream_timeout_timer_timeout))
        {
            return;
        }
        reset_timeout_requests();
        start_stream_timeout_timer();
    }

    void handle_con_activity_timer_timeout(const boost::system::error_code& ec)
    {
        if (!timer_common_check(conn_activity_timer, ec, &asio_client_connection::handle_con_activity_timer_timeout))
        {
            return;
        }
        conn_activity_timeout_handler();
        start_conn_active_watcher();
    }

    void handle_con_inactivity_timer_timeout(const boost::system::error_code& ec)
    {
        if (!timer_common_check(conn_activity_timer, ec, &asio_client_connection::handle_con_inactivity_timer_timeout))
        {
            return;
        }
        conn_activity_timeout_handler();
        start_conn_inactivity_watcher();
    }


    void handle_ping_timeout(const boost::system::error_code& ec)
    {
        if (!timer_common_check(ping_timer, ec, &asio_client_connection::handle_ping_timeout))
        {
            return;
        }
        submit_ping();
        start_ping_watcher();
    }

    virtual void graceful_restart_connection()
    {
        write_clear_callback = [this]()
        {
            disconnect();
            connect_to_host(schema, authority);
            return true;
        };
        terminate_session();
    }

    virtual void start_request_delay_execution_timer()
    {
        delay_request_execution_timer.expires_from_now(boost::posix_time::millisec(10));
        auto self = this->shared_from_this();
        delay_request_execution_timer.async_wait
        (
            [this, self](const boost::system::error_code & ec)
        {
            handle_request_execution_timer_timeout(ec);
        });
    }

    void handle_request_execution_timer_timeout(const boost::system::error_code& ec)
    {
        if (!timer_common_check(delay_request_execution_timer, ec,
                                &asio_client_connection::handle_request_execution_timer_timeout))
        {
            return;
        }
        resume_delayed_request_execution();
        start_request_delay_execution_timer();
    }

    void on_probe_connected_event(const boost::system::error_code& err,
                                  boost::asio::ip::tcp::resolver::iterator endpoint_iterator)
    {
        boost::system::error_code ignored_ec;
        client_probe_socket.lowest_layer().close(ignored_ec);
        if (!err)
        {
            on_prefered_host_up();
        }
    }

    void on_connected_event(const boost::system::error_code& err,
                            boost::asio::ip::tcp::resolver::iterator endpoint_iterator)
    {
        static thread_local boost::asio::ip::tcp::resolver::iterator end_of_resolve_result;
        if (!err)
        {
            if (connected() != 0)
            {
                handle_connection_error();
            }
        }
        else
        {
            std::cerr << __FUNCTION__ << " err: " << err << std::endl;
            if (endpoint_iterator != end_of_resolve_result)
            {
                // The connection failed. Try the next endpoint in the list.
                client_socket.close();
                boost::asio::ip::tcp::endpoint endpoint = *endpoint_iterator;
                auto next_endpoint_iterator = ++endpoint_iterator;
                auto self = this->shared_from_this();
                client_socket.async_connect(endpoint,
                                            [this, self, next_endpoint_iterator](const boost::system::error_code & err)
                {
                    on_connected_event(err, next_endpoint_iterator);
                });
            }
            else
            {
                handle_connection_error();
            }
        }
    }

    boost::asio::ip::tcp::socket& socket()
    {
        return client_socket;
    }

    void handle_connect_timeout(const boost::system::error_code& ec)
    {
        if (!timer_common_check(connect_timer, ec, &asio_client_connection::handle_connect_timeout))
        {
            return;
        }
        if (is_client_stopped)
        {
            return;
        }
        stop();
    }

    void handle_connection_error()
    {
        std::cerr << __FUNCTION__ << ":" << schema << "://" << authority << std::endl;
        fail();
        if (reconnect_to_alt_addr())
        {
            return;
        }
        worker->free_client(this);
        return;
    }

    void handle_read_complete(const boost::system::error_code& e, std::size_t bytes_transferred)
    {
        if (e)
        {
            if (boost::asio::error::misc_errors::eof == e)
            {
                std::cerr << "EOF, remote disconnected: " << schema << "://" << authority << std::endl;
            }

            if (e != boost::asio::error::operation_aborted)
            {
                std::cerr << "read error_code:" << e << ", bytes_transferred:" << bytes_transferred << std::endl;
                return handle_connection_error();
            }
            return;
        }
        restart_timeout_timer();
        if (session->on_read(input_buffer.data(), bytes_transferred) != 0)
        {
            return handle_connection_error();
        }

        do_read();
    }

    void do_read()
    {
        if (is_client_stopped)
        {
            return;
        }
        auto self = this->shared_from_this();
        client_socket.async_read_some(
            boost::asio::buffer(input_buffer),
            [this, self](const boost::system::error_code & e, std::size_t bytes_transferred)
        {
            handle_read_complete(e, bytes_transferred);
        });
    }

    void handle_write_complete(const boost::system::error_code& e, std::size_t bytes_transferred)
    {
        if (e)
        {
            if (e != boost::asio::error::operation_aborted)
            {
                std::cerr << "write error_code:" << e << ", bytes_transferred:" << bytes_transferred << std::endl;
                return handle_connection_error();
            }
            return;
        }

        restart_timeout_timer();

        is_write_in_progress = false;

        if (write_clear_callback)
        {
            auto func = std::move(write_clear_callback);
            auto write_allowed = func();
            if (!write_allowed)
            {
                return;
            }
        }

        do_write();
    }

    void handle_write_signal()
    {
        session->on_write();
        do_write();
    }


    void do_write()
    {
        if (is_write_in_progress || is_client_stopped || output_data_length <= 0)
        {
            return;
        }

        auto& buffer = output_buffers[output_buffer_index];
        auto length = output_data_length;

        is_write_in_progress = true;
        output_data_length = 0;
        output_buffer_index = ((++output_buffer_index) % output_buffers.size());

        auto self = this->shared_from_this();
        boost::asio::async_write(
            client_socket, boost::asio::buffer(buffer.data(), length),
            [this, self](const boost::system::error_code & e, std::size_t bytes_transferred)
        {
            handle_write_complete(e, bytes_transferred);
        });
    }

    void stop()
    {
        if (is_client_stopped)
        {
            return;
        }
        std::cerr << __FUNCTION__ << ":" << schema << "://" << authority << std::endl;
        is_client_stopped = true;
        boost::system::error_code ignored_ec;
        client_socket.lowest_layer().close(ignored_ec);
        connect_timer.cancel();
        rps_timer.cancel();
        delay_request_execution_timer.cancel();
        conn_activity_timer.cancel();
        ping_timer.cancel();
        conn_inactivity_timer.cancel();
        stream_timeout_timer.cancel();
        timing_script_request_timeout_timer.cancel();
        connect_back_to_preferred_host_timer.cancel();
        delayed_reconnect_timer.cancel();
    }

    void on_resolve_result_event(const boost::system::error_code& err,
                                 boost::asio::ip::tcp::resolver::iterator endpoint_iterator)
    {
        if (!err)
        {
            // Attempt a connection to the first endpoint in the list. Each endpoint
            // will be tried until we successfully establish a connection.
            boost::asio::ip::tcp::endpoint endpoint = *endpoint_iterator;
            auto next_endpoint_iterator = ++endpoint_iterator;
            auto self = this->shared_from_this();
            client_socket.lowest_layer().async_connect(endpoint,
                                                       [this, self, next_endpoint_iterator](const boost::system::error_code & err)
            {
                on_connected_event(err, next_endpoint_iterator);
            });
        }
        else
        {
            std::cerr << "Error: " << err.message() << "\n";
        }
    }

    void on_probe_resolve_result_event(const boost::system::error_code& err,
                                       boost::asio::ip::tcp::resolver::iterator endpoint_iterator)
    {
        if (!err)
        {
            // Attempt a connection to the first endpoint in the list. Each endpoint
            // will be tried until we successfully establish a connection.
            boost::asio::ip::tcp::endpoint endpoint = *endpoint_iterator;
            auto next_endpoint_iterator = ++endpoint_iterator;
            auto self = this->shared_from_this();
            client_probe_socket.lowest_layer().async_connect(endpoint,
                                                             [this, self, next_endpoint_iterator](const boost::system::error_code & err)
            {
                on_probe_connected_event(err, next_endpoint_iterator);
            });
        }
        else
        {
            std::cerr << "Error: " << err.message() << "\n";
        }
    }

    boost::asio::io_context& io_context;
    boost::asio::ip::tcp::resolver dns_resolver;
    boost::asio::ip::tcp::socket client_socket;
    boost::asio::ip::tcp::socket client_probe_socket;
    bool is_write_in_progress = false;
    bool is_client_stopped = false;

    /// Buffer for incoming data.
    std::array<uint8_t, 8_k> input_buffer;
    std::vector<std::vector<uint8_t>> output_buffers;
    size_t output_data_length = 0;
    size_t output_buffer_index = 0;

    boost::asio::deadline_timer connect_timer;
    boost::asio::deadline_timer delay_request_execution_timer;
    boost::asio::deadline_timer rps_timer;
    boost::asio::deadline_timer conn_activity_timer;
    boost::asio::deadline_timer ping_timer;
    boost::asio::deadline_timer conn_inactivity_timer;
    boost::asio::deadline_timer stream_timeout_timer;
    boost::asio::deadline_timer timing_script_request_timeout_timer;
    boost::asio::deadline_timer connect_back_to_preferred_host_timer;
    boost::asio::deadline_timer delayed_reconnect_timer;


    std::function<bool(void)> write_clear_callback;
};




}
#endif
