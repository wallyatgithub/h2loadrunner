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


std::atomic<uint64_t> Client::client_unique_id(0);

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
        probe_skt_fd(-1),
        new_connection_requested(false),
        final(false),
        rps_duration_started(0),
        rps_req_pending(0),
        rps_req_inflight(0),
        curr_stream_id(0),
        parent_client(parent),
        schema(dest_schema),
        authority(dest_authority),
        connectfn(&Client::connect),
        rps(conf->rps)
{
    init_client_unique_id();

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

void Client::init_client_unique_id()
{
    this_client_id = client_unique_id++;
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
        std::cerr<<"c-ares ares_init_options failed: "<<status<<std::endl;
        exit(EXIT_FAILURE);
    }
}
void Client::init_lua_states()
{
    for (auto scenario_index = 0; scenario_index < config->json_config_schema.scenarios.size(); scenario_index++)
    {
        std::vector<lua_State*> requests_lua_states;
        for (auto request_index = 0; request_index < config->json_config_schema.scenarios[scenario_index].requests.size(); request_index++)
        {
            auto& request = config->json_config_schema.scenarios[scenario_index].requests[request_index];
            lua_State* L = luaL_newstate();
            luaL_openlibs(L);
            if (request.luaScript.size())
            {
                luaL_dostring(L, request.luaScript.c_str());
            }
            requests_lua_states.push_back(L);
        }
        lua_states.push_back(requests_lua_states);
    }
}

void Client::init_connection_targert()
{
    if (schema.empty())
    {
        schema = config->scheme;
    }
    if (authority.empty())
    {
        if (config->port != config->default_port)
        {
            authority = config->host + ":" + util::utos(config->port);
        }
        else
        {
            authority = config->host;
        }
    }

    if (is_controller_client() && (config->no_tls_proto != Config::PROTO_HTTP1_1) &&
        config->json_config_schema.load_share_hosts.size())
    {
        auto init_hosts = [this]()
        {
            std::vector<std::string> hosts;
            for (auto& host: config->json_config_schema.load_share_hosts)
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
        auto startIndex = this_client_id % hosts.size();
        authority = hosts[startIndex];
        preferred_authority = authority;
        ares_addr = nullptr;
        next_addr = nullptr;
        current_addr = nullptr;
        for (auto count = 0; count < hosts.size() - 1; count++)
        {
            candidate_addresses.push_back(hosts[(++startIndex)%hosts.size()]);
        }
        std::random_device random_device;
        std::mt19937 generator(random_device());
        std::shuffle(candidate_addresses.begin(), candidate_addresses.end(), generator);

        setup_connect_with_async_fqdn_lookup();
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
        for (auto& L: V)
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
    else if (ares_addr)
    {
      rv = make_socket(ares_addr->nodes);
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

void Client::timeout()
{
    if (should_reconnect_on_disconnect())
    {
        // it will need to reconnect anyway, why bother to disconnect
        return;
    }
    process_timedout_streams();

    disconnect();
}

void Client::restart_timeout()
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
    if (CLIENT_CONNECTED == state)
    {
        std::cerr<<"===============disconnected from "<<authority<<"==============="<<std::endl;
    }

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
    stop_timer_watcher(send_ping_watcher);
    stop_timer_watcher(delayed_reconnect_watcher);
    stop_timer_watcher(connect_to_preferred_host_watcher);

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
    curr_stream_id = 0;
    init_timer_watchers();
    if (write_clear_callback)
    {
        auto func = std::move(write_clear_callback);
    }
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
        return -1;
    }

    if (worker->current_phase == Phase::MAIN_DURATION_GRACEFUL_SHUTDOWN)
    {
        return -1;
    }

    if (requests_to_submit.empty() && (config->json_config_schema.scenarios.size()))
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
        size_t scenario_index = 0;
        size_t request_index = 0;
        if (config->json_config_schema.scenarios.size() && destination_client->requests_to_submit.size())
        {
            scenario_index = destination_client->requests_to_submit.front().scenario_index;
            request_index = destination_client->requests_to_submit.front().curr_request_idx;
        }
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
            if (scenario_index < worker->scenario_stats.size()&&
                request_index < worker->scenario_stats[scenario_index].size())
            {
                ++worker->scenario_stats[scenario_index][request_index]->req_started;
            }
        }
        // if an active timeout is set and this is the last request to be submitted
        // on this connection, start the active timeout.
        if (worker->config->conn_active_timeout > 0. && req_left == 0 && is_controller_client())
        {
            for (auto& client: dest_clients) // "this" is also in dest_clients
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

    if (!should_reconnect_on_disconnect())
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

    if (MAX_STREAM_TO_BE_EXHAUSTED == errCode)
    {
        std::cerr << "stream exhausted on this client. Restart client:" << std::endl;
        write_clear_callback = [this]()
        {
            disconnect();
            resolve_fqdn_and_connect(schema, authority);
        };
        writefn = &Client::write_clear_with_callback;
        terminate_session();
        return;
    }

    if (!should_reconnect_on_disconnect())
    {
        req_left = 0;
    }

    if (streams.size() == 0)
    {
        terminate_session();
    }

    std::cerr << "Process Request Failure:" << worker->stats.req_failed
              << ", errorCode: " << errCode
              << std::endl;
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

void Client::report_app_info()
{
    if (worker->id == 0 && !worker->app_info_report_done)
    {
        worker->app_info_report_done = true;
        std::cerr << "Application protocol: " << selected_proto << std::endl;
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
    size_t scenario_index = 0;
    size_t request_index = 0;
    if (worker->scenario_stats.size() > 0)
    {
        auto request_data = requests_awaiting_response.find(stream_id);
        if (request_data != requests_awaiting_response.end())
        {
            scenario_index = request_data->second.scenario_index;
            request_index = request_data->second.curr_request_idx;
        }
    }

    if (streams.count(stream_id))
    {
        streams.erase(stream_id);
    }
    bool stats_eligible = (worker->current_phase == Phase::MAIN_DURATION || worker->current_phase == Phase::MAIN_DURATION_GRACEFUL_SHUTDOWN);
    streams.insert(std::make_pair(stream_id, Stream(scenario_index, request_index, stats_eligible)));
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
        auto stream_it = streams.find(it->second);
        if (stream_it != streams.end())
        {
            session->submit_rst_stream(it->second);
            if (stream_it->second.statistics_eligible)
            {
                worker->stats.req_timedout++;
            }
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

void Client::inc_status_counter_and_validate_response(int32_t stream_id)
{
    uint16_t status = 0;
    auto itr = streams.find(stream_id);
    if (itr == std::end(streams))
    {
        return;
    }
    auto& stream = (*itr).second;

    if (!stream.statistics_eligible)
    {
        stream.status_success = 1;
        return;
    }

    status = stream.req_stat.status;

    if (status < 600)
    {
        ++worker->stats.status[status / 100];
    }

    if (status < 400)
    {
        stream.status_success = 1;
    }
    else
    {
        stream.status_success = 0;
    }

    auto request_data = requests_awaiting_response.find(stream_id);
    if (request_data != requests_awaiting_response.end())
    {
        auto scenario_index = request_data->second.scenario_index;
        auto request_index = request_data->second.curr_request_idx;
        if (scenario_index < worker->scenario_stats.size() &&
            request_index < worker->scenario_stats[scenario_index].size()&&
            status < 600)
        {
            auto& stats = worker->scenario_stats[scenario_index][request_index];
            ++stats->status[status / 100];
        }
        if (config->json_config_schema.scenarios[scenario_index].requests[request_index].validate_response_function_present)
        {
            stream.status_success = validate_response_with_lua(lua_states[scenario_index][request_index], request_data->second);
        }
        else if (config->json_config_schema.scenarios[scenario_index].requests[request_index].response_match_rules.size())
        {
            auto& request = config->json_config_schema.scenarios[scenario_index].requests[request_index];
            bool run_match_rule = true;
            bool matched = false;
            rapidjson::Document json_payload;
            if (request.response_match.payload_match.size())
            {
                json_payload.Parse(request_data->second.resp_payload.c_str());
                if (json_payload.HasParseError())
                {
                    run_match_rule = false;
                }
            }
            if (run_match_rule)
            {
                for (auto& match_rule: request.response_match_rules)
                {
                    matched = match_rule.match(request_data->second.resp_headers, json_payload);
                    if (!matched)
                    {
                        break;
                    }
                }
            }
            if (matched)
            {
                stream.status_success = 1;
            }
            else
            {
                stream.status_success = 0;
            }
        }
        else if (request_data->second.expected_status_code)
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
    if (stream.status_success == 0)
    {
        log_failed_request(*config, request_data->second, stream_id);
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

void Client::update_scenario_based_stats(size_t scenario_index, size_t request_index, bool success, bool status_success)
{
    if (worker->scenario_stats.size() == 0)
    {
        return;
    }
    auto& stats = worker->scenario_stats[scenario_index][request_index];
    ++stats->req_done;
    if (success)
    {
        ++stats->req_success;
        if (status_success == 1)
        {
            ++stats->req_status_success;
        }
        else
        {
            ++stats->req_failed;
        }
    }
    else
    {
        ++stats->req_failed;
        ++stats->req_error;
    }
    /*

    auto resp_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            req_stat->stream_close_time - req_stat->request_time).count();
    worker->stats.max_resp_time_ms = std::max(worker->stats.max_resp_time_ms.load(), resp_time_ms);
    worker->stats.min_resp_time_ms = std::min(worker->stats.min_resp_time_ms.load(), resp_time_ms);
    worker->stats.total_resp_time_ms += resp_time_ms;

    stats->max_resp_time_ms = std::max(stats->max_resp_time_ms.load(), resp_time_ms);
    stats->min_resp_time_ms = std::min(stats->min_resp_time_ms.load(), resp_time_ms);
    stats->total_resp_time_ms += resp_time_ms;
    */
}

void Client::on_stream_close(int32_t stream_id, bool success, bool final)
{
    record_stream_close_time(stream_id);

    brief_log_to_file(stream_id, success);

    inc_status_counter_and_validate_response(stream_id);

    auto finished_request = requests_awaiting_response.find(stream_id);

    if (worker->current_phase == Phase::MAIN_DURATION ||
        worker->current_phase == Phase::MAIN_DURATION_GRACEFUL_SHUTDOWN)
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
        if (streams.count(stream_id) && streams.at(stream_id).statistics_eligible)
        {
            if (success)
            {
                req_stat->completed = true;
                ++worker->stats.req_success;
                ++cstat.req_success;

                if (streams.count(stream_id) && streams.at(stream_id).status_success == 1)
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

            if (finished_request != requests_awaiting_response.end())
            {
                bool status_success = (streams.count(stream_id) && streams.at(stream_id).status_success == 1) ? true : false;
                update_scenario_based_stats(finished_request->second.scenario_index,
                                            finished_request->second.curr_request_idx,
                                            success, status_success);
            }
        }

    }

    worker->report_progress();

    if (finished_request != requests_awaiting_response.end())
    {
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
    }
    if (config->verbose)
    {
        std::string str((const char*)data, len);
        std::cout<<"received data: "<<std::endl<<str<<std::endl;
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
            report_app_info();
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

        report_app_info();
    }

    state = CLIENT_CONNECTED;

    session->on_connect();

    record_connect_time();

    update_this_in_dest_client_map();

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
        rps_watcher.repeat = std::max(0.01, 1. / rps);
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

    if (authority != preferred_authority && config->json_config_schema.connect_back_to_preferred_host)
    {
        ev_timer_start(worker->loop, &connect_to_preferred_host_watcher);
    }

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

int Client::connected()
{
    if (!util::check_socket_connected(fd))
    {
        std::cerr<<"check_socket_connected failed"<<std::endl;
        return ERR_CONNECT_FAIL;
    }
    std::cerr<<"===============connected to "<<authority<<"==============="<<std::endl;

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
                                                                  size_t scenario_index,
                                                                  size_t index_in_config_template)
{
    auto& request_template = config->json_config_schema.scenarios[scenario_index].requests[index_in_config_template];

    new_request.method = &request_template.method;
    new_request.schema = &request_template.schema;
    new_request.authority = &request_template.authority;
    new_request.string_collection.emplace_back(reassemble_str_with_variable(config, scenario_index, index_in_config_template,
                                                                            request_template.tokenized_payload,
                                                                            new_request.user_id));
    new_request.req_payload = &(new_request.string_collection.back());
    new_request.req_headers = &request_template.headers_in_map;
    new_request.expected_status_code = request_template.expected_status_code;
    new_request.delay_before_executing_next = request_template.delay_before_executing_next;
}

size_t Client::get_index_of_next_scenario_to_run()
{
    if (config->json_config_schema.scenarios.size() <= 1)
    {
        return 0;
    }
    auto init_size_vec = [](Config* config)
    {
        std::vector<size_t> vec;
        for (size_t scenario_index = 0; scenario_index < config->json_config_schema.scenarios.size(); scenario_index++)
        {
            vec.push_back(config->json_config_schema.scenarios[scenario_index].requests.size());;
        }
        return vec;
    };
    auto init_schedule_map = [](Config* config, uint64_t common_multiple)
    {
        std::map<uint64_t, size_t> schedule_map;
        uint64_t totalWeight = 0;
        for (size_t scenario_index = 0; scenario_index < config->json_config_schema.scenarios.size(); scenario_index++)
        {
            auto& scenario = config->json_config_schema.scenarios[scenario_index];
            totalWeight += ((scenario.weight * common_multiple)/scenario.requests.size());
            schedule_map[totalWeight] = scenario_index;
        }
        return schedule_map;
    };

    static thread_local auto schedule_map = init_schedule_map(config, find_common_multiple(init_size_vec(config)));
    static thread_local auto total_weight = (schedule_map.rbegin()->first);
    static thread_local std::random_device                  randDev;
    static thread_local std::mt19937                        generator(randDev());
    static thread_local std::uniform_int_distribution<int>  distr(0, total_weight - 1);

    size_t scenario_index = 0;
    uint64_t randomNumber = distr(generator);
    auto iter = schedule_map.upper_bound(randomNumber);
    if (iter != schedule_map.end())
    {
        scenario_index = iter->second;
    }
    return scenario_index;

}

Request_Data Client::prepare_first_request()
{
    auto controller = parent_client ? parent_client : this;

    size_t scenario_index = get_index_of_next_scenario_to_run();

    auto& scenario = config->json_config_schema.scenarios[scenario_index];

    static thread_local Request_Data dummy_data;

    Request_Data new_request;
    new_request.scenario_index = scenario_index;

    size_t curr_index = 0;
    new_request.curr_request_idx = curr_index;

    new_request.user_id = controller->runtime_scenario_data[scenario_index].curr_req_variable_value;
    if (controller->runtime_scenario_data[scenario_index].req_variable_value_end)
    {
        controller->runtime_scenario_data[scenario_index].curr_req_variable_value++;
        if (controller->runtime_scenario_data[scenario_index].curr_req_variable_value >= controller->runtime_scenario_data[scenario_index].req_variable_value_end)
        {
            std::cerr<<"user id (variable_value) wrapped, start over from range start"<<", scenario index: "<<scenario_index<<std::endl;
            controller->runtime_scenario_data[scenario_index].curr_req_variable_value = controller->runtime_scenario_data[scenario_index].req_variable_value_start;
        }
    }

    populate_request_from_config_template(new_request, scenario_index, curr_index);

    auto& request_template = scenario.requests[curr_index];
    new_request.string_collection.emplace_back(reassemble_str_with_variable(config, scenario_index, curr_index,
                                                                            request_template.tokenized_path,
                                                                            new_request.user_id));
    new_request.path = &(new_request.string_collection.back());

    if (scenario.requests[curr_index].make_request_function_present)
    {
        if (!update_request_with_lua(lua_states[scenario_index][curr_index], dummy_data, new_request))
        {
          std::cerr << "lua script failure for first request, cannot continue, exit"<< std::endl;
          exit(EXIT_FAILURE);
        }
    }
    update_content_length(new_request);

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
        std::cerr<<"this is not expected; contact support to report this error"<<std::endl;
        printBacktrace();
        abort(); // this should never happen
        return prepare_first_request();
    }
}

bool Client::prepare_next_request(Request_Data& finished_request)
{
    size_t scenario_index = finished_request.scenario_index;
    Scenario& scenario = config->json_config_schema.scenarios[scenario_index];

    size_t curr_index = ((finished_request.curr_request_idx+ 1) % scenario.requests.size());
    if (curr_index == 0)
    {
        return false;
    }


    Request_Data new_request;
    new_request.scenario_index = scenario_index;
    new_request.curr_request_idx = curr_index;

    auto& request_template = scenario.requests[curr_index];
    new_request.user_id = finished_request.user_id;
    populate_request_from_config_template(new_request, scenario_index, curr_index);

    if (request_template.uri.typeOfAction == "input")
    {
         new_request.string_collection.emplace_back(reassemble_str_with_variable(config, scenario_index, curr_index,
                                                                                 request_template.tokenized_path,
                                                                                 new_request.user_id));
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
        if (!update_request_with_lua(lua_states[scenario_index][curr_index], finished_request, new_request))
        {
            return false; // lua script returns error or kills the request, abort this scenario
        }
    }

    update_content_length(new_request);

    enqueue_request(finished_request, std::move(new_request));

    return true;
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
    lua_getglobal(L, make_request);
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
        lua_settop(L, 0);
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

int Client::resolve_fqdn_and_connect(const std::string& schema, const std::string& authority, ares_addrinfo_callback callback)
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
        std::cerr<<"===============connecting to "<<schema<<"://"<<authority<<"==============="<<std::endl;
    }
    return resolve_fqdn_and_connect(schema, authority);
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
            std::cerr<<"try with preferred host: "<<authority<<std::endl;
            resolve_fqdn_and_connect(schema, authority);
        }
        else if (candidate_addresses.size())
        {
            authority = std::move(candidate_addresses.front());
            candidate_addresses.pop_front();
            std::cerr<<"switching to candidate host: "<<authority<<std::endl;
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



bool Client::is_test_finished()
{
    if (0 == req_left || worker->current_phase == Phase::DURATION_OVER)
    {
        return true;
    }
    else
    {
        return false;
    }
}

bool Client::probe_address(ares_addrinfo* ares_addr)
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
    if (ares_addr)
    {
        auto& addr = ares_addr->nodes;
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

void Client::update_this_in_dest_client_map()
{
    auto& clients = parent_client ? parent_client->dest_clients : dest_clients;
    for( auto it = clients.begin(); it != clients.end(); )
    {
      if(it->second == this)
      {
          clients.erase(it);
          break;
      }
      else
      {
          ++it;
      }
    }
    std::string dest = schema;
    dest.append("://").append(authority);
    clients[dest] = this;
}

bool Client::should_reconnect_on_disconnect()
{
    if (!is_test_finished())
    {
        if (is_controller_client() && (dest_clients.size() > 1))
        {
            return true;
        }
        if (config->json_config_schema.connection_retry_on_disconnect)
        {
            return true;
        }
    }
    return false;
}

void Client::submit_ping()
{
    session->submit_ping();
    signal_write();
}

bool Client::rps_mode()
{
    return (rps > 0.0);
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

void Client::restore_connectfn()
{
    connectfn = &Client::connect;
}

void Client::slice_user_id()
{
  if (config->nclients > 1 && (is_controller_client()))
  {
      for (size_t index = 0; index < config->json_config_schema.scenarios.size(); index++)
      {
          auto& scenario = config->json_config_schema.scenarios[index];
          Runtime_Scenario_Data scenario_data;
          if (!scenario.variable_range_slicing)
          {
              std::random_device                  rand_dev;
              std::mt19937                        generator(rand_dev());
              std::uniform_int_distribution<uint64_t>  distr(scenario.variable_range_start,
                                                             scenario.variable_range_end);
              scenario_data.curr_req_variable_value = distr(generator);
              scenario_data.req_variable_value_start = scenario.variable_range_start;
              scenario_data.req_variable_value_end = scenario.variable_range_end;
          }
          else
          {
              auto tokens_per_client = ((scenario.variable_range_end - scenario.variable_range_start)/(config->nclients));
              if (tokens_per_client == 0)
              {
                  if (worker->id == 0)
                  {
                      std::cerr<<"Error: number of user IDs is smaller than number of clients, cannot continue"<<std::endl;
                      exit(EXIT_FAILURE);
                  }
                  else
                  {
                      std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                      return;
                  }
              }
              auto tokens_left = ((scenario.variable_range_end - scenario.variable_range_start)%(config->nclients));
              scenario_data.req_variable_value_start = scenario.variable_range_start +
                                                       (this_client_id * tokens_per_client) +
                                                       std::min(this_client_id, tokens_left);
              scenario_data.req_variable_value_end = scenario_data.req_variable_value_start +
                                                     tokens_per_client +
                                                     (this_client_id >= tokens_left ? 0: 1);

              scenario_data.curr_req_variable_value = scenario_data.req_variable_value_start;

              if (config->verbose)
              {
                  std::cerr<<", client Id:"<<this_client_id
                           <<", scenario index: " << index
                           <<", variable id start:"<<scenario_data.req_variable_value_start
                           <<", variable id end:"<<scenario_data.req_variable_value_end
                           <<std::endl;
              }
          }
          runtime_scenario_data.push_back(scenario_data);
      }
  }
  else
  {
      for (size_t index = 0; index < config->json_config_schema.scenarios.size(); index++)
      {
          auto& scenario = config->json_config_schema.scenarios[index];
          Runtime_Scenario_Data scenario_data;
          scenario_data.curr_req_variable_value = scenario.variable_range_start;
          scenario_data.req_variable_value_start = scenario.variable_range_start;
          scenario_data.req_variable_value_end = scenario.variable_range_end;
          runtime_scenario_data.push_back(scenario_data);
      }
  }
}

void Client::log_failed_request(const h2load::Config& config, const h2load::Request_Data& failed_req, int32_t stream_id)
{
    if (config.json_config_schema.failed_request_log_file.empty())
    {
        return;
    }

    auto req_stat = get_req_stat(stream_id);
    if (!req_stat)
    {
        return;
    }

    static boost::asio::io_service work_offload_io_service;
    static boost::thread_group work_offload_thread_pool;
    static boost::asio::io_service::work work(work_offload_io_service);
    auto create_log_thread = []()
    {
        work_offload_thread_pool.create_thread(boost::bind(&boost::asio::io_service::run, &work_offload_io_service));
        return true;
    };
    static bool dummyCode = create_log_thread();
    static std::ofstream log_file(config.json_config_schema.failed_request_log_file);

    std::stringstream ss;

    auto start_c = std::chrono::system_clock::to_time_t(req_stat->request_wall_time);
    ss << "start timestamp: "<<std::put_time(std::localtime(&start_c), "%F %T")<< std::endl;

    auto now = std::chrono::system_clock::now();
    auto now_c = std::chrono::system_clock::to_time_t(now);
    ss << "current time: "<<std::put_time(std::localtime(&now_c), "%F %T")<< std::endl;

    auto stream_response_interval_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    req_stat->stream_close_time - req_stat->request_time).count();
    ss << "duration(ms): "<<stream_response_interval_ms<< std::endl;

    ss<<failed_req;


    auto log_func = [](const std::string& msg)
    {
        log_file << msg;
        log_file << std::endl;
    };
    auto log_routine = std::bind(log_func, ss.str());

    work_offload_io_service.post(log_routine);
}

bool Client::validate_response_with_lua(lua_State* L, const Request_Data& finished_request)
{
    lua_getglobal(L, validate_response);
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

        lua_pushlstring(L, finished_request.resp_payload.c_str(), finished_request.resp_payload.size());
        lua_pcall(L, 2, 1, 0);
        int top = lua_gettop(L);
        for (int i = 0; i < top; i++)
        {
            auto type = lua_type(L, -1);
            switch (type)
            {
                case LUA_TBOOLEAN:
                {
                    retCode = lua_toboolean(L, -1);
                    break;
                }
                default:
                {
                    std::cerr << "error occured in lua function validate_response" << std::endl;
                    retCode = false;
                    break;
                }
            }
            lua_pop(L, 1);
        }
    }
    else
    {
        lua_settop(L, 0);
    }
    return retCode;
}

void Client::record_stream_close_time(int32_t stream_id)
{
    auto req_stat = get_req_stat(stream_id);
    if (!req_stat)
    {
        return;
    }
    req_stat->stream_close_time = std::chrono::steady_clock::now();
}

void Client::brief_log_to_file(int32_t stream_id, bool success)
{
    if (worker->config->log_fd != -1)
    {
        auto req_stat = get_req_stat(stream_id);
        if (!req_stat)
        {
            return;
        }
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


}
