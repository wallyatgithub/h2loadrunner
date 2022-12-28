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
#include "util.h"

#include "h2load_http2_session.h"
#include "h2load_http1_session.h"
#include "h2load_utils.h"
#include "base_client.h"
#include "h2load_Config.h"
#include "h2load_stats.h"
#include "config_schema.h"
#include <deque>

namespace h2load
{

class asio_client_connection
    : public h2load::base_client, private boost::noncopyable
{
public:
    asio_client_connection
    (
        boost::asio::io_service& io_ctx,
        uint32_t id,
        base_worker* wrker,
        size_t req_todo,
        Config* conf,
        boost::asio::ssl::context& ssl_context,
        base_client* parent = nullptr,
        const std::string& dest_schema = "",
        const std::string& dest_authority = "",
        PROTO_TYPE proto = PROTO_UNSPECIFIED
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

    virtual void disconnect();

    virtual void start_warmup_timer();

    virtual void stop_warmup_timer();

    virtual void clear_default_addr_info();

    virtual int connected();

    virtual void feed_timing_script_request_timeout_timer();

    virtual size_t push_data_to_output_buffer(const uint8_t* data, size_t length);

    virtual void signal_write();

    virtual bool any_pending_data_to_write();

    virtual std::shared_ptr<base_client> create_dest_client(const std::string& dst_sch,
                                                            const std::string& dest_authority,
                                                            PROTO_TYPE proto = PROTO_UNSPECIFIED);

    virtual int connect_to_host(const std::string& dest_schema, const std::string& dest_authority);

    virtual void probe_and_connect_to(const std::string& schema, const std::string& authority);

    virtual void setup_graceful_shutdown();

    virtual bool is_write_signaled()
    {
        return write_signaled;
    };

    virtual void stop_delayed_execution_timer();

private:

    bool is_error_due_to_aborted_operation(const boost::system::error_code& e);

    void start_ping_watcher();

    void restart_rps_timer();

    bool timer_common_check(boost::asio::deadline_timer& timer, const boost::system::error_code& ec,
                                   void (asio_client_connection::*handler)(const boost::system::error_code&), bool check_stop_flag = true,
                                   bool check_self_destruction_timer_flag = true);

    virtual void start_rps_timer();

    void start_self_destruction_timer();

    void handle_self_destruction_timer_timeout(const boost::system::error_code& ec);

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

    void handle_read_complete(bool is_quic, const boost::system::error_code& e, const std::size_t bytes_transferred);

    template<typename SOCKET>
    void common_tcp_read(SOCKET& socket);

    void do_tcp_read();

    void do_ssl_read();

    void do_read();

    bool handle_write_complete(bool is_quic, const boost::system::error_code& e, std::size_t bytes_transferred);

    void handle_write_signal();

    template<typename SOCKET>
    void common_tcp_write(SOCKET& socket);

    void do_tcp_write();

    void do_ssl_write();

    void do_write();

    void stop();

    template <typename SOCKET>
    void start_async_connect(boost::asio::ip::tcp::resolver::iterator endpoint_iterator, SOCKET& socket);

    template <typename SOCKET>
    void start_async_connect(boost::asio::ip::tcp::endpoint endpoint, SOCKET& socket);

    void on_resolve_result_event(const boost::system::error_code& err,
                                 boost::asio::ip::tcp::resolver::iterator endpoint_iterator);

    void on_probe_resolve_result_event(const boost::system::error_code& err,
                                       boost::asio::ip::tcp::resolver::iterator endpoint_iterator);
#ifdef ENABLE_HTTP3
    void do_udp_read();
    void on_udp_resolve_result_event(const boost::system::error_code& err,
                                     boost::asio::ip::udp::resolver::iterator endpoint_iterator);
    void start_udp_async_connect(boost::asio::ip::udp::resolver::iterator endpoint_iterator);

    void start_udp_async_connect(boost::asio::ip::udp::endpoint remote_endpoint);

    void pre_udp_async_connect(boost::asio::ip::udp::endpoint remote_endpoint);

    void post_udp_async_connect(boost::asio::ip::udp::endpoint remote_endpoint);

    void do_udp_write();

    int handle_http3_write_signal();
    int quic_pkt_timeout();
    void quic_restart_pkt_timer();

    void handle_quic_pkt_timer_timeout(const boost::system::error_code& ec);
    void quic_close_connection();

    void handle_quic_read_complete(std::size_t bytes_transferred);

    size_t get_one_available_buffer_for_quic_output();

    void send_buffer_to_udp_output(size_t index);

    void return_buffer_to_quic_output(size_t index);

    bool get_buffer_index_to_write_to_udp(size_t& index);

    void send_buffer_to_quic_output(size_t index);

#endif

    boost::asio::io_service& io_context;
    boost::asio::ip::tcp::resolver tcp_dns_resolver;
    boost::asio::ip::tcp::socket tcp_client_socket;
#ifdef ENABLE_HTTP3
    boost::asio::ip::udp::resolver udp_dns_resolver;
    std::unique_ptr<boost::asio::ip::udp::socket> udp_client_socket;
    std::vector<std::vector<uint8_t>> quic_output_buffers;
    std::vector<nghttp2::Address> quic_remote_addresses;
    std::vector<size_t> quic_output_buffer_sizes;
    std::deque<size_t> quic_output_buffer_indexes;
    std::deque<size_t> udp_output_buffer_indexes;
    boost::asio::ip::udp::endpoint remote_addr;
    boost::asio::deadline_timer quic_pkt_timer;
    bool quic_close_sent = false;
    struct  Defer_Or_DoNothing
    {
    public:
        std::function<void(void)> m_func;
        Defer_Or_DoNothing(std::function<void(void)> f): m_func(f) {};
        ~Defer_Or_DoNothing()
        {
            if (m_func)
            {
                m_func();
            }
        };
        void release()
        {
            auto t = std::move(m_func);
        };
    };
#endif
    boost::asio::ip::tcp::socket tcp_client_probe_socket;
    boost::asio::ssl::context& ssl_ctx;
    std::unique_ptr<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>> ssl_socket;
    std::deque<std::unique_ptr<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>> old_ssl_sockets;
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
    boost::asio::deadline_timer self_destruction_timer;
    bool self_destruction_timer_active = false;
    std::function<void(asio_client_connection*)> do_read_fn, do_write_fn;
};




}
#endif
