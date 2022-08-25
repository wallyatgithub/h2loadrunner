#include <iostream>
#include <istream>
#include <ostream>
#include <string>
#include <array>
#include <cstring>
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
#include "base_client.h"
#include "h2load_Config.h"
#include "h2load_stats.h"
#include "config_schema.h"
#include "asio_client_connection.h"
#include "base_worker.h"

#ifdef ENABLE_HTTP3
#include <nghttp3/nghttp3.h>
#include "h2load_http3_session.h"
#endif

namespace h2load
{



asio_client_connection::asio_client_connection
(
    boost::asio::io_service& io_ctx,
    uint32_t id,
    base_worker* wrker,
    size_t req_todo,
    Config* conf,
    boost::asio::ssl::context& ssl_context,
    base_client* parent,
    const std::string& dest_schema,
    const std::string& dest_authority
)
    : base_client(id, wrker, req_todo, conf, parent, dest_schema, dest_authority),
      io_context(io_ctx),
      tcp_dns_resolver(io_ctx),
      tcp_client_socket(io_ctx),
#ifdef ENABLE_HTTP3
      udp_dns_resolver(io_ctx),
      udp_client_socket(io_ctx),
      quic_output_buffers(2, std::vector<std::vector<uint8_t>>(max_quic_pkt_to_send, std::vector<uint8_t>(single_buffer_size,
                                                                                                          0))),
      quic_pkt_timer(io_ctx),
#endif
      tcp_client_probe_socket(io_ctx),
      input_buffer(single_buffer_size, 0),
      output_buffers(2, std::vector<uint8_t>(single_buffer_size, 0)),
      connect_timer(io_ctx),
      delay_request_execution_timer(io_ctx),
      rps_timer(io_ctx),
      conn_activity_timer(io_ctx),
      ping_timer(io_ctx),
      conn_inactivity_timer(io_ctx),
      stream_timeout_timer(io_ctx),
      timing_script_request_timeout_timer(io_ctx),
      connect_back_to_preferred_host_timer(io_ctx),
      delayed_reconnect_timer(io_ctx),
      ssl_ctx(ssl_context),
      ssl_socket(io_ctx, ssl_context),
      ssl_handshake_timer(io_ctx),
      do_read_fn(&asio_client_connection::do_tcp_read),
      do_write_fn(&asio_client_connection::do_tcp_write)
{
    init_connection_targert();
}

asio_client_connection::~asio_client_connection()
{
    std::cerr << "deallocate connection: " << schema << "://" << authority << std::endl;
    //printBacktrace();
    disconnect();
    final_cleanup();
}

void asio_client_connection::start_conn_active_watcher()
{
    if (!(config->conn_active_timeout > 0.))
    {
        return;
    }
    conn_activity_timer.expires_from_now(boost::posix_time::millisec((size_t)(1000 * config->conn_active_timeout)));
    conn_activity_timer.async_wait
    (
        [this](const boost::system::error_code & ec)
    {
        handle_con_activity_timer_timeout(ec);
    });
}

void asio_client_connection::start_ssl_handshake_watcher()
{
    ssl_handshake_timer.expires_from_now(boost::posix_time::millisec((size_t)(1000 * 2)));
    ssl_handshake_timer.async_wait
    (
        [this](const boost::system::error_code & ec)
    {
        handle_ssl_handshake_timeout(ec);
    });
}

void asio_client_connection::start_conn_inactivity_watcher()
{
    if (!(config->conn_inactivity_timeout > 0.))
    {
        return;
    }

    conn_inactivity_timer.expires_from_now(boost::posix_time::millisec((size_t)(1000 * config->conn_inactivity_timeout)));
    conn_inactivity_timer.async_wait
    (
        [this](const boost::system::error_code & ec)
    {
        handle_con_activity_timer_timeout(ec);
    });
}

void asio_client_connection::start_stream_timeout_timer()
{
    stream_timeout_timer.expires_from_now(boost::posix_time::millisec(10));

    stream_timeout_timer.async_wait
    (
        [this](const boost::system::error_code & ec)
    {
        handle_stream_timeout_timer_timeout(ec);
    });
}

void asio_client_connection::restart_timeout_timer()
{
    start_conn_inactivity_watcher();
    start_ping_watcher();
}

void asio_client_connection::stop_rps_timer()
{
    rps_timer.cancel();
}

void asio_client_connection::start_timing_script_request_timeout_timer(double duration)
{
    timing_script_request_timeout_timer.expires_from_now(boost::posix_time::millisec((size_t)(duration * 1000)));
    timing_script_request_timeout_timer.async_wait
    (
        [this](const boost::system::error_code & ec)
    {
        handle_timing_script_request_timeout(ec);
    });
}

void asio_client_connection::stop_timing_script_request_timeout_timer()
{
    timing_script_request_timeout_timer.cancel();
}

void asio_client_connection::start_connect_timeout_timer()
{
    // set a longer timeout if too many connections are to be established
    uint32_t timeout = (config->nclients / 1000) + 5;
    connect_timer.expires_from_now(boost::posix_time::seconds(timeout));
    connect_timer.async_wait
    (
        [this](const boost::system::error_code & ec)
    {
        handle_connect_timeout(ec);
    });
}

void asio_client_connection::stop_connect_timeout_timer()
{
    connect_timer.cancel();
}

void asio_client_connection::start_connect_to_preferred_host_timer()
{
    connect_back_to_preferred_host_timer.expires_from_now(boost::posix_time::millisec(1000));
    connect_back_to_preferred_host_timer.async_wait
    (
        [this](const boost::system::error_code & ec)
    {
        connect_to_prefered_host_timer_handler(ec);
    });
}

void asio_client_connection::start_delayed_reconnect_timer()
{
    delayed_reconnect_timer.expires_from_now(boost::posix_time::millisec(1000));
    delayed_reconnect_timer.async_wait
    (
        [this](const boost::system::error_code & ec)
    {
        handle_delayed_reconnect_timer_timeout(ec);
    });
}

void asio_client_connection::stop_conn_inactivity_timer()
{
    conn_inactivity_timer.cancel();
}

int asio_client_connection::make_async_connection()
{
    return connect_to_host(schema, authority);
}

int asio_client_connection::do_connect()
{
    return connect();
}

void asio_client_connection::disconnect()
{
#ifdef ENABLE_HTTP3
    quic_close_connection();
#endif

    stop();
    cleanup_due_to_disconnect();
}

void asio_client_connection::start_warmup_timer()
{
    worker->start_warmup_timer();
}

void asio_client_connection::stop_warmup_timer()
{
    worker->stop_warmup_timer();
}

void asio_client_connection::clear_default_addr_info()
{
}

int asio_client_connection::connected()
{
    if (config->verbose)
    {
        std::cerr << __FUNCTION__ << ":" << authority << std::endl;
    }

    is_client_stopped = false;

    do_read();

    if (connection_made() != 0)
    {
        call_connected_callbacks(false);
        return -1;
    }

    call_connected_callbacks(true);

    return 0;
}

void asio_client_connection::feed_timing_script_request_timeout_timer()
{
    auto task = [this]()
    {
        handle_con_activity_timer_timeout(boost::asio::error::timed_out);
    };
    io_context.post(task);
}

size_t asio_client_connection::push_data_to_output_buffer(const uint8_t* data, size_t length)
{
    if (output_buffers[output_buffer_index].capacity() - output_data_length < length)
    {
        std::vector<uint8_t> tempBuffer(output_data_length, 0);
        std::memcpy(tempBuffer.data(), output_buffers[output_buffer_index].data(), output_data_length);
        output_buffers[output_buffer_index].reserve(output_data_length + length);
        std::memcpy(output_buffers[output_buffer_index].data(), tempBuffer.data(), output_data_length);
    }
    std::memcpy(output_buffers[output_buffer_index].data() + output_data_length, data, length);
    output_data_length += length;
    return length;
}
void asio_client_connection::signal_write()
{
    if (!write_signaled)
    {
        io_context.post([this]()
        {
            handle_write_signal();
            write_signaled = false;
        });
        write_signaled = true;
    }
}
bool asio_client_connection::any_pending_data_to_write()
{
    return (output_data_length > 0);
}

std::shared_ptr<base_client> asio_client_connection::create_dest_client(const std::string& dst_sch,
                                                                        const std::string& dest_authority)
{
    auto new_client =
        std::make_shared<asio_client_connection>(io_context, this->id, worker,
                                                 req_todo, config, ssl_ctx, this, dst_sch, dest_authority);
    return new_client;
}

void asio_client_connection::setup_connect_with_async_fqdn_lookup()
{
    return;
}

int asio_client_connection::connect_to_host(const std::string& dest_schema, const std::string& dest_authority)
{
    if (config->verbose)
    {
        std::cerr << __FUNCTION__ << ":" << dest_authority << std::endl;
    }

    std::string host;
    std::string port;
    if (!get_host_and_port_from_authority(dest_schema, dest_authority, host, port))
    {
        exit(1);
    }
#ifdef ENABLE_HTTP3
    if (config->is_quic())
    {
        boost::asio::ip::udp::resolver::query query(host, port);
        udp_dns_resolver.async_resolve(query,
                                       [this](const boost::system::error_code & err, boost::asio::ip::udp::resolver::iterator endpoint_iterator)
        {
            on_udp_resolve_result_event(err, endpoint_iterator);
        });
    }
    else
#endif
    {
        if (schema == "https")
        {
            do_read_fn = &asio_client_connection::do_ssl_read;
            do_write_fn = &asio_client_connection::do_ssl_write;
            ssl = ssl_socket.native_handle();
        }
        else
        {
            do_read_fn = &asio_client_connection::do_tcp_read;
            do_write_fn = &asio_client_connection::do_tcp_write;
        }

        boost::asio::ip::tcp::resolver::query query(host, port);
        tcp_dns_resolver.async_resolve(query,
                                       [this](const boost::system::error_code & err, boost::asio::ip::tcp::resolver::iterator endpoint_iterator)
        {
            on_resolve_result_event(err, endpoint_iterator);
        });
    }
    start_connect_timeout_timer();
    schema = dest_schema;
    authority = dest_authority;
    state = CLIENT_CONNECTING;

    return 0;

}

void asio_client_connection::probe_and_connect_to(const std::string& schema, const std::string& authority)
{
    std::string host;
    std::string port;
    if (!get_host_and_port_from_authority(schema, authority, host, port))
    {
        return;
    }

    boost::asio::ip::tcp::resolver::query query(host, port);
    tcp_dns_resolver.async_resolve(query,
                                   [this](const boost::system::error_code & err, boost::asio::ip::tcp::resolver::iterator endpoint_iterator)
    {
        on_probe_resolve_result_event(err, endpoint_iterator);
    });
}

void asio_client_connection::setup_graceful_shutdown()
{
    write_clear_callback = [this]()
    {
        disconnect();
        io_context.post([this]()
        {
            worker->free_client(this);
        });
        return false;
    };
}

bool asio_client_connection::is_error_due_to_aborted_operation(const boost::system::error_code& e)
{
    if (config->verbose)
    {
        if (boost::asio::error::misc_errors::eof == e)
        {
            std::cerr << "EOF: remote disconnected: " << schema << "://" << authority << std::endl;
        }
    }
    if (e == boost::asio::error::operation_aborted)
    {
        return true;
    }
#ifdef _WINDOWS
    if ((e.value() == ERROR_CONNECTION_ABORTED) ||
        (e.value() == ERROR_REQUEST_ABORTED) ||
        (e.value() == WSA_E_CANCELLED) ||
        (e.value() == WSA_OPERATION_ABORTED))
    {
        return true;
    }
#endif
    return false;
}

void asio_client_connection::start_ping_watcher()
{
    if (!(config->json_config_schema.interval_to_send_ping > 0.))
    {
        return;
    }
    ping_timer.expires_from_now(boost::posix_time::millisec((size_t)(1000 *
                                                                     config->json_config_schema.interval_to_send_ping)));
    ping_timer.async_wait
    (
        [this](const boost::system::error_code & ec)
    {
        handle_ping_timeout(ec);
    });
}

void asio_client_connection::restart_rps_timer()
{
    rps_timer.expires_from_now(boost::posix_time::millisec(std::max(10, 1000 / (int)rps)));
    rps_timer.async_wait
    (
        [this](const boost::system::error_code & ec)
    {
        handle_rps_timer_timeout(ec);
    });
}

bool asio_client_connection::timer_common_check(boost::asio::deadline_timer& timer, const boost::system::error_code& ec,
                                                void (asio_client_connection::*handler)(const boost::system::error_code&))
{
    if (ec)
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
        timer.async_wait
        (
            [this, handler](const boost::system::error_code & ec)
        {
            (this->*handler)(ec);
        });
        return false;
    }
    return true;
}
void asio_client_connection::start_rps_timer()
{
    restart_rps_timer();
}

void asio_client_connection::conn_activity_timeout_handler()
{
    timeout();
}

void asio_client_connection::handle_delayed_reconnect_timer_timeout(const boost::system::error_code& ec)
{
    if (!timer_common_check(delayed_reconnect_timer, ec, &asio_client_connection::handle_delayed_reconnect_timer_timeout))
    {
        return;
    }
    reconnect_to_used_host();
}

void asio_client_connection::connect_to_prefered_host_timer_handler(const boost::system::error_code& ec)
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

void asio_client_connection::handle_rps_timer_timeout(const boost::system::error_code& ec)
{
    if (!timer_common_check(rps_timer, ec, &asio_client_connection::handle_rps_timer_timeout))
    {
        return;
    }
    restart_rps_timer();
    on_rps_timer();
}

void asio_client_connection::handle_timing_script_request_timeout(const boost::system::error_code& ec)
{
    if (!timer_common_check(timing_script_request_timeout_timer,
                            ec, &asio_client_connection::handle_timing_script_request_timeout))
    {
        return;
    }
    timing_script_timeout_handler();
}

void asio_client_connection::handle_stream_timeout_timer_timeout(const boost::system::error_code& ec)
{
    if (!timer_common_check(stream_timeout_timer, ec, &asio_client_connection::handle_stream_timeout_timer_timeout))
    {
        return;
    }
    reset_timeout_requests();
    start_stream_timeout_timer();
}

void asio_client_connection::handle_ssl_handshake_timeout(const boost::system::error_code& ec)
{
    if (!timer_common_check(ssl_handshake_timer, ec, &asio_client_connection::handle_ssl_handshake_timeout))
    {
        return;
    }
    handle_connection_error();
}

void asio_client_connection::handle_con_activity_timer_timeout(const boost::system::error_code& ec)
{
    if (!timer_common_check(conn_activity_timer, ec, &asio_client_connection::handle_con_activity_timer_timeout))
    {
        return;
    }
    conn_activity_timeout_handler();
    start_conn_active_watcher();
}

void asio_client_connection::handle_con_inactivity_timer_timeout(const boost::system::error_code& ec)
{
    if (!timer_common_check(conn_activity_timer, ec, &asio_client_connection::handle_con_inactivity_timer_timeout))
    {
        return;
    }
    conn_activity_timeout_handler();
    start_conn_inactivity_watcher();
}


void asio_client_connection::handle_ping_timeout(const boost::system::error_code& ec)
{
    if (!timer_common_check(ping_timer, ec, &asio_client_connection::handle_ping_timeout))
    {
        return;
    }
    submit_ping();
    start_ping_watcher();
}

void asio_client_connection::graceful_restart_connection()
{
    write_clear_callback = [this]()
    {
        disconnect();
        connect_to_host(schema, authority);
        return true;
    };
    terminate_session();
}

void asio_client_connection::start_request_delay_execution_timer()
{
    delay_request_execution_timer.expires_from_now(boost::posix_time::millisec(10));
    delay_request_execution_timer.async_wait
    (
        [this](const boost::system::error_code & ec)
    {
        handle_request_execution_timer_timeout(ec);
    });
}

void asio_client_connection::handle_request_execution_timer_timeout(const boost::system::error_code& ec)
{
    if (!timer_common_check(delay_request_execution_timer, ec,
                            &asio_client_connection::handle_request_execution_timer_timeout))
    {
        return;
    }
    resume_delayed_request_execution();
    start_request_delay_execution_timer();
}

void asio_client_connection::on_probe_connected_event(const boost::system::error_code& err,
                                                      boost::asio::ip::tcp::resolver::iterator endpoint_iterator)
{
    boost::system::error_code ignored_ec;
    tcp_client_probe_socket.lowest_layer().close(ignored_ec);
    if (!err)
    {
        on_prefered_host_up();
    }
}

void asio_client_connection::start_async_handshake()
{
    ssl_socket.async_handshake(
        boost::asio::ssl::stream_base::client,
        [this](const boost::system::error_code & e)
    {
        if (e)
        {
            handle_connection_error();
        }
        else
        {
            ssl_handshake_timer.cancel();
            if (connected() != 0)
            {
                handle_connection_error();
            }
        }
    });
}

template<typename SOCKET>
void asio_client_connection::on_connected_event(const boost::system::error_code& err,
                                                boost::asio::ip::tcp::resolver::iterator endpoint_iterator, SOCKET& socket)
{
    if (config->verbose)
    {
        std::cerr << __FUNCTION__ << ":" << authority << std::endl;
    }
    static thread_local boost::asio::ip::tcp::resolver::iterator end_of_resolve_result;
    if (!err)
    {
        socket.lowest_layer().set_option(boost::asio::ip::tcp::no_delay(true));
        boost::asio::socket_base::receive_buffer_size rcv_option(config->json_config_schema.skt_recv_buffer_size);
        socket.lowest_layer().set_option(rcv_option);
        boost::asio::socket_base::receive_buffer_size snd_option(config->json_config_schema.skt_send_buffer_size);
        socket.lowest_layer().set_option(snd_option);

        if (schema != "https")
        {
            if (connected() != 0)
            {
                handle_connection_error();
            }
        }
        else
        {
            start_async_handshake();
            start_ssl_handshake_watcher();
        }
    }
    else
    {
        if (config->verbose)
        {
            std::cerr << __FUNCTION__ << " err: " << err << std::endl;
        }
        if (is_error_due_to_aborted_operation(err))
        {
            return;
        }
        if (endpoint_iterator != end_of_resolve_result)
        {
            socket.lowest_layer().close();
            boost::asio::ip::tcp::endpoint endpoint = *endpoint_iterator;
            auto next_endpoint_iterator = ++endpoint_iterator;
            socket.lowest_layer().async_connect(endpoint,
                                                [this, next_endpoint_iterator, &socket](const boost::system::error_code & err)
            {
                on_connected_event(err, next_endpoint_iterator, socket);
            });
        }
        else
        {
            handle_connection_error();
        }
    }
}

void asio_client_connection::handle_connect_timeout(const boost::system::error_code& ec)
{
    if (!timer_common_check(connect_timer, ec, &asio_client_connection::handle_connect_timeout))
    {
        return;
    }
    if (is_client_stopped)
    {
        return;
    }
    handle_connection_error();
}

void asio_client_connection::handle_connection_error()
{
    if (config->verbose)
    {
        std::cerr << __FUNCTION__ << ": " << schema << "://" << authority << std::endl;
    }
    call_connected_callbacks(false);
    // for http1 reconnect
    if (try_again_or_fail() == 0)
    {
        return;
    }

    fail();
    if (reconnect_to_alt_addr())
    {
        is_client_stopped = false;
        return;
    }
    io_context.post([this]()
    {
        worker->free_client(this);
    });
    return;
}

void asio_client_connection::handle_read_complete(const boost::system::error_code& e,
                                                  const std::size_t bytes_transferred)
{
    if (e)
    {
        if (config->verbose)
        {
            std::cerr << "read error code: " << e << ", bytes_transferred: " << bytes_transferred << std::endl;
        }
        if (!is_error_due_to_aborted_operation(e))
        {
            return handle_connection_error();
        }
        return;
    }
    if (!session)
    {
        // a read finish callback gets scheduled while a connection switch is ongoing, do nothing
        return;
    }
    worker->stats.bytes_total += bytes_transferred;
    restart_timeout_timer();
#ifdef ENABLE_HTTP3
    if (udp_client_socket.is_open())
    {
        assert(quic.conn);
        int rv;
        ngtcp2_pkt_info pi{};
        ++worker->stats.udp_dgram_recv;
        auto local_sockaddr = udp_client_socket.local_endpoint().data();
        ngtcp2_socklen local_addr_len = udp_client_socket.local_endpoint().size();

        auto remote_sockaddr = remote_addr.data();
        ngtcp2_socklen remote_addr_len = remote_addr.size();

        auto path = ngtcp2_path
        {
            {
                local_sockaddr,
                local_addr_len,
            },
            {
                remote_sockaddr,
                remote_addr_len,
            },
        };
        auto now = std::chrono::steady_clock::now().time_since_epoch();
        rv = ngtcp2_conn_read_pkt(quic.conn, &path, &pi, input_buffer.data(), bytes_transferred,
                                  std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
        if (rv != 0)
        {
            std::cerr << "ngtcp2_conn_read_pkt: " << ngtcp2_strerror(rv) << std::endl;

            if (!quic.last_error.error_code)
            {
                if (rv == NGTCP2_ERR_CRYPTO)
                {
                    ngtcp2_connection_close_error_set_transport_error_tls_alert(
                        &quic.last_error, ngtcp2_conn_get_tls_alert(quic.conn), nullptr,
                        0);
                }
                else
                {
                    ngtcp2_connection_close_error_set_transport_error_liberr(
                        &quic.last_error, rv, nullptr, 0);
                }
            }
            return handle_connection_error();
        }

    }
    else

#endif

    {
        if (session->on_read(input_buffer.data(), bytes_transferred) != 0)
        {
            return handle_connection_error();
        }
    }
    if (bytes_transferred >= input_buffer.size())
    {
        input_buffer.resize(2 * bytes_transferred);
    }
    do_read();
}

template<typename SOCKET>
void asio_client_connection::common_read(SOCKET& socket)
{
    if (is_client_stopped)
    {
        return;
    }
    socket.async_read_some(
        boost::asio::buffer(input_buffer),
        [this](const boost::system::error_code & e, std::size_t bytes_transferred)
    {
        handle_read_complete(e, bytes_transferred);
    });
}

void asio_client_connection::do_tcp_read()
{
    common_read(tcp_client_socket);
}

void asio_client_connection::do_ssl_read()
{
    common_read(ssl_socket);
}

void asio_client_connection::do_read()
{
    do_read_fn(this);
}

void asio_client_connection::handle_write_complete(const boost::system::error_code& e, std::size_t bytes_transferred)
{
    if (e)
    {
        if (!is_error_due_to_aborted_operation(e))
        {
            if (config->verbose)
            {
                std::cerr << "write error code: " << e << ", bytes_transferred: " << bytes_transferred << std::endl;
            }
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

void asio_client_connection::handle_write_signal()
{
    if (!session)
    {
        // a write signal is scheduled while connection switch is ongoing
        return;
    }
#ifdef ENABLE_HTTP3
    if (udp_client_socket.is_open())
    {
        handle_http3_write_signal();
    }
    else
#endif
    {
        for (;;)
        {
            auto output_data_length_before = output_data_length;
            session->on_write();
            auto bytes_to_write = output_data_length - output_data_length_before;
            if (!bytes_to_write)
            {
                break;
            }
        }
    }
    do_write();
}

template<typename SOCKET>
void asio_client_connection::common_write(SOCKET& socket)
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

    boost::asio::async_write(
        socket, boost::asio::buffer(buffer.data(), length),
        [this](const boost::system::error_code & e, std::size_t bytes_transferred)
    {
        handle_write_complete(e, bytes_transferred);
    });
}

void asio_client_connection::do_tcp_write()
{
    common_write(tcp_client_socket);
}

void asio_client_connection::do_ssl_write()
{
    common_write(ssl_socket);
}

void asio_client_connection::do_write()
{
    do_write_fn(this);
}

void asio_client_connection::stop()
{
    if (is_client_stopped)
    {
        return;
    }
    is_client_stopped = true;
    boost::system::error_code ignored_ec;
    tcp_client_socket.lowest_layer().close(ignored_ec);
    ssl_socket.lowest_layer().close(ignored_ec);
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
    ssl_handshake_timer.cancel();
#ifdef ENABLE_HTTP3
    quic_pkt_timer.cancel();
#endif
}

template <typename SOCKET>
void asio_client_connection::start_async_connect(boost::asio::ip::tcp::resolver::iterator endpoint_iterator,
                                                 SOCKET& socket)
{
    static thread_local boost::asio::ip::tcp::resolver::iterator end_of_resolve_result;
    if (endpoint_iterator == end_of_resolve_result)
    {
        handle_connection_error();
        return;
    }

    boost::asio::ip::tcp::endpoint endpoint = *endpoint_iterator;
    auto next_endpoint_iterator = ++endpoint_iterator;
    socket.lowest_layer().async_connect(endpoint,
                                        [this, next_endpoint_iterator, &socket](const boost::system::error_code & err)
    {
        on_connected_event(err, next_endpoint_iterator, socket);
    });
}
void asio_client_connection::on_resolve_result_event(const boost::system::error_code& err,
                                                     boost::asio::ip::tcp::resolver::iterator endpoint_iterator)
{
    if (config->verbose)
    {
        std::cerr << __FUNCTION__ << ":" << authority << std::endl;
    }
    if (!err)
    {
        if (schema != "https")
        {
            start_async_connect(endpoint_iterator, tcp_client_socket);
        }
        else
        {
            start_async_connect(endpoint_iterator, ssl_socket);
        }
    }
    else
    {
        std::cerr << "Error: " << err.message() << "\n";
    }
}

void asio_client_connection::on_probe_resolve_result_event(const boost::system::error_code& err,
                                                           boost::asio::ip::tcp::resolver::iterator endpoint_iterator)
{
    if (!err)
    {
        boost::asio::ip::tcp::endpoint endpoint = *endpoint_iterator;
        auto next_endpoint_iterator = ++endpoint_iterator;
        tcp_client_probe_socket.lowest_layer().async_connect(endpoint,
                                                             [this, next_endpoint_iterator](const boost::system::error_code & err)
        {
            on_probe_connected_event(err, next_endpoint_iterator);
        });
    }
    else
    {
        std::cerr << "Error: " << err.message() << "\n";
    }
}


#ifdef ENABLE_HTTP3

void asio_client_connection::quic_restart_pkt_timer()
{
    auto expiry = ngtcp2_conn_get_expiry(quic.conn);
    auto now = std::chrono::duration_cast<std::chrono::nanoseconds>
               (std::chrono::steady_clock::now().time_since_epoch()).count();

    quic_pkt_timer.expires_from_now(boost::posix_time::millisec((expiry - now) / (1000 * 1000)));
    quic_pkt_timer.async_wait
    (
        [this](const boost::system::error_code & ec)
    {
        handle_quic_pkt_timer_timeout(ec);
    });

}

int asio_client_connection::quic_pkt_timeout()
{
    int rv;
    auto now = std::chrono::duration_cast<std::chrono::nanoseconds>
               (std::chrono::steady_clock::now().time_since_epoch()).count();

    rv = ngtcp2_conn_handle_expiry(quic.conn, now);
    if (rv != 0)
    {
        ngtcp2_connection_close_error_set_transport_error_liberr(&quic.last_error,
                                                                 rv, nullptr, 0);
        return -1;
    }
    signal_write();
    return 0;
}

void asio_client_connection::handle_quic_pkt_timer_timeout(const boost::system::error_code& ec)
{
    if (!timer_common_check(quic_pkt_timer, ec, &asio_client_connection::handle_quic_pkt_timer_timeout))
    {
        return;
    }
    quic_restart_pkt_timer();
    if (quic_pkt_timeout() != 0)
    {
        handle_connection_error();
    }
}

void asio_client_connection::do_udp_read()
{
    if (is_client_stopped)
    {
        return;
    }
    udp_client_socket.async_receive_from(
        boost::asio::buffer(input_buffer), remote_addr,
        [this](const boost::system::error_code & e, std::size_t bytes_transferred)
    {
        handle_read_complete(e, bytes_transferred);
    });
}

void asio_client_connection::on_udp_resolve_result_event(const boost::system::error_code& err,
                                                         boost::asio::ip::udp::resolver::iterator endpoint_iterator)
{
    if (config->verbose)
    {
        std::cerr << __FUNCTION__ << ":" << authority << std::endl;
    }
    if (!err)
    {
        start_udp_async_connect(endpoint_iterator);
    }
    else
    {
        std::cerr << "Error: " << err.message() << "\n";
    }
}

void asio_client_connection::start_udp_async_connect(boost::asio::ip::udp::resolver::iterator endpoint_iterator)
{
    static thread_local boost::asio::ip::udp::resolver::iterator end_of_resolve_result;
    if (endpoint_iterator == end_of_resolve_result)
    {
        std::cerr << __FUNCTION__ << ": start_udp_async_connect failed "<< std::endl;
        handle_connection_error();
        return;
    }

    boost::asio::ip::udp::endpoint remote_endpoint = *endpoint_iterator;
    auto next_endpoint_iterator = ++endpoint_iterator;
    if (remote_endpoint.address().is_v4())
    {
        boost::asio::ip::udp::endpoint local_endpoint(boost::asio::ip::udp::v4(), 0);
        udp_client_socket.open(boost::asio::ip::udp::v4());
        udp_client_socket.bind(local_endpoint);
    }
    else
    {
        boost::asio::ip::udp::endpoint local_endpoint(boost::asio::ip::udp::v6(), 0);
        udp_client_socket.open(boost::asio::ip::udp::v6());
        udp_client_socket.bind(local_endpoint);
    }
    do_read_fn = &asio_client_connection::do_udp_read;
    do_write_fn = &asio_client_connection::do_udp_write;

    udp_client_socket.lowest_layer().async_connect(remote_endpoint,
                                                   [this, endpoint_iterator](const boost::system::error_code & err)
    {
        if (!err)
        {
            boost::asio::ip::udp::endpoint remote_endpoint = *endpoint_iterator;
            auto local_sockaddr = udp_client_socket.local_endpoint().data();
            auto local_addr_len = udp_client_socket.local_endpoint().size();

            auto remote_sockaddr = remote_endpoint.data();
            auto remote_addr_len = remote_endpoint.size();

            if (quic_init(local_sockaddr, local_addr_len, remote_sockaddr,
                          remote_addr_len) != 0)
            {
                std::cerr << "quic_init failed" << std::endl;
                exit(1);
            }
            signal_write();
            do_read();
        }
        else
        {
            if (config->verbose)
            {
                std::cerr << __FUNCTION__ << " err: " << err << std::endl;
            }
            auto iter = endpoint_iterator;
            auto next_endpoint_iterator = ++iter;
            if (next_endpoint_iterator != end_of_resolve_result)
            {
                udp_client_socket.close();
                start_udp_async_connect(next_endpoint_iterator);
            }
            else
            {
                handle_connection_error();
            }
        }

    });


}

int asio_client_connection::handle_http3_write_signal()
{
    if (quic.close_requested)
    {
        return -1;
    }

    int rv;
    std::array<nghttp3_vec, 16> vec;
    size_t pktcnt = 0;
    size_t max_pktcnt = max_quic_pkt_to_send;
    ngtcp2_path_storage ps;
    quic_output_pkt_count = 0;
    auto max_udp_payload_size = ngtcp2_conn_get_max_udp_payload_size(quic.conn);

    ngtcp2_path_storage_zero(&ps);

    for (auto& buf : quic_output_buffers[output_buffer_index])
    {
        buf.resize(0);
    }

    auto s = static_cast<Http3Session*>(session.get());


    for (;;)
    {
        int64_t stream_id = -1;
        int fin = 0;
        ssize_t sveccnt = 0;

        if (session && ngtcp2_conn_get_max_data_left(quic.conn))
        {
            sveccnt = s->write_stream(stream_id, fin, vec.data(), vec.size());
            if (sveccnt == -1)
            {
                return -1;
            }
        }

        ngtcp2_ssize ndatalen;
        auto v = vec.data();
        auto vcnt = static_cast<size_t>(sveccnt);

        uint32_t flags = NGTCP2_WRITE_STREAM_FLAG_MORE;
        if (fin)
        {
            flags |= NGTCP2_WRITE_STREAM_FLAG_FIN;
        }

        auto now = std::chrono::steady_clock::now().time_since_epoch();

        auto nwrite = ngtcp2_conn_writev_stream(
                          quic.conn, &ps.path, nullptr, quic_output_buffers[output_buffer_index][quic_output_pkt_count].data(),
                          max_udp_payload_size, &ndatalen,
                          flags, stream_id, reinterpret_cast<const ngtcp2_vec*>(v), vcnt,
                          std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
        if (nwrite < 0)
        {
            switch (nwrite)
            {
                case NGTCP2_ERR_STREAM_DATA_BLOCKED:
                    assert(ndatalen == -1);
                    s->block_stream(stream_id);
                    continue;
                case NGTCP2_ERR_STREAM_SHUT_WR:
                    assert(ndatalen == -1);
                    s->shutdown_stream_write(stream_id);
                    continue;
                case NGTCP2_ERR_WRITE_MORE:
                    assert(ndatalen >= 0);
                    if (s->add_write_offset(stream_id, ndatalen) != 0)
                    {
                        return -1;
                    }
                    continue;
            }

            ngtcp2_connection_close_error_set_transport_error_liberr(
                &quic.last_error, nwrite, nullptr, 0);
            return -1;
        }
        else if (ndatalen >= 0 && s->add_write_offset(stream_id, ndatalen) != 0)
        {
            return -1;
        }
        quic_output_buffers[output_buffer_index][quic_output_pkt_count].resize(nwrite);
        memcpy(&quic_remote_addresses[output_buffer_index][quic_output_pkt_count].su, ps.path.remote.addr,
               ps.path.remote.addrlen);
        quic_remote_addresses[output_buffer_index][quic_output_pkt_count].len = ps.path.remote.addrlen;

        quic_restart_pkt_timer();

        quic_output_pkt_count++;

        if (max_pktcnt == quic_output_pkt_count ||
            (quic_output_buffers[output_buffer_index][quic_output_pkt_count - 1].size() <
             quic_output_buffers[output_buffer_index][0].size()))
        {
            do_udp_write();
            return 0;
        }
    }
}

void asio_client_connection::do_udp_write()
{
    if (is_write_in_progress || is_client_stopped || quic_output_pkt_count <= 0
        || quic_output_buffers[output_buffer_index][0].empty())
    {
        return;
    }

    quic_buffer_to_send.resize(quic_output_pkt_count);
    for (size_t i = 0; i < quic_buffer_to_send.size(); i++)
    {
        std::swap(quic_buffer_to_send[i], quic_output_buffers[output_buffer_index][i]);
    }
    boost::asio::ip::udp::endpoint remote_addr;
    remote_addr.resize(quic_remote_addresses[output_buffer_index][0].len);
    memcpy(remote_addr.data(), &quic_remote_addresses[output_buffer_index][0].su, quic_remote_addresses[output_buffer_index][0].len);

    output_buffer_index = ((++output_buffer_index) % quic_output_buffers.size());
    quic_output_pkt_count = 0;
    is_write_in_progress = true;

    udp_client_socket.async_send_to(boost::asio::buffer(quic_buffer_to_send), remote_addr,
        [this](const boost::system::error_code & e, std::size_t bytes_transferred)
    {
        worker->stats.udp_dgram_sent += quic_buffer_to_send.size();
        auto orig_buffer_index = (output_buffer_index ? output_buffer_index - 1 : quic_output_buffers.size() - 1);
        for (size_t i = 0; i < quic_buffer_to_send.size(); i++)
        {
            std::swap(quic_buffer_to_send[i], quic_output_buffers[orig_buffer_index][i]);
        }
        handle_write_complete(e, bytes_transferred);
    });
}

void asio_client_connection::quic_close_connection()
{
    if (udp_client_socket.is_open() && quic.conn && (!quic_close_sent))
    {
        quic_close_sent = true;
        auto buffer_to_send = std::make_shared<std::vector<uint8_t>>(single_buffer_size, 0);
        ngtcp2_path_storage ps;
        ngtcp2_path_storage_zero(&ps);
        auto nwrite = ngtcp2_conn_write_connection_close(
                          quic.conn, &ps.path, nullptr, buffer_to_send->data(),
                          buffer_to_send->capacity(), &quic.last_error,
                          std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());

        if (nwrite)
        {
            buffer_to_send->resize(nwrite);
            boost::asio::ip::udp::endpoint remote_addr;
            remote_addr.resize(ps.path.remote.addrlen);
            memcpy(remote_addr.data(), &ps.path.remote.addr, ps.path.remote.addrlen);
            auto old_udp_client_socket = std::make_shared<boost::asio::ip::udp::socket>(std::move(udp_client_socket));

            old_udp_client_socket->async_send_to(boost::asio::buffer(*buffer_to_send), remote_addr,
                [buffer_to_send, old_udp_client_socket](const boost::system::error_code & e, std::size_t bytes_transferred)
            {
            });

            return;
        }
    }
}

#endif

}
