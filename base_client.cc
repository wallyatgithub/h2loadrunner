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
                         base_client* parent, const std::string& dest_schema,
                         const std::string& dest_authority):
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
    new_connection_requested(false),
    final(false),
    rps_req_pending(0),
    rps_req_inflight(0),
    parent_client(parent),
    schema(dest_schema),
    authority(dest_authority),
    rps(conf->rps),
    this_client_id(),
    rps_duration_started(),
#ifdef ENABLE_HTTP3
    quic {},
#endif
    ssl(nullptr)
{
    init_req_left();

    slice_var_ids();

    init_lua_states();

    update_this_in_dest_client_map();

#ifdef ENABLE_HTTP3
    ngtcp2_connection_close_error_default(&quic.last_error);
#endif // ENABLE_HTTP3

}

base_client::~base_client()
{
#ifdef ENABLE_HTTP3
    if (config->is_quic())
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
        start_warmup_timer();
    }

    if (worker->config->conn_inactivity_timeout > 0.)
    {
        start_conn_inactivity_watcher();
    }

    auto rv = make_async_connection();
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
        for (auto& client : dest_clients)
        {
            pendingStreams += client.second->streams.size();
        }
        return pendingStreams;
    }
}

void base_client::try_new_connection()
{
    new_connection_requested = true;
}

void base_client::record_ttfb()
{
    if (recorded(cstat.ttfb))
    {
        return;
    }

    cstat.ttfb = std::chrono::steady_clock::now();
}

void base_client::log_failed_request(const h2load::Config& config, const h2load::Request_Data& failed_req,
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

bool base_client::validate_response_with_lua(lua_State* L, const Request_Data& finished_request)
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
    reconnect_to_alt_addr();
    //ev_break (EV_A_ EVBREAK_ALL);
}

void base_client::reconnect_to_used_host()
{
    if (CLIENT_CONNECTED == state)
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

bool base_client::reconnect_to_alt_addr()
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

    if (req_left == 0)
    {
        stop_timing_script_request_timeout_timer();
        return;
    }

    double duration =
        config->timings[reqidx] - config->timings[reqidx - 1];

    while (duration < 1e-9)
    {
        if (submit_request() != 0)
        {
            stop_timing_script_request_timeout_timer();
            return;
        }
        signal_write();
        if (req_left == 0)
        {
            stop_timing_script_request_timeout_timer();
            return;
        }

        duration =
            config->timings[reqidx] - config->timings[reqidx - 1];
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
        authority = host + ":" + util::utos(config->port);
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

        setup_connect_with_async_fqdn_lookup();
    }
    preferred_authority = authority;
}

void base_client::set_prefered_authority(const std::string& authority)
{
    preferred_authority = authority;
}

void base_client::on_status_code(int64_t stream_id, uint16_t status)
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

void base_client::sanitize_request(Request_Data& new_request)
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

void base_client::enqueue_request(Request_Data& finished_request, Request_Data&& new_request)
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

bool base_client::should_reconnect_on_disconnect()
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

    auto request_data = requests_awaiting_response.find(stream_id);
    if (request_data != requests_awaiting_response.end())
    {
        auto scenario_index = request_data->second.scenario_index;
        auto request_index = request_data->second.curr_request_idx;
        if (scenario_index < worker->scenario_stats.size() &&
            request_index < worker->scenario_stats[scenario_index].size() &&
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
                for (auto& match_rule : request.response_match_rules)
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
            if (status == request_data->second.expected_status_code)
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
        log_failed_request(*config, request_data->second, stream_id);
    }
}


int base_client::try_again_or_fail()
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
            if (!is_test_finished())
            {
                std::cerr << "connect to host cannot be done:" << authority << std::endl;
            }
        }
    }

    process_abandoned_streams();

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

void base_client::final_cleanup()
{
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
}

void base_client::cleanup_due_to_disconnect()
{
    if (CLIENT_CONNECTED == state)
    {
        std::cerr << "===============disconnected from " << authority << "===============" << std::endl;
    }

    worker->get_client_ids().erase(this->get_client_unique_id());
    for (auto& client_set : worker->get_client_pool())
    {
        client_set.second.erase(this);
    }

    record_client_end_time();
    streams.clear();
    session.reset();
    state = CLIENT_IDLE;

    auto iter = stream_user_callback_queue.begin();
    while (iter != stream_user_callback_queue.end())
    {
        auto cb_queue_size_before_cb = stream_user_callback_queue.size();
        if (iter->second.response_callback)
        {
            iter->second.response_callback();
        }
        auto cb_queue_size_after_cb = stream_user_callback_queue.size();

        // iter cb function removed itself from the queue, proceed with the new begin()
        if (cb_queue_size_before_cb - cb_queue_size_after_cb >= 1)
        {
            iter =  stream_user_callback_queue.begin();
        }
        else
        {
            // no removal done by iter cb, manually remove iter and proceed with next
            iter = stream_user_callback_queue.erase(iter);
        }
    }

    call_connected_callbacks(false);

    for (auto& req : requests_awaiting_response)
    {
        if (req.second.request_sent_callback)
        {
            req.second.request_sent_callback(-1, this);
            auto dummy = std::move(req.second.request_sent_callback);
        }
    }

    for (auto& req : requests_to_submit)
    {
        if (req.request_sent_callback)
        {
            req.request_sent_callback(-1, this);
        }
    }
    requests_to_submit.clear();
}

void base_client::record_client_end_time()
{
    // Unlike client_start_time, we overwrite client_end_time.  This
    // handles multiple connect/disconnect for HTTP/1.1 benchmark.
    cstat.client_end_time = std::chrono::steady_clock::now();
}

void base_client::run_post_response_action(Request_Data& finished_request)
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

void base_client::run_pre_request_action(Request_Data& new_request)
{

}

bool base_client::parse_uri_and_poupate_request(const std::string& uri, Request_Data& new_request)
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

void base_client::parse_and_save_cookies(Request_Data& finished_request)
{
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


void base_client::populate_request_from_config_template(Request_Data& new_request,
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
    for (auto& h : request_template.headers_with_variable)
    {
        auto name = assemble_string(h.first, scenario_index, request_index, *new_request.scenario_data_per_user);
        auto value = assemble_string(h.second, scenario_index, request_index, *new_request.scenario_data_per_user);
        new_request.req_headers_of_individual.emplace(std::make_pair(name, value));
    }
}

void base_client::terminate_sub_clients()
{
    for (auto& sub_client : dest_clients)
    {
        if (sub_client.second != this && sub_client.second->session)
        {
            sub_client.second->terminate_session();
        }
    }
}

bool base_client::is_test_finished()
{
    if (0 == req_left || worker->current_phase > Phase::MAIN_DURATION)
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
    auto& clients = parent_client ? parent_client->dest_clients : dest_clients;
    for (auto it = clients.begin(); it != clients.end();)
    {
        if (it->second == this)
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
            Scenario_Data_Per_Client scenario_data_per_conn(range_start, range_end, curr_vals);
            scenario_data_per_connection.push_back(scenario_data_per_conn);
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
            Scenario_Data_Per_Client scenario_data_per_conn(range_start, range_end, curr_vals);
            scenario_data_per_connection.push_back(scenario_data_per_conn);
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

void base_client::produce_request_cookie_header(Request_Data& req_to_be_sent)
{
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

bool base_client::update_request_with_lua(lua_State* L, const Request_Data& finished_request,
                                          Request_Data& request_to_send)
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
                    headers.erase(method_header);
                    request_to_send.string_collection.emplace_back(headers[path_header]);
                    request_to_send.path = &(request_to_send.string_collection.back());
                    headers.erase(path_header);
                    request_to_send.string_collection.emplace_back(headers[authority_header]);
                    request_to_send.authority = &(request_to_send.string_collection.back());
                    headers.erase(authority_header);
                    request_to_send.string_collection.emplace_back(headers[scheme_header]);
                    request_to_send.schema = &(request_to_send.string_collection.back());
                    headers.erase(scheme_header);
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
    session->terminate();
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
    if (CLIENT_CONNECTED != state)
    {
        return;
    }

    if (req_left == 0)
    {
        stop_rps_timer();
        return;
    }

    auto now = std::chrono::steady_clock::now();
    auto d = now - rps_duration_started;
    auto duration = std::chrono::duration<double>(d).count();
    if (duration < 0.0)
    {
        return;
    }
    auto n = static_cast<size_t>(round(duration * config->rps));
    rps_req_pending = n; // += n; do not accumulate to avoid burst of load
    rps_duration_started = now - d + std::chrono::duration<double>(static_cast<double>(n) / config->rps);

    if (rps_req_pending == 0)
    {
        return;
    }

    auto nreq = session->max_concurrent_streams() - streams.size();
    if (nreq == 0)
    {
        return;
    }

    nreq = config->is_timing_based_mode() ? std::max(nreq, req_left)
           : std::min(nreq, req_left);
    nreq = std::min(nreq, rps_req_pending);

    for (; nreq > 0; --nreq)
    {
        auto retCode = submit_request();
        if (retCode != 0)
        {
            break;
        }
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

void base_client::on_stream_close(int64_t stream_id, bool success, bool final)
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
        size_t scenario_index = finished_request->second.scenario_index;
        Scenario& scenario = config->json_config_schema.scenarios[scenario_index];
        if (!scenario.run_requests_in_parallel)
        {
            prepare_next_request(finished_request->second);
        }
        process_stream_user_callback(stream_id);
        requests_awaiting_response.erase(finished_request);
    }
    streams.erase(stream_id);

    if (req_left == 0 && req_inflight == 0)
    {
        setup_graceful_shutdown();
        terminate_session();
        return;
    }

    if (!final && req_left > 0)
    {
        if (config->timing_script)
        {
            feed_timing_script_request_timeout_timer();
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

void base_client::on_header_frame_begin(int64_t stream_id, uint8_t flags)
{
    auto it = requests_awaiting_response.find(stream_id);
    if (it != requests_awaiting_response.end())
    {
        Request_Data& request_data = it->second;
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

int base_client::connection_made()
{
    stop_connect_timeout_timer();

    auto ret = select_protocol_and_allocate_session();
    if (ret != 0)
    {
        return ret;
    }

    std::string base_uri = schema;
    base_uri.append("://").append(authority);
    worker->get_client_pool()[base_uri].insert(this);
    worker->get_client_ids()[this->get_client_unique_id()] = this;

    state = CLIENT_CONNECTED;

    session->on_connect();

    record_connect_time();

    update_this_in_dest_client_map();

    if (parent_client != nullptr)
    {
        while (requests_to_submit.size())
        {
            // re-push to parent to be scheduled immediately
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

    start_request_delay_execution_timer();

    if (rps_mode())
    {
        start_rps_timer();
        rps_duration_started = std::chrono::steady_clock::now();
    }

    start_stream_timeout_timer();

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

        double duration = config->timings[reqidx];

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
            start_timing_script_request_timeout_timer(duration);
        }
    }
    signal_write();
    std::cerr << "===============connected to " << authority << "===============" << std::endl;
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
    auto request = requests_awaiting_response.find(stream_id);
    if (request != requests_awaiting_response.end())
    {
        request->second.resp_payload.append((const char*)data, len); // TODO: handle grpc payload
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

void base_client::on_request_start(int64_t stream_id)
{
    size_t scenario_index = 0;
    size_t request_index = 0;
    auto request_data = requests_awaiting_response.find(stream_id);
    if ((worker->scenario_stats.size() > 0) && (request_data != requests_awaiting_response.end()))
    {
        scenario_index = request_data->second.scenario_index;
        request_index = request_data->second.curr_request_idx;
    }

    if (streams.count(stream_id))
    {
        streams.erase(stream_id);
    }
    bool stats_eligible = (worker->current_phase == Phase::MAIN_DURATION
                           || worker->current_phase == Phase::MAIN_DURATION_GRACEFUL_SHUTDOWN);
    streams.insert(std::make_pair(stream_id, Stream(scenario_index, request_index, stats_eligible)));
    auto curr_timepoint = std::chrono::steady_clock::now();
    auto timeout_interval = request_data->second.stream_timeout_in_ms;
    if (!timeout_interval)
    {
        timeout_interval = config->json_config_schema.stream_timeout_in_ms;
    }
    auto timeout_timepoint = curr_timepoint + std::chrono::milliseconds(timeout_interval);
    stream_timestamp.insert(std::make_pair(timeout_timepoint, stream_id));

    if ((request_data != requests_awaiting_response.end()) && (request_data->second.request_sent_callback))
    {
        request_data->second.request_sent_callback(stream_id, this);
        auto dummy = std::move(request_data->second.request_sent_callback);
    }

}

void base_client::record_request_time(RequestStat* req_stat)
{
    req_stat->request_time = std::chrono::steady_clock::now();
    req_stat->request_wall_time = std::chrono::system_clock::now();
}

Request_Data base_client::get_request_to_submit()
{
    if (!requests_to_submit.empty())
    {
        auto queued_request = std::move(requests_to_submit.front());
        requests_to_submit.pop_front();
        return queued_request;
    }
    else
    {
        std::cerr << "this is not expected; contact support to report this error" << std::endl;
        printBacktrace();
        abort(); // this should never happen
        return prepare_first_request();
    }
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

    auto request = requests_awaiting_response.find(stream_id);
    if (request != requests_awaiting_response.end())
    {
        std::string header_name;
        header_name.assign((const char*)name, namelen);
        std::string header_value;
        header_value.assign((const char*)value, valuelen);
        header_value.erase(0, header_value.find_first_not_of(' '));
        assert(request->second.resp_headers.size());
        auto it = request->second.resp_headers.back().find(header_name);
        if (it != request->second.resp_headers.back().end())
        {
            // Set-Cookie case most likely
            it->second.append("; ").append(header_value);
        }
        else
        {
            request->second.resp_headers.back()[header_name] = header_value;
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
    return parent_client;
}

size_t& base_client::get_current_req_index()
{
    return reqidx;
}

std::map<int64_t, Request_Data>& base_client::requests_waiting_for_response()
{
    return requests_awaiting_response;
}

bool base_client::is_final()
{
    return final;
}
void base_client::set_final(bool val)
{
    final = val;
}
size_t base_client::get_req_left()
{
    return req_left;
}

base_client* base_client::find_or_create_dest_client(Request_Data& request_to_send)
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
            auto new_client = create_dest_client(*request_to_send.schema, *request_to_send.authority);
            worker->check_in_client(new_client);
            dest_clients[dest] = new_client.get();
            new_client->connect_to_host(new_client->schema, new_client->authority);
        }
        return dest_clients[dest];
    }
    else
    {
        return parent_client->find_or_create_dest_client(request_to_send);
    }
}


bool base_client::is_controller_client()
{
    return (parent_client == nullptr);
}

Request_Data base_client::prepare_first_request()
{
    auto controller = parent_client ? parent_client : this;

    size_t scenario_index = get_index_of_next_scenario_to_run();

    auto& scenario = config->json_config_schema.scenarios[scenario_index];

    Request_Data new_request(controller->scenario_data_per_connection[scenario_index].get_curr_vars());

    controller->scenario_data_per_connection[scenario_index].inc_var();

    new_request.scenario_index = scenario_index;

    new_request.curr_request_idx = 0;

    populate_request_from_config_template(new_request, scenario_index, new_request.curr_request_idx);

    auto& request_template = scenario.requests[new_request.curr_request_idx];

    if (INPUT_WITH_VARIABLE == request_template.uri.uri_action)
    {
        auto uri = assemble_string(request_template.tokenized_path_with_vars, scenario_index, new_request.curr_request_idx,
                                   *new_request.scenario_data_per_user);
        if (!parse_uri_and_poupate_request(uri, new_request))
        {
            std::cerr << "abort whole scenario sequence, as uri is invalid:" << uri << std::endl;
            abort();
        }
    }
    else
    {
        new_request.string_collection.emplace_back(assemble_string(request_template.tokenized_path_with_vars, scenario_index,
                                                                   new_request.curr_request_idx, *new_request.scenario_data_per_user));
        new_request.path = &(new_request.string_collection.back());
    }

    if (scenario.requests[new_request.curr_request_idx].make_request_function_present)
    {
        static thread_local Request_Data dummy_data(0);
        if (!update_request_with_lua(lua_states[scenario_index][new_request.curr_request_idx], dummy_data, new_request))
        {
            std::cerr << "lua script failure for first request, cannot continue, exit" << std::endl;
            exit(EXIT_FAILURE);
        }
    }
    update_content_length(new_request);
    sanitize_request(new_request);

    if (scenario.run_requests_in_parallel)
    {
        for (auto req_idx = new_request.curr_request_idx; req_idx < scenario.requests.size() - 1; req_idx++)
        {
            prepare_next_request(new_request);
        }
    }
    return new_request;
}

bool base_client::prepare_next_request(Request_Data& finished_request)
{
    run_post_response_action(finished_request);

    size_t scenario_index = finished_request.scenario_index;
    Scenario& scenario = config->json_config_schema.scenarios[scenario_index];

    size_t curr_req_index = ((finished_request.curr_request_idx + 1) % scenario.requests.size());
    if (curr_req_index == 0)
    {
        return false;
    }
    auto& request_template = scenario.requests[curr_req_index];
    if (!request_template.clear_old_cookies)
    {
        parse_and_save_cookies(finished_request);
    }
    std::unique_ptr<Request_Data> new_request;
    if (scenario.run_requests_in_parallel)
    {
        new_request = std::make_unique<Request_Data>(finished_request.scenario_data_per_user);
    }
    else
    {
        new_request = std::make_unique<Request_Data>(std::move(finished_request.scenario_data_per_user));
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
            new_request->string_collection.emplace_back(*finished_request.path);
            new_request->path = &(new_request->string_collection.back());
            new_request->string_collection.emplace_back(*finished_request.schema);
            new_request->schema = &(new_request->string_collection.back());
            new_request->string_collection.emplace_back(*finished_request.authority);
            new_request->authority = &(new_request->string_collection.back());
            break;
        }
        case FROM_RESPONSE_HEADER:
        {
            std::string* uri_header_value = nullptr;
            for (auto& header_map : finished_request.resp_headers)
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
                    new_request->string_collection.emplace_back(*finished_request.schema);
                    new_request->schema = &(new_request->string_collection.back());
                    new_request->string_collection.emplace_back(*finished_request.authority);
                    new_request->authority = &(new_request->string_collection.back());
                }
            }
            else
            {
                if (config->verbose)
                {
                    std::cout << "response status code:" << finished_request.status_code << std::endl;
                    std::cerr << "abort whole scenario sequence, as header not found: " << request_template.uri.input << std::endl;
                    for (auto& header_map : finished_request.resp_headers)
                    {
                        for (auto& header : header_map)
                        {
                            std::cout << header.first << ":" << header.second << std::endl;
                        }
                    }
                    std::cout << "response payload:" << finished_request.resp_payload << std::endl;
                }
                return false;
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
                return false;
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
        if (!update_request_with_lua(lua_states[scenario_index][curr_req_index], finished_request, *new_request))
        {
            return false; // lua script returns error or kills the request, abort this scenario
        }
    }

    update_content_length(*new_request);
    sanitize_request(*new_request);

    enqueue_request(finished_request, std::move(*new_request));

    return true;
}

void base_client::update_content_length(Request_Data& data)
{
    if (data.req_payload->size())
    {
        std::string content_length = "content-length";
        data.req_headers_of_individual[content_length] = std::to_string(data.req_payload->size());
    }
}


int base_client::submit_request()
{
    if (is_null_destination(*config) && requests_to_submit.empty())
    {
        return 0;
    }

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

    decltype(this) destination_client = this;
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
            if (scenario_index < worker->scenario_stats.size() &&
                request_index < worker->scenario_stats[scenario_index].size())
            {
                ++worker->scenario_stats[scenario_index][request_index]->req_started;
            }
        }
        // if an active timeout is set and this is the last request to be submitted
        // on this connection, start the active timeout.
        if (worker->config->conn_active_timeout > 0. && req_left == 0 && is_controller_client())
        {
            for (auto& client : dest_clients) // "this" is also in dest_clients
            {
                client.second->start_conn_active_watcher();
            }
        }
    }

    return 0;
}

void base_client::process_request_failure(int errCode)
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
        graceful_restart_connection();
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

void base_client::print_app_info()
{
    if (worker->id == 0 && !worker->app_info_report_done)
    {
        worker->app_info_report_done = true;
        std::cerr << "Application protocol: " << selected_proto << std::endl;
    }
}

int base_client::select_protocol_and_allocate_session()
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
            if (config->is_quic())
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
        auto proto = config->no_tls_proto;
        if (preferred_non_tls_proto.size())
        {
            if (preferred_non_tls_proto == "h2c")
            {
                proto = h2load::Config::PROTO_HTTP2;
            }
            else
            {
                proto = h2load::Config::PROTO_HTTP1_1;
            }
        }
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
    if (http_parser_parse_url(authority.c_str(), authority.size(), true, &u) == 0 && util::has_uri_field(u, UF_HOST))
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

    if (stream_user_callback_queue.size() > MAX_STREAM_SAVED_FOR_CALLBACK)
    {
        stream_user_callback_queue.erase(stream_user_callback_queue.begin());
    }
    stream_user_callback_queue[stream_id].stream_id = stream_id;
}

void base_client::process_stream_user_callback(int64_t stream_id)
{
    auto it = requests_awaiting_response.find(stream_id);
    if (it !=  requests_awaiting_response.end() && stream_user_callback_queue.count(stream_id))
    {
        stream_user_callback_queue[stream_id].resp_headers = std::move(it->second.resp_headers);
        stream_user_callback_queue[stream_id].resp_payload = std::move(it->second.resp_payload);
        stream_user_callback_queue[stream_id].response_available = true;
        stream_user_callback_queue[stream_id].resp_trailer_present = it->second.resp_trailer_present;
        if (stream_user_callback_queue[stream_id].response_callback)
        {
            stream_user_callback_queue[stream_id].response_callback();
            stream_user_callback_queue.erase(stream_id);
        }
    }
}

void base_client::pass_response_to_lua(int64_t stream_id, lua_State* L)
{
    if (stream_user_callback_queue.count(stream_id))
    {
        auto push_response_to_lua_stack = [this, L, stream_id]()
        {
            std::map<std::string, std::string, ci_less>* trailer = nullptr;
            if (stream_user_callback_queue[stream_id].resp_headers.size() > 1 &&
                stream_user_callback_queue[stream_id].resp_trailer_present)
            {
                trailer = &stream_user_callback_queue[stream_id].resp_headers.back();
                if (config->verbose)
                {
                    std::cout << "number of header frames: " << stream_user_callback_queue[stream_id].resp_headers.size()
                              << ", trailer found" << std::endl;
                }
            }
            lua_createtable(L, 0, std::accumulate(stream_user_callback_queue[stream_id].resp_headers.begin(),
                                                  stream_user_callback_queue[stream_id].resp_headers.end(),
                                                  0,
                                                  [trailer](uint64_t sum, const std::map<std::string, std::string, ci_less>& val)
            {
                return sum + (&val == trailer ? 0 : val.size());
            }));
            for (auto& header_map : stream_user_callback_queue[stream_id].resp_headers)
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

            lua_pushlstring(L, stream_user_callback_queue[stream_id].resp_payload.c_str(),
                            stream_user_callback_queue[stream_id].resp_payload.size());

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

        if (stream_user_callback_queue[stream_id].response_available)
        {
            push_response_to_lua_stack();
            lua_resume_if_yielded(L, 3);
            stream_user_callback_queue.erase(stream_id);
        }
        else
        {
            auto callback = [push_response_to_lua_stack, L, this]()
            {
                push_response_to_lua_stack();
                lua_resume_if_yielded(L, 3);
            };
            stream_user_callback_queue[stream_id].response_callback = callback;
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

    if (!ssl)
    {
        ssl = SSL_new(worker->get_ssl_ctx());

        quic.conn_ref.get_conn = get_conn;
        quic.conn_ref.user_data = this;

        SSL_set_app_data(ssl, &quic.conn_ref);
        SSL_set_connect_state(ssl);
        SSL_set_quic_use_legacy_codepoint(ssl, 0);
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

    auto config = worker->config;

    ngtcp2_settings settings;
    ngtcp2_settings_default(&settings);
    if (config->verbose)
    {
        settings.log_printf = debug_log_printf;
    }
    //settings.cc_algo = config.cc_algo; // TODO: 
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

    if (config->npn_list[0] == NGHTTP3_ALPN_H3)
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
            std::cerr << __FUNCTION__ << ", timestamp:"<<current_timestamp_nanoseconds() << std::endl;
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
    if (config->is_quic())
    {
        quic.close_requested = true;
        signal_write();
    }
}


#endif // ENABLE_HTTP3

}
