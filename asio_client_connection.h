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

#include <string>
#include <array>
#include <vector>
#ifdef _WINDOWS
#include <sdkddkver.h>
#include <WinError.h>
#include <Winsock2.h>
#endif
#include <boost/asio.hpp>
#include <boost/noncopyable.hpp>
#include <boost/asio/ssl.hpp>

#include "h2load_http2_session.h"
#include "h2load_http1_session.h"
#include "h2load_utils.h"
#include "Client_Interface.h"
#include "h2load_Config.h"
#include "h2load_stats.h"
#include "config_schema.h"

namespace h2load
{

class asio_client_connection
    : public h2load::Client_Interface, private boost::noncopyable
{
public:
    asio_client_connection
    (
        boost::asio::io_service& io_ctx,
        uint32_t id,
        Worker_Interface* wrker,
        size_t req_todo,
        Config* conf,
        boost::asio::ssl::context& ssl_context,
        Client_Interface* parent = nullptr,
        const std::string& dest_schema = "",
        const std::string& dest_authority = ""
    );

    virtual ~asio_client_connection();

    virtual void start_conn_active_watcher();

    virtual void start_conn_inactivity_watcher();

    virtual void start_ssl_handshake_watcher();

    virtual void start_stream_timeout_timer();

    virtual void restart_timeout_timer();

    virtual void stop_rps_timer();

    virtual void start_timing_script_request_timeout_timer(double duration);

    virtual void stop_timing_script_request_timeout_timer();

    virtual void start_connect_timeout_timer();

    virtual void stop_connect_timeout_timer();

    virtual void start_connect_to_preferred_host_timer();

    virtual void start_delayed_reconnect_timer();

    virtual void stop_conn_inactivity_timer();

    virtual int make_async_connection();

    virtual int do_connect();

    virtual void disconnect();

    virtual void start_warmup_timer();

    virtual void stop_warmup_timer();

    virtual void clear_default_addr_info();

    virtual int connected();

    virtual void feed_timing_script_request_timeout_timer();

    virtual size_t push_data_to_output_buffer(const uint8_t* data, size_t length);

    virtual void signal_write();

    virtual bool any_pending_data_to_write();

    virtual std::shared_ptr<Client_Interface> create_dest_client(const std::string& dst_sch,
                                                                 const std::string& dest_authority);

    virtual void setup_connect_with_async_fqdn_lookup();

    virtual int connect_to_host(const std::string& dest_schema, const std::string& dest_authority);

    virtual void probe_and_connect_to(const std::string& schema, const std::string& authority);

    virtual void setup_graceful_shutdown();

private:

    bool is_error_due_to_aborted_operation(const boost::system::error_code& e);

    void start_ping_watcher();

    void restart_rps_timer();

    bool timer_common_check(boost::asio::deadline_timer& timer, const boost::system::error_code& ec,
                            void (asio_client_connection::*handler)(const boost::system::error_code&));

    virtual void start_rps_timer();

    virtual void conn_activity_timeout_handler();

    virtual void handle_delayed_reconnect_timer_timeout(const boost::system::error_code& ec);

    virtual void connect_to_prefered_host_timer_handler(const boost::system::error_code& ec);

    void handle_rps_timer_timeout(const boost::system::error_code& ec);

    void handle_timing_script_request_timeout(const boost::system::error_code& ec);

    void handle_stream_timeout_timer_timeout(const boost::system::error_code& ec);

    void handle_ssl_handshake_timeout(const boost::system::error_code& ec);

    void handle_con_activity_timer_timeout(const boost::system::error_code& ec);

    void handle_con_inactivity_timer_timeout(const boost::system::error_code& ec);

    void handle_ping_timeout(const boost::system::error_code& ec);

    virtual void graceful_restart_connection();

    virtual void start_request_delay_execution_timer();

    void handle_request_execution_timer_timeout(const boost::system::error_code& ec);

    void on_probe_connected_event(const boost::system::error_code& err,
                                  boost::asio::ip::tcp::resolver::iterator endpoint_iterator);

    void start_async_handshake();

    template<typename SOCKET>
    void on_connected_event(const boost::system::error_code& err,
                            boost::asio::ip::tcp::resolver::iterator endpoint_iterator, SOCKET& socket);

    void handle_connect_timeout(const boost::system::error_code& ec);

    void handle_connection_error();

    void handle_read_complete(const boost::system::error_code& e, const std::size_t bytes_transferred);

    template<typename SOCKET>
    void common_read(SOCKET& socket);

    void do_tcp_read();

    void do_ssl_read();

    void do_read();

    void handle_write_complete(const boost::system::error_code& e, std::size_t bytes_transferred);

    void handle_write_signal();

    template<typename SOCKET>
    void common_write(SOCKET& socket);

    void do_tcp_write();

    void do_ssl_write();

    void do_write();

    void stop();

    template <typename SOCKET>
    void start_async_connect(boost::asio::ip::tcp::resolver::iterator endpoint_iterator, SOCKET& socket);

    void on_resolve_result_event(const boost::system::error_code& err,
                                 boost::asio::ip::tcp::resolver::iterator endpoint_iterator);

    void on_probe_resolve_result_event(const boost::system::error_code& err,
                                       boost::asio::ip::tcp::resolver::iterator endpoint_iterator);

    boost::asio::io_service& io_context;
    boost::asio::ip::tcp::resolver dns_resolver;
    boost::asio::ip::tcp::socket client_socket;
    boost::asio::ip::tcp::socket client_probe_socket;
    boost::asio::ssl::context& ssl_ctx;
    boost::asio::ssl::stream<boost::asio::ip::tcp::socket> ssl_socket;
    bool is_write_in_progress = false;
    bool is_client_stopped = false;
    bool write_signaled = false;

    std::vector<uint8_t> input_buffer;
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
    boost::asio::deadline_timer ssl_handshake_timer;
    std::function<void(asio_client_connection&)> do_read_fn, do_write_fn;

    std::function<bool(void)> write_clear_callback;
};




}
#endif
