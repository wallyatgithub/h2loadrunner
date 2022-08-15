#include <openssl/err.h>
#include <openssl/ssl.h>
#include <regex>
#include <algorithm>
#include <cctype>
#include <execinfo.h>
#include <iomanip>
#include <string>
#include <boost/asio/io_service.hpp>
#include <boost/thread/thread.hpp>

#ifdef USE_LIBEV
extern "C" {
#include <ares.h>
}
#include "memchunk.h"
#endif

#include "h2load.h"
#include "libev_client.h"
#include "h2load_Config.h"
#include "libev_worker.h"
#include "h2load_Cookie.h"


#include "h2load_utils.h"
#include "tls.h"

namespace h2load
{


libev_client::libev_client(uint32_t id, libev_worker* wrker, size_t req_todo, Config* conf,
               libev_client* parent, const std::string& dest_schema,
               const std::string& dest_authority)
    : base_client(id, wrker, req_todo, conf, parent, dest_schema, dest_authority),
      wb(&static_cast<libev_worker*>(worker)->mcpool),
      next_addr(conf->addrs),
      current_addr(nullptr),
      ares_address(nullptr),
      fd(-1),
      probe_skt_fd(-1),
      connectfn(&libev_client::connect)
{

    ev_io_init(&wev, writecb, 0, EV_WRITE);
    ev_io_init(&rev, readcb, 0, EV_READ);

    wev.data = this;
    rev.data = this;

    ev_io_init(&probe_wev, probe_writecb, 0, EV_WRITE);

    probe_wev.data = this;

    init_timer_watchers();

    init_ares();
    // TODO: move this to base class, but this calls a virtual func
    init_connection_targert();
#ifdef ENABLE_HTTP3
    setup_quic_pkt_timer();
#endif // ENABLE_HTTP3

}

void libev_client::init_ares()
{
    struct ares_options options;
    options.sock_state_cb = ares_socket_state_cb;
    options.sock_state_cb_data = this;
    auto optmask = ARES_OPT_SOCK_STATE_CB;
    auto status = ares_init_options(&channel, &options, optmask);
    if (status)
    {
        std::cerr << "c-ares ares_init_options failed: " << status << std::endl;
        exit(EXIT_FAILURE);
    }
}

void libev_client::init_timer_watchers()
{
    ev_timer_init(&conn_inactivity_watcher, conn_activity_timeout_cb, 0.,
                  config->conn_inactivity_timeout);
    conn_inactivity_watcher.data = this;

    ev_timer_init(&conn_active_watcher, conn_activity_timeout_cb,
                  config->conn_active_timeout, 0.);
    conn_active_watcher.data = this;

    ev_timer_init(&request_timeout_watcher, client_request_timeout_cb, 0., 0.);
    request_timeout_watcher.data = this;

    ev_timer_init(&rps_watcher, rps_cb, 0., 0.);
    rps_watcher.data = this;

    ev_timer_init(&stream_timeout_watcher, stream_timeout_cb, 0., 0.01);
    stream_timeout_watcher.data = this;

    ev_timer_init(&connection_timeout_watcher, client_connection_timeout_cb, 2., 0.);
    connection_timeout_watcher.data = this;

    ev_timer_init(&delayed_request_watcher, delayed_request_cb, 0.01, 0.01);
    delayed_request_watcher.data = this;

    ev_timer_init(&send_ping_watcher, ping_w_cb, 0., config->json_config_schema.interval_to_send_ping);
    send_ping_watcher.data = this;

    ev_timer_init(&delayed_reconnect_watcher, reconnect_to_used_host_cb, 1.0, 0.0);
    delayed_reconnect_watcher.data = this;

    ev_timer_init(&connect_to_preferred_host_watcher, connect_to_prefered_host_cb, 1.0, 1.0);
    connect_to_preferred_host_watcher.data = this;

}

libev_client::~libev_client()
{
    disconnect();

    if (ssl)
    {
        SSL_free(ssl);
    }

    final_cleanup();

    ares_freeaddrinfo(ares_address);
}

int libev_client::do_read()
{
    return readfn(*this);
}
int libev_client::do_write()
{
    return writefn(*this);
}

template<class T>
int libev_client::make_socket(T* addr)
{
    fd = util::create_nonblock_socket(addr->ai_family);
    if (fd == -1)
    {
        return -1;
    }
    if (schema.empty())
    {
        schema = config->scheme;
    }
    if (schema == "https")
    {
        if (!ssl)
        {
            ssl = SSL_new(static_cast<libev_worker*>(worker)->ssl_ctx);
        }

        std::string host = tokenize_string(authority, ":")[0];
        if (host.empty())
        {
            host = config->host;
        }

        if (!util::numeric_host(host.c_str()))
        {
            SSL_set_tlsext_host_name(ssl, host.c_str());
        }

        SSL_set_fd(ssl, fd);
        SSL_set_connect_state(ssl);
    }

    auto rv = ::connect(fd, addr->ai_addr, addr->ai_addrlen);
    if (rv != 0 && errno != EINPROGRESS)
    {
        if (ssl)
        {
            SSL_free(ssl);
            ssl = nullptr;
        }
        close(fd);
        fd = -1;
        return -1;
    }
    return 0;
}

void libev_client::clear_default_addr_info()
{
    ares_address = nullptr;
    next_addr = nullptr;
    current_addr = nullptr;
}

void libev_client::start_conn_inactivity_watcher()
{
    ev_timer_again(static_cast<libev_worker*>(worker)->loop, &conn_inactivity_watcher);
}
void libev_client::stop_conn_inactivity_timer()
{
    ev_timer_stop(static_cast<libev_worker*>(worker)->loop, &conn_inactivity_watcher);
}

int libev_client::make_async_connection()
{
    int rv;
    if (current_addr)
    {
        rv = make_socket(current_addr);
        if (rv == -1)
        {
            return -1;
        }
    }
    else if (next_addr)
    {
        addrinfo* addr = nullptr;
        while (next_addr)
        {
            addr = next_addr;
            next_addr = next_addr->ai_next;
            rv = make_socket(addr);
            if (rv == 0)
            {
                break;
            }
        }

        if (fd == -1)
        {
            return -1;
        }

        assert(addr);

        current_addr = addr;
    }
    else if (ares_address)
    {
        rv = make_socket(ares_address->nodes);
    }
    /*
    else
    {
        return resolve_fqdn_and_connect(schema, authority);
    }
    */
    if (fd == -1)
    {
        return -1;
    }

    writefn = &libev_client::connected;
    state = CLIENT_CONNECTING;

    ev_io_set(&rev, fd, EV_READ);
    ev_io_set(&wev, fd, EV_WRITE);

    ev_io_start(static_cast<libev_worker*>(worker)->loop, &wev);
    return 0;
}

void libev_client::probe_and_connect_to(const std::string& schema, const std::string& authority)
{
    resolve_fqdn_and_connect(schema, authority, ares_addrinfo_query_callback_for_probe);

}

void libev_client::restart_timeout_timer()
{
    if (config->conn_inactivity_timeout > 0.)
    {
        ev_timer_again(static_cast<libev_worker*>(worker)->loop, &conn_inactivity_watcher);
    }
    if (config->json_config_schema.interval_to_send_ping > 0.)
    {
        ev_timer_again(static_cast<libev_worker*>(worker)->loop, &send_ping_watcher);
    }
}

void libev_client::setup_graceful_shutdown()
{
    auto write_clear_callback = [this]()
    {
        disconnect();
    };
    writefn = &libev_client::write_clear_with_callback;
}

void libev_client::disconnect()
{
    cleanup_due_to_disconnect();

    auto stop_timer_watcher = [this](ev_timer & watcher)
    {
        if (ev_is_active(&watcher))
        {
            ev_timer_stop(static_cast<libev_worker*>(worker)->loop, &watcher);
        }
    };

    auto stop_io_watcher = [this](ev_io & watcher)
    {
        if (ev_is_active(&watcher))
        {
            ev_io_stop(static_cast<libev_worker*>(worker)->loop, &watcher);
        }
    };

    stop_timer_watcher(conn_inactivity_watcher);
    stop_timer_watcher(conn_active_watcher);
    stop_timer_watcher(rps_watcher);
    stop_timer_watcher(request_timeout_watcher);
    stop_timer_watcher(stream_timeout_watcher);
    stop_timer_watcher(connection_timeout_watcher);
    stop_timer_watcher(delayed_request_watcher);
    stop_timer_watcher(send_ping_watcher);
    stop_timer_watcher(delayed_reconnect_watcher);
    stop_timer_watcher(connect_to_preferred_host_watcher);

    wb.reset();
    stop_io_watcher(wev);
    stop_io_watcher(rev);
    for (auto& it : ares_io_watchers)
    {
        stop_io_watcher(it.second);
    }
    if (probe_skt_fd != -1)
    {
        if (ev_is_active(&probe_wev))
        {
            ev_io_stop(static_cast<libev_worker*>(worker)->loop, &probe_wev);
        }
        close(probe_skt_fd);
        probe_skt_fd = -1;
    }
    if (ssl)
    {
        SSL_set_shutdown(ssl, SSL_get_shutdown(ssl) | SSL_RECEIVED_SHUTDOWN);
        ERR_clear_error();

        if (SSL_shutdown(ssl) != 1)
        {
            SSL_free(ssl);
            ssl = nullptr;
        }
    }
    if (fd != -1)
    {
        shutdown(fd, SHUT_WR);
        close(fd);
        fd = -1;
    }

    final = false;
    /* prepare for possible re-connect */
    init_timer_watchers();
    if (write_clear_callback)
    {
        auto func = std::move(write_clear_callback);
    }
}

void libev_client::start_conn_active_watcher()
{
    ev_timer_start(static_cast<libev_worker*>(worker)->loop, &conn_active_watcher);
}

void libev_client::graceful_restart_connection()
{
    write_clear_callback = [this]()
    {
        disconnect();
        resolve_fqdn_and_connect(schema, authority);
    };
    writefn = &libev_client::write_clear_with_callback;
    terminate_session();
}

void libev_client::start_rps_timer()
{
    rps_watcher.repeat = std::max(0.1, 1. / rps);
    ev_timer_again(static_cast<libev_worker*>(worker)->loop, &rps_watcher);
}

void libev_client::stop_rps_timer()
{
    ev_timer_stop(static_cast<libev_worker*>(worker)->loop, &rps_watcher);
}

void libev_client::start_stream_timeout_timer()
{
    stream_timeout_watcher.repeat = 0.01;
    ev_timer_again(static_cast<libev_worker*>(worker)->loop, &stream_timeout_watcher);
}

void libev_client::start_warmup_timer()
{
    worker->start_warmup_timer();
}
void libev_client::stop_warmup_timer()
{
    ev_timer_stop(static_cast<libev_worker*>(worker)->loop, &static_cast<libev_worker*>(worker)->warmup_watcher);
}

void libev_client::start_timing_script_request_timeout_timer(double duration)
{
    request_timeout_watcher.repeat = duration;
    ev_timer_again(static_cast<libev_worker*>(worker)->loop, &request_timeout_watcher);
}

void libev_client::start_connect_to_preferred_host_timer()
{
    ev_timer_start(static_cast<libev_worker*>(worker)->loop, &connect_to_preferred_host_watcher);
}

void libev_client::stop_timing_script_request_timeout_timer()
{
    ev_timer_stop(static_cast<libev_worker*>(worker)->loop, &request_timeout_watcher);
}

void libev_client::conn_activity_timeout_handler()
{
    ev_timer_stop(static_cast<libev_worker*>(worker)->loop, &conn_inactivity_watcher);
    ev_timer_stop(static_cast<libev_worker*>(worker)->loop, &conn_active_watcher);

    if (util::check_socket_connected(fd))
    {
        timeout();
    }
}

int libev_client::on_read(const uint8_t* data, size_t len)
{
    auto rv = session->on_read(data, len);
    if (rv != 0)
    {
        return -1;
    }
    if (worker->current_phase == Phase::MAIN_DURATION)
    {
        worker->stats.bytes_total += len;
    }
    signal_write();
    return 0;
}

int libev_client::on_write()
{
    if (wb.rleft() >= BACKOFF_WRITE_BUFFER_THRES)
    {
        return 0;
    }

    if (session->on_write() != 0)
    {
        return -1;
    }
    return 0;
}

int libev_client::read_clear()
{
    uint8_t buf[8_k];

    for (;;)
    {
        ssize_t nread;
        while ((nread = read(fd, buf, sizeof(buf))) == -1 && errno == EINTR)
            ;
        if (nread == -1)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                return 0;
            }
            return -1;
        }

        if (nread == 0)
        {
            return -1;
        }

        if (on_read(buf, nread) != 0)
        {
            return -1;
        }
    }

    return 0;
}

int libev_client::write_clear_with_callback()
{
    writefn = &libev_client::write_clear;
    auto func = std::move(write_clear_callback);
    auto retCode = do_write();
    if (retCode == 0 && func)
    {
        func();
    }
    return retCode;
}

int libev_client::write_clear()
{
    std::array<struct iovec, 2> iov;

    for (;;)
    {
        if (on_write() != 0)
        {
            return -1;
        }

        auto iovcnt = wb.riovec(iov.data(), iov.size());

        if (iovcnt == 0)
        {
            break;
        }

        ssize_t nwrite;
        while ((nwrite = writev(fd, iov.data(), iovcnt)) == -1 && errno == EINTR)
            ;

        if (nwrite == -1)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                ev_io_start(static_cast<libev_worker*>(worker)->loop, &wev);
                return 0;
            }
            return -1;
        }

        wb.drain(nwrite);
    }

    ev_io_stop(static_cast<libev_worker*>(worker)->loop, &wev);

    return 0;
}

void libev_client::start_request_delay_execution_timer()
{
    ev_timer_start(static_cast<libev_worker*>(worker)->loop, &delayed_request_watcher);
}
int libev_client::connected()
{
    if (!util::check_socket_connected(fd))
    {
        std::cerr << "check_socket_connected failed" << std::endl;
        call_connected_callbacks(false);
        return ERR_CONNECT_FAIL;
    }

    ev_io_start(static_cast<libev_worker*>(worker)->loop, &rev);
    ev_io_stop(static_cast<libev_worker*>(worker)->loop, &wev);

    if (ssl)
    {
        readfn = &libev_client::tls_handshake;
        writefn = &libev_client::tls_handshake;

        return do_write();
    }

    readfn = &libev_client::read_clear;
    writefn = &libev_client::write_clear;

    if (connection_made() != 0)
    {
        call_connected_callbacks(false);
        return -1;
    }
    call_connected_callbacks(true);
    return 0;
}

int libev_client::tls_handshake()
{
    ERR_clear_error();

    auto rv = SSL_do_handshake(ssl);

    if (rv <= 0)
    {
        auto err = SSL_get_error(ssl, rv);
        switch (err)
        {
            case SSL_ERROR_WANT_READ:
                ev_io_stop(static_cast<libev_worker*>(worker)->loop, &wev);
                return 0;
            case SSL_ERROR_WANT_WRITE:
                ev_io_start(static_cast<libev_worker*>(worker)->loop, &wev);
                return 0;
            default:
                std::cerr << get_tls_error_string() << std::endl;
                return -1;
        }
    }

    ev_io_stop(static_cast<libev_worker*>(worker)->loop, &wev);

    readfn = &libev_client::read_tls;
    writefn = &libev_client::write_tls;

    if (connection_made() != 0)
    {
        return -1;
    }

    return 0;
}

int libev_client::read_tls()
{
    uint8_t buf[8_k];

    ERR_clear_error();

    for (;;)
    {
        auto rv = SSL_read(ssl, buf, sizeof(buf));

        if (rv <= 0)
        {
            auto err = SSL_get_error(ssl, rv);
            switch (err)
            {
                case SSL_ERROR_WANT_READ:
                    return 0;
                case SSL_ERROR_WANT_WRITE:
                    // renegotiation started
                    return -1;
                default:
                    return -1;
            }
        }

        if (on_read(buf, rv) != 0)
        {
            return -1;
        }
    }
}

int libev_client::write_tls()
{
    ERR_clear_error();

    struct iovec iov;

    for (;;)
    {
        if (on_write() != 0)
        {
            return -1;
        }

        auto iovcnt = wb.riovec(&iov, 1);

        if (iovcnt == 0)
        {
            break;
        }

        auto rv = SSL_write(ssl, iov.iov_base, iov.iov_len);

        if (rv <= 0)
        {
            auto err = SSL_get_error(ssl, rv);
            switch (err)
            {
                case SSL_ERROR_WANT_READ:
                    // renegotiation started
                    return -1;
                case SSL_ERROR_WANT_WRITE:
                    ev_io_start(static_cast<libev_worker*>(worker)->loop, &wev);
                    return 0;
                default:
                    return -1;
            }
        }

        wb.drain(rv);
    }

    ev_io_stop(static_cast<libev_worker*>(worker)->loop, &wev);

    return 0;
}

void libev_client::signal_write()
{
    ev_io_start(static_cast<libev_worker*>(worker)->loop, &wev);
}

std::shared_ptr<base_client> libev_client::create_dest_client(const std::string& dst_sch,
                                                             const std::string& dest_authority)
{
    auto new_client = std::make_shared<libev_client>(this->id, static_cast<libev_worker*>(worker), this->req_todo, this->config,
                                               this, dst_sch, dest_authority);
    return new_client;
}

int libev_client::resolve_fqdn_and_connect(const std::string& schema, const std::string& authority,
                                     ares_addrinfo_callback callback)
{
    std::string port;
    std::string host;
    if (!get_host_and_port_from_authority(schema, authority, host, port))
    {
        return 1;
    }

    ares_addrinfo_hints hints;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = 0;
    hints.ai_flags = AI_ADDRCONFIG;
    ares_getaddrinfo(channel, host.c_str(), port.c_str(), &hints, callback, this);
    return 0;
}

int libev_client::connect_to_host(const std::string& schema, const std::string& authority)
{
    //if (config->verbose)
    {
        std::cerr << "===============connecting to " << schema << "://" << authority << "===============" << std::endl;
    }
    return resolve_fqdn_and_connect(schema, authority);
}

void libev_client::start_delayed_reconnect_timer()
{
    ev_timer_start(static_cast<libev_worker*>(worker)->loop, &delayed_reconnect_watcher);
}

bool libev_client::probe_address(ares_addrinfo* ares_address)
{
    if (probe_skt_fd != -1)
    {
        if (ev_is_active(&probe_wev))
        {
            ev_io_stop(static_cast<libev_worker*>(worker)->loop, &probe_wev);
        }
        close(probe_skt_fd);
        probe_skt_fd = -1;
    }
    if (ares_address)
    {
        auto& addr = ares_address->nodes;
        probe_skt_fd = util::create_nonblock_socket(addr->ai_family);
        if (probe_skt_fd != -1)
        {
            auto rv = ::connect(probe_skt_fd, addr->ai_addr, addr->ai_addrlen);
            if (rv != 0 && errno != EINPROGRESS)
            {
                close(probe_skt_fd);
                probe_skt_fd = -1;
            }
            else
            {
                ev_io_set(&probe_wev, probe_skt_fd, EV_WRITE);
                ev_io_start(static_cast<libev_worker*>(worker)->loop, &probe_wev);
                return true;
            }
        }
    }
    return false;
}

int libev_client::do_connect()
{
    return connectfn(*this);
}

int libev_client::connect_with_async_fqdn_lookup()
{
    restore_connectfn(); // one time deal
    return connect_to_host(schema, authority);
}

void libev_client::setup_connect_with_async_fqdn_lookup()
{
    connectfn = &libev_client::connect_with_async_fqdn_lookup;
}

void libev_client::feed_timing_script_request_timeout_timer()
{
    if (!ev_is_active(&request_timeout_watcher))
    {
        ev_feed_event(static_cast<libev_worker*>(worker)->loop, &request_timeout_watcher, EV_TIMER);
    }
}

void libev_client::start_connect_timeout_timer()
{
    ev_timer_start(static_cast<libev_worker*>(worker)->loop, &connection_timeout_watcher);
}

void libev_client::stop_connect_timeout_timer()
{
    ev_timer_stop(static_cast<libev_worker*>(worker)->loop, &connection_timeout_watcher);
}

void libev_client::restore_connectfn()
{
    connectfn = &libev_client::connect;
}

size_t libev_client::push_data_to_output_buffer(const uint8_t* data, size_t length)
{
    if (wb.rleft() >= BACKOFF_WRITE_BUFFER_THRES)
    {
        return NGHTTP2_ERR_WOULDBLOCK;
    }
    return wb.append(data, length);
}

bool libev_client::any_pending_data_to_write()
{
    return (wb.rleft() > 0);
}

#ifdef ENABLE_HTTP3
void libev_client::setup_quic_pkt_timer()
{
    ev_timer_init(&quic.pkt_timer, quic_pkt_timeout_cb, 0., 0.);
    quic.pkt_timer.data = this;
}


}
