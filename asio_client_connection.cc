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
      dns_resolver(io_ctx),
      client_socket(io_ctx),
      client_probe_socket(io_ctx),
      input_buffer(16 * 1024, 0),
      output_buffers(2, std::vector<uint8_t>(64 * 1024, 0)),
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
      ssl_socket(std::make_unique<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>(io_ctx, ssl_context)),
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

    if (schema == "https")
    {
        do_read_fn = &asio_client_connection::do_ssl_read;
        do_write_fn = &asio_client_connection::do_ssl_write;
        ssl = ssl_socket->native_handle();
    }
    else
    {
        do_read_fn = &asio_client_connection::do_tcp_read;
        do_write_fn = &asio_client_connection::do_tcp_write;
    }

    boost::asio::ip::tcp::resolver::query query(host, port);
    dns_resolver.async_resolve(query,
                               [this](const boost::system::error_code & err, boost::asio::ip::tcp::resolver::iterator endpoint_iterator)
    {
        on_resolve_result_event(err, endpoint_iterator);
    });
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
    dns_resolver.async_resolve(query,
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
    client_probe_socket.lowest_layer().close(ignored_ec);
    if (!err)
    {
        on_prefered_host_up();
    }
}

void asio_client_connection::start_async_handshake()
{
    ssl_socket->async_handshake(
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
    if (session->on_read(input_buffer.data(), bytes_transferred) != 0)
    {
        return handle_connection_error();
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
    common_read(client_socket);
}

void asio_client_connection::do_ssl_read()
{
    common_read(*ssl_socket);
}

void asio_client_connection::do_read()
{
    do_read_fn(*this);
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
    common_write(client_socket);
}

void asio_client_connection::do_ssl_write()
{
    common_write(*ssl_socket);
}

void asio_client_connection::do_write()
{
    do_write_fn(*this);
}

void asio_client_connection::stop()
{
    if (is_client_stopped)
    {
        return;
    }
    is_client_stopped = true;
    boost::system::error_code ignored_ec;
    client_socket.lowest_layer().close(ignored_ec);
    ssl_socket.reset(nullptr);
    ssl_socket = std::make_unique<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>(io_context, ssl_ctx);
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
}

template <typename SOCKET>
void asio_client_connection::start_async_connect(boost::asio::ip::tcp::resolver::iterator endpoint_iterator,
                                                 SOCKET& socket)
{
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
            start_async_connect(endpoint_iterator, client_socket);
        }
        else
        {
            start_async_connect(endpoint_iterator, *ssl_socket);
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
        client_probe_socket.lowest_layer().async_connect(endpoint,
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



}
