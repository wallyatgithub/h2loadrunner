#include <openssl/err.h>
#include <openssl/ssl.h>
#include <regex>
#include <algorithm>
#include <cctype>
#include <string>

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
             Client* initiating_client, const std::string& dest_schema,
             const std::string& dest_authority)
    : wb(&worker->mcpool),
      cstat {},
        worker(worker),
        config(conf),
        ssl(nullptr),
        next_addr(conf->addrs),
        current_addr(nullptr),
        ares_addr(nullptr),
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
        curr_req_variable_value(0),
        parent_client(initiating_client),
        schema(dest_schema),
        authority(dest_authority)
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

    ev_timer_init(&stream_timeout_watcher, stream_timeout_cb, 0., 0.);
    stream_timeout_watcher.data = this;

    ev_timer_init(&connection_timeout_watcher, client_connection_timeout_cb, 2., 0.);
    ev_timer_init(&release_ancestor_watcher, client_connection_timeout_cb, 5., 5.);
    connection_timeout_watcher.data = this;

    for (auto& request : conf->json_config_schema.scenario)
    {
        lua_State* L = luaL_newstate();
        luaL_openlibs(L);
        if (request.luaScript.size())
        {
            luaL_dostring(L, request.luaScript.c_str());
        }
        lua_states.push_back(L);
    }

    struct ares_options options;
    options.sock_state_cb = ares_socket_state_cb;
    options.sock_state_cb_data = this;
    auto optmask = ARES_OPT_SOCK_STATE_CB;
    auto status = ares_init_options(&channel, &options, optmask);
    if (status)
    {
        std::cerr<<"c-ares ares_init_options failed: "<<status<<std::endl;
        exit(EXIT_FAILURE);
    }

    if (schema.empty())
    {
        schema = conf->scheme;
    }
    if (authority.empty())
    {
        if (conf->port != conf->default_port)
        {
            authority = conf->host + ":" + util::utos(conf->port);
        }
        else
        {
            authority = conf->host;
        }
    }
    if (!initiating_client)
    {
        std::string dest = schema;
        dest.append("://").append(authority);
        dest_client[dest] = this;
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

    for (auto& client: dest_client)
    {
        if (client.second != this)
        {
            delete client.second;
        }
    }
    dest_client.clear();

    ares_freeaddrinfo(ares_addr);
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
    else if (ares_addr)
    {
      rv = make_socket(ares_addr->nodes);
    }

    if (fd == -1)
    {
        return -1;
    }

    writefn = &Client::connected;
    state = CLIENT_CONNECTING;
    ev_timer_start(worker->loop, &connection_timeout_watcher);
    if (ancestor_to_release.get())
    {
        ev_timer_start(worker->loop, &release_ancestor_watcher);
    }


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
    std::cout<<"===============disconnected from "<<authority<<"==============="<<std::endl;

    record_client_end_time();

    ev_timer_stop(worker->loop, &conn_inactivity_watcher);
    ev_timer_stop(worker->loop, &conn_active_watcher);
    ev_timer_stop(worker->loop, &rps_watcher);
    ev_timer_stop(worker->loop, &request_timeout_watcher);
    ev_timer_stop(worker->loop, &stream_timeout_watcher);
    ev_timer_stop(worker->loop, &connection_timeout_watcher);
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
    if (!any_request_to_submit())
    {
        return 0;
    }
    if (session->max_concurrent_streams() <= streams.size())
    {
        return 0;
    }

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
        auto it = request->second.resp_headers.find(header_name);
        if (it != request->second.resp_headers.end())
        {
            // Set-Cookie case most likely
            it->second.append("; ").append(header_value);
        }
        else
        {
            request->second.resp_headers[header_name] = header_value;
        }
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

        if (request != requests_awaiting_response.end() &&
            request->second.expected_status_code)
        {
            if (status != request->second.expected_status_code)
            {
                stream.status_success = 0;
            }
            else
            {
                stream.status_success = 1;
            }
        }
    }
}

void Client::on_status_code(int32_t stream_id, uint16_t status)
{

    auto request_data = requests_awaiting_response.find(stream_id);
    if (request_data != requests_awaiting_response.end())
    {
        request_data->second.status_code = status;
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

    if (request_data != requests_awaiting_response.end() &&
        request_data->second.expected_status_code)
    {
        if (status != request_data->second.expected_status_code)
        {
            stream.status_success = 0;
        }
        else
        {
            stream.status_success = 1;
        }
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
        request->second.resp_payload.append((const char*)data, len);
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
    std::cout<<"===============connected to "<<authority<<"==============="<<std::endl;

    if (!util::check_socket_connected(fd))
    {
        std::cout<<"check_socket_connected failed"<<std::endl;
        return ERR_CONNECT_FAIL;
    }
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

void Client::parse_and_save_cookies(Request_Data& finished_request)
{
    if (finished_request.resp_headers.find("Set-Cookie") != finished_request.resp_headers.end())
    {
        auto new_cookies = Cookie::parse_cookie_string(finished_request.resp_headers["Set-Cookie"],
                                               finished_request.authority, finished_request.schema);
        for (auto& cookie: new_cookies)
        {
            if (Cookie::is_cookie_acceptable(cookie))
            {
                finished_request.saved_cookies[cookie.cookie_key] = std::move(cookie);
            }
        }
    }
}

void Client::move_cookies_to_new_request(Request_Data& finished_request, Request_Data& new_request)
{
    new_request.saved_cookies.swap(finished_request.saved_cookies);
}

void Client::produce_request_cookie_header(Request_Data& req_to_be_sent)
{
    if (req_to_be_sent.saved_cookies.empty())
    {
        return;
    }
    auto iter = req_to_be_sent.req_headers.find("Cookie");
    std::set<std::string> cookies_from_config;
    if (iter != req_to_be_sent.req_headers.end())
    {
        auto cookie_vec = Cookie::parse_cookie_string(iter->second, req_to_be_sent.authority, req_to_be_sent.schema);
        for (auto& cookie: cookie_vec)
        {
            cookies_from_config.insert(cookie.cookie_key);
        }
    }
    const std::string cookie_delimeter = "; ";
    std::string cookies_to_append;
    for (auto& cookie: req_to_be_sent.saved_cookies)
    {
        if (cookies_from_config.count(cookie.first))
        {
            // an overriding header from config carries the same cookie, config takes precedence
            continue;
        }
        else if (!Cookie::is_cookie_allowed_to_be_sent(cookie.second, req_to_be_sent.schema, req_to_be_sent.authority, req_to_be_sent.path))
        {
            // cookie not allowed to be sent for this request
            continue;
        }
        if (!cookies_to_append.empty())
        {
            cookies_to_append.append(cookie_delimeter);
        }
        cookies_to_append.append(cookie.first).append("=").append(cookie.second.cookie_value);
    }
    if (!cookies_to_append.empty())
    {
        if (iter != req_to_be_sent.req_headers.end() && !iter->second.empty())
        {
            iter->second.append(cookie_delimeter).append(cookies_to_append);
        }
        else
        {
            req_to_be_sent.req_headers["Cookie"] = std::move(cookies_to_append);
        }
    }
}

void Client::populate_request_from_config_template(Request_Data& new_request,
                                                                  size_t index_in_config_template)
{
    static thread_local size_t full_var_str_len =
                std::to_string(config->json_config_schema.variable_range_end).size();

    auto& request_template = config->json_config_schema.scenario[index_in_config_template];

    if (index_in_config_template == 0)
    {
        new_request.path = reassemble_str_with_variable(request_template.tokenized_path,
                                                        new_request.user_id,
                                                        full_var_str_len);
    }
    new_request.method = request_template.method;
    new_request.schema = config->scheme;
    if (config->port != config->default_port)
    {
        new_request.authority = config->host + ":" + util::utos(config->port);
    }
    else
    {
        new_request.authority = config->host;
    }

    new_request.req_payload = reassemble_str_with_variable(request_template.tokenized_payload,
                                                           new_request.user_id,
                                                           full_var_str_len);;
    new_request.req_headers = request_template.headers_in_map;
    new_request.expected_status_code = request_template.expected_status_code;
}

Request_Data Client::get_request_to_submit()
{
    static thread_local size_t full_var_str_len =
                std::to_string(config->json_config_schema.variable_range_end).size();
    static Request_Data dummy_data;

    if (!requests_to_submit.empty())
    {
        auto request_data = requests_to_submit.front();
        requests_to_submit.pop_front();
        return request_data;
    }
    else
    {
        Request_Data request_data;
        size_t curr_index = 0;
        request_data.user_id = curr_req_variable_value;
        populate_request_from_config_template(request_data, 0);

        if (config->json_config_schema.scenario[curr_index].luaScript.size())
        {
            if (!update_request_with_lua(lua_states[curr_index], dummy_data, request_data))
            {
              std::cerr << "lua script failure for first request, cannot continue, exit"<< std::endl;
              exit(EXIT_FAILURE);
            }
        }
        update_content_length(request_data);

        if (config->json_config_schema.variable_range_end)
        {
            curr_req_variable_value++;
            if (curr_req_variable_value > config->json_config_schema.variable_range_end)
            {
                curr_req_variable_value = config->json_config_schema.variable_range_start;
            }
        }

        request_data.next_request = (curr_index + 1);
        return request_data;
    }
}

bool Client::prepare_next_request(Request_Data& finished_request)
{
    static thread_local size_t full_var_str_len =
                  std::to_string(config->json_config_schema.variable_range_end).size();
    if (finished_request.next_request >= config->json_config_schema.scenario.size())
    {
        return false;
    }

    Request_Data new_request;
    auto& request_template = config->json_config_schema.scenario[finished_request.next_request];
    new_request.user_id = finished_request.user_id;
    populate_request_from_config_template(new_request, finished_request.next_request);

    if (request_template.path.typeOfAction == "input")
    {
         new_request.path = reassemble_str_with_variable(request_template.tokenized_path,
                                                         new_request.user_id,
                                                         full_var_str_len);
    }
    else if (request_template.path.typeOfAction == "sameWithLastOne")
    {
        new_request.path = finished_request.path;
    }
    else if (request_template.path.typeOfAction == "fromResponseHeader")
    {
        auto header = finished_request.resp_headers.find(request_template.path.input);
        if (header != finished_request.resp_headers.end())
        {
            http_parser_url u {};
            if (http_parser_parse_url(header->second.c_str(), header->second.size(), 0, &u) != 0)
            {
                std::cerr << "abort whole scenario sequence, as invalid URI found in header: " << header->second << std::endl;
                return false;
            }
            else
            {
                new_request.path = get_reqline(header->second.c_str(), u);
                if (util::has_uri_field(u, UF_SCHEMA) && util::has_uri_field(u, UF_HOST))
                {
                    new_request.schema = util::get_uri_field(header->second.c_str(), u, UF_SCHEMA).str();
                    util::inp_strlower(new_request.schema);
                    new_request.authority = util::get_uri_field(header->second.c_str(), u, UF_HOST).str();
                    util::inp_strlower(new_request.authority);
                    if (util::has_uri_field(u, UF_PORT))
                    {
                        new_request.authority.append(":").append(util::utos(u.port));
                    }
                }
            }
        }
        else
        {
            if (config->verbose)
            {
                std::cout<<"response status code:"<<finished_request.status_code<<std::endl;
                std::cerr << "abort whole scenario sequence, as header not found: " << request_template.path.input << std::endl;
                for (auto& header: finished_request.resp_headers)
                {
                    std::cout<<header.first<<":"<<header.second<<std::endl;
                }
                std::cout<<"response payload:"<<finished_request.resp_payload<<std::endl;
            }
            return false;
        }
    }

    if (!request_template.clear_old_cookies)
    {
        parse_and_save_cookies(finished_request);
        move_cookies_to_new_request(finished_request, new_request);
        produce_request_cookie_header(new_request);
    }

    if (request_template.luaScript.size())
    {
        if (!update_request_with_lua(lua_states[finished_request.next_request], finished_request, new_request))
        {
            return false; // lua script returns error, abort this sequence
        }
    }

    update_content_length(new_request);

    new_request.next_request = finished_request.next_request + 1;

    enqueue_request(new_request);

    return true;
}

void Client::enqueue_request(Request_Data& new_request)
{
    Client* next_client_to_run = find_or_create_dest_client(new_request);
    next_client_to_run->requests_to_submit.push_back(std::move(new_request));
    Submit_Requet_Wrapper auto_submitter(this, next_client_to_run);
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
                    std::map<std::string, std::string, ci_less> headers;
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
                    request_to_send.authority = headers[":authority"];
                    headers.erase(":authority");
                    request_to_send.schema = headers[":schema"];
                    headers.erase(":schema");
                    break;
                }
                default:
                {
                    std::cerr << "error occured in lua function make_request, abort the whole scenario sequence" << std::endl;
                    retCode = false;
                    break;
                }
            }
            lua_pop(L, 1);
        }
    }
    else
    {
        std::cerr << "lua script provisioned but required function not present, abort the whole scenario sequence" << std::endl;
        retCode = false;
    }
    return retCode;
}

Client* Client::find_or_create_dest_client(Request_Data& request_to_send)
{
    if (!parent_client) // this is the first connection
    {
        std::string dest = request_to_send.schema;
        dest.append("://").append(request_to_send.authority);
        auto it = dest_client.find(dest);
        if (it == dest_client.end())
        {
            auto new_client = std::make_unique<Client>(this->id, this->worker, this->req_todo, this->config,
                                                       this, request_to_send.schema, request_to_send.authority);
            dest_client[dest] = new_client.get();
            new_client->connect_to_host(new_client->schema, new_client->authority);
            new_client.release();
        }
        return dest_client[dest];
    }
    else
    {
        return parent_client->find_or_create_dest_client(request_to_send);
    }
}

int Client::resolve_fqdn_and_connect(const std::string& schema, const std::string& authority)
{
    state = CLIENT_CONNECTING;
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
    ares_getaddrinfo(channel, vec[0].c_str(), port.c_str(), &hints, ares_addrinfo_query_callback, this);
    return 0;
}

int Client::connect_to_host(const std::string& schema, const std::string& authority)
{
    //if (config->verbose)
    {
        std::cout<<"===============connecting to "<<schema<<"://"<<authority<<"==============="<<std::endl;
    }
    return resolve_fqdn_and_connect(schema, authority);
}

bool Client::any_request_to_submit()
{
    if (!parent_client) // this is the parent
    {
        return true;
    }
    else
    {
        return (!requests_to_submit.empty());
    }
}

void Client::terminate_sub_clients()
{
    for (auto& sub_client: dest_client)
    {
        if (sub_client.second->session)
        {
            sub_client.second->terminate_session();
        }
    }
}

void Client::substitute_ancestor(Client* ancestor)
{
    worker = ancestor->worker;
    requests_to_submit.swap(ancestor->requests_to_submit);
    parent_client = ancestor->parent_client;
    schema = ancestor->schema;
    authority = ancestor->authority;
    req_todo = ancestor->req_todo;
    req_left = ancestor->req_left;
    curr_req_variable_value = ancestor->curr_req_variable_value;
    ancestor_to_release.reset(ancestor);

    if (parent_client)
    {
        dest_client.swap(ancestor->dest_client);

        std::string dest = schema;
        dest.append("://").append(authority);

        if (parent_client->dest_client[dest] == ancestor)
        {
            parent_client->dest_client[dest] = this;
        }
        else
        {
            abort(); // this should not happen, abort for debug purpose
        }
    }
    else
    {
        for (size_t index = 0; index < worker->clients.size(); index++)
        {
            if (worker->clients[index] == ancestor)
            {
                worker->clients[index] = this;
                break;
            }
        }

    }
}

}


