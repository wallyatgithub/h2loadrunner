#ifdef _WINDOWS
#include <sdkddkver.h>
#endif
#include <boost/asio/io_service.hpp>
#include <boost/thread/thread.hpp>
#include <regex>
#include <algorithm>
#include <cctype>
#ifndef _WINDOWS
#include <execinfo.h>
#endif
#include <iomanip>
#include <string>
#include <random>
#include <numeric>
#include <cassert>

#include <iostream>

#ifdef HAVE_LIBNGTCP2_CRYPTO_OPENSSL
#  include <ngtcp2/ngtcp2_crypto_openssl.h>
#endif // HAVE_LIBNGTCP2_CRYPTO_OPENSSL
#ifdef HAVE_LIBNGTCP2_CRYPTO_BORINGSSL
#  include <ngtcp2/ngtcp2_crypto_boringssl.h>
#endif // HAVE_LIBNGTCP2_CRYPTO_BORINGSSL

#include <openssl/err.h>
#include <openssl/rand.h>

#ifdef ENABLE_HTTP3
#include "h2load_http3_session.h"
#endif

#include "tls.h"
#include "base_client.h"
#include "base_worker.h"

#include "h2load_utils.h"
#include "h2load_lua.h"


namespace h2load
{
std::atomic<uint64_t> Unique_Id::client_unique_id(0);

Unique_Id::Unique_Id()
{
    my_id = client_unique_id++;
}

base_client::base_client(uint32_t id, base_worker* wrker, size_t req_todo, Config* conf,
                         SSL_CTX* ssl_ctx, base_client* parent, const std::string& dest_schema,
                         const std::string& dest_authority, PROTO_TYPE proto):
    worker(wrker),
    cstat(),
    config(conf),
    reqidx(0),
    state(CLIENT_IDLE),
    req_todo(req_todo),
    req_left(req_todo),
    req_inflight(0),
    req_started(0),
    req_done(0),
    id(id),
    conn_normal_close_restart_to_be_done(false),
    final(false),
    rps_req_pending(0),
    rps_req_inflight(0),
    ssl_context(ssl_ctx),
    schema(dest_schema),
    authority(dest_authority),
    proto_type(proto),
    rps(conf->rps),
    this_client_id(),
    time_point_of_last_rps_timer_expiry(),
#ifdef ENABLE_HTTP3
    quic {},
#endif
    ssl(nullptr)
{
    init_req_left();

    slice_var_ids();

    init_lua_states();

    parent_client = worker->get_shared_ptr_of_client(parent);

    update_this_in_dest_client_map();

#ifdef ENABLE_HTTP3
    ngtcp2_connection_close_error_default(&quic.last_error);
#endif // ENABLE_HTTP3
}

base_client::~base_client()
{
#ifdef ENABLE_HTTP3
    if (is_quic())
    {
        quic_free();
    }
#endif // ENABLE_HTTP3
}

int base_client::connect()
{
    if (is_test_finished())
    {
        return -1;
    }
    if (!config->is_timing_based_mode() ||
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
        start_warmup_timer();
    }

    if (config->conn_inactivity_timeout > 0.)
    {
        start_conn_inactivity_watcher();
    }

    auto rv = connect_to_host(schema, authority);
    if (rv != 0)
    {
        return rv;
    }

    start_connect_timeout_timer();
    return 0;
}

uint64_t base_client::get_client_unique_id()
{
    return this_client_id.my_id;
}

uint64_t base_client::get_total_pending_streams()
{
    if (parent_client != nullptr)
    {
        return parent_client->get_total_pending_streams();
    }
    else
    {
        auto pendingStreams = streams.size();
        std::set<base_client*> clients_handled;
        clients_handled.insert(this);
        for (auto& clients_with_same_proto_version : client_registry)
        {
            for (auto& client : clients_with_same_proto_version.second)
            {
                if (clients_handled.count(client.second) == 0)
                {
                    pendingStreams += client.second->streams.size();
                    clients_handled.insert(client.second);
                }
            }
        }
        return pendingStreams;
    }
}

void base_client::set_open_new_conn_needed()
{
    conn_normal_close_restart_to_be_done = true;
}

void base_client::record_ttfb()
{
    if (recorded(cstat.ttfb))
    {
        return;
    }

    cstat.ttfb = std::chrono::steady_clock::now();
}

void base_client::log_failed_request(const h2load::Config& config, const h2load::Request_Response_Data& failed_req,
                                     int64_t stream_id)
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
    ss << "start timestamp: " << std::put_time(std::localtime(&start_c), "%F %T") << std::endl;

    auto now = std::chrono::system_clock::now();
    auto now_c = std::chrono::system_clock::to_time_t(now);
    ss << "current time: " << std::put_time(std::localtime(&now_c), "%F %T") << std::endl;

    auto stream_response_interval_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                           req_stat->stream_close_time - req_stat->request_time).count();
    ss << "duration(ms): " << stream_response_interval_ms << std::endl;

    ss << failed_req;


    auto log_func = [](const std::string & msg)
    {
        log_file << msg;
        log_file << std::endl;
    };
    auto log_routine = std::bind(log_func, ss.str());

    work_offload_io_service.post(log_routine);
}

bool base_client::validate_response_with_lua(lua_State* L, const Request_Response_Data& finished_request)
{
    lua_getglobal(L, validate_response);
    bool retCode = true;
    if (lua_isfunction(L, -1))
    {
        lua_createtable(L, 0, std::accumulate(finished_request.resp_headers.begin(),
                                              finished_request.resp_headers.end(),
                                              0,
                                              [](uint64_t sum, const std::map<std::string, std::string, ci_less>& val)
        {
            return sum + val.size();
        }));
        for (auto& header_map : finished_request.resp_headers)
        {
            for (auto& header : header_map)
            {
                lua_pushlstring(L, header.first.c_str(), header.first.size());
                lua_pushlstring(L, header.second.c_str(), header.second.size());
                lua_rawset(L, -3);
            }
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

void base_client::record_stream_close_time(int64_t stream_id)
{
    auto req_stat = get_req_stat(stream_id);
    if (!req_stat)
    {
        return;
    }
    req_stat->stream_close_time = std::chrono::steady_clock::now();
}

void base_client::connection_timeout_handler()
{
    stop_connect_timeout_timer();
    fail();
    reconnect_to_other_or_same_addr();
    //ev_break (EV_A_ EVBREAK_ALL);
}

void base_client::reconnect_to_used_host()
{
    if (CLIENT_IDLE != state)
    {
        return;
    }
    if (used_addresses.size())
    {
        authority = std::move(used_addresses.front());
        used_addresses.pop_front();
        std::cerr << "switch to used host: " << authority << std::endl;
        connect_to_host(schema, authority);
    }
    else
    {
        std::cerr << "retry current host: " << authority << std::endl;
        connect_to_host(schema, authority);
    }
}

bool base_client::reconnect_to_other_or_same_addr()
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
            connect_to_host(schema, authority);
        }
        else if (candidate_addresses.size())
        {
            authority = std::move(candidate_addresses.front());
            candidate_addresses.pop_front();
            std::cerr << "switching to candidate host: " << authority << std::endl;
            connect_to_host(schema, authority);
        }
        else
        {
            start_delayed_reconnect_timer();
        }
        return true;
    }
    return false;
}

void base_client::timing_script_timeout_handler()
{
    reset_timeout_requests();

    if (streams.size() >= (size_t)config->max_concurrent_streams)
    {
        stop_timing_script_request_timeout_timer();
        return;
    }

    if (submit_request() != 0)
    {
        stop_timing_script_request_timeout_timer();
        return;
    }
    signal_write();

    if (get_number_of_request_left() == 0)
    {
        stop_timing_script_request_timeout_timer();
        return;
    }


    auto start = get_controller_client()->reqidx ? config->timings[get_controller_client()->reqidx - 1] : 0;
    double duration =
        config->timings[get_controller_client()->reqidx] - start;

    while (duration < 1e-9)
    {
        if (submit_request() != 0)
        {
            stop_timing_script_request_timeout_timer();
            return;
        }
        signal_write();
        if (get_number_of_request_left() == 0)
        {
            stop_timing_script_request_timeout_timer();
            return;
        }

        duration =
            config->timings[get_controller_client()->reqidx] - config->timings[get_controller_client()->reqidx - 1];
    }

    start_timing_script_request_timeout_timer(duration);
}

void base_client::brief_log_to_file(int64_t stream_id, bool success)
{
    if (!config->json_config_schema.log_file.size())
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
    static bool dummy = create_log_thread();
    static std::ofstream log_file(config->json_config_schema.log_file);

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

    auto log_func = [](const std::string & msg)
    {
        log_file << msg;
    };
    auto log_routine = std::bind(log_func, std::string((const char*)buf.data(), nwrite));

    work_offload_io_service.post(log_routine);

}

void base_client::init_req_left()
{
    if (req_todo == 0)   // this means infinite number of requests are to be made
    {
        // This ensures that number of requests are unbounded
        // Just a positive number is fine, we chose the first positive number
        req_left = 1;
    }
}

void base_client::init_connection_targert()
{
    if (schema.empty())
    {
        schema = config->scheme;
    }
    if (authority.empty())
    {
        auto host =
            config->connect_to_host.empty() ? config->host : config->connect_to_host;
        if (is_it_an_ipv6_address(host))
        {
            std::string buffer = "[";
            buffer.append(host).append("]");
            host = buffer;
        }
        /*
                if (config->port != config->default_port)
                {
                    authority = host + ":" + util::utos(config->port);
                }
                else
                {
                    authority = host;
                }
        */
        if ((schema == schema_http && 80 == config->port) || (schema == schema_https && 443 == config->port))
        {
            authority = host;
        }
        else
        {
            authority = host + ":" + util::utos(config->port);
        }
    }

    if (is_controller_client() &&
        config->json_config_schema.load_share_hosts.size())
    {
        auto init_hosts = [this]()
        {
            std::vector<std::string> hosts;
            for (auto& host_item : config->json_config_schema.load_share_hosts)
            {
                std::string host = host_item.host;
                if (is_it_an_ipv6_address(host))
                {
                    std::string buffer = "[";
                    buffer.append(host).append("]");
                    host = buffer;
                }

                hosts.push_back(host);
                if (host_item.port)
                {
                    hosts.back().append(":").append(std::to_string(host_item.port));
                }
            }
            if (std::find(std::begin(hosts), std::end(hosts), authority) == std::end(hosts))
            {
                hosts.push_back(authority);
            }
            return hosts;
        };
        static std::vector<std::string> hosts = init_hosts();
        auto startIndex = this_client_id.my_id % hosts.size();
        authority = hosts[startIndex];
        clear_default_addr_info();
        for (auto count = 0; count < hosts.size() - 1; count++)
        {
            candidate_addresses.push_back(hosts[(++startIndex) % hosts.size()]);
        }
        std::random_device random_device;
        std::mt19937 generator(random_device());
        std::shuffle(candidate_addresses.begin(), candidate_addresses.end(), generator);
    }
    preferred_authority = authority;
}

void base_client::set_prefered_authority(const std::string& authority)
{
    preferred_authority = authority;
}

void base_client::on_status_code(int64_t stream_id, uint16_t status)
{
    auto itr = streams.find(stream_id);
    if (itr != std::end(streams))
    {
        auto& stream = (*itr).second;
        stream.req_stat.status = status;
        if (stream.request_response)
        {
            stream.request_response->status_code = status;
        }
    }
}

void base_client::sanitize_request(Request_Response_Data& new_request)
{
    if (!new_request.schema || new_request.schema->empty())
    {
        new_request.string_collection.emplace_back(schema);
        new_request.schema = &(new_request.string_collection.back());
    }
    if (!new_request.authority || new_request.authority->empty())
    {
        new_request.string_collection.emplace_back(authority);
        new_request.authority = &(new_request.string_collection.back());
    }
}

void base_client::move_queued_request_to_controller_queue(bool immediate_schedule)
{
    if (get_controller_client() == this)
    {
        return;
    }
    while (requests_to_submit.size())
    {
        if (immediate_schedule)
        {
            get_controller_client()->requests_to_submit.push_front(std::move(requests_to_submit.back()));
            requests_to_submit.pop_back();
        }
        else
        {
            get_controller_client()->requests_to_submit.push_back(std::move(requests_to_submit.front()));
            requests_to_submit.pop_front();
        }
    }
}

const std::unique_ptr<Request_Response_Data>& base_client::enqueue_request(const Request_Response_Data& finished_request, std::unique_ptr<Request_Response_Data>& new_request)
{
    auto client = get_controller_client();
    if (!finished_request.delay_before_executing_next)
    {
        client->requests_to_submit.push_back(std::move(new_request));
        return client->requests_to_submit.back();
    }
    else
    {
        const std::chrono::milliseconds delay_duration(finished_request.delay_before_executing_next);
        std::chrono::steady_clock::time_point curr_time_point = std::chrono::steady_clock::now();
        std::chrono::steady_clock::time_point timeout_timepoint = curr_time_point + delay_duration;
        return client->delayed_requests_to_submit.insert(std::make_pair(timeout_timepoint, std::move(new_request)))->second;
    }
}

bool base_client::should_reconnect_on_disconnect()
{
    if (!is_test_finished())
    {
        /*
        if (is_controller_client())
        {
            return true;
        }
        */
        if (config->json_config_schema.connection_retry_on_disconnect)
        {
            return true;
        }
    }
    return false;
}

void base_client::inc_status_counter_and_validate_response(int64_t stream_id)
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

    auto& request_data = stream.request_response;
    if (request_data)
    {
        auto scenario_index = request_data->scenario_index;
        auto request_index = request_data->curr_request_idx;
        if (scenario_index < worker->scenario_stats.size() &&
            request_index < worker->scenario_stats[scenario_index].size() &&
            status < 600)
        {
            auto& stats = worker->scenario_stats[scenario_index][request_index];
            ++stats->status[status / 100];
        }
        if (config->json_config_schema.scenarios[scenario_index].requests[request_index].validate_response_function_present)
        {
            stream.status_success = validate_response_with_lua(lua_states[scenario_index][request_index], *request_data);
        }
        else if (config->json_config_schema.scenarios[scenario_index].requests[request_index].response_match_rules.size())
        {
            auto& request = config->json_config_schema.scenarios[scenario_index].requests[request_index];
            bool run_match_rule = true;
            bool matched = false;
            rapidjson::Document json_payload;
            if (request.response_match.payload_match.size())
            {
                json_payload.Parse(request_data->resp_payload.c_str());
                if (json_payload.HasParseError())
                {
                    run_match_rule = false;
                }
            }
            if (run_match_rule)
            {
                for (auto& match_rule : request.response_match_rules)
                {
                    matched = match_rule.match(request_data->resp_headers, json_payload);
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
        else if (request_data->expected_status_code)
        {
            if (status == request_data->expected_status_code)
            {
                stream.status_success = 1;
            }
            else
            {
                stream.status_success = 0;
            }
        }
    }
    if (stream.status_success == 0)
    {
        log_failed_request(*config, *request_data, stream_id);
    }
}


int base_client::try_again_or_fail()
{
    if (conn_normal_close_restart_to_be_done)
    {
        conn_normal_close_restart_to_be_done = false;

        if (get_number_of_request_left())
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
            if (connect_to_host(schema, authority) == 0)
            {
                return 0;
            }
            if (!is_test_finished())
            {
                std::cerr << "connect to host cannot be done:" << authority << std::endl;
            }
        }
    }

    return -1;
}

void base_client::fail()
{
    disconnect();

    process_abandoned_streams();
}

void base_client::timeout()
{
    if (should_reconnect_on_disconnect())
    {
        // it will need to re-connect anyway, why bother
        return;
    }
    process_timedout_streams();

    disconnect();
}

void base_client::process_abandoned_streams()
{
    while (streams.size())
    {
        auto begin = streams.begin();
        on_stream_close(begin->first, false);
    }

    if (worker->current_phase != Phase::MAIN_DURATION)
    {
        return;
    }

    auto req_abandoned = req_inflight;

    if (is_test_finished() && is_controller_client())
    {
        req_abandoned += get_number_of_request_left();
        get_controller_client()->req_left = 0;
    }

    worker->stats.req_failed += req_abandoned;
    worker->stats.req_error += req_abandoned;

    req_inflight = 0;
}

void base_client::process_requests_to_submit_upon_error(bool fail_all)
{
    if (is_controller_client())
    {
        return;
    }
    if (requests_to_submit.empty())
    {
        get_controller_client()->fail_one_request_of_client(this);
        return;
    }
    while (requests_to_submit.size())
    {
        auto req = std::move(requests_to_submit.front());
        requests_to_submit.pop_front();
        early_fail_of_request(req, this);
        if (!fail_all)
        {
            break;
        }
    }
    move_queued_request_to_controller_queue(true);
}

void base_client::record_connect_start_time()
{
    cstat.connect_start_time = std::chrono::steady_clock::now();
}

void base_client::record_connect_time()
{
    cstat.connect_time = std::chrono::steady_clock::now();
}


void base_client::clear_connect_times()
{
    cstat.connect_start_time = std::chrono::steady_clock::time_point();
    cstat.connect_time = std::chrono::steady_clock::time_point();
    cstat.ttfb = std::chrono::steady_clock::time_point();
}

void base_client::record_client_start_time()
{
    // Record start time only once at the very first connection is going
    // to be made.
    if (recorded(cstat.client_start_time))
    {
        return;
    }

    cstat.client_start_time = std::chrono::steady_clock::now();
}


void base_client::clean_up_this_in_dest_client_map()
{
    auto& controller_client_registry = get_controller_client()->client_registry;
    for (auto clients_with_same_proto = controller_client_registry.begin();
         clients_with_same_proto != controller_client_registry.end();)
    {
        for (auto iter = clients_with_same_proto->second.begin(); iter != clients_with_same_proto->second.end();)
        {
            if (iter->second == this)
            {
                iter = clients_with_same_proto->second.erase(iter);
            }
            else
            {
                iter++;
            }
        }
        if (clients_with_same_proto->second.empty())
        {
            clients_with_same_proto = controller_client_registry.erase(clients_with_same_proto);
        }
        else
        {
            clients_with_same_proto++;
        }
    }

    for (auto& client_map : worker->get_client_pool())
    {
        for (auto& client_set : client_map.second)
        {
            client_set.second.erase(this);
        }
    }
}

void base_client::final_cleanup()
{
    worker->sample_client_stat(&cstat);
    ++worker->client_smp.n;

    stop_rps_timer();
    stop_delayed_execution_timer();
    stop_timing_script_request_timeout_timer();

    for (auto& V : lua_states)
    {
        for (auto& L : V)
        {
            lua_close(L);
        }
    }
    lua_states.clear();

    bool fail_all = true;
    process_requests_to_submit_upon_error(fail_all);

    clean_up_this_in_dest_client_map();
}

void base_client::cleanup_due_to_disconnect()
{
    clean_up_this_in_dest_client_map();

    if (!config->disable_connection_trace)
    {
        if (CLIENT_CONNECTED == state)
        {
            std::cerr << "=============== disconnected from " << authority << "===============" << std::endl;
        }
    }
    if (CLIENT_CONNECTING == state)
    {
        std::cerr << "=============== failed to connect to " << authority << "===============" << std::endl;
    }

    worker->get_client_ids().erase(this->get_client_unique_id());

    record_client_end_time();
    session.reset();
    state = CLIENT_IDLE;

    auto iter = per_stream_user_callbacks.begin();
    while (iter != per_stream_user_callbacks.end())
    {
        auto cb_queue_size_before_cb = per_stream_user_callbacks.size();
        if (iter->second.response_callback)
        {
            iter->second.response_callback();
        }
        auto cb_queue_size_after_cb = per_stream_user_callbacks.size();

        // iter cb function removed itself from the queue, proceed with the new begin()
        if (cb_queue_size_before_cb - cb_queue_size_after_cb >= 1)
        {
            iter =  per_stream_user_callbacks.begin();
        }
        else
        {
            // no removal done by iter cb, manually remove iter and proceed with next
            iter = per_stream_user_callbacks.erase(iter);
        }
    }

    call_connected_callbacks(false);

    bool fail_one_req_done = false;
    while (streams.size())
    {
        auto iter = streams.begin();
        on_stream_close(iter->first, false);
        fail_one_req_done = true;
    }

    if (!conn_normal_close_restart_to_be_done)
    {
        // not a graceful disconnect, fail one unsent request
        process_requests_to_submit_upon_error(false);
    }
}

void base_client::record_client_end_time()
{
    // Unlike client_start_time, we overwrite client_end_time.  This
    // handles multiple connect/disconnect for HTTP/1.1 benchmark.
    cstat.client_end_time = std::chrono::steady_clock::now();
}

void base_client::run_post_response_action(Request_Response_Data& finished_request)
{
    auto& actual_regex_value_pickers =
        config->json_config_schema.scenarios
        [finished_request.scenario_index].requests[finished_request.curr_request_idx].actual_regex_value_pickers;
    rapidjson::Document req_json_payload;
    rapidjson::Document resp_json_payload;
    bool req_json_decoded = false;
    bool resp_json_decoded = false;
    for (auto& value_picker : actual_regex_value_pickers)
    {
        std::string result;
        std::string source;
        switch (value_picker.where_to_pick_up_from)
        {
            case SOURCE_TYPE_RES_HEADER:
            {
                for (auto& header_map : finished_request.resp_headers)
                {
                    auto iter = header_map.find(value_picker.source);
                    if (iter != header_map.end())
                    {
                        source = iter->second;
                        break;
                    }
                }
                break;
            }
            case SOURCE_TYPE_REQ_HEADER:
            {
                auto iter = finished_request.req_headers_of_individual.find(value_picker.source);
                if (iter != finished_request.req_headers_of_individual.end())
                {
                    source = iter->second;
                    break;
                }
                iter = finished_request.req_headers_from_config->find(value_picker.source);
                if (iter != finished_request.req_headers_from_config->end())
                {
                    source = iter->second;
                    break;
                }
                if (source == path_header_name)
                {
                    source = *finished_request.path;
                    break;
                }
                if (source == scheme_header_name)
                {
                    source = *finished_request.schema;
                    break;
                }
                if (source == method_header_name)
                {
                    source = *finished_request.method;
                    break;
                }
                if (source == authority_header_name)
                {
                    source = *finished_request.authority;
                    break;
                }
                break;
            }
            case SOURCE_TYPE_REQ_POINTER:
            {
                if (!req_json_decoded)
                {
                    req_json_payload.Parse(finished_request.req_payload->c_str());
                    req_json_decoded = true;
                }
                source = getValueFromJsonPtr(req_json_payload, value_picker.source);
            }
            case SOURCE_TYPE_RES_POINTER:
            {
                if (!resp_json_decoded)
                {
                    resp_json_payload.Parse(finished_request.resp_payload.c_str());
                    resp_json_decoded = true;
                }
                source = getValueFromJsonPtr(resp_json_payload, value_picker.source);
            }
            default:
            {
            }
        }
        std::smatch match_result;
        if (std::regex_search(source, match_result, value_picker.picker_regexp))
        {
            result = match_result[0];
        }
        else
        {
            result.clear();
        }
        finished_request.scenario_data_per_user->variable_index_to_value[value_picker.unique_id] = std::move(result);
        if (config->verbose)
        {
            std::cout << "where_to_pick_up_from: " << value_picker.where_to_pick_up_from << std::endl;
            std::cout << "target:" << value_picker.source << std::endl;
            std::cout << "var id:" << value_picker.unique_id << std::endl;
            std::cout << "target value located: " << source << std::endl;
            std::cout << "target value match result: " <<
                      finished_request.scenario_data_per_user->variable_index_to_value[value_picker.unique_id] << std::endl;
        }
    }
    if (config->verbose)
    {
        std::cout << "variable_index_to_value size: " << finished_request.scenario_data_per_user->variable_index_to_value.size()
                  << std::endl;
        for (auto i = 0; i < finished_request.scenario_data_per_user->variable_index_to_value.size(); i++)
        {
            std::cout << "var: " << finished_request.scenario_data_per_user->variable_index_to_value[i] << std::endl;
        }
    }
}

void base_client::run_pre_request_action(Request_Response_Data& new_request)
{

}

bool base_client::parse_uri_and_poupate_request(const std::string& uri, Request_Response_Data& new_request)
{
    if (uri.size())
    {
        http_parser_url u {};
        if (http_parser_parse_url(uri.c_str(), uri.size(), 0, &u) != 0)
        {
            std::cerr << "abort whole scenario sequence, as invalid URI found in header: " << uri << std::endl;
            return false;
        }
        else
        {
            new_request.string_collection.emplace_back(get_reqline(uri.c_str(), u));
            new_request.path = &(new_request.string_collection.back());
            if (util::has_uri_field(u, UF_SCHEMA) && util::has_uri_field(u, UF_HOST))
            {
                new_request.string_collection.emplace_back(util::get_uri_field(uri.c_str(), u, UF_SCHEMA).str());
                util::inp_strlower(new_request.string_collection.back());
                new_request.schema = &(new_request.string_collection.back());
                new_request.string_collection.emplace_back(util::get_uri_field(uri.c_str(), u, UF_HOST).str());
                util::inp_strlower(new_request.string_collection.back());
                if (util::has_uri_field(u, UF_PORT))
                {
                    new_request.string_collection.back().append(":").append(util::utos(u.port));
                }
                new_request.authority = &(new_request.string_collection.back());
            }
        }
    }
    return true;
}

void base_client::parse_and_save_cookies(Request_Response_Data& finished_request)
{
    if (!config->json_config_schema.builtin_cookie_support)
    {
        return;
    }
    for (auto& header_map : finished_request.resp_headers)
    {
        auto iter = header_map.find("Set-Cookie");
        if (iter != header_map.end())
        {
            auto new_cookies = Cookie::parse_cookie_string(iter->second,
                                                           *finished_request.authority, *finished_request.schema);
            for (auto& cookie : new_cookies)
            {
                if (Cookie::is_cookie_acceptable(cookie))
                {
                    finished_request.scenario_data_per_user->saved_cookies[cookie.cookie_key] = std::move(cookie);
                }
            }
        }
    }
}


void base_client::populate_request_from_config_template(Request_Response_Data& new_request,
                                                        size_t scenario_index,
                                                        size_t request_index)
{
    auto& request_template = config->json_config_schema.scenarios[scenario_index].requests[request_index];

    new_request.method = &request_template.method;
    new_request.schema = &request_template.schema;
    new_request.authority = &request_template.authority;
    new_request.string_collection.emplace_back(assemble_string(request_template.tokenized_payload_with_vars, scenario_index,
                                                               request_index, *new_request.scenario_data_per_user));

    new_request.req_payload = &(new_request.string_collection.back());

    new_request.req_headers_from_config = &request_template.headers_in_map;
    new_request.expected_status_code = request_template.expected_status_code;
    new_request.delay_before_executing_next = request_template.delay_before_executing_next;
    new_request.proto_type = request_template.proto_type;
    for (auto& h : request_template.headers_with_variable)
    {
        auto name = assemble_string(h.first, scenario_index, request_index, *new_request.scenario_data_per_user);
        auto value = assemble_string(h.second, scenario_index, request_index, *new_request.scenario_data_per_user);
        switch (name[0])
        {
            case ':':
            {
                if (name == authority_header)
                {
                    new_request.string_collection.emplace_back(value);
                    new_request.authority = &(new_request.string_collection.back());
                    continue;
                }
                else if (name == method_header)
                {
                    new_request.string_collection.emplace_back(value);
                    new_request.method = &(new_request.string_collection.back());
                    continue;
                }
                else if (name == path_header)
                {
                    new_request.string_collection.emplace_back(value);
                    new_request.path = &(new_request.string_collection.back());
                    continue;
                }
                else if (name == scheme_header)
                {
                    new_request.string_collection.emplace_back(value);
                    new_request.schema = &(new_request.string_collection.back());
                    continue;
                }
                break;
            }
            case 'h':
            case 'H':
            {
                auto header_name = name;
                util::inp_strlower(header_name);
                if (header_name == host_header)
                {
                    new_request.string_collection.emplace_back(value);
                    new_request.authority = &(new_request.string_collection.back());
                    continue;
                }
                break;
            }
            default:
            {
            }
        }
        new_request.req_headers_of_individual.emplace(std::make_pair(name, value));
    }
}

void base_client::terminate_sub_clients()
{
    std::set<base_client*> client_to_terminate;
    for (auto& sub_clients_with_same_proto : client_registry)
    {
        for (auto& sub_client : sub_clients_with_same_proto.second)
        {
            if (sub_client.second != this && sub_client.second->session)
            {
                client_to_terminate.insert(sub_client.second);
            }
        }
    }
    for (auto& client: client_to_terminate)
    {
        client->setup_graceful_shutdown();
        client->terminate_session();
    }
}

bool base_client::is_test_finished()
{
    if (0 == get_number_of_request_left() || worker->current_phase > Phase::MAIN_DURATION)
    {
        return true;
    }
    else
    {
        return false;
    }
}


void base_client::update_this_in_dest_client_map()
{
    clean_up_this_in_dest_client_map();
    if (schema.empty() || authority.empty())
    {
        return;
    }
    std::string base_uri = schema;
    base_uri.append("://").append(authority);

    worker->get_client_pool()[proto_type][base_uri].insert(this);
    worker->get_client_pool()[PROTO_UNSPECIFIED][base_uri].insert(this);

    auto& clients_with_proto_type = get_controller_client()->client_registry[proto_type];
    clients_with_proto_type[base_uri] = this;

    auto& clients_with_unspecified_proto = get_controller_client()->client_registry[PROTO_UNSPECIFIED];
    clients_with_unspecified_proto[base_uri] = this;
}

void base_client::init_lua_states()
{
    for (auto scenario_index = 0; scenario_index < config->json_config_schema.scenarios.size(); scenario_index++)
    {
        std::vector<lua_State*> requests_lua_states;
        for (auto request_index = 0; request_index < config->json_config_schema.scenarios[scenario_index].requests.size();
             request_index++)
        {
            auto& request = config->json_config_schema.scenarios[scenario_index].requests[request_index];
            lua_State* L = luaL_newstate();
            luaL_openlibs(L);
            register_3rd_party_lib_func_to_lua(L);
            if (request.luaScript.size())
            {
                luaL_dostring(L, request.luaScript.c_str());
            }
            requests_lua_states.push_back(L);
        }
        lua_states.push_back(requests_lua_states);
    }
}

void base_client::slice_var_ids()
{
    if (config->nclients > 1 && (is_controller_client()))
    {
        for (size_t index = 0; index < config->json_config_schema.scenarios.size(); index++)
        {
            auto& scenario = config->json_config_schema.scenarios[index];
            std::vector<uint64_t> range_start, curr_vals, range_end;
            size_t total_vars = scenario.variable_manager.get_number_of_generic_variables();
            for (size_t var_id = 0; var_id < total_vars; var_id++)
            {
                if (var_id < scenario.number_of_variables_from_value_pickers)
                {
                    range_start.push_back(0);
                    range_end.push_back(0);
                    curr_vals.push_back(0);
                    continue;
                }
                auto variable_range_start = scenario.variable_manager.var_range_start(var_id);
                auto variable_range_end = scenario.variable_manager.var_range_end(var_id);
                if (!scenario.variable_range_slicing)
                {
                    std::random_device                  rand_dev;
                    std::mt19937                        generator(rand_dev());
                    std::uniform_int_distribution<uint64_t>  distr(variable_range_start,
                                                                   variable_range_end);
                    range_start.push_back(variable_range_start);
                    range_end.push_back(variable_range_end);
                    curr_vals.push_back(distr(generator));
                }
                else
                {
                    auto tokens_per_client = ((variable_range_end - variable_range_start) / (config->nclients));
                    if (tokens_per_client == 0)
                    {
                        if (worker->id == 0)
                        {
                            std::cerr <<
                                      "Error: number of variable values is smaller than number of clients, cannot continue; Please correct this variable definition: "
                                      << std::endl << scenario.variable_manager.get_var_name(var_id)
                                      << ", range start:" << variable_range_start
                                      << ", range end:" << variable_range_end
                                      << std::endl;
                            exit(EXIT_FAILURE);
                        }
                        else
                        {
                            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                            return;
                        }
                    }
                    auto tokens_left = ((variable_range_end - variable_range_start) % (config->nclients));
                    range_start.push_back(variable_range_start +
                                          (this_client_id.my_id * tokens_per_client) +
                                          std::min(this_client_id.my_id, tokens_left));
                    range_end.push_back(range_start.back() +
                                        tokens_per_client +
                                        (this_client_id.my_id >= tokens_left ? 0 : 1));

                    curr_vals.push_back(range_start.back());

                    if (config->verbose)
                    {
                        static std::mutex mu;
                        std::lock_guard<std::mutex> guard(mu);
                        std::cerr << ", client Id:" << this_client_id.my_id
                                  << ", scenario index: " << index
                                  << ", variable name: " << scenario.variable_manager.get_var_name(var_id)
                                  << ", variable id start:" << range_start.back()
                                  << ", variable id end:" << range_end.back()
                                  << std::endl;
                    }
                }
            }
            Variable_Data_Per_Client var_data_per_conn(range_start, range_end, curr_vals);
            variable_data_per_connection.push_back(var_data_per_conn);
        }
    }
    else
    {
        for (size_t index = 0; index < config->json_config_schema.scenarios.size(); index++)
        {
            auto& scenario = config->json_config_schema.scenarios[index];
            std::vector<uint64_t> range_start, curr_vals, range_end;
            for (size_t var_id = 0; var_id < scenario.variable_manager.get_number_of_generic_variables(); var_id++)
            {
                auto variable_range_start = scenario.variable_manager.var_range_start(var_id);
                auto variable_range_end = scenario.variable_manager.var_range_end(var_id);
                range_start.push_back(variable_range_start);
                range_end.push_back(variable_range_end);
                curr_vals.push_back(variable_range_start);
            }
            Variable_Data_Per_Client var_data_per_conn(range_start, range_end, curr_vals);
            variable_data_per_connection.push_back(var_data_per_conn);
        }
    }
}

bool base_client::rps_mode()
{
    return (rps > 0.0);
}

void base_client::update_scenario_based_stats(size_t scenario_index, size_t request_index, bool success,
                                              bool status_success)
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
}


size_t base_client::get_index_of_next_scenario_to_run()
{
    if (config->json_config_schema.scenarios.size() <= 1)
    {
        return 0;
    }
    auto init_size_vec = [](Config * config)
    {
        std::vector<size_t> vec;
        for (size_t scenario_index = 0; scenario_index < config->json_config_schema.scenarios.size(); scenario_index++)
        {
            vec.push_back(config->json_config_schema.scenarios[scenario_index].requests.size());;
        }
        return vec;
    };
    auto init_schedule_map = [](Config * config, uint64_t common_multiple)
    {
        std::map<uint64_t, size_t> schedule_map;
        uint64_t totalWeight = 0;
        for (size_t scenario_index = 0; scenario_index < config->json_config_schema.scenarios.size(); scenario_index++)
        {
            auto& scenario = config->json_config_schema.scenarios[scenario_index];
            if (scenario.weight > 0)
            {
                totalWeight += ((scenario.weight * common_multiple) / scenario.requests.size());
                schedule_map[totalWeight] = scenario_index;
            }
        }
        if (schedule_map.empty())
        {
            std::cerr << "all scenarios have weight of 0, cannot continue" << std::endl;
            exit(1);
        }
        return schedule_map;
    };
    static thread_local auto seq_no = config->json_config_schema.config_update_sequence_number;
    static thread_local auto schedule_map = init_schedule_map(config, find_common_multiple(init_size_vec(config)));
    static thread_local auto total_weight = (schedule_map.rbegin()->first);
    static thread_local std::random_device                  randDev;
    static thread_local std::mt19937_64                     generator(randDev());
    static thread_local std::uniform_int_distribution<int>  distr(0, total_weight - 1);

    if (seq_no != config->json_config_schema.config_update_sequence_number)
    {
        seq_no = config->json_config_schema.config_update_sequence_number;
        schedule_map = init_schedule_map(config, find_common_multiple(init_size_vec(config)));
        total_weight = (schedule_map.rbegin()->first);
        distr.param(std::uniform_int_distribution<>::param_type(0, total_weight - 1));
    }

    size_t scenario_index = 0;
    uint64_t randomNumber = distr(generator);
    auto iter = schedule_map.upper_bound(randomNumber);
    if (iter != schedule_map.end())
    {
        scenario_index = iter->second;
    }
    return scenario_index;

}

void base_client::submit_ping()
{
    session->submit_ping();
    signal_write();
}

void base_client::produce_request_cookie_header(Request_Response_Data& req_to_be_sent)
{
    if (!config->json_config_schema.builtin_cookie_support)
    {
        return;
    }
    if (req_to_be_sent.scenario_data_per_user->saved_cookies.empty())
    {
        return;
    }
    auto iter = req_to_be_sent.req_headers_from_config->find("Cookie");
    std::set<std::string> cookies_from_config;
    if (iter != req_to_be_sent.req_headers_from_config->end())
    {
        auto cookie_vec = Cookie::parse_cookie_string(iter->second, *req_to_be_sent.authority, *req_to_be_sent.schema);
        for (auto& cookie : cookie_vec)
        {
            cookies_from_config.insert(cookie.cookie_key);
        }
    }
    const std::string cookie_delimeter = "; ";
    std::string cookies_to_append;
    for (auto& cookie : req_to_be_sent.scenario_data_per_user->saved_cookies)
    {
        if (cookies_from_config.count(cookie.first))
        {
            // an overriding header from config carries the same cookie, config takes precedence
            continue;
        }
        else if (!Cookie::is_cookie_allowed_to_be_sent(cookie.second, *req_to_be_sent.schema, *req_to_be_sent.authority,
                                                       *req_to_be_sent.path))
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
        if (iter != req_to_be_sent.req_headers_from_config->end() && !iter->second.empty())
        {
            req_to_be_sent.req_headers_of_individual["Cookie"] = iter->second;
            req_to_be_sent.req_headers_of_individual["Cookie"].append(cookie_delimeter).append(cookies_to_append);
        }
        else
        {
            req_to_be_sent.req_headers_of_individual["Cookie"] = std::move(cookies_to_append);
        }
    }
}

bool base_client::update_request_with_lua(lua_State* L, const Request_Response_Data& finished_request,
                                          Request_Response_Data& request_to_send)
{
    lua_getglobal(L, make_request);
    bool retCode = true;
    if (lua_isfunction(L, -1))
    {
        lua_createtable(L, 0, std::accumulate(finished_request.resp_headers.begin(),
                                              finished_request.resp_headers.end(),
                                              0,
                                              [](uint64_t sum, const std::map<std::string, std::string, ci_less>& val)
        {
            return sum + val.size();
        }));
        for (auto& header_map : finished_request.resp_headers)
        {
            for (auto& header : header_map)
            {
                lua_pushlstring(L, header.first.c_str(), header.first.size());
                lua_pushlstring(L, header.second.c_str(), header.second.size());
                lua_rawset(L, -3);
            }
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

        lua_createtable(L, 0, request_to_send.req_headers_from_config->size());
        for (auto& header : * (request_to_send.req_headers_from_config))
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
                            std::cerr << "key type" << lua_type(L, -2) << std::endl;
                            std::cerr << "value type" << lua_type(L, -1) << std::endl;
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
                    //headers.erase(method_header);
                    request_to_send.string_collection.emplace_back(headers[path_header]);
                    request_to_send.path = &(request_to_send.string_collection.back());
                    //headers.erase(path_header);
                    request_to_send.string_collection.emplace_back(headers[authority_header]);
                    request_to_send.authority = &(request_to_send.string_collection.back());
                    //headers.erase(authority_header);
                    request_to_send.string_collection.emplace_back(headers[scheme_header]);
                    request_to_send.schema = &(request_to_send.string_collection.back());
                    //headers.erase(scheme_header);

                    //headers.erase(host_header);
                    remove_reserved_http_headers(headers);
                    request_to_send.req_headers_of_individual = std::move(headers);

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

void base_client::terminate_session()
{
    if (session)
    {
        session->terminate();
    }
    else
    {
        if (write_clear_callback)
        {
            write_clear_callback();
        }
    }
}

void base_client::reset_timeout_requests()
{
    if (stream_timestamp.empty())
    {
        return;
    }
    std::chrono::steady_clock::time_point curr_time_point = std::chrono::steady_clock::now();
    auto no_timeout_it = stream_timestamp.upper_bound(curr_time_point);
    auto it = stream_timestamp.begin();
    bool call_signal_write = false;
    while (it != no_timeout_it)
    {
        auto stream_it = streams.find(it->second);
        if (stream_it != streams.end())
        {
            session->reset_stream(it->second);
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

void base_client::on_rps_timer()
{
    reset_timeout_requests();
    assert(!config->timing_script);

/*
    if (CLIENT_CONNECTED != state)
    {
        return;
    }
*/
    if (get_number_of_request_left() == 0)
    {
        stop_rps_timer();
        return;
    }

    auto now = std::chrono::steady_clock::now();
    auto d = now - time_point_of_last_rps_timer_expiry;
    auto duration = std::chrono::duration<double>(d).count();
    if (duration < 0.0)
    {
        return;
    }
    auto n = static_cast<size_t>(round(duration * config->rps));
    rps_req_pending = n;
    time_point_of_last_rps_timer_expiry = time_point_of_last_rps_timer_expiry + std::chrono::duration<double>(static_cast<double>(n) / config->rps);

    if (rps_req_pending == 0)
    {
        return;
    }

    auto nreq = get_max_concurrent_stream() - get_total_pending_streams();
    if (nreq <= 0)
    {
        return;
    }

    nreq = config->is_timing_based_mode() ? std::max(nreq, get_number_of_request_left())
           : std::min(nreq, get_number_of_request_left());
    nreq = std::min(nreq, rps_req_pending);
    for (; nreq > 0; --nreq)
    {
        auto retCode = submit_request();
/*
        if (retCode != 0)
        {
            break;
        }
*/
        rps_req_inflight++;
        rps_req_pending--;
    }
    // client->signal_write(); // submit_request already calls signal_write()
}
void base_client::process_timedout_streams()
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


size_t base_client::get_number_of_request_inflight()
{
    return streams.size();
}

void base_client::on_stream_close(int64_t stream_id, bool success, bool final)
{
    record_stream_close_time(stream_id);

    brief_log_to_file(stream_id, success);

    inc_status_counter_and_validate_response(stream_id);

    auto itr = streams.find(stream_id);
    if (itr == streams.end())
    {
        return;
    }

    auto& finished_request = itr->second.request_response;

    if (worker->current_phase == Phase::MAIN_DURATION ||
        worker->current_phase == Phase::MAIN_DURATION_GRACEFUL_SHUTDOWN)
    {
        if (req_inflight > 0)
        {
            --req_inflight;
        }
        if (get_controller_client()->req_inflight_of_all_clients > 0)
        {
            --get_controller_client()->req_inflight_of_all_clients;
        }
        if (rps_mode() && get_controller_client()->rps_req_inflight)
        {
            --get_controller_client()->rps_req_inflight;
        }
        auto req_stat = get_req_stat(stream_id);
        if (!req_stat)
        {
            return;
        }

        if (itr->second.statistics_eligible)
        {
            if (success)
            {
                req_stat->completed = true;
                ++worker->stats.req_success;
                ++cstat.req_success;

                if (itr->second.status_success == 1)
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

            if (finished_request)
            {
                bool status_success = (streams.count(stream_id) && streams.at(stream_id).status_success == 1) ? true : false;
                update_scenario_based_stats(finished_request->scenario_index,
                                            finished_request->curr_request_idx,
                                            success, status_success);
            }
        }

    }

    worker->report_progress();

    if (finished_request)
    {
        size_t scenario_index = finished_request->scenario_index;
        Scenario& scenario = config->json_config_schema.scenarios[scenario_index];
        auto req_idx = finished_request->curr_request_idx;
        if ((req_idx < (scenario.requests.size() - 1))&&
            (scenario.requests[req_idx].page_id != scenario.requests[req_idx + 1].page_id))
        {
            prepare_next_request(*finished_request);
        }
        process_stream_user_callback(stream_id);
    }

    streams.erase(itr);

    if (get_number_of_request_left() == 0 && get_controller_client()->req_inflight_of_all_clients == 0)
    {
        get_controller_client()->terminate_sub_clients();
        get_controller_client()->setup_graceful_shutdown();
        get_controller_client()->terminate_session();
        return;
    }

    move_queued_request_to_controller_queue(true);

    if (!final && !is_test_finished())
    {
        if (config->timing_script)
        {
            feed_timing_script_request_timeout_timer();
        }
        else if (!rps_mode())
        {
            get_controller_client()->submit_request();
        }
        else if (get_controller_client()->rps_req_pending)
        {
            if (get_controller_client()->submit_request() == 0)
            {
                --get_controller_client()->rps_req_pending;
            }
        }
    }
}

void base_client::on_header_frame_begin(int64_t stream_id, uint8_t flags)
{
    auto it = streams.find(stream_id);
    if (it != streams.end() && it->second.request_response)
    {
        auto& request_data = *it->second.request_response;
        std::map<std::string, std::string, ci_less> dummy;
        request_data.resp_headers.push_back(dummy);
        if (request_data.resp_headers.size() > 1 && (flags & NGHTTP2_FLAG_END_STREAM)) // TODO: add payload check?
        {
            request_data.resp_trailer_present = true;
        }
    }
}

void base_client::on_prefered_host_up()
{
    std::cerr << "preferred host is up: " << preferred_authority << std::endl;
    if (authority != preferred_authority && state == CLIENT_CONNECTED)
    {
        std::cerr << "switching back to preferred host: " << preferred_authority << std::endl;
        disconnect();
        candidate_addresses.push_back(std::move(authority));
        authority = preferred_authority;
        connect_to_host(schema, authority);
    }
}

void base_client::early_fail_of_request(std::unique_ptr<Request_Response_Data>& req, base_client* client)
{
    if (req->request_sent_callback)
    {
        req->request_sent_callback(-1, client);
    }
    size_t stream_id = 0;
    on_request_start(stream_id, req, true); // do not count into statistics as DONE
    // auto req_stat = get_req_stat(stream_id);
    // record_request_time(req_stat);
    on_stream_close(stream_id, false);
}

// TODO: optimize the design
void base_client::fail_one_request_of_client(base_client* disconnected_client)
{
    if (!disconnected_client)
    {
        return;
    }

    if (disconnected_client == get_controller_client())
    {
        return;
    }

    if (disconnected_client->requests_to_submit.size())
    {
        // client local queue has at least one req, no need to do this on controller
        return;
    }

    if (!is_controller_client())
    {
        return get_controller_client()->fail_one_request_of_client(disconnected_client);
    }

    decltype(requests_to_submit) temp;
    while (requests_to_submit.size())
    {
        auto req = std::move(requests_to_submit.front());
        requests_to_submit.pop_front();
        if (*req->schema == disconnected_client->schema &&
            *req->authority == disconnected_client->authority &&
            (req->proto_type == disconnected_client->proto_type || req->proto_type == PROTO_UNSPECIFIED))
        {
            early_fail_of_request(req, disconnected_client);
            break;
        }
        else
        {
            temp.push_back(std::move(req));
        }
    }
    while (temp.size())
    {
        auto req = std::move(temp.back());
        temp.pop_back();
        requests_to_submit.push_front(std::move(req));
    }
}

void base_client::submit_request_upon_connected()
{
    if (!is_controller_client())
    {
        return get_controller_client()->submit_request_upon_connected();
    }

    start_request_delay_execution_timer();

    if (rps_mode() &&
        (0 == std::chrono::duration_cast<std::chrono::microseconds>
         (time_point_of_last_rps_timer_expiry.time_since_epoch()).count()))
    {
        start_rps_timer();
        time_point_of_last_rps_timer_expiry = std::chrono::steady_clock::now();
    }

    if (rps_mode())
    {
        if (rps_req_pending)
        {
            assert(get_number_of_request_left());
            if (submit_request() == 0)
            {
                ++rps_req_inflight;
                --rps_req_pending;
            }
        }
    }
    else if (!config->timing_script)
    {

        auto max_concurrent_streams = get_max_concurrent_stream();

        auto nreq = config->is_timing_based_mode()
                    ? std::max(get_number_of_request_left(), max_concurrent_streams)
                    : std::min(get_number_of_request_left(), max_concurrent_streams);

        for (; nreq > 0; --nreq)
        {
            /*
            if (submit_request() != 0)
            {
                break;
            }
            */
            submit_request();
        }
    }
    else
    {
        auto start = reqidx ? config->timings[reqidx - 1] : 0.0;
        double duration = config->timings[reqidx] - start;

        while (duration < 1e-9)
        {
            if (submit_request() != 0)
            {
                break;
            }
            duration = config->timings[reqidx] - start;
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
            start_timing_script_request_timeout_timer(duration);
        }
    }
}

void base_client::connected_event_on_controller()
{
    get_controller_client()->start_request_delay_execution_timer();

    if (get_controller_client()->rps_mode() &&
        (0 == std::chrono::duration_cast<std::chrono::microseconds>
         (get_controller_client()->time_point_of_last_rps_timer_expiry.time_since_epoch()).count()))
    {
        get_controller_client()->start_rps_timer();
        get_controller_client()->time_point_of_last_rps_timer_expiry = std::chrono::steady_clock::now();
    }

    if (get_controller_client()->rps_mode())
    {
        if (get_controller_client()->rps_req_pending)
        {
            assert(get_number_of_request_left());
            if (get_controller_client()->submit_request() == 0)
            {
                ++get_controller_client()->rps_req_inflight;
                --get_controller_client()->rps_req_pending;
            }
        }
    }
    else if (!get_controller_client()->config->timing_script)
    {

        auto max_concurrent_streams = get_max_concurrent_stream();

        auto nreq = get_controller_client()->config->is_timing_based_mode()
                    ? std::max(get_number_of_request_left(), max_concurrent_streams)
                    : std::min(get_number_of_request_left(), max_concurrent_streams);

        for (; nreq > 0; --nreq)
        {
            get_controller_client()->submit_request();
        }
    }
    else
    {
        auto start = get_controller_client()->reqidx ? get_controller_client()->config->timings[get_controller_client()->reqidx
                                                                                                - 1] : 0.0;
        double duration =
            get_controller_client()->config->timings[get_controller_client()->reqidx] - start;

        while (duration < 1e-9)
        {
            if (get_controller_client()->submit_request() != 0)
            {
                break;
            }
            duration = get_controller_client()->config->timings[get_controller_client()->reqidx];
            if (get_controller_client()->reqidx == 0)
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
            get_controller_client()->start_timing_script_request_timeout_timer(duration);
        }
    }
    // not needed any more, as signal_write is called by submit_request
    // get_controller_client()->signal_write();
}

int base_client::connection_made()
{
    stop_connect_timeout_timer();

    auto ret = select_protocol_and_allocate_session();
    if (ret != 0)
    {
        return ret;
    }

    worker->get_client_ids()[this->get_client_unique_id()] = this;

    state = CLIENT_CONNECTED;

    session->on_connect();

    record_connect_time();

    update_this_in_dest_client_map();

    start_stream_timeout_timer();

    move_queued_request_to_controller_queue(true);

    submit_request_upon_connected();
    if (!config->disable_connection_trace)
    {
        std::cerr << "===============connected to " << authority << "===============" << std::endl;
    }
    if (authority != preferred_authority &&
        config->json_config_schema.connect_back_to_preferred_host &&
        (!is_test_finished()))
    {
        std::cerr << "current connected to: " << authority << ", prefered connection to: " << preferred_authority << std::endl;
        start_connect_to_preferred_host_timer();
    }

    return 0;
}

void base_client::on_data_chunk(int64_t stream_id, const uint8_t* data, size_t len)
{
    auto it = streams.find(stream_id);
    if (it != streams.end() && it->second.request_response)
    {
        // TODO: handle grpc payload with multiple data chunks, each with size prefixed
        it->second.request_response->resp_payload.append((const char*)data, len);
    }
    if (config->verbose)
    {
        std::string str((const char*)data, len);
        std::cout << "received data: " << std::endl << str << std::endl;
    }
}

void base_client::resume_delayed_request_execution()
{
    std::chrono::steady_clock::time_point curr_time_point = std::chrono::steady_clock::now();
    auto barrier = delayed_requests_to_submit.upper_bound(curr_time_point);
    auto it = delayed_requests_to_submit.begin();
    while (it != barrier)
    {
        requests_to_submit.emplace_back(std::move(it->second));
        it = delayed_requests_to_submit.erase(it);
    }
}

RequestStat* base_client::get_req_stat(int64_t stream_id)
{
    auto it = streams.find(stream_id);
    if (it == std::end(streams))
    {
        return nullptr;
    }

    return &(*it).second.req_stat;
}

void base_client::execute_request_sent_callback(Request_Response_Data& request, int64_t stream_id)
{
    if (request.request_sent_callback)
    {
        request.request_sent_callback(stream_id, this);
        auto dummy = std::move(request.request_sent_callback);
    }
}


void base_client::on_request_start(int64_t stream_id, std::unique_ptr<Request_Response_Data>& rr_data, bool do_not_stat)
{
    auto timeout_interval = 0;
    if (rr_data)
    {
        timeout_interval = rr_data->stream_timeout_in_ms ?
                           rr_data->stream_timeout_in_ms :
                           config->json_config_schema.stream_timeout_in_ms;
        execute_request_sent_callback(*rr_data, stream_id);
    }

    if (streams.count(stream_id))
    {
        streams.erase(stream_id);
    }
    bool stats_eligible = (worker->current_phase == Phase::MAIN_DURATION
                           || worker->current_phase == Phase::MAIN_DURATION_GRACEFUL_SHUTDOWN) && (!do_not_stat);
    streams.insert(std::make_pair(stream_id, Stream(stats_eligible, rr_data)));
    auto curr_timepoint = std::chrono::steady_clock::now();
    auto timeout_timepoint = curr_timepoint + std::chrono::milliseconds(timeout_interval);
    stream_timestamp.insert(std::make_pair(timeout_timepoint, stream_id));
}

void base_client::record_request_time(RequestStat* req_stat)
{
    req_stat->request_time = std::chrono::steady_clock::now();
    req_stat->request_wall_time = std::chrono::system_clock::now();
}

std::unique_ptr<Request_Response_Data> base_client::get_request_to_submit()
{
    if (requests_to_submit.empty())
    {
        std::cerr << "this is not expected; contact support to report this error" << std::endl;
        printBacktrace();
        abort(); // this should never happen
        prepare_first_request();
    }
    auto queued_request = std::move(requests_to_submit.front());
    requests_to_submit.pop_front();
    return queued_request;
}

void base_client::on_header(int64_t stream_id, const uint8_t* name, size_t namelen,
                            const uint8_t* value, size_t valuelen)
{
    auto itr = streams.find(stream_id);
    if (itr == std::end(streams))
    {
        return;
    }
    auto& stream = (*itr).second;

    auto& request = stream.request_response;
    if (request)
    {
        std::string header_name;
        header_name.assign((const char*)name, namelen);
        std::string header_value;
        header_value.assign((const char*)value, valuelen);
        header_value.erase(0, header_value.find_first_not_of(' '));
        assert(request->resp_headers.size());
        auto it = request->resp_headers.back().find(header_name);
        if (it != request->resp_headers.back().end())
        {
            // Set-Cookie case most likely
            it->second.append("; ").append(header_value);
        }
        else
        {
            request->resp_headers.back()[header_name] = header_value;
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

        on_status_code(stream_id, status);
    }
}

Config* base_client::get_config()
{
    return config;
}
Stats& base_client::get_stats()
{
    return worker->stats;
}


base_client* base_client::get_controller_client()
{
    if (parent_client)
    {
        return parent_client.get();
    }
    else
    {
        return this;
    }
}

size_t& base_client::get_current_req_index()
{
    return get_controller_client()->reqidx;
}

const std::unique_ptr<Request_Response_Data>& base_client::get_request_response_data(int64_t stream_id)
{
    auto iter = streams.find(stream_id);
    if (iter != streams.end())
    {
        return iter->second.request_response;
    }
    else
    {
        return dummy_rr_data;
    }
}

bool base_client::is_final()
{
    return final;
}
void base_client::set_final(bool val)
{
    final = val;
}

bool base_client::not_connected()
{
    return (CLIENT_IDLE == state);
}

base_client* base_client::find_or_create_dest_client(Request_Response_Data& request_to_send)
{
    if (config->verbose)
    {
        std::cerr<<__func__<<": request_to_send: "<<request_to_send<<std::endl;
    }
    if (!config->json_config_schema.open_new_connection_based_on_authority_header)
    {
        return get_controller_client();
    }
    if (is_controller_client())
    {
        std::string dest = *(request_to_send.schema);
        dest.append("://").append(*request_to_send.authority);
        auto proto = request_to_send.proto_type;

        decltype(this) destination_client = nullptr;

        auto map_of_map_iter = client_registry.find(proto);
        if (map_of_map_iter != client_registry.end())
        {
            auto iter = map_of_map_iter->second.find(dest);
            if (iter != map_of_map_iter->second.end())
            {
                destination_client = iter->second;
            }
        }
        if (!destination_client)
        {
            auto new_client = create_dest_client(*request_to_send.schema, *request_to_send.authority, proto);
            worker->check_in_client(new_client);
            new_client->connect_to_host(new_client->schema, new_client->authority);
            destination_client = new_client.get();
        }
        else
        {
            if (destination_client->not_connected())
            {
                assert(destination_client->get_proto_type() == proto);
                destination_client->connect_to_host(*request_to_send.schema, *request_to_send.authority);
            }
        }
        return destination_client;
    }
    else
    {
        return parent_client->find_or_create_dest_client(request_to_send);
    }
}

PROTO_TYPE base_client::get_proto_type()
{
    return proto_type;
}

bool base_client::is_controller_client()
{
    return (parent_client.get() == nullptr);
}

void base_client::prepare_first_request()
{
    static const Request_Response_Data dummy_data(0);

    auto controller = get_controller_client();

    size_t scenario_index = get_index_of_next_scenario_to_run();

    auto& scenario = config->json_config_schema.scenarios[scenario_index];

    auto new_request = std::make_unique<Request_Response_Data>(controller->variable_data_per_connection[scenario_index].get_curr_vars());

    controller->variable_data_per_connection[scenario_index].inc_var();

    new_request->scenario_index = scenario_index;

    new_request->curr_request_idx = 0;

    populate_request_from_config_template(*new_request, scenario_index, new_request->curr_request_idx);

    auto& request_template = scenario.requests[new_request->curr_request_idx];

    if (INPUT_WITH_VARIABLE == request_template.uri.uri_action)
    {
        auto uri = assemble_string(request_template.tokenized_path_with_vars, scenario_index, new_request->curr_request_idx,
                                   *new_request->scenario_data_per_user);
        if (!parse_uri_and_poupate_request(uri, *new_request))
        {
            std::cerr << "abort whole scenario sequence, as uri is invalid:" << uri << std::endl;
            abort();
        }
    }
    else
    {
        new_request->string_collection.emplace_back(assemble_string(request_template.tokenized_path_with_vars, scenario_index,
                                                                   new_request->curr_request_idx, *new_request->scenario_data_per_user));
        new_request->path = &(new_request->string_collection.back());
    }

    if (scenario.requests[new_request->curr_request_idx].make_request_function_present)
    {
        if (!update_request_with_lua(lua_states[scenario_index][new_request->curr_request_idx], dummy_data, *new_request))
        {
            std::cerr << "lua script failure for first request, cannot continue, exit" << std::endl;
            exit(EXIT_FAILURE);
        }
    }

    update_content_length(*new_request);

    sanitize_request(*new_request);

    auto start_idx = new_request->curr_request_idx;

    auto req_ptr = enqueue_request(dummy_data, new_request).get();

    for (auto req_idx = start_idx; req_idx < scenario.requests.size() - 1; req_idx++)
    {
        bool next_req_is_of_same_page = (scenario.requests[req_idx].page_id == scenario.requests[req_idx + 1].page_id);
        if (!next_req_is_of_same_page)
        {
            break;
        }
        req_ptr = prepare_next_request(*req_ptr, false).get();
        if (!req_ptr || req_ptr->schema == &emptyString)
        {
            break;
        }
    }

}

const std::unique_ptr<Request_Response_Data>& base_client::prepare_next_request(Request_Response_Data& prev_request, bool check_next_req)
{
    static const auto dummy_data = std::make_unique<Request_Response_Data>(std::vector<uint64_t>(0));

    run_post_response_action(prev_request);

    size_t scenario_index = prev_request.scenario_index;
    Scenario& scenario = config->json_config_schema.scenarios[scenario_index];

    size_t curr_req_index = ((prev_request.curr_request_idx + 1) % scenario.requests.size());
    if (curr_req_index == 0)
    {
        return dummy_data;
    }
    auto& request_template = scenario.requests[curr_req_index];
    if (!request_template.clear_old_cookies)
    {
        parse_and_save_cookies(prev_request);
    }
    std::unique_ptr<Request_Response_Data> new_request;
    bool parallel_requests_of_same_page = (scenario.requests[prev_request.curr_request_idx].page_id == scenario.requests[curr_req_index].page_id);
    if (parallel_requests_of_same_page)
    {
        new_request = std::make_unique<Request_Response_Data>(prev_request.scenario_data_per_user);
    }
    else
    {
        new_request = std::make_unique<Request_Response_Data>(std::move(prev_request.scenario_data_per_user));
    }

    new_request->scenario_index = scenario_index;
    new_request->curr_request_idx = curr_req_index;

    populate_request_from_config_template(*new_request, scenario_index, new_request->curr_request_idx);

    switch (request_template.uri.uri_action)
    {
        case INPUT_URI:
        {
            new_request->string_collection.emplace_back(assemble_string(request_template.tokenized_path_with_vars, scenario_index,
                                                                        new_request->curr_request_idx, *new_request->scenario_data_per_user));

            new_request->path = &(new_request->string_collection.back());

            break;
        }
        case SAME_WITH_LAST_ONE:
        {
            new_request->string_collection.emplace_back(*prev_request.path);
            new_request->path = &(new_request->string_collection.back());
            new_request->string_collection.emplace_back(*prev_request.schema);
            new_request->schema = &(new_request->string_collection.back());
            new_request->string_collection.emplace_back(*prev_request.authority);
            new_request->authority = &(new_request->string_collection.back());
            break;
        }
        case FROM_RESPONSE_HEADER:
        {
            std::string* uri_header_value = nullptr;
            for (auto& header_map : prev_request.resp_headers)
            {
                auto header = header_map.find(request_template.uri.input);
                if (header != header_map.end())
                {
                    uri_header_value = &header->second;
                }
            }
            new_request->authority = nullptr;
            if (uri_header_value && parse_uri_and_poupate_request(*uri_header_value, *new_request))
            {
                if (!new_request->authority)
                {
                    new_request->string_collection.emplace_back(*prev_request.schema);
                    new_request->schema = &(new_request->string_collection.back());
                    new_request->string_collection.emplace_back(*prev_request.authority);
                    new_request->authority = &(new_request->string_collection.back());
                }
            }
            else
            {
                if (config->verbose)
                {
                    std::cout << "response status code:" << prev_request.status_code << std::endl;
                    std::cerr << "abort whole scenario sequence, as header not found: " << request_template.uri.input << std::endl;
                    for (auto& header_map : prev_request.resp_headers)
                    {
                        for (auto& header : header_map)
                        {
                            std::cout << header.first << ":" << header.second << std::endl;
                        }
                    }
                    std::cout << "response payload:" << prev_request.resp_payload << std::endl;
                }
                return dummy_data;
            }
            break;
        }
        case INPUT_WITH_VARIABLE:
        {
            auto uri = assemble_string(request_template.tokenized_path_with_vars, scenario_index, new_request->curr_request_idx,
                                       *new_request->scenario_data_per_user);
            if (!parse_uri_and_poupate_request(uri, *new_request))
            {
                std::cerr << "abort whole scenario sequence, as uri is invalid:" << uri << std::endl;
                return dummy_data;
            }
            break;
        }
        default:
        {

        }

    }

    produce_request_cookie_header(*new_request);

    run_pre_request_action(*new_request);

    if (request_template.luaScript.size())
    {
        if (!update_request_with_lua(lua_states[scenario_index][curr_req_index], prev_request, *new_request))
        {
            return dummy_data; // lua script returns error or kills the request, abort this scenario
        }
    }

    update_content_length(*new_request);
    sanitize_request(*new_request);

    auto& ret = enqueue_request(prev_request, new_request);
    auto req_ptr = ret.get();

    if (check_next_req)
    {
        for (auto req_idx = curr_req_index; req_idx < scenario.requests.size() - 1; req_idx++)
        {
            if (scenario.requests[req_idx].page_id != scenario.requests[req_idx + 1].page_id)
            {
                break;
            }
            req_ptr = prepare_next_request(*req_ptr, false).get();
            if (!req_ptr || req_ptr->schema == &emptyString)
            {
                break;
            }
        }
    }

    return ret;

}

void base_client::update_content_length(Request_Response_Data& data)
{
    if (data.req_payload->size())
    {
        std::string content_length = "content-length";
        data.req_headers_of_individual[content_length] = std::to_string(data.req_payload->size());
    }
}

size_t base_client::get_max_concurrent_stream()
{
    return config->max_concurrent_streams;
}

int base_client::submit_request()
{
    if (requests_to_submit.empty() && request_template_unavailable(*config))
    {
        return 0;
    }

    if (!is_controller_client())
    {
        return parent_client->submit_request();
    }

    auto max_concurrent_streams = get_max_concurrent_stream();

    if (get_total_pending_streams() >= max_concurrent_streams)
    {
        return MAX_CONCURRENT_STREAM_REACHED;
    }

    if (get_number_of_request_left() <= 0)
    {
        return -1;
    }

    if (worker->current_phase == Phase::MAIN_DURATION_GRACEFUL_SHUTDOWN)
    {
        return -1;
    }

    auto submit_one_request = [this]()
    {
        if (requests_to_submit.empty() && (config->json_config_schema.scenarios.size()))
        {
            prepare_first_request();
        }

        decltype(this) destination_client = this;
        if (config->json_config_schema.open_new_connection_based_on_authority_header)
        {
            destination_client = find_or_create_dest_client(*requests_to_submit.front());
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
                scenario_index = destination_client->requests_to_submit.front()->scenario_index;
                request_index = destination_client->requests_to_submit.front()->curr_request_idx;
            }
            auto retCode = destination_client->session->submit_request();

            if (retCode != 0)
            {
                destination_client->process_submit_request_failure(retCode);
                return retCode;
            }

            destination_client->signal_write();

            if (worker->current_phase != Phase::MAIN_DURATION)
            {
                return 0;
            }

            ++worker->stats.req_started;
            ++destination_client->req_started;
            ++destination_client->req_inflight;
            ++get_controller_client()->req_inflight_of_all_clients;
            if (!worker->config->is_timing_based_mode())
            {
                dec_number_of_request_left();
            }

            if (scenario_index < worker->scenario_stats.size() &&
                request_index < worker->scenario_stats[scenario_index].size())
            {
                ++worker->scenario_stats[scenario_index][request_index]->req_started;
            }

            // if an active timeout is set and this is the last request to be submitted
            // on this connection, start the active timeout.
            if (config->conn_active_timeout > 0. && get_number_of_request_left() == 0 && is_controller_client())
            {
                for (auto& dest_client_with_same_proto_version : client_registry) // "this" is also in client_registry
                {
                    for (auto& client : dest_client_with_same_proto_version.second)
                    {
                        client.second->start_conn_active_watcher();
                    }
                }
            }
            return 0;
        }
        else
        {
            return CONNECTION_ESTABLISH_IN_PROGRESSS;
        }
    };

    uint8_t retry_count = 0;
    while (retry_count < max_concurrent_streams)
    {
        auto retCode = submit_one_request();
        if (0 == retCode)
        {
            return retCode;
        }
        if (CONNECTION_ESTABLISH_IN_PROGRESSS == retCode || MAX_CONCURRENT_STREAM_REACHED == retCode)
        {
            ++retry_count;
        }
    }

    return -1;
}

void base_client::process_submit_request_failure(int errCode)
{
    if (worker->current_phase != Phase::MAIN_DURATION)
    {
        return;
    }

    if (MAX_CONCURRENT_STREAM_REACHED == errCode)
    {
        return;
    }

    if (MAX_STREAM_TO_BE_EXHAUSTED == errCode)
    {
        std::cerr << "stream exhausted on this client. Restart client:" << std::endl;
        graceful_restart_connection();
        return;
    }

    if (!should_reconnect_on_disconnect())
    {
        if (is_controller_client())
        {
            worker->stats.req_failed += req_left;
            worker->stats.req_error += req_left;
            req_left = 0;
        }
    }

    if (streams.size() == 0)
    {
        setup_graceful_shutdown();
        terminate_session();
        return;
    }

    std::cerr << "Process Request Failure:" << worker->stats.req_failed
              << ", errorCode: " << errCode
              << std::endl;
}

void base_client::print_app_info()
{
    if (selected_proto.compare(http1_proto) == 0)
    {
        proto_type = PROTO_HTTP1;
    }
    else if (selected_proto.find(http2_proto, 0) == 0)
    {
        proto_type = PROTO_HTTP2;
    }
    else if (selected_proto.find(http2_cleartext_proto, 0) == 0)
    {
        proto_type = PROTO_HTTP2;
    }
    else if (selected_proto.find(http3_proto, 0) == 0)
    {
        proto_type = PROTO_HTTP3;
    }
    else
    {
        proto_type = PROTO_INVALID;
    }
    if (worker->id == 0 && !worker->app_info_report_done)
    {
        worker->app_info_report_done = true;
        std::cerr << "Application protocol: " << selected_proto << std::endl;
    }
}

int base_client::select_protocol_and_allocate_session()
{
    auto allocate_session_and_set_selected_non_tls_proto = [this](uint8_t proto)
    {
        switch (proto)
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
    };

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
            if (is_quic())
            {
#ifdef ENABLE_HTTP3
                assert(session);
                if (!util::streq(StringRef{&NGHTTP3_ALPN_H3[1]}, proto) &&
                    !util::streq_l("h3-29", proto))
                {
                    return -1;
                }
#endif // ENABLE_HTTP3
            }
            else if (util::check_h2_is_selected(proto))
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
            auto proto = config->no_tls_proto;
            if (PROTO_TYPE::PROTO_UNSPECIFIED != proto_type)
            {
                if (proto_type == PROTO_TYPE::PROTO_HTTP2)
                {
                    proto = h2load::Config::PROTO_HTTP2;
                }
                else if (proto_type == PROTO_TYPE::PROTO_HTTP1)
                {
                    proto = h2load::Config::PROTO_HTTP1_1;
                }
                else
                {
                    std::cerr << "http3 requested, " << authority << " does not support TLS, fallback to http/1.1" << std::endl;
                    proto = h2load::Config::PROTO_HTTP1_1;
                }
            }
            else
            {
                std::cerr << "No protocol negotiated. Fallback behaviour may be activated"
                          << std::endl;

                for (const auto& protocol : config->npn_list)
                {
                    if (util::streq(NGHTTP2_H1_1_ALPN, StringRef {protocol}))
                    {
                        std::cerr
                                << "Server does not support NPN/ALPN. Falling back to HTTP/1.1."
                                << std::endl;
                        proto = h2load::Config::PROTO_HTTP1_1;
                        break;
                    }
                }
            }
            allocate_session_and_set_selected_non_tls_proto(proto);
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
        auto proto = config->no_tls_proto;
        if (PROTO_TYPE::PROTO_UNSPECIFIED != proto_type)
        {
            if (proto_type == PROTO_TYPE::PROTO_HTTP2)
            {
                proto = h2load::Config::PROTO_HTTP2;
            }
            else if (proto_type == PROTO_TYPE::PROTO_HTTP1)
            {
                proto = h2load::Config::PROTO_HTTP1_1;
            }
            else
            {
                std::cerr << "http3 requested, " << authority << " does not support TLS, fallback to http/1.1" << std::endl;
                proto = h2load::Config::PROTO_HTTP1_1;
            }
        }
        allocate_session_and_set_selected_non_tls_proto(proto);
        print_app_info();
    }

    return 0;
}

void base_client::report_tls_info()
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

bool base_client::get_host_and_port_from_authority(const std::string& schema, const std::string& authority,
                                                   std::string& host, std::string& port)
{
    http_parser_url u {};
    http_parser_parse_url(authority.c_str(), authority.size(), true, &u);
    if (util::has_uri_field(u, UF_HOST))
    {
        host = util::get_uri_field(authority.c_str(), u, UF_HOST).str();
        if (util::has_uri_field(u, UF_PORT))
        {
            port = std::to_string(u.port);
        }
        else
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
    }
    else
    {
        std::cerr << __FUNCTION__ << ": invalid authority:" << authority << std::endl;
        return false;
    }
    if (host.size() && host[0] == '[' && host[host.size() - 1] == ']')
    {
        host = host.substr(1, host.size() - 2);
    }
    return true;
}

void base_client::call_connected_callbacks(bool success)
{
    for (auto& callback : connected_callbacks)
    {
        if (callback)
        {
            callback(success, this);
        }
    }
    connected_callbacks.clear();
}


void base_client::install_connected_callback(std::function<void(bool, h2load::base_client*)> callback)
{
    connected_callbacks.push_back(callback);
}

void base_client::queue_stream_for_user_callback(int64_t stream_id)
{
    const size_t MAX_STREAM_SAVED_FOR_CALLBACK = 500;

    if (per_stream_user_callbacks.size() > MAX_STREAM_SAVED_FOR_CALLBACK)
    {
        per_stream_user_callbacks.erase(per_stream_user_callbacks.begin());
    }
    per_stream_user_callbacks[stream_id].stream_id = stream_id;
}

void base_client::process_stream_user_callback(int64_t stream_id)
{
    auto it = streams.find(stream_id);
    if (it !=  streams.end() && per_stream_user_callbacks.count(stream_id) &&
        it->second.request_response)
    {
        per_stream_user_callbacks[stream_id].resp_headers = std::move(it->second.request_response->resp_headers);
        per_stream_user_callbacks[stream_id].resp_payload = std::move(it->second.request_response->resp_payload);
        per_stream_user_callbacks[stream_id].response_available = true;
        per_stream_user_callbacks[stream_id].resp_trailer_present = it->second.request_response->resp_trailer_present;
        if (per_stream_user_callbacks[stream_id].response_callback)
        {
            per_stream_user_callbacks[stream_id].response_callback();
            per_stream_user_callbacks.erase(stream_id);
        }
    }
}

void base_client::pass_response_to_lua(int64_t stream_id, lua_State* L)
{
    if (per_stream_user_callbacks.count(stream_id))
    {
        auto push_response_to_lua_stack = [this, L, stream_id]()
        {
            std::map<std::string, std::string, ci_less>* trailer = nullptr;
            if (per_stream_user_callbacks[stream_id].resp_headers.size() > 1 &&
                per_stream_user_callbacks[stream_id].resp_trailer_present)
            {
                trailer = &per_stream_user_callbacks[stream_id].resp_headers.back();
                if (config->verbose)
                {
                    std::cout << "number of header frames: " << per_stream_user_callbacks[stream_id].resp_headers.size()
                              << ", trailer found" << std::endl;
                }
            }
            lua_createtable(L, 0, std::accumulate(per_stream_user_callbacks[stream_id].resp_headers.begin(),
                                                  per_stream_user_callbacks[stream_id].resp_headers.end(),
                                                  0,
                                                  [trailer](uint64_t sum, const std::map<std::string, std::string, ci_less>& val)
            {
                return sum + (&val == trailer ? 0 : val.size());
            }));
            for (auto& header_map : per_stream_user_callbacks[stream_id].resp_headers)
            {
                if (&header_map == trailer)
                {
                    continue;
                }
                for (auto& header : header_map)
                {
                    lua_pushlstring(L, header.first.c_str(), header.first.size());
                    lua_pushlstring(L, header.second.c_str(), header.second.size());
                    lua_rawset(L, -3);
                }
            }

            lua_pushlstring(L, per_stream_user_callbacks[stream_id].resp_payload.c_str(),
                            per_stream_user_callbacks[stream_id].resp_payload.size());

            if (trailer)
            {
                lua_createtable(L, 0, trailer->size());
                for (auto& header : *trailer)
                {
                    lua_pushlstring(L, header.first.c_str(), header.first.size());
                    lua_pushlstring(L, header.second.c_str(), header.second.size());
                    lua_rawset(L, -3);
                }
            }
            else
            {
                lua_createtable(L, 0, 0);
            }
        };

        if (per_stream_user_callbacks[stream_id].response_available)
        {
            push_response_to_lua_stack();
            lua_resume_if_yielded(L, 3);
            per_stream_user_callbacks.erase(stream_id);
        }
        else
        {
            auto callback = [push_response_to_lua_stack, L, this]()
            {
                push_response_to_lua_stack();
                lua_resume_if_yielded(L, 3);
            };
            per_stream_user_callbacks[stream_id].response_callback = callback;
        }
    }
    else
    {
        lua_createtable(L, 0, 0);
        lua_pushlstring(L, "", 0);
        lua_createtable(L, 0, 0);
        lua_resume_if_yielded(L, 3);
    }
}

std::string base_client::assemble_string(const String_With_Variables_In_Between& source, size_t scenario_index,
                                         size_t req_index, Scenario_Data_Per_User& scenario_data_per_user)
{
    std::string result;
    Scenario& scenario_template = config->json_config_schema.scenarios[scenario_index];
    auto string_size = std::accumulate(source.string_segments.begin(), source.string_segments.end(), 0, [](size_t count,
                                                                                                           const std::string & s)
    {
        return count + s.size();
    });

    for (auto& var_id : source.variable_ids_in_between)
    {
        if (var_id < scenario_template.number_of_variables_from_value_pickers)
        {
            string_size += scenario_data_per_user.variable_index_to_value[var_id].size();
        }
        else
        {
            string_size += scenario_template.variable_manager.estimate_value_size(var_id, req_index,
                                                                                  scenario_data_per_user.user_ids[var_id]);
        }
    }

    if (config->verbose)
    {
        std::cout << source << std::endl;
        std::cout << "variable_index_to_value size: " << scenario_data_per_user.variable_index_to_value.size() << std::endl;
        for (size_t index = 0; index < source.variable_ids_in_between.size(); index++)
        {
            std::cout << "string: " << source.string_segments[index] << std::endl;
            auto var_id = source.variable_ids_in_between[index];
            if (var_id < scenario_template.number_of_variables_from_value_pickers)
            {
                std::cout << "var: " << scenario_template.variable_manager.get_var_name(var_id)
                          << ", value: " << scenario_data_per_user.variable_index_to_value[var_id]
                          << std::endl;
            }
            else
            {
                std::cout << "var: " << scenario_template.variable_manager.get_var_name(var_id)
                          << ", value: "
                          << scenario_template.variable_manager.produce_value(var_id, req_index, scenario_data_per_user.user_ids[var_id])
                          << std::endl;
            }
        }

    }

    result.reserve(string_size);
    for (size_t index = 0; index < source.variable_ids_in_between.size(); index++)
    {
        result.append(source.string_segments[index]);
        auto var_id = source.variable_ids_in_between[index];
        if (var_id < scenario_template.number_of_variables_from_value_pickers)
        {
            result.append(scenario_data_per_user.variable_index_to_value[var_id]);
        }
        else
        {
            result.append(scenario_template.variable_manager.produce_value(var_id, req_index,
                                                                           scenario_data_per_user.user_ids[var_id]));
        }

    }
    result.append(source.string_segments.back());
    return result;
}

size_t base_client::get_number_of_request_left()
{
    return get_controller_client()->req_left;
}
void base_client::dec_number_of_request_left()
{
    --get_controller_client()->req_left;
}

#ifdef ENABLE_HTTP3

namespace
{
int handshake_completed(ngtcp2_conn* conn, void* user_data)
{
    auto c = static_cast<base_client*>(user_data);

    if (c->quic_handshake_completed() != 0)
    {
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }

    return 0;
}
} // namespace

int base_client::quic_handshake_completed()
{
    return connection_made();
}

namespace
{
int recv_stream_data(ngtcp2_conn* conn, uint32_t flags, int64_t stream_id,
                     uint64_t offset, const uint8_t* data, size_t datalen,
                     void* user_data, void* stream_user_data)
{
    auto c = static_cast<base_client*>(user_data);
    if (c->quic_recv_stream_data(flags, stream_id, data, datalen) != 0)
    {
        // TODO Better to do this gracefully rather than
        // NGTCP2_ERR_CALLBACK_FAILURE.  Perhaps, call
        // ngtcp2_conn_write_application_close() ?
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }
    return 0;
}
} // namespace

int base_client::quic_recv_stream_data(uint32_t flags, int64_t stream_id,
                                       const uint8_t* data, size_t datalen)
{
    if (worker->current_phase == Phase::MAIN_DURATION)
    {
        worker->stats.bytes_total += datalen;
    }

    auto s = static_cast<Http3Session*>(session.get());
    auto nconsumed = s->read_stream(flags, stream_id, data, datalen);
    if (nconsumed == -1)
    {
        return -1;
    }

    ngtcp2_conn_extend_max_stream_offset(quic.conn, stream_id, nconsumed);
    ngtcp2_conn_extend_max_offset(quic.conn, nconsumed);

    return 0;
}

namespace
{
int acked_stream_data_offset(ngtcp2_conn* conn, int64_t stream_id,
                             uint64_t offset, uint64_t datalen, void* user_data,
                             void* stream_user_data)
{
    auto c = static_cast<base_client*>(user_data);
    if (c->quic_acked_stream_data_offset(stream_id, datalen) != 0)
    {
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }
    return 0;
}
} // namespace

int base_client::quic_acked_stream_data_offset(int64_t stream_id, size_t datalen)
{
    auto s = static_cast<Http3Session*>(session.get());
    if (s->add_ack_offset(stream_id, datalen) != 0)
    {
        return -1;
    }
    return 0;
}

namespace
{
int stream_close(ngtcp2_conn* conn, uint32_t flags, int64_t stream_id,
                 uint64_t app_error_code, void* user_data,
                 void* stream_user_data)
{
    auto c = static_cast<base_client*>(user_data);

    if (!(flags & NGTCP2_STREAM_CLOSE_FLAG_APP_ERROR_CODE_SET))
    {
        app_error_code = NGHTTP3_H3_NO_ERROR;
    }

    if (c->quic_stream_close(stream_id, app_error_code) != 0)
    {
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }
    return 0;
}
} // namespace

int base_client::quic_stream_close(int64_t stream_id, uint64_t app_error_code)
{
    auto s = static_cast<Http3Session*>(session.get());
    if (s->close_stream(stream_id, app_error_code) != 0)
    {
        return -1;
    }
    return 0;
}

namespace
{
int stream_reset(ngtcp2_conn* conn, int64_t stream_id, uint64_t final_size,
                 uint64_t app_error_code, void* user_data,
                 void* stream_user_data)
{
    auto c = static_cast<base_client*>(user_data);
    if (c->quic_stream_reset(stream_id, app_error_code) != 0)
    {
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }
    return 0;
}
} // namespace

int base_client::quic_stream_reset(int64_t stream_id, uint64_t app_error_code)
{
    auto s = static_cast<Http3Session*>(session.get());
    if (s->shutdown_stream_read(stream_id) != 0)
    {
        return -1;
    }
    return 0;
}

namespace
{
int stream_stop_sending(ngtcp2_conn* conn, int64_t stream_id,
                        uint64_t app_error_code, void* user_data,
                        void* stream_user_data)
{
    auto c = static_cast<base_client*>(user_data);
    if (c->quic_stream_stop_sending(stream_id, app_error_code) != 0)
    {
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }
    return 0;
}
} // namespace

int base_client::quic_stream_stop_sending(int64_t stream_id,
                                          uint64_t app_error_code)
{
    auto s = static_cast<Http3Session*>(session.get());
    if (s->shutdown_stream_read(stream_id) != 0)
    {
        return -1;
    }
    return 0;
}

namespace
{
int extend_max_local_streams_bidi(ngtcp2_conn* conn, uint64_t max_streams,
                                  void* user_data)
{
    auto c = static_cast<base_client*>(user_data);

    if (c->quic_extend_max_local_streams() != 0)
    {
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }

    return 0;
}
} // namespace

int base_client::quic_extend_max_local_streams()
{
    auto s = static_cast<Http3Session*>(session.get());
    if (s->extend_max_local_streams() != 0)
    {
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }
    return 0;
}

namespace
{
int get_new_connection_id(ngtcp2_conn* conn, ngtcp2_cid* cid, uint8_t* token,
                          size_t cidlen, void* user_data)
{
    if (RAND_bytes(cid->data, cidlen) != 1)
    {
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }

    cid->datalen = cidlen;

    if (RAND_bytes(token, NGTCP2_STATELESS_RESET_TOKENLEN) != 1)
    {
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }

    return 0;
}
} // namespace

namespace
{
void debug_log_printf(void* user_data, const char* fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    fprintf(stderr, "\n");
}
} // namespace

namespace
{
int generate_cid(ngtcp2_cid& dest)
{
    dest.datalen = 8;

    if (RAND_bytes(dest.data, dest.datalen) != 1)
    {
        return -1;
    }

    return 0;
}
} // namespace

// qlog write callback -- excerpted from ngtcp2/examples/client_base.cc
namespace
{
void qlog_write_cb(void* user_data, uint32_t flags, const void* data,
                   size_t datalen)
{
    auto c = static_cast<base_client*>(user_data);
    c->quic_write_qlog(data, datalen);
}
} // namespace

void base_client::quic_write_qlog(const void* data, size_t datalen)
{
    assert(quic.qlog_file != nullptr);
    fwrite(data, 1, datalen, quic.qlog_file);
}

namespace
{
void rand(uint8_t* dest, size_t destlen, const ngtcp2_rand_ctx* rand_ctx)
{
    util::random_bytes(dest, dest + destlen,
                       *static_cast<std::mt19937*>(rand_ctx->native_handle));
}
} // namespace

namespace
{
int recv_rx_key(ngtcp2_conn* conn, ngtcp2_crypto_level level, void* user_data)
{
    if (level != NGTCP2_CRYPTO_LEVEL_APPLICATION)
    {
        return 0;
    }

    auto c = static_cast<base_client*>(user_data);

    if (c->quic_make_http3_session() != 0)
    {
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }

    return 0;
}
} // namespace

int base_client::quic_make_http3_session()
{
    auto s = std::make_unique<Http3Session>(this);
    if (s->init_conn() == -1)
    {
        return -1;
    }
    session = std::move(s);

    return 0;
}

namespace
{
ngtcp2_conn* get_conn(ngtcp2_crypto_conn_ref* conn_ref)
{
    auto c = static_cast<base_client*>(conn_ref->user_data);
    return c->quic.conn;
}
} // namespace

int base_client::quic_init(const sockaddr* local_addr, socklen_t local_addrlen,
                           const sockaddr* remote_addr, socklen_t remote_addrlen)
{
    int rv;

    quic.init();

    if (!ssl)
    {
        ssl = SSL_new(ssl_context);

        quic.conn_ref.get_conn = get_conn;
        quic.conn_ref.user_data = this;

        SSL_set_app_data(ssl, &quic.conn_ref);
        SSL_set_connect_state(ssl);
        SSL_set_quic_use_legacy_codepoint(ssl, 0);
    }
    if (config->json_config_schema.tls_keylog_file.size())
    {
        if (local_addr->sa_family == AF_INET)
        {
            tls_keylog_file_name = config->json_config_schema.tls_keylog_file + "_" + std::to_string(ntohs(((
                                                                                                                struct sockaddr_in*)local_addr)->sin_port)) + ".log";
        }
        else
        {
            tls_keylog_file_name = config->json_config_schema.tls_keylog_file + "_" + std::to_string(ntohs(((
                                                                                                                struct sockaddr_in6*)local_addr)->sin6_port)) + ".log";
        }
        std::remove(tls_keylog_file_name.c_str());
        SSL_set_ex_data(ssl, SSL_EXT_DATA_INDEX_KEYLOG_FILE, (void*)tls_keylog_file_name.c_str());
    }

    auto callbacks = ngtcp2_callbacks
    {
        ngtcp2_crypto_client_initial_cb,
        nullptr, // recv_client_initial
        ngtcp2_crypto_recv_crypto_data_cb,
        h2load::handshake_completed,
        nullptr, // recv_version_negotiation
        ngtcp2_crypto_encrypt_cb,
        ngtcp2_crypto_decrypt_cb,
        ngtcp2_crypto_hp_mask_cb,
        h2load::recv_stream_data,
        h2load::acked_stream_data_offset,
        nullptr, // stream_open
        h2load::stream_close,
        nullptr, // recv_stateless_reset
        ngtcp2_crypto_recv_retry_cb,
        h2load::extend_max_local_streams_bidi,
        nullptr, // extend_max_local_streams_uni
        h2load::rand,
        get_new_connection_id,
        nullptr, // remove_connection_id
        ngtcp2_crypto_update_key_cb,
        nullptr, // path_validation
        nullptr, // select_preferred_addr
        h2load::stream_reset,
        nullptr, // extend_max_remote_streams_bidi
        nullptr, // extend_max_remote_streams_uni
        nullptr, // extend_max_stream_data
        nullptr, // dcid_status
        nullptr, // handshake_confirmed
        nullptr, // recv_new_token
        ngtcp2_crypto_delete_crypto_aead_ctx_cb,
        ngtcp2_crypto_delete_crypto_cipher_ctx_cb,
        nullptr, // recv_datagram
        nullptr, // ack_datagram
        nullptr, // lost_datagram
        ngtcp2_crypto_get_path_challenge_data_cb,
        h2load::stream_stop_sending,
        nullptr, // version_negotiation
        h2load::recv_rx_key,
        nullptr, // recv_tx_key
    };

    ngtcp2_cid scid, dcid;
    if (generate_cid(scid) != 0)
    {
        return -1;
    }
    if (generate_cid(dcid) != 0)
    {
        return -1;
    }

    ngtcp2_settings settings;
    ngtcp2_settings_default(&settings);
    if (config->verbose)
    {
        settings.log_printf = debug_log_printf;
    }
    settings.cc_algo = static_cast<ngtcp2_cc_algo>(config->cc_algo);
    settings.initial_ts = current_timestamp_nanoseconds();
    settings.rand_ctx.native_handle = &worker->randgen;
    if (!config->qlog_file_base.empty())
    {
        assert(quic.qlog_file == nullptr);
        auto path = config->qlog_file_base;
        path += '.';
        path += util::utos(worker->id);
        path += '.';
        path += util::utos(id);
        path += ".sqlog";
        quic.qlog_file = fopen(path.c_str(), "w");
        if (quic.qlog_file == nullptr)
        {
            std::cerr << "Failed to open a qlog file: " << path << std::endl;
            return -1;
        }
        settings.qlog.write = qlog_write_cb;
    }
    if (config->max_udp_payload_size)
    {
        settings.max_udp_payload_size = config->max_udp_payload_size;
        settings.no_udp_payload_size_shaping = 1;
    }

    ngtcp2_transport_params params;
    ngtcp2_transport_params_default(&params);
    auto max_stream_data =
        std::min((1 << 26) - 1, (1 << config->window_bits) - 1);
    params.initial_max_stream_data_bidi_local = max_stream_data;
    params.initial_max_stream_data_uni = max_stream_data;
    params.initial_max_data = (1 << config->connection_window_bits) - 1;
    params.initial_max_streams_bidi = 0;
    params.initial_max_streams_uni = 100;
    params.max_idle_timeout = 30 * NGTCP2_SECONDS;

    auto path = ngtcp2_path
    {
        {
            const_cast<sockaddr*>(local_addr),
            local_addrlen,
        },
        {
            const_cast<sockaddr*>(remote_addr),
            remote_addrlen,
        },
    };

    assert(config->npn_list.size());

    uint32_t quic_version;

    if (config->npn_list[0] == NGHTTP3_ALPN_H3 || PROTO_HTTP3 == proto_type)
    {
        quic_version = NGTCP2_PROTO_VER_V1;
    }
    else
    {
        quic_version = NGTCP2_PROTO_VER_MIN;
    }

    rv = ngtcp2_conn_client_new(&quic.conn, &dcid, &scid, &path, quic_version,
                                &callbacks, &settings, &params, nullptr, this);
    if (rv != 0)
    {
        return -1;
    }

    ngtcp2_conn_set_tls_native_handle(quic.conn, ssl);

    return 0;
}

void base_client::quic_free()
{
    if (quic.conn)
    {
        ngtcp2_conn_del(quic.conn);
        quic.conn = nullptr;
        if (config->verbose)
        {
            std::cerr << __FUNCTION__ << ", timestamp:" << current_timestamp_nanoseconds() << std::endl;
        }
    }
    if (quic.qlog_file != nullptr)
    {
        fclose(quic.qlog_file);
        quic.qlog_file = nullptr;
    }
}

void base_client::request_connection_close()
{
    if (is_quic())
    {
        quic.close_requested = true;
        signal_write();
    }
}

bool base_client::is_quic()
{
    if (PROTO_TYPE::PROTO_UNSPECIFIED == proto_type)
    {
        return config->is_quic();
    }
    else
    {
        return (PROTO_TYPE::PROTO_HTTP3 == proto_type);
    }
}

#endif // ENABLE_HTTP3

}
