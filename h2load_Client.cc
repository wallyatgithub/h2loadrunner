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


std::atomic<uint32_t> Client::client_unique_id(0);

Client::Client(uint32_t id, Worker* worker, size_t req_todo, Config* conf,
             Client* parent, const std::string& dest_schema,
             const std::string& dest_authority)
       :wb(&worker->mcpool),
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
        parent_client(parent),
        schema(dest_schema),
        authority(dest_authority),
        totalTrans_till_last_check(0),
        total_leading_Req_till_last_check(0),
        rps(conf->rps)
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

    init_timer_watchers();

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
    if (!parent)
    {
        std::string dest = schema;
        dest.append("://").append(authority);
        dest_clients[dest] = this;
    }

    timestamp_of_last_rps_check = std::chrono::steady_clock::now();
    timestamp_of_last_tps_check = timestamp_of_last_rps_check;

    if (!parent && conf->json_config_schema.load_share_hosts.size())
    {
        auto init_hosts = [this, conf]()
        {
            std::vector<std::string> hosts;
            for (auto& host: conf->json_config_schema.load_share_hosts)
            {
                hosts.push_back(host.host);
                if (host.port)
                {
                    hosts.back().append(":").append(std::to_string(host.port));
                }
            }
            if (std::find(std::begin(hosts), std::end(hosts), authority) == std::end(hosts))
            {
                hosts.push_back(authority);
            }
            return hosts;
        };
        static std::vector<std::string> hosts = init_hosts();
        auto myId = client_unique_id++;
        auto startIndex = myId % hosts.size();
        authority = hosts[startIndex];
        ares_addr = nullptr;
        next_addr = nullptr;
        current_addr = nullptr;
        for (auto count = 1; count < hosts.size(); count++)
        {
            candidate_addresses.push_back(hosts[(++startIndex)%hosts.size()]);
        }
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

    ev_timer_init(&release_ancestor_watcher, release_ancestor_cb, 5., 0.);
    release_ancestor_watcher.data = this;

    ev_timer_init(&delayed_request_watcher, delayed_request_cb, 0.01, 0.01);
    delayed_request_watcher.data = this;

    ev_timer_init(&restart_client_watcher, restart_client_w_cb, 0., 0.);
    restart_client_watcher.data = this;

    ev_timer_init(&adaptive_traffic_watcher, adaptive_traffic_timeout_cb, 1.0, 1.0);
    adaptive_traffic_watcher.data = this;

    ev_timer_init(&delayed_reconnect_watcher, delayed_reconnect_cb, 1.0, 0.0);
    delayed_reconnect_watcher.data = this;
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

    std::string dest = schema;
    dest.append("://").append(authority);
    if (parent_client && parent_client->dest_clients.count(dest) && parent_client->dest_clients[dest] == this)
    {
        parent_client->dest_clients.erase(dest);
    }

/*
 * client is deleted in writecb/readcb

    for (auto& client: dest_clients)
    {
        if (client.second != this)
        {
            delete client.second;
        }
    }
    dest_clients.clear();
*/
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
    else
    {
        return resolve_fqdn_and_connect(schema, authority);
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
    transfer_controllership();

    record_client_end_time();
    auto stop_timer_watcher = [this](ev_timer& watcher)
    {
        if (ev_is_active(&watcher))
        {
            ev_timer_stop(worker->loop, &watcher);
        }
    };

    auto stop_io_watcher = [this](ev_io& watcher)
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
    stop_timer_watcher(release_ancestor_watcher);
    stop_timer_watcher(restart_client_watcher);
    stop_timer_watcher(adaptive_traffic_watcher);
    stop_timer_watcher(delayed_reconnect_watcher);

    streams.clear();
    session.reset();
    wb.reset();
    state = CLIENT_IDLE;
    stop_io_watcher(wev);
    stop_io_watcher(rev);
    for (auto& it: ares_io_watchers)
    {
        stop_io_watcher(it.second);
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
}

uint64_t Client::get_total_pending_streams()
{
    if (parent_client != nullptr)
    {
        return parent_client->get_total_pending_streams();
    }
    else
    {
        auto pendingStreams = streams.size();
        for (auto& client: dest_clients)
        {
            pendingStreams += client.second->streams.size();
        }
        return pendingStreams;
    }
}

bool Client::is_controller_client()
{
    return (parent_client == nullptr);
}

Client* Client::get_controller_client()
{
    return parent_client;
}

int Client::submit_request()
{
    if (!is_controller_client())
    {
        return parent_client->submit_request();
    }

    if (session->max_concurrent_streams() <= get_total_pending_streams())
    {
        return 0;
    }

    if (requests_to_submit.empty())
    {
        requests_to_submit.push_back(std::move(prepare_first_request()));
    }

    Client* destination_client = this;
    if (config->json_config_schema.open_new_connection_based_on_authority_header)
    {
        destination_client = find_or_create_dest_client(requests_to_submit.front());
        if (destination_client != this)
        {
            destination_client->requests_to_submit.push_back(std::move(requests_to_submit.front()));
            requests_to_submit.pop_front();
        }
    }

    if (destination_client->state == CLIENT_CONNECTED)
    {
        auto retCode = destination_client->session->submit_request();

        if (retCode != 0)
        {
            destination_client->process_request_failure(retCode);
            return retCode;
        }

        destination_client->signal_write();

        if (worker->current_phase != Phase::MAIN_DURATION)
        {
            return 0;
        }
        if (is_controller_client())
        {
            ++worker->stats.req_started;
            ++req_started;
            ++req_inflight;
            if (!worker->config->is_timing_based_mode())
            {
                --req_left;
            }
        }
        // if an active timeout is set and this is the last request to be submitted
        // on this connection, start the active timeout.
        if (worker->config->conn_active_timeout > 0. && req_left == 0 && is_controller_client())
        {
            ev_timer_start(worker->loop, &conn_active_watcher);
            for (auto& client: dest_clients)
            {
               ev_timer_start(worker->loop, &client.second->conn_active_watcher);
            }
        }
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

    auto req_abandoned = req_inflight;

    if (!config->json_config_schema.connection_retry_on_disconnect)
    {
        req_abandoned += req_left;
        req_left = 0;
    }

    worker->stats.req_failed += req_abandoned;
    worker->stats.req_error += req_abandoned;

    req_inflight = 0;
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

    if (MAX_STREAM_TO_BE_EXHAUSTED == errCode)
    {
        std::cout << "stream exhausted on this client. Restart client:" << std::endl;
        restart_client_watcher.data = this;
        ev_timer_start(worker->loop, &restart_client_watcher);
        return;
    }

    if (streams.size() == 0)
    {
        terminate_session();
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
        header_value.erase(0, header_value.find_first_not_of(' '));
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

        on_status_code(stream_id,status);
    }
}

void Client::mark_response_success_or_failure(int32_t stream_id)
{
    uint16_t status = 0;
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

    status = stream.req_stat.status;

    auto request_data = requests_awaiting_response.find(stream_id);
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
    else
    {
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

    auto request_data = requests_awaiting_response.find(stream_id);
    if (request_data != requests_awaiting_response.end())
    {
        request_data->second.status_code = status;
    }

    auto itr = streams.find(stream_id);
    if (itr != std::end(streams))
    {
        auto& stream = (*itr).second;
        stream.req_stat.status = status;
    }
}

void Client::on_stream_close(int32_t stream_id, bool success, bool final)
{
    mark_response_success_or_failure(stream_id);

    if (worker->current_phase == Phase::MAIN_DURATION)
    {
        if (req_inflight > 0)
        {
            --req_inflight;
        }
        if (rps_mode() && rps_req_inflight)
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
        auto resp_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                req_stat->stream_close_time - req_stat->request_time);
        worker->stats.max_resp_time_ms = std::max(worker->stats.max_resp_time_ms.load(), (uint64_t)resp_time_ms.count());
        worker->stats.min_resp_time_ms = std::min(worker->stats.min_resp_time_ms.load(), (uint64_t)resp_time_ms.count());
        worker->stats.total_resp_time_ms += resp_time_ms.count();

        ++worker->stats.req_done;
        ++req_done;

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

    auto finished_request = requests_awaiting_response.find(stream_id);
    if (finished_request != requests_awaiting_response.end())
    {
        if (is_leading_request(finished_request->second))
        {
            cstat.leading_req_done++;
        }
        finished_request->second.transaction_stat->successful = (streams[stream_id].status_success == 1);
        prepare_next_request(finished_request->second);
        requests_awaiting_response.erase(finished_request);
    }

    streams.erase(stream_id);

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
        else if (!rps_mode())
        {
          submit_request();
        }
        else if (rps_req_pending)
        {
            if (submit_request() == 0)
            {
                --rps_req_pending;
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
        if (config->verbose)
        {
            std::string str((const char*)data, len);
            std::cout<<"received data: "<<str<<std::endl;
        }
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

    if (config->nclients > 1 && (nullptr == parent_client))
    {
        if (!config->variable_range_slicing)
        {
            std::random_device                  rand_dev;
            std::mt19937                        generator(rand_dev());
            std::uniform_int_distribution<uint64_t>  distr(config->json_config_schema.variable_range_start,
                                                           config->json_config_schema.variable_range_end);
            curr_req_variable_value = distr(generator);
            req_variable_value_start = config->json_config_schema.variable_range_start;
            req_variable_value_end = config->json_config_schema.variable_range_end;
        }
        else
        {
            size_t nclients_per_worker = config->nclients / config->nthreads;
            ssize_t extra_clients = config->nclients % config->nthreads;

            uint64_t this_work_id = worker->id;
            uint64_t this_client_id = id;
            uint64_t req_per_client = (config->json_config_schema.variable_range_end - config->json_config_schema.variable_range_start)/
                                      (config->nclients);

            uint64_t req_per_client_left = (config->json_config_schema.variable_range_end - config->json_config_schema.variable_range_start)%
                                      (config->nclients);

            uint64_t normal_reqs_per_worker = req_per_client * nclients_per_worker;
            uint64_t this_client_req_value_start = config->json_config_schema.variable_range_start +
                                                   this_work_id * normal_reqs_per_worker +
                                                   this_client_id * req_per_client;
            if (extra_clients)
            {
                // all workers before handle 1 more client
                this_client_req_value_start += (this_work_id > extra_clients ? extra_clients:this_work_id)* req_per_client;
            }

            if ((this_work_id + 1 == config->nthreads) && (this_client_id +1  == nclients_per_worker))
            {
                req_per_client += req_per_client_left;
            }

            req_variable_value_start = this_client_req_value_start;
            curr_req_variable_value = req_variable_value_start;
            req_variable_value_end = this_client_req_value_start + req_per_client - 1;
            std::cout<<"worker Id:"<<this_work_id
                     <<", client Id:"<<this_client_id
                     <<", start:"<<req_variable_value_start
                     <<", end:"<<req_variable_value_end
                     <<std::endl;
        }
    }
    else
    {
        curr_req_variable_value = config->json_config_schema.variable_range_start;
        req_variable_value_start = config->json_config_schema.variable_range_start;
        req_variable_value_end = config->json_config_schema.variable_range_end;
    }

    session->on_connect();

    record_connect_time();

    if (parent_client != nullptr)
    {
        while (requests_to_submit.size())
        {
            // re-push to parent for to be scheduled immediately
            parent_client->requests_to_submit.push_front(std::move(requests_to_submit.back()));
            requests_to_submit.pop_back();
            if (!rps_mode())
            {
                if (parent_client->submit_request() != 0)
                {
                    break;
                }
            }
        }
        return 0;
    }

    if (rps_mode())
    {
        rps_watcher.repeat = std::max(0.1, 1. / rps);
        ev_timer_again(worker->loop, &rps_watcher);
        rps_duration_started = ev_now(worker->loop);
    }
    else
    {
        //start_watcher(adaptive_traffic_watcher, adaptive_traffic_timeout_cb);
    }

    stream_timeout_watcher.repeat = 0.01;
    ev_timer_again(worker->loop, &stream_timeout_watcher);

    if (rps_mode())
    {
        assert(req_left);

        ++rps_req_inflight;

        submit_request();
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
    ev_timer_start(worker->loop, &delayed_request_watcher);

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
                std::cerr<<get_tls_error_string()<<std::endl;
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
    if (data.req_payload->size())
    {
        std::string content_length = "content-length";
        //data.req_headers.erase(content_length);
        data.shadow_req_headers[content_length] = std::to_string(data.req_payload->size());
    }
}

void Client::parse_and_save_cookies(Request_Data& finished_request)
{
    if (finished_request.resp_headers.find("Set-Cookie") != finished_request.resp_headers.end())
    {
        auto new_cookies = Cookie::parse_cookie_string(finished_request.resp_headers["Set-Cookie"],
                                               *finished_request.authority, *finished_request.schema);
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
    auto iter = req_to_be_sent.req_headers->find("Cookie");
    std::set<std::string> cookies_from_config;
    if (iter != req_to_be_sent.req_headers->end())
    {
        auto cookie_vec = Cookie::parse_cookie_string(iter->second, *req_to_be_sent.authority, *req_to_be_sent.schema);
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
        else if (!Cookie::is_cookie_allowed_to_be_sent(cookie.second, *req_to_be_sent.schema, *req_to_be_sent.authority, *req_to_be_sent.path))
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
        if (iter != req_to_be_sent.req_headers->end() && !iter->second.empty())
        {
            req_to_be_sent.shadow_req_headers["Cookie"] = iter->second;
            req_to_be_sent.shadow_req_headers["Cookie"].append(cookie_delimeter).append(cookies_to_append);
        }
        else
        {
            req_to_be_sent.shadow_req_headers["Cookie"] = std::move(cookies_to_append);
        }
    }
}

void Client::populate_request_from_config_template(Request_Data& new_request,
                                                                  size_t index_in_config_template)
{
    static thread_local size_t full_var_str_len =
                std::to_string(config->json_config_schema.variable_range_end).size();

    auto& request_template = config->json_config_schema.scenario[index_in_config_template];

    new_request.method = &request_template.method;
    new_request.schema = &request_template.schema;
    new_request.authority = &request_template.authority;
    new_request.string_collection.emplace_back(reassemble_str_with_variable(request_template.tokenized_payload,
                                              new_request.user_id,
                                              full_var_str_len));
    new_request.req_payload = &(new_request.string_collection.back());
    new_request.req_headers = &request_template.headers_in_map;
    new_request.expected_status_code = request_template.expected_status_code;
    new_request.delay_before_executing_next = request_template.delay_before_executing_next;
}

Request_Data Client::prepare_first_request()
{
    auto controller = parent_client ? parent_client : this;

    static thread_local size_t full_var_str_len =
                std::to_string(config->json_config_schema.variable_range_end).size();

    static thread_local Request_Data dummy_data;

    Request_Data new_request;
    size_t curr_index = 0;
    new_request.next_request_idx = ((curr_index + 1) % config->json_config_schema.scenario.size());

    new_request.user_id = controller->curr_req_variable_value;
    if (controller->req_variable_value_end)
    {
        controller->curr_req_variable_value++;
        if (controller->curr_req_variable_value > controller->req_variable_value_end)
        {
            std::cout<<"user id (variable_value) wrapped, start over from range start"<<std::endl;
            controller->curr_req_variable_value = controller->req_variable_value_start;
        }
    }

    populate_request_from_config_template(new_request, curr_index);

    auto& request_template = config->json_config_schema.scenario[curr_index];
    new_request.string_collection.emplace_back(reassemble_str_with_variable(request_template.tokenized_path,
                                              new_request.user_id,
                                              full_var_str_len));
    new_request.path = &(new_request.string_collection.back());

    if (config->json_config_schema.scenario[curr_index].luaScript.size())
    {
        if (!update_request_with_lua(lua_states[curr_index], dummy_data, new_request))
        {
          std::cerr << "lua script failure for first request, cannot continue, exit"<< std::endl;
          exit(EXIT_FAILURE);
        }
    }
    update_content_length(new_request);
    new_request.transaction_stat = std::make_shared<TransactionStat>();
    worker->stats.transaction_done++;

    return new_request;
}


Request_Data Client::get_request_to_submit()
{
    if (!requests_to_submit.empty())
    {
        auto queued_request = std::move(requests_to_submit.front());
        requests_to_submit.pop_front();
        return queued_request;
    }
    else
    {
        return prepare_first_request();
    }
}

bool Client::prepare_next_request(Request_Data& finished_request)
{
    static thread_local size_t full_var_str_len =
                  std::to_string(config->json_config_schema.variable_range_end).size();

    size_t curr_index = finished_request.next_request_idx;

    if (curr_index == 0)
    {
        if (finished_request.transaction_stat->successful)
        {
            worker->stats.transaction_successful++;
            auto end_time = std::chrono::steady_clock::now();
            auto trans_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  end_time - finished_request.transaction_stat->start_time);

            worker->stats.trans_max_resp_time_ms = std::max(worker->stats.trans_max_resp_time_ms.load(),
                                                            (uint64_t)trans_duration.count());
            worker->stats.trans_min_resp_time_ms = std::min(worker->stats.trans_min_resp_time_ms.load(),
                                                            (uint64_t)trans_duration.count());
        }
        if (parent_client != nullptr)
        {
            parent_client->cstat.trans_done++;
        }
        else
        {
            cstat.trans_done++;
        }
        return false;
    }

    Request_Data new_request;
    new_request.transaction_stat = finished_request.transaction_stat;
    new_request.next_request_idx = ((curr_index + 1) % config->json_config_schema.scenario.size());

    auto& request_template = config->json_config_schema.scenario[curr_index];
    new_request.user_id = finished_request.user_id;
    populate_request_from_config_template(new_request, curr_index);

    if (request_template.uri.typeOfAction == "input")
    {
         new_request.string_collection.emplace_back(reassemble_str_with_variable(request_template.tokenized_path,
                                                         new_request.user_id,
                                                         full_var_str_len));
         new_request.path = &(new_request.string_collection.back());
    }
    else if (request_template.uri.typeOfAction == "sameWithLastOne")
    {
        new_request.string_collection.emplace_back(*finished_request.path);
        new_request.path = &(new_request.string_collection.back());
        new_request.string_collection.emplace_back(*finished_request.schema);
        new_request.schema = &(new_request.string_collection.back());
        new_request.string_collection.emplace_back(*finished_request.authority);
        new_request.authority = &(new_request.string_collection.back());
    }
    else if (request_template.uri.typeOfAction == "fromResponseHeader")
    {
        auto header = finished_request.resp_headers.find(request_template.uri.input);
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
                new_request.string_collection.emplace_back(get_reqline(header->second.c_str(), u));
                new_request.path = &(new_request.string_collection.back());
                if (util::has_uri_field(u, UF_SCHEMA) && util::has_uri_field(u, UF_HOST))
                {
                    new_request.string_collection.emplace_back(util::get_uri_field(header->second.c_str(), u, UF_SCHEMA).str());
                    util::inp_strlower(new_request.string_collection.back());
                    new_request.schema = &(new_request.string_collection.back());
                    new_request.string_collection.emplace_back(util::get_uri_field(header->second.c_str(), u, UF_HOST).str());
                    util::inp_strlower(new_request.string_collection.back());
                    if (util::has_uri_field(u, UF_PORT))
                    {
                        new_request.string_collection.back().append(":").append(util::utos(u.port));
                    }
                    new_request.authority = &(new_request.string_collection.back());
                }
                else
                {
                  new_request.string_collection.emplace_back(*finished_request.schema);
                  new_request.schema = &(new_request.string_collection.back());
                  new_request.string_collection.emplace_back(*finished_request.authority);
                  new_request.authority = &(new_request.string_collection.back());
                }
            }
        }
        else
        {
            if (config->verbose)
            {
                std::cout<<"response status code:"<<finished_request.status_code<<std::endl;
                std::cerr << "abort whole scenario sequence, as header not found: " << request_template.uri.input << std::endl;
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
        if (!update_request_with_lua(lua_states[curr_index], finished_request, new_request))
        {
            return false; // lua script returns error or kills the request, abort this scenario
        }
    }

    update_content_length(new_request);

    enqueue_request(finished_request, std::move(new_request));

    return true;
}

std::map<std::string, Client*>::const_iterator Client::get_client_serving_first_request()
{
    std::string key = config->json_config_schema.scenario[0].schema;
    key.append("://");
    key.append(config->json_config_schema.scenario[0].authority);
    if (parent_client != nullptr)
    {
        return parent_client->dest_clients.find(key);
    }
    else
    {
        return this->dest_clients.find(key);
    }
}

void Client::enqueue_request(Request_Data& finished_request, Request_Data&& new_request)
{
    auto client = this->parent_client ? this->parent_client : this;
    if (!finished_request.delay_before_executing_next)
    {
        client->requests_to_submit.push_back(std::move(new_request));
    }
    else
    {
        const std::chrono::milliseconds delay_duration(finished_request.delay_before_executing_next);
        std::chrono::steady_clock::time_point curr_time_point = std::chrono::steady_clock::now();
        std::chrono::steady_clock::time_point timeout_timepoint = curr_time_point + delay_duration;
        client->delayed_requests_to_submit.insert(std::make_pair(timeout_timepoint, std::move(new_request)));
    }
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
        lua_pushlstring(L, method_header.c_str(), method_header.size());
        lua_pushlstring(L, finished_request.method->c_str(), finished_request.method->size());
        lua_rawset(L, -3);
        lua_pushlstring(L, path_header.c_str(), path_header.size());
        lua_pushlstring(L, finished_request.path->c_str(), finished_request.path->size());
        lua_rawset(L, -3);
        lua_pushlstring(L, scheme_header.c_str(), scheme_header.size());
        lua_pushlstring(L, finished_request.schema->c_str(), finished_request.schema->size());
        lua_rawset(L, -3);
        lua_pushlstring(L, authority_header.c_str(), authority_header.size());
        lua_pushlstring(L, finished_request.authority->c_str(), finished_request.authority->size());
        lua_rawset(L, -3);


        lua_pushlstring(L, finished_request.resp_payload.c_str(), finished_request.resp_payload.size());

        lua_createtable(L, 0, request_to_send.req_headers->size());
        for (auto& header : *(request_to_send.req_headers))
        {
            lua_pushlstring(L, header.first.c_str(), header.first.size());
            lua_pushlstring(L, header.second.c_str(), header.second.size());
            lua_rawset(L, -3);
        }
        lua_pushlstring(L, method_header.c_str(), method_header.size());
        lua_pushlstring(L, request_to_send.method->c_str(), request_to_send.method->size());
        lua_rawset(L, -3);
        lua_pushlstring(L, path_header.c_str(), path_header.size());
        lua_pushlstring(L, request_to_send.path->c_str(), request_to_send.path->size());
        lua_rawset(L, -3);
        lua_pushlstring(L, scheme_header.c_str(), scheme_header.size());
        lua_pushlstring(L, request_to_send.schema->c_str(), request_to_send.schema->size());
        lua_rawset(L, -3);
        lua_pushlstring(L, authority_header.c_str(), authority_header.size());
        lua_pushlstring(L, request_to_send.authority->c_str(), request_to_send.authority->size());
        lua_rawset(L, -3);

        lua_pushlstring(L, request_to_send.req_payload->c_str(), request_to_send.req_payload->size());

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
                    request_to_send.string_collection.emplace_back(str, len);
                    request_to_send.req_payload = &(request_to_send.string_collection.back());
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
                    request_to_send.string_collection.emplace_back(headers[method_header]);
                    request_to_send.method = &(request_to_send.string_collection.back());
                    headers.erase(method_header);
                    request_to_send.string_collection.emplace_back(headers[path_header]);
                    request_to_send.path = &(request_to_send.string_collection.back());
                    headers.erase(path_header);
                    request_to_send.string_collection.emplace_back(headers[authority_header]);
                    request_to_send.authority= &(request_to_send.string_collection.back());
                    headers.erase(authority_header);
                    request_to_send.string_collection.emplace_back(headers[scheme_header]);
                    request_to_send.schema = &(request_to_send.string_collection.back());
                    headers.erase(scheme_header);
                    request_to_send.shadow_req_headers = std::move(headers);
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
    if (!config->json_config_schema.open_new_connection_based_on_authority_header)
    {
        return this->parent_client ? this->parent_client : this;
    }
    if (is_controller_client()) // this is the first connection
    {
        std::string dest = *(request_to_send.schema);
        dest.append("://").append(*request_to_send.authority);
        auto it = dest_clients.find(dest);
        if (it == dest_clients.end())
        {
            auto new_client = std::make_unique<Client>(this->id, this->worker, this->req_todo, this->config,
                                                       this, *request_to_send.schema, *request_to_send.authority);
            dest_clients[dest] = new_client.get();
            new_client->connect_to_host(new_client->schema, new_client->authority);
            new_client.release();
        }
        return dest_clients[dest];
    }
    else
    {
        return parent_client->find_or_create_dest_client(request_to_send);
    }
}

int Client::resolve_fqdn_and_connect(const std::string& schema, const std::string& authority)
{
    state = CLIENT_CONNECTING;
    ares_freeaddrinfo(ares_addr);
    ares_addr = nullptr;
    next_addr = nullptr;
    current_addr = nullptr;
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
    if (parent_client == nullptr)
    {
        // no parent_client, means "this" is the parent, i.e., the bootstraper
        // the bootstraper always has request to submit,
        // if none from queue, create from template
        return true;
    }
    else
    {
        return (!requests_to_submit.empty());
    }
}

void Client::terminate_sub_clients()
{
    for (auto& sub_client: dest_clients)
    {
        if (sub_client.second != this && sub_client.second->session)
        {
            sub_client.second->terminate_session();
        }
    }
}

void Client::transfer_controllership()
{
    if (dest_clients.empty() || !is_controller_client())
    {
        return;
    }
    Client* new_controller = nullptr;
    for (auto& client: dest_clients)
    {
        if (client.second != this)
        {
            new_controller = client.second;
            new_controller->parent_client = nullptr;
            new_controller->dest_clients = dest_clients;
        }
    }
    if (new_controller == nullptr)
    {
        return;
    }

    for (size_t index = 0; index < worker->clients.size(); index++)
    {
        if (worker->clients[index] == this)
        {
            worker->clients[index] = new_controller;
            break;
        }
    }

    for (auto& client: dest_clients)
    {
        if (client.second != new_controller)
        {
            client.second->parent_client = new_controller;
        }
    }
    parent_client = new_controller;
    dest_clients.clear();

    new_controller->req_done = req_done;
    new_controller->req_inflight = req_inflight;
    new_controller->req_left = req_left;
    new_controller->req_todo = req_todo;
    new_controller->req_started = req_started;
    new_controller->curr_req_variable_value = curr_req_variable_value;
    new_controller->req_variable_value_start = req_variable_value_start;
    new_controller->req_variable_value_end = req_variable_value_end;
    if (rps_mode())
    {
        new_controller->rps_watcher.repeat = std::max(0.1, 1. / rps);
        ev_timer_again(worker->loop, &new_controller->rps_watcher);
        new_controller->rps_duration_started = ev_now(worker->loop);
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
    req_variable_value_start = ancestor->req_variable_value_start;
    req_variable_value_end = ancestor->req_variable_value_end;
    ancestor_to_release.reset(ancestor);

    if (parent_client != nullptr)
    {
        dest_clients.swap(ancestor->dest_clients);

        std::string dest = schema;
        dest.append("://").append(authority);

        if (parent_client->dest_clients[dest] == ancestor)
        {
            parent_client->dest_clients[dest] = this;
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
        for (auto& it: dest_clients)
        {
            it.second->parent_client = this;
        }
    }
}

double Client::calc_tps()
{
    if (parent_client != nullptr)
    {
        // sub client does not have control over transaction
        return calc_rps();
    }

    size_t totalTrans = cstat.trans_done;
    auto curr_timestamp = std::chrono::steady_clock::now();

    auto delta_trans = totalTrans - totalTrans_till_last_check;
    totalTrans_till_last_check = totalTrans;

    auto delta_ms = std::chrono::duration_cast<std::chrono::milliseconds>(curr_timestamp - timestamp_of_last_tps_check);
    timestamp_of_last_tps_check = curr_timestamp;

    return delta_ms.count()>0 ? (double)((1000*delta_trans)/delta_ms.count()) : 0;
}

double Client::calc_rps()
{
    size_t totalReq = cstat.leading_req_done;
    auto curr_timestamp = std::chrono::steady_clock::now();

    auto delta_reqs = totalReq - total_leading_Req_till_last_check;
    total_leading_Req_till_last_check = totalReq;

    auto delta_ms = std::chrono::duration_cast<std::chrono::milliseconds>(curr_timestamp - timestamp_of_last_rps_check);
    timestamp_of_last_rps_check = curr_timestamp;

    return delta_ms.count()>0 ? (double)((1000*delta_reqs)/delta_ms.count()) : 0;
}

double Client::adjust_traffic_needed()
{
    const uint32_t gap_in_second = 5;
    double no_adjust = 0.0;

    if (config->rps_enabled())
    {
        // respect user configured rps, even it is too large
        // in this case, user will observe lower rps reported than configured
        return no_adjust;
    }
    if (parent_client != nullptr)
    {
        return no_adjust;
    }

    auto tps = calc_tps();
    auto rps = calc_rps();

    if ((rps > tps) && (tps>0.0) && ((total_leading_Req_till_last_check - totalTrans_till_last_check)/tps >= gap_in_second))
    {
        std::cout<<"adjust traffic needed for this connection, client id: "<<id
                 <<", schema: "<<schema<<", authority: "<<authority
                 <<", total Reqquest done: "<<total_leading_Req_till_last_check
                 <<", total transaction done: "<<totalTrans_till_last_check
                 <<", actual transaction per second:"<<tps
                 <<std::endl;
        return tps;
    }
    return no_adjust;
}

bool Client::rps_mode()
{
    return (rps > 0.0);
}

void Client::switch_to_non_rps_mode()
{
    ev_timer_stop(worker->loop, &rps_watcher);
    rps = 0.0;

    auto nreq = config->is_timing_based_mode()
                ? std::max(req_left, (session->max_concurrent_streams() - streams.size()))
                : std::min(req_left, (session->max_concurrent_streams() - streams.size()));

    bool write_socket = false;
    for (; nreq > 0; --nreq)
    {
        if (submit_request() != 0)
        {
            break;
        }
        write_socket = true;
    }
    if (write_socket)
    {
        signal_write();
    }
}
void Client::switch_mode(double new_rps)
{
    bool old_rps_mode = rps_mode();
    rps = new_rps;
    bool new_rps_mode = rps_mode();

    if (!old_rps_mode && new_rps_mode)
    {
        std::cout<<"switching controller connection to rps mode to lower the traffic"
                 <<", rps: "<<rps
                 <<std::endl;
        rps_watcher.repeat = std::max(0.01, 1. / rps);
        ev_timer_again(worker->loop, &rps_watcher);
        rps_duration_started = ev_now(worker->loop);
    }
    else if (old_rps_mode && !new_rps_mode)
    {
        switch_to_non_rps_mode();
    }
    else if (old_rps_mode && new_rps_mode)
    {
        ev_timer_again(worker->loop, &rps_watcher);
    }
    else
    {
        switch_to_non_rps_mode();
    }
}

bool Client::is_leading_request(Request_Data& request)
{
    if (config->json_config_schema.scenario.size() == 1)
    {
        return true;
    }
    else if (request.next_request_idx == 1)
    {
        return true;
    }
    return false;
}


bool Client::reconnect_to_alt_addr()
{
    if (!config->json_config_schema.connection_retry_on_disconnect)
    {
        return false;
    }
    init_timer_watchers();

    if (candidate_addresses.size())
    {
        used_addresses.push_back(std::move(authority));
        authority = std::move(candidate_addresses.front());
        candidate_addresses.pop_front();
        resolve_fqdn_and_connect(schema, authority);
        return true;
    }
    else if (used_addresses.size())
    {
        ev_timer_start(worker->loop, &delayed_reconnect_watcher);
        return true;
    }
    return false;
}

}


