#include <openssl/err.h>
#include <openssl/ssl.h>
#include <regex>
#include <algorithm>
#include <cctype>
#include <string>

#include "h2load.h"
#include "h2load_utils.h"
#include "tls.h"

namespace h2load
{



Client::Client(uint32_t id, Worker* worker, size_t req_todo, Config* conf)
    : wb(&worker->mcpool),
      cstat {},
        worker(worker),
        config(conf),
        ssl(nullptr),
        next_addr(conf->addrs),
        current_addr(nullptr),
        reqidx(0),
        state(CLIENT_IDLE),
        req_todo(req_todo),
        req_left(req_todo),
        req_inflight(0),
        req_started(0),
        req_done(0),
        id(id),
        fd(-1),
        new_connection_requested(false),
        final(false),
        rps_duration_started(0),
        rps_req_pending(0),
        rps_req_inflight(0),
        curr_stream_id(0),
        curr_req_variable_value(0)
{
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

    ev_timer_init(&conn_inactivity_watcher, conn_timeout_cb, 0.,
                  worker->config->conn_inactivity_timeout);
    conn_inactivity_watcher.data = this;

    ev_timer_init(&conn_active_watcher, conn_timeout_cb,
                  worker->config->conn_active_timeout, 0.);
    conn_active_watcher.data = this;

    ev_timer_init(&request_timeout_watcher, client_request_timeout_cb, 0., 0.);
    request_timeout_watcher.data = this;

    ev_timer_init(&rps_watcher, rps_cb, 0., 0.);
    rps_watcher.data = this;

    ev_timer_init(&stream_timeout_watcher, stream_timeout_cb, 0., 0.);
    stream_timeout_watcher.data = this;

    for (auto& scenario : conf->json_config_schema.scenarios)
    {
        lua_State* L = luaL_newstate();
        luaL_openlibs(L);
        if (scenario.luaScript.size())
        {
            luaL_dostring(L, scenario.luaScript.c_str());
        }
        lua_states.push_back(L);
    }

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
    for (auto& L : lua_states)
    {
        lua_close(L);
    }
    lua_states.clear();
}

int Client::do_read()
{
    return readfn(*this);
}
int Client::do_write()
{
    return writefn(*this);
}

int Client::make_socket(addrinfo* addr)
{
    fd = util::create_nonblock_socket(addr->ai_family);
    if (fd == -1)
    {
        return -1;
    }
    if (config->scheme == "https")
    {
        if (!ssl)
        {
            ssl = SSL_new(worker->ssl_ctx);
        }

        auto config = worker->config;

        if (!util::numeric_host(config->host.c_str()))
        {
            SSL_set_tlsext_host_name(ssl, config->host.c_str());
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

int Client::connect()
{
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
        std::cout << "Warm-up started for thread #" << worker->id << "."
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
    else
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

    writefn = &Client::connected;

    ev_io_set(&rev, fd, EV_READ);
    ev_io_set(&wev, fd, EV_WRITE);

    ev_io_start(worker->loop, &wev);

    return 0;
}

void Client::timeout()
{
    process_timedout_streams();

    disconnect();
}

void Client::restart_timeout()
{
    if (worker->config->conn_inactivity_timeout > 0.)
    {
        ev_timer_again(worker->loop, &conn_inactivity_watcher);
    }
}

int Client::try_again_or_fail()
{
    disconnect();

    if (new_connection_requested)
    {
        new_connection_requested = false;

        if (req_left)
        {

            if (worker->current_phase == Phase::MAIN_DURATION)
            {
                // At the moment, we don't have a facility to re-start request
                // already in in-flight.  Make them fail.
                worker->stats.req_failed += req_inflight;
                worker->stats.req_error += req_inflight;

                req_inflight = 0;
            }

            // Keep using current address
            if (connect() == 0)
            {
                return 0;
            }
            std::cerr << "client could not connect to host" << std::endl;
        }
    }

    process_abandoned_streams();

    return -1;
}

void Client::fail()
{
    disconnect();

    process_abandoned_streams();
}

void Client::disconnect()
{
    record_client_end_time();

    ev_timer_stop(worker->loop, &conn_inactivity_watcher);
    ev_timer_stop(worker->loop, &conn_active_watcher);
    ev_timer_stop(worker->loop, &rps_watcher);
    ev_timer_stop(worker->loop, &request_timeout_watcher);
    ev_timer_stop(worker->loop, &stream_timeout_watcher);
    streams.clear();
    session.reset();
    wb.reset();
    state = CLIENT_IDLE;
    ev_io_stop(worker->loop, &wev);
    ev_io_stop(worker->loop, &rev);
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
}

int Client::submit_request()
{
    auto retCode = session->submit_request();
    if (retCode != 0)
    {
        return retCode;
    }

    if (worker->current_phase != Phase::MAIN_DURATION)
    {
        return 0;
    }

    ++worker->stats.req_started;
    ++req_started;
    ++req_inflight;
    if (!worker->config->is_timing_based_mode())
    {
        --req_left;
    }
    // if an active timeout is set and this is the last request to be submitted
    // on this connection, start the active timeout.
    if (worker->config->conn_active_timeout > 0. && req_left == 0)
    {
        ev_timer_start(worker->loop, &conn_active_watcher);
    }

    return 0;
}

void Client::process_timedout_streams()
{
    if (worker->current_phase != Phase::MAIN_DURATION)
    {
        return;
    }

    for (auto& p : streams)
    {
        auto& req_stat = p.second.req_stat;
        if (!req_stat.completed)
        {
            req_stat.stream_close_time = std::chrono::steady_clock::now();
        }
    }

    worker->stats.req_timedout += req_inflight;

    process_abandoned_streams();
}

void Client::process_abandoned_streams()
{
    if (worker->current_phase != Phase::MAIN_DURATION)
    {
        return;
    }

    auto req_abandoned = req_inflight + req_left;

    worker->stats.req_failed += req_abandoned;
    worker->stats.req_error += req_abandoned;

    req_inflight = 0;
    req_left = 0;
}

void restart_client_w_cb(struct ev_loop* loop, ev_timer* w, int revents)
{
    auto client = static_cast<Client*>(w->data);
    ev_timer_stop(client->worker->loop, &client->retart_client_watcher);
    std::cout << "Restart client:" << std::endl;
    client->terminate_session();
    client->disconnect();
    auto new_client = std::make_unique<Client>(client->id, client->worker, client->req_todo, client->config);
    if (new_client->connect() != 0)
    {
        std::cerr << "client could not connect to host" << std::endl;
        new_client->fail();
    }
    else
    {
        for (auto& cl : client->worker->clients)
        {
            if (cl == client)
            {
                auto index = &cl - &client->worker->clients[0];
                client->req_todo = client->req_done;
                client->worker->stats.req_todo += client->req_todo;
                client->worker->clients[index] = new_client.get();
                new_client->ancestor.reset(client);
                break;
            }
        }
        new_client.release();
    }
}

void Client::process_request_failure(int errCode)
{
    if (worker->current_phase != Phase::MAIN_DURATION)
    {
        return;
    }

    worker->stats.req_failed += req_left;
    worker->stats.req_error += req_left;

    req_left = 0;

    if (streams.size() == 0)
    {
        if (MAX_STREAM_TO_BE_EXHAUSTED != errCode)
        {
            terminate_session();
        }
    }
    if (MAX_STREAM_TO_BE_EXHAUSTED == errCode)
    {
        std::cout << "stream exhausted on this client. Restart client:" << std::endl;
        retart_client_watcher.data = this;
        ev_timer_init(&retart_client_watcher, restart_client_w_cb, 0.0, 0.);
        ev_timer_start(worker->loop, &retart_client_watcher);
        return;
    }
    std::cout << "Process Request Failure:" << worker->stats.req_failed
              << ", errorCode: " << errCode
              << std::endl;
}

void Client::report_tls_info()
{
    if (worker->id == 0 && !worker->tls_info_report_done)
    {
        worker->tls_info_report_done = true;
        auto cipher = SSL_get_current_cipher(ssl);
        std::cout << "TLS Protocol: " << tls::get_tls_protocol(ssl) << "\n"
                  << "Cipher: " << SSL_CIPHER_get_name(cipher) << std::endl;
        print_server_tmp_key(ssl);
    }
}

void Client::report_app_info()
{
    if (worker->id == 0 && !worker->app_info_report_done)
    {
        worker->app_info_report_done = true;
        std::cout << "Application protocol: " << selected_proto << std::endl;
    }
}

void Client::terminate_session()
{
    session->terminate();
    // http1 session needs writecb to tear down session.
    signal_write();
}

void Client::on_request_start(int32_t stream_id)
{
    streams[stream_id] = Stream();
    auto curr_timepoint = std::chrono::steady_clock::now();
    stream_timestamp.insert(std::make_pair(curr_timepoint, stream_id));
}

void Client::reset_timeout_requests()
{
    if (stream_timestamp.empty())
    {
        return;
    }
    const std::chrono::milliseconds timeout_duration(config->stream_timeout_in_ms);
    std::chrono::steady_clock::time_point curr_time_point = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point timeout_timepoint = curr_time_point - timeout_duration;
    auto no_timeout_it = stream_timestamp.upper_bound(timeout_timepoint);
    auto it = stream_timestamp.begin();
    bool call_signal_write = false;
    while (it != no_timeout_it)
    {
        if (streams.find(it->second) != streams.end())
        {
            session->submit_rst_stream(it->second);
            worker->stats.req_timedout++;
            call_signal_write = true;
        }
        it = stream_timestamp.erase(it);
    }
    if (call_signal_write)
    {
        signal_write();
    }
}


void Client::on_header(int32_t stream_id, const uint8_t* name, size_t namelen,
                       const uint8_t* value, size_t valuelen)
{
    auto itr = streams.find(stream_id);
    if (itr == std::end(streams))
    {
        return;
    }
    auto& stream = (*itr).second;

    if (worker->current_phase != Phase::MAIN_DURATION)
    {
        // If the stream is for warm-up phase, then mark as a success
        // But we do not update the count for 2xx, 3xx, etc status codes
        // Same has been done in on_status_code function
        stream.status_success = 1;
        return;
    }

    auto request = requests_awaiting_response.find(stream_id);
    if (request != requests_awaiting_response.end())
    {
        std::string header_name;
        header_name.assign((const char*)name, namelen);
        std::string header_value;
        header_value.assign((const char*)value, valuelen);
        request->second.resp_headers[header_name] = header_value;
    }

    if (stream.status_success == -1 && namelen == 7 &&
        util::streq_l(":status", name, namelen))
    {
        int status = 0;
        for (size_t i = 0; i < valuelen; ++i)
        {
            if ('0' <= value[i] && value[i] <= '9')
            {
                status *= 10;
                status += value[i] - '0';
                if (status > 999)
                {
                    stream.status_success = 0;
                    return;
                }
            }
            else
            {
                break;
            }
        }

        stream.req_stat.status = status;
        if (status >= 200 && status < 300)
        {
            ++worker->stats.status[2];
            stream.status_success = 1;
        }
        else if (status < 400)
        {
            ++worker->stats.status[3];
            stream.status_success = 1;
        }
        else if (status < 600)
        {
            ++worker->stats.status[status / 100];
            stream.status_success = 0;
        }
        else
        {
            stream.status_success = 0;
        }
    }
}

void Client::on_status_code(int32_t stream_id, uint16_t status)
{

    auto request = requests_awaiting_response.find(stream_id);
    if (request != requests_awaiting_response.end())
    {
        request->second.status_code = status;
    }

    auto itr = streams.find(stream_id);
    if (itr == std::end(streams))
    {
        return;
    }
    auto& stream = (*itr).second;

    if (worker->current_phase != Phase::MAIN_DURATION)
    {
        stream.status_success = 1;
        return;
    }

    stream.req_stat.status = status;
    if (status >= 200 && status < 300)
    {
        ++worker->stats.status[2];
        stream.status_success = 1;
    }
    else if (status < 400)
    {
        ++worker->stats.status[3];
        stream.status_success = 1;
    }
    else if (status < 600)
    {
        ++worker->stats.status[status / 100];
        stream.status_success = 0;
    }
    else
    {
        stream.status_success = 0;
    }
}

void Client::on_stream_close(int32_t stream_id, bool success, bool final)
{
    if (worker->current_phase == Phase::MAIN_DURATION)
    {
        if (req_inflight > 0)
        {
            --req_inflight;
        }
        if (config->rps_enabled() && rps_req_inflight)
        {
            --rps_req_inflight;
        }
        auto req_stat = get_req_stat(stream_id);
        if (!req_stat)
        {
            return;
        }

        req_stat->stream_close_time = std::chrono::steady_clock::now();
        if (success)
        {
            req_stat->completed = true;
            ++worker->stats.req_success;
            ++cstat.req_success;

            if (streams[stream_id].status_success == 1)
            {
                ++worker->stats.req_status_success;
            }
            else
            {
                ++worker->stats.req_failed;
            }

            worker->sample_req_stat(req_stat);

            // Count up in successful cases only
            ++worker->request_times_smp.n;
        }
        else
        {
            ++worker->stats.req_failed;
            ++worker->stats.req_error;
        }
        ++worker->stats.req_done;
        ++req_done;

        auto resp_time_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                req_stat->stream_close_time - req_stat->request_time);

        worker->stats.max_resp_time_us = std::max(worker->stats.max_resp_time_us.load(), (uint64_t)resp_time_us.count());
        worker->stats.min_resp_time_us = std::min(worker->stats.min_resp_time_us.load(), (uint64_t)resp_time_us.count());

        if (worker->config->log_fd != -1)
        {
            auto start = std::chrono::duration_cast<std::chrono::microseconds>(
                             req_stat->request_wall_time.time_since_epoch());
            auto delta = std::chrono::duration_cast<std::chrono::microseconds>(
                             req_stat->stream_close_time - req_stat->request_time);

            std::array<uint8_t, 256> buf;
            auto p = std::begin(buf);
            p = util::utos(p, start.count());
            *p++ = '\t';
            if (success)
            {
                p = util::utos(p, req_stat->status);
            }
            else
            {
                *p++ = '-';
                *p++ = '1';
            }
            *p++ = '\t';
            p = util::utos(p, delta.count());
            *p++ = '\n';

            auto nwrite = static_cast<size_t>(std::distance(std::begin(buf), p));
            assert(nwrite <= buf.size());
            while (write(worker->config->log_fd, buf.data(), nwrite) == -1 &&
                   errno == EINTR)
                ;
        }
    }

    worker->report_progress();
    streams.erase(stream_id);

    auto request = requests_awaiting_response.find(stream_id);
    if (request != requests_awaiting_response.end())
    {
        prepare_next_request(request->second);
        requests_awaiting_response.erase(request);
    }

    if (req_left == 0 && req_inflight == 0)
    {
        terminate_session();
        return;
    }

    if (!final && req_left > 0)
    {
        if (config->timing_script)
        {
            if (!ev_is_active(&request_timeout_watcher))
            {
                ev_feed_event(worker->loop, &request_timeout_watcher, EV_TIMER);
            }
        }
        else if (!config->rps_enabled())
        {
            if (submit_request() != 0)
            {
                process_request_failure();
            }
        }
        else if (rps_req_pending)
        {
            --rps_req_pending;
            auto retCode = submit_request();
            if (retCode != 0)
            {
                process_request_failure(retCode);
            }
        }
    }
}

void Client::on_data_chunk(int32_t stream_id, const uint8_t* data, size_t len)
{
    auto request = requests_awaiting_response.find(stream_id);
    if (request != requests_awaiting_response.end())
    {
        request->second.resp_payload.assign((const char*)data, len);
    }
}
RequestStat* Client::get_req_stat(int32_t stream_id)
{
    auto it = streams.find(stream_id);
    if (it == std::end(streams))
    {
        return nullptr;
    }

    return &(*it).second.req_stat;
}

int Client::connection_made()
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
            std::cout << "No protocol negotiated. Fallback behaviour may be activated"
                      << std::endl;

            for (const auto& proto : config->npn_list)
            {
                if (util::streq(NGHTTP2_H1_1_ALPN, StringRef {proto}))
                {
                    std::cout
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
            report_app_info();
        }

        if (!session)
        {
            std::cout
                    << "No supported protocol was negotiated. Supported protocols were:"
                    << std::endl;
            for (const auto& proto : config->npn_list)
            {
                std::cout << proto.substr(1) << std::endl;
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

        report_app_info();
    }

    state = CLIENT_CONNECTED;

    session->on_connect();

    record_connect_time();

    if (config->rps_enabled())
    {
        rps_watcher.repeat = std::max(0.01, 1. / config->rps);
        ev_timer_again(worker->loop, &rps_watcher);
        rps_duration_started = ev_now(worker->loop);
    }

    stream_timeout_watcher.repeat = 0.01;
    ev_timer_again(worker->loop, &stream_timeout_watcher);

    if (config->rps_enabled())
    {
        assert(req_left);

        ++rps_req_inflight;

        auto retCode = submit_request();
        if (retCode != 0)
        {
            process_request_failure(retCode);
        }
    }
    else if (!config->timing_script)
    {
        auto nreq = config->is_timing_based_mode()
                    ? std::max(req_left, session->max_concurrent_streams())
                    : std::min(req_left, session->max_concurrent_streams());

        for (; nreq > 0; --nreq)
        {
            if (submit_request() != 0)
            {
                process_request_failure();
                break;
            }
        }
    }
    else
    {

        ev_tstamp duration = config->timings[reqidx];

        while (duration < 1e-9)
        {
            if (submit_request() != 0)
            {
                process_request_failure();
                break;
            }
            duration = config->timings[reqidx];
            if (reqidx == 0)
            {
                // if reqidx wraps around back to 0, we uses up all lines and
                // should break
                break;
            }
        }

        if (duration >= 1e-9)
        {
            // double check since we may have break due to reqidx wraps
            // around back to 0
            request_timeout_watcher.repeat = duration;
            ev_timer_again(worker->loop, &request_timeout_watcher);
        }
    }
    signal_write();

    return 0;
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

int Client::connected()
{
    if (!util::check_socket_connected(fd))
    {
        return ERR_CONNECT_FAIL;
    }
    ev_io_start(worker->loop, &rev);
    ev_io_stop(worker->loop, &wev);
    ancestor.reset();

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

void Client::record_request_time(RequestStat* req_stat)
{
    req_stat->request_time = std::chrono::steady_clock::now();
    req_stat->request_wall_time = std::chrono::system_clock::now();
}

void Client::record_connect_start_time()
{
    cstat.connect_start_time = std::chrono::steady_clock::now();
}

void Client::record_connect_time()
{
    cstat.connect_time = std::chrono::steady_clock::now();
}

void Client::record_ttfb()
{
    if (recorded(cstat.ttfb))
    {
        return;
    }

    cstat.ttfb = std::chrono::steady_clock::now();
}

void Client::clear_connect_times()
{
    cstat.connect_start_time = std::chrono::steady_clock::time_point();
    cstat.connect_time = std::chrono::steady_clock::time_point();
    cstat.ttfb = std::chrono::steady_clock::time_point();
}

void Client::record_client_start_time()
{
    // Record start time only once at the very first connection is going
    // to be made.
    if (recorded(cstat.client_start_time))
    {
        return;
    }

    cstat.client_start_time = std::chrono::steady_clock::now();
}

void Client::record_client_end_time()
{
    // Unlike client_start_time, we overwrite client_end_time.  This
    // handles multiple connect/disconnect for HTTP/1.1 benchmark.
    cstat.client_end_time = std::chrono::steady_clock::now();
}

void Client::signal_write()
{
    ev_io_start(worker->loop, &wev);
}

void Client::try_new_connection()
{
    new_connection_requested = true;
}


void Client::replace_variable(std::string& input, const std::string& variable_name, uint64_t variable_value)
{
    if (variable_name.size() && (input.find(variable_name) != std::string::npos))
    {
        size_t full_length = std::to_string(config->json_config_schema.variable_range_end).size();
        std::string curr_var_value = std::to_string(variable_value);
        std::string padding;
        padding.reserve(full_length - curr_var_value.size());
        for (size_t i = 0; i < full_length - curr_var_value.size(); i++)
        {
            padding.append("0");
        }
        curr_var_value.insert(0, padding);
        input = std::regex_replace(input, std::regex(variable_name), curr_var_value);
    }
}

void Client::update_content_length(Request_Data& data)
{
    if (data.req_payload.size())
    {
        std::string content_length = "content-length";
        data.req_headers.erase(content_length);
        data.req_headers[content_length] = std::to_string(data.req_payload.size());
    }
}
Request_Data Client::get_request_to_submit()
{
    static thread_local size_t full_var_str_len =
                std::to_string(config->json_config_schema.variable_range_end).size();
    static Request_Data dummy_data;

    if (!requests_to_submit.empty())
    {
        auto data = requests_to_submit.front();
        requests_to_submit.pop_front();
        return data;
    }
    else
    {
        Request_Data data;
        size_t curr_index = 0;
        data.user_id = curr_req_variable_value;
        auto& first_scenario = config->json_config_schema.scenarios[curr_index];
        data.path = reassemble_str_with_variable(first_scenario.tokenized_path,
                                                 data.user_id,
                                                 full_var_str_len);
        data.method = first_scenario.method;
        data.req_payload = reassemble_str_with_variable(first_scenario.tokenized_payload,
                                                        data.user_id,
                                                        full_var_str_len);;
        data.req_headers = first_scenario.headers_in_map;

        if (first_scenario.luaScript.size())
        {
            if (!update_request_with_lua(lua_states[curr_index], dummy_data, data))
            {
              std::cerr << "lua script failure for first request, cannot continue, exit"<< std::endl;
              exit(EXIT_FAILURE);
            }
        }
        update_content_length(data);

        if (config->json_config_schema.variable_range_end)
        {
            curr_req_variable_value++;
            if (curr_req_variable_value > config->json_config_schema.variable_range_end)
            {
                curr_req_variable_value = config->json_config_schema.variable_range_start;
            }
        }

        data.next_request = 1;
        return data;
    }
}

bool Client::prepare_next_request(const Request_Data& finished_request)
{
    static thread_local size_t full_var_str_len =
                  std::to_string(config->json_config_schema.variable_range_end).size();
    if (finished_request.next_request >= config->json_config_schema.scenarios.size())
    {
        return false;
    }

    Request_Data new_request;

    std::string session_cookie;
    if (finished_request.resp_headers.find("Set-Cookie"))
    {
        session_cookie = finished_request.resp_headers["Set-Cookie"];
    }
    else
    {
        session_cookie = finished_request.session_cookie;
    }
    new_request.session_cookie = session_cookie;
    finished_request.resp_headers["Set-Cookie"] = new_request.session_cookie;

    auto& next_scenario = config->json_config_schema.scenarios[finished_request.next_request];
    new_request.user_id = finished_request.user_id;
    new_request.method = next_scenario.method;
    new_request.req_headers = next_scenario.headers_in_map;
    new_request.req_payload = reassemble_str_with_variable(next_scenario.tokenized_payload,
                                                           new_request.user_id,
                                                           full_var_str_len);

    if (next_scenario.path.typeOfAction == "input")
    {
         new_request.path = reassemble_str_with_variable(next_scenario.tokenized_path,
                                                         new_request.user_id,
                                                         full_var_str_len);
    }
    else if (next_scenario.path.typeOfAction == "sameWithLastOne")
    {
        new_request.path = finished_request.path;
    }
    else if (next_scenario.path.typeOfAction == "fromResponseHeader")
    {
        auto header = finished_request.resp_headers.find(next_scenario.path.input);
        if (header != finished_request.resp_headers.end())
        {
            http_parser_url u {};
            if (http_parser_parse_url(header->second.c_str(), header->second.size(), 0, &u) != 0)
            {
                std::cerr << "abort whole scenarios sequence, as invalid URI found in header: " << header->second << std::endl;
                return false;
            }
            else
            {
                new_request.path = get_reqline(header->second.c_str(), u);
            }
        }
        else
        {
            return false;
        }
    }

    if (next_scenario.luaScript.size())
    {
        if (!update_request_with_lua(lua_states[finished_request.next_request], finished_request, new_request))
        {
            return false; // lua script returns error, abort this sequence
        }
    }

    update_content_length(new_request);

    new_request.next_request = finished_request.next_request + 1;

    requests_to_submit.push_back(std::move(new_request));
    return true;
}

bool Client::update_request_with_lua(lua_State* L, const Request_Data& finished_request, Request_Data& request_to_send)
{
    lua_getglobal(L, "make_request");
    bool retCode = true;
    if (lua_isfunction(L, -1))
    {
        lua_createtable(L, 0, finished_request.resp_headers.size());

        for (auto& header : finished_request.resp_headers)
        {
            lua_pushlstring(L, header.first.c_str(), header.first.size());
            lua_pushlstring(L, header.second.c_str(), header.second.size());
            lua_rawset(L, -3);
        }
        static std::string method_name = ":method";
        lua_pushlstring(L, method_name.c_str(), method_name.size());
        lua_pushlstring(L, finished_request.method.c_str(), finished_request.method.size());
        lua_rawset(L, -3);
        static std::string path_name = ":path";
        lua_pushlstring(L, path_name.c_str(), path_name.size());
        lua_pushlstring(L, finished_request.path.c_str(), finished_request.path.size());
        lua_rawset(L, -3);

        lua_pushlstring(L, finished_request.resp_payload.c_str(), finished_request.resp_payload.size());

        lua_createtable(L, 0, request_to_send.req_headers.size());
        for (auto& header : request_to_send.req_headers)
        {
            lua_pushlstring(L, header.first.c_str(), header.first.size());
            lua_pushlstring(L, header.second.c_str(), header.second.size());
            lua_rawset(L, -3);
        }
        lua_pushlstring(L, method_name.c_str(), method_name.size());
        lua_pushlstring(L, request_to_send.method.c_str(), request_to_send.method.size());
        lua_rawset(L, -3);
        lua_pushlstring(L, path_name.c_str(), path_name.size());
        lua_pushlstring(L, request_to_send.path.c_str(), request_to_send.path.size());
        lua_rawset(L, -3);

        lua_pushlstring(L, request_to_send.req_payload.c_str(), request_to_send.req_payload.size());

        lua_pcall(L, 4, 2, 0);
        int top = lua_gettop(L);
        for (int i = 0; i < top; i++)
        {
            switch (lua_type(L, -1))
            {
                case LUA_TSTRING:
                {
                    size_t len;
                    const char* str = lua_tolstring(L, -1, &len);
                    request_to_send.req_payload.assign(str, len);
                    break;
                }
                case LUA_TTABLE:
                {
                    std::map<std::string, std::string> headers;
                    lua_pushnil(L);
                    while (lua_next(L, -2) != 0)
                    {
                        size_t len;
                        /* uses 'key' (at index -2) and 'value' (at index -1) */
                        if ((LUA_TSTRING != lua_type(L, -2)) || (LUA_TSTRING != lua_type(L, -1)))
                        {
                            std::cerr << "invalid http headers returned from lua function make_request" << std::endl;
                        }
                        const char* k = lua_tolstring(L, -2, &len);
                        std::string key(k, len);
                        const char* v = lua_tolstring(L, -1, &len);
                        std::string value(v, len);
                        //util::inp_strlower(key);
                        headers[key] = value;
                        /* removes 'value'; keeps 'key' for next iteration */
                        lua_pop(L, 1);
                    }
                    request_to_send.method = headers[":method"];
                    headers.erase(":method");
                    request_to_send.path = headers[":path"];
                    headers.erase(":path");
                    request_to_send.req_headers = headers;
                    break;
                }
                default:
                {
                    std::cerr << "error occured in lua function make_request, abort the whole scenarios sequence" << std::endl;
                    retCode = false;
                    break;
                }
            }
            lua_pop(L, 1);
        }
    }
    else
    {
        std::cerr << "lua script provisioned but required function not present, abort the whole scenarios sequence" << std::endl;
        retCode = false;
    }
    return retCode;
}

}

