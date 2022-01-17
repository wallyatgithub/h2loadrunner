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

#include <ares.h>

#include "h2load.h"
#include "h2load_Client.h"
#include "h2load_Config.h"
#include "h2load_Worker.h"
#include "h2load_Cookie.h"


#include "h2load_utils.h"
#include "tls.h"

namespace h2load
{


Client::Client(uint32_t id, Worker* worker, size_t req_todo, Config* conf,
               Client* parent, const std::string& dest_schema,
               const std::string& dest_authority)
    : Client_Interface(id, worker, req_todo, conf, parent, dest_schema, dest_authority),
      wb(&worker->mcpool),
      ssl(nullptr),
      next_addr(conf->addrs),
      current_addr(nullptr),
      ares_address(nullptr),
      fd(-1),
      probe_skt_fd(-1),
      connectfn(&Client::connect)
{
    slice_user_id();

    if (req_todo == 0)   // this means infinite number of requests are to be made
    {
        // This ensures that number of requests are unbounded
        // Just a positive number is fine, we chose the first positive number
        req_left = 1;
    }
    ev_io_init(&wev, writecb, 0, EV_WRITE);
    ev_io_init(&rev, readcb, 0, EV_READ);

    wev.data = this;
    rev.data = this;

    ev_io_init(&probe_wev, probe_writecb, 0, EV_WRITE);

    probe_wev.data = this;

    init_timer_watchers();

    init_lua_states();

    init_ares();

    init_connection_targert();

    update_this_in_dest_client_map();
}

void Client::init_ares()
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

void Client::init_timer_watchers()
{
    ev_timer_init(&conn_inactivity_watcher, conn_activity_timeout_cb, 0.,
                  worker->config->conn_inactivity_timeout);
    conn_inactivity_watcher.data = this;

    ev_timer_init(&conn_active_watcher, conn_activity_timeout_cb,
                  worker->config->conn_active_timeout, 0.);
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

Client::~Client()
{
    disconnect();

    if (ssl)
    {
        SSL_free(ssl);
    }

    worker->sample_client_stat(&cstat);
    ++worker->client_smp.n;
    for (auto& V : lua_states)
    {
        for (auto& L : V)
        {
            lua_close(L);
        }
    }
    lua_states.clear();

    std::string dest = schema;
    dest.append("://").append(authority);
    if (parent_client && parent_client->dest_clients.count(dest) && parent_client->dest_clients[dest] == this)
    {
        parent_client->dest_clients.erase(dest);
    }

    ares_freeaddrinfo(ares_address);
}

int Client::do_read()
{
    return readfn(*this);
}
int Client::do_write()
{
    return writefn(*this);
}

template<class T>
int Client::make_socket(T* addr)
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
            ssl = SSL_new(worker->ssl_ctx);
        }

        auto config = worker->config;
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

void Client::clear_default_addr_info()
{
    ares_address = nullptr;
    next_addr = nullptr;
    current_addr = nullptr;
}

int Client::connect()
{
    if (is_test_finished())
    {
        return -1;
    }
    int rv;

    if (!worker->config->is_timing_based_mode() ||
        worker->current_phase == Phase::MAIN_DURATION)
    {
        record_client_start_time();
        clear_connect_times();
        record_connect_start_time();
    }
    else if (worker->current_phase == Phase::INITIAL_IDLE)
    {
        worker->current_phase = Phase::WARM_UP;
        std::cerr << "Warm-up started for thread #" << worker->id << "."
                  << std::endl;
        ev_timer_start(worker->loop, &worker->warmup_watcher);
    }

    if (worker->config->conn_inactivity_timeout > 0.)
    {
        ev_timer_again(worker->loop, &conn_inactivity_watcher);
    }

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

    writefn = &Client::connected;
    state = CLIENT_CONNECTING;
    ev_timer_start(worker->loop, &connection_timeout_watcher);

    ev_io_set(&rev, fd, EV_READ);
    ev_io_set(&wev, fd, EV_WRITE);

    ev_io_start(worker->loop, &wev);
    return 0;
}

void Client::restart_timeout_timer()
{
    if (worker->config->conn_inactivity_timeout > 0.)
    {
        ev_timer_again(worker->loop, &conn_inactivity_watcher);
    }
    if (config->json_config_schema.interval_to_send_ping > 0.)
    {
        ev_timer_again(worker->loop, &send_ping_watcher);
    }
}

void Client::disconnect()
{
    if (CLIENT_CONNECTED == state)
    {
        std::cerr << "===============disconnected from " << authority << "===============" << std::endl;
    }

    record_client_end_time();
    auto stop_timer_watcher = [this](ev_timer & watcher)
    {
        if (ev_is_active(&watcher))
        {
            ev_timer_stop(worker->loop, &watcher);
        }
    };

    auto stop_io_watcher = [this](ev_io & watcher)
    {
        if (ev_is_active(&watcher))
        {
            ev_io_stop(worker->loop, &watcher);
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

    streams.clear();
    session.reset();
    wb.reset();
    state = CLIENT_IDLE;
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
            ev_io_stop(worker->loop, &probe_wev);
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

void Client::start_conn_active_watcher(Client_Interface* client)
{
    ev_timer_start(worker->loop, &(dynamic_cast<Client*>(client)->conn_active_watcher));
}

void Client::graceful_restart_connection()
{
    write_clear_callback = [this]()
    {
        disconnect();
        resolve_fqdn_and_connect(schema, authority);
    };
    writefn = &Client::write_clear_with_callback;
    terminate_session();
}

void Client::report_tls_info()
{
    if (worker->id == 0 && !worker->tls_info_report_done)
    {
        worker->tls_info_report_done = true;
        auto cipher = SSL_get_current_cipher(ssl);
        std::cerr << "TLS Protocol: " << tls::get_tls_protocol(ssl) << "\n"
                  << "Cipher: " << SSL_CIPHER_get_name(cipher) << std::endl;
        print_server_tmp_key(ssl);
    }
}

void Client::start_rps_timer()
{
    rps_watcher.repeat = std::max(0.01, 1. / rps);
    ev_timer_again(worker->loop, &rps_watcher);
    rps_duration_started = std::chrono::steady_clock::now();
}

void Client::stop_rps_timer()
{
    ev_timer_stop(worker->loop, &rps_watcher);
}

void Client::start_stream_timeout_timer()
{
    stream_timeout_watcher.repeat = 0.01;
    ev_timer_again(worker->loop, &stream_timeout_watcher);
}

int Client::select_protocol_and_allocate_session()
{
    if (ssl)
    {
        report_tls_info();

        const unsigned char* next_proto = nullptr;
        unsigned int next_proto_len;

#ifndef OPENSSL_NO_NEXTPROTONEG
        SSL_get0_next_proto_negotiated(ssl, &next_proto, &next_proto_len);
#endif // !OPENSSL_NO_NEXTPROTONEG
#if OPENSSL_VERSION_NUMBER >= 0x10002000L
        if (next_proto == nullptr)
        {
            SSL_get0_alpn_selected(ssl, &next_proto, &next_proto_len);
        }
#endif // OPENSSL_VERSION_NUMBER >= 0x10002000L

        if (next_proto)
        {
            auto proto = StringRef {next_proto, next_proto_len};
            if (util::check_h2_is_selected(proto))
            {
                session = std::make_unique<Http2Session>(this);
            }
            else if (util::streq(NGHTTP2_H1_1, proto))
            {
                session = std::make_unique<Http1Session>(this);
            }

            // Just assign next_proto to selected_proto anyway to show the
            // negotiation result.
            selected_proto = proto.str();
        }
        else
        {
            std::cerr << "No protocol negotiated. Fallback behaviour may be activated"
                      << std::endl;

            for (const auto& proto : config->npn_list)
            {
                if (util::streq(NGHTTP2_H1_1_ALPN, StringRef {proto}))
                {
                    std::cerr
                            << "Server does not support NPN/ALPN. Falling back to HTTP/1.1."
                            << std::endl;
                    session = std::make_unique<Http1Session>(this);
                    selected_proto = NGHTTP2_H1_1.str();
                    break;
                }
            }
        }

        if (!selected_proto.empty())
        {
            print_app_info();
        }

        if (!session)
        {
            std::cerr
                    << "No supported protocol was negotiated. Supported protocols were:"
                    << std::endl;
            for (const auto& proto : config->npn_list)
            {
                std::cerr << proto.substr(1) << std::endl;
            }
            disconnect();
            return -1;
        }
    }
    else
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

void Client::start_timing_script_request_timeout_timer(double duration)
{
    request_timeout_watcher.repeat = duration;
    ev_timer_again(worker->loop, &request_timeout_watcher);
}

void Client::start_connect_to_preferred_host_timer()
{
    ev_timer_start(worker->loop, &connect_to_preferred_host_watcher);
}

int Client::on_read(const uint8_t* data, size_t len)
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

int Client::on_write()
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

int Client::read_clear()
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

int Client::write_clear_with_callback()
{
    writefn = &Client::write_clear;
    auto func = std::move(write_clear_callback);
    auto retCode = do_write();
    if (retCode == 0 && func)
    {
        func();
    }
    return retCode;
}

int Client::write_clear()
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
                ev_io_start(worker->loop, &wev);
                return 0;
            }
            return -1;
        }

        wb.drain(nwrite);
    }

    ev_io_stop(worker->loop, &wev);

    return 0;
}

void Client::start_request_delay_execution_timer()
{
    ev_timer_start(worker->loop, &delayed_request_watcher);
}
int Client::connected()
{
    if (!util::check_socket_connected(fd))
    {
        std::cerr << "check_socket_connected failed" << std::endl;
        return ERR_CONNECT_FAIL;
    }
    std::cerr << "===============connected to " << authority << "===============" << std::endl;

    ev_io_start(worker->loop, &rev);
    ev_io_stop(worker->loop, &wev);
    ev_timer_stop(worker->loop, &connection_timeout_watcher);

    if (ssl)
    {
        readfn = &Client::tls_handshake;
        writefn = &Client::tls_handshake;

        return do_write();
    }

    readfn = &Client::read_clear;
    writefn = &Client::write_clear;

    if (connection_made() != 0)
    {
        return -1;
    }

    return 0;
}

int Client::tls_handshake()
{
    ERR_clear_error();

    auto rv = SSL_do_handshake(ssl);

    if (rv <= 0)
    {
        auto err = SSL_get_error(ssl, rv);
        switch (err)
        {
            case SSL_ERROR_WANT_READ:
                ev_io_stop(worker->loop, &wev);
                return 0;
            case SSL_ERROR_WANT_WRITE:
                ev_io_start(worker->loop, &wev);
                return 0;
            default:
                std::cerr << get_tls_error_string() << std::endl;
                return -1;
        }
    }

    ev_io_stop(worker->loop, &wev);

    readfn = &Client::read_tls;
    writefn = &Client::write_tls;

    if (connection_made() != 0)
    {
        return -1;
    }

    return 0;
}

int Client::read_tls()
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

int Client::write_tls()
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
                    ev_io_start(worker->loop, &wev);
                    return 0;
                default:
                    return -1;
            }
        }

        wb.drain(rv);
    }

    ev_io_stop(worker->loop, &wev);

    return 0;
}

void Client::signal_write()
{
    ev_io_start(worker->loop, &wev);
}

void Client::try_new_connection()
{
    new_connection_requested = true;
}

std::unique_ptr<Client_Interface> Client::create_dest_client(const std::string& dst_sch,
                                                             const std::string& dest_authority)
{
    auto new_client = std::make_unique<Client>(this->id, this->worker, this->req_todo, this->config,
                                               this, dst_sch, dst_sch);
    return new_client;
}

int Client::resolve_fqdn_and_connect(const std::string& schema, const std::string& authority,
                                     ares_addrinfo_callback callback)
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
    ares_addrinfo_hints hints;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = 0;
    hints.ai_flags = AI_ADDRCONFIG;
    ares_getaddrinfo(channel, vec[0].c_str(), port.c_str(), &hints, callback, this);
    return 0;
}

int Client::connect_to_host(const std::string& schema, const std::string& authority)
{
    //if (config->verbose)
    {
        std::cerr << "===============connecting to " << schema << "://" << authority << "===============" << std::endl;
    }
    return resolve_fqdn_and_connect(schema, authority);
}

bool Client::reconnect_to_alt_addr()
{
    if (CLIENT_CONNECTED == state)
    {
        return false;
    }
    if (should_reconnect_on_disconnect())
    {
        if (authority != preferred_authority)
        {
            used_addresses.push_back(std::move(authority));
            authority = preferred_authority;
            std::cerr << "try with preferred host: " << authority << std::endl;
            resolve_fqdn_and_connect(schema, authority);
        }
        else if (candidate_addresses.size())
        {
            authority = std::move(candidate_addresses.front());
            candidate_addresses.pop_front();
            std::cerr << "switching to candidate host: " << authority << std::endl;
            resolve_fqdn_and_connect(schema, authority);
        }
        else
        {
            ev_timer_start(worker->loop, &delayed_reconnect_watcher);
        }
        return true;
    }
    return false;
}

bool Client::probe_address(ares_addrinfo* ares_address)
{
    if (probe_skt_fd != -1)
    {
        if (ev_is_active(&probe_wev))
        {
            ev_io_stop(worker->loop, &probe_wev);
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
                ev_io_start(worker->loop, &probe_wev);
                return true;
            }
        }
    }
    return false;
}

int Client::do_connect()
{
    return connectfn(*this);
}

int Client::connect_with_async_fqdn_lookup()
{
    restore_connectfn(); // one time deal
    return connect_to_host(schema, authority);
}

void Client::setup_connect_with_async_fqdn_lookup()
{
    connectfn = &Client::connect_with_async_fqdn_lookup;
}

void Client::feed_timing_script_request_timeout_timer()
{
    if (!ev_is_active(&request_timeout_watcher))
    {
        ev_feed_event(worker->loop, &request_timeout_watcher, EV_TIMER);
    }
}

void Client::restore_connectfn()
{
    connectfn = &Client::connect;
}

size_t Client::send_out_data(const uint8_t* data, size_t length)
{
    if (wb.rleft() >= BACKOFF_WRITE_BUFFER_THRES)
    {
        return NGHTTP2_ERR_WOULDBLOCK;
    }
    return wb.append(data, length);
}

bool Client::any_pending_data_to_write()
{
    return (wb.rleft() > 0);
}

}
