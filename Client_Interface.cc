#include <boost/asio/io_service.hpp>
#include <boost/thread/thread.hpp>
#include <regex>
#include <algorithm>
#include <cctype>
#include <execinfo.h>
#include <iomanip>
#include <string>

#include "Client_Interface.h"
#include "h2load_utils.h"


namespace h2load
{
std::atomic<uint64_t> Unique_Id::client_unique_id(0);

Unique_Id::Unique_Id()
{
    my_id = client_unique_id++;
}

Client_Interface::Client_Interface(uint32_t id, Worker_Interface* wrker, size_t req_todo, Config* conf,
                                   Client_Interface* parent, const std::string& dest_schema,
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
    rps_duration_started()
{
}

int Client_Interface::connect()
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
        start_conn_inactivity_timer();
    }

    auto rv = make_async_connection();
    if (rv != 0)
    {
        return rv;
    }
    start_connect_timeout_timer();
    return 0;
}

uint64_t Client_Interface::get_total_pending_streams()
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

void Client_Interface::record_ttfb()
{
    if (recorded(cstat.ttfb))
    {
        return;
    }

    cstat.ttfb = std::chrono::steady_clock::now();
}

void Client_Interface::log_failed_request(const h2load::Config& config, const h2load::Request_Data& failed_req,
                                          int32_t stream_id)
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

bool Client_Interface::validate_response_with_lua(lua_State* L, const Request_Data& finished_request)
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

void Client_Interface::record_stream_close_time(int32_t stream_id)
{
    auto req_stat = get_req_stat(stream_id);
    if (!req_stat)
    {
        return;
    }
    req_stat->stream_close_time = std::chrono::steady_clock::now();
}

void Client_Interface::connection_timeout_handler()
{
    stop_connect_timeout_timer();
    fail();
    reconnect_to_alt_addr();
    //ev_break (EV_A_ EVBREAK_ALL);
}

void Client_Interface::timing_script_timeout_handler()
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

void Client_Interface::brief_log_to_file(int32_t stream_id, bool success)
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


void Client_Interface::init_connection_targert()
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
            for (auto& host : config->json_config_schema.load_share_hosts)
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
        auto startIndex = this_client_id.my_id % hosts.size();
        authority = hosts[startIndex];
        preferred_authority = authority;
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
}

void Client_Interface::on_status_code(int32_t stream_id, uint16_t status)
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

void Client_Interface::enqueue_request(Request_Data& finished_request, Request_Data&& new_request)
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

bool Client_Interface::should_reconnect_on_disconnect()
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

void Client_Interface::inc_status_counter_and_validate_response(int32_t stream_id)
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


int Client_Interface::try_again_or_fail()
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

void Client_Interface::fail()
{
    disconnect();

    process_abandoned_streams();
}

void Client_Interface::timeout()
{
    if (should_reconnect_on_disconnect())
    {
        // it will need to reconnect anyway, why bother to disconnect
        return;
    }
    process_timedout_streams();

    disconnect();
}

void Client_Interface::process_abandoned_streams()
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

void Client_Interface::record_connect_start_time()
{
    cstat.connect_start_time = std::chrono::steady_clock::now();
}

void Client_Interface::record_connect_time()
{
    cstat.connect_time = std::chrono::steady_clock::now();
}


void Client_Interface::clear_connect_times()
{
    cstat.connect_start_time = std::chrono::steady_clock::time_point();
    cstat.connect_time = std::chrono::steady_clock::time_point();
    cstat.ttfb = std::chrono::steady_clock::time_point();
}

void Client_Interface::record_client_start_time()
{
    // Record start time only once at the very first connection is going
    // to be made.
    if (recorded(cstat.client_start_time))
    {
        return;
    }

    cstat.client_start_time = std::chrono::steady_clock::now();
}

void Client_Interface::record_client_end_time()
{
    // Unlike client_start_time, we overwrite client_end_time.  This
    // handles multiple connect/disconnect for HTTP/1.1 benchmark.
    cstat.client_end_time = std::chrono::steady_clock::now();
}

bool Client_Interface::prepare_next_request(Request_Data& finished_request)
{
    size_t scenario_index = finished_request.scenario_index;
    Scenario& scenario = config->json_config_schema.scenarios[scenario_index];

    size_t curr_index = ((finished_request.curr_request_idx + 1) % scenario.requests.size());
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
                std::cout << "response status code:" << finished_request.status_code << std::endl;
                std::cerr << "abort whole scenario sequence, as header not found: " << request_template.uri.input << std::endl;
                for (auto& header : finished_request.resp_headers)
                {
                    std::cout << header.first << ":" << header.second << std::endl;
                }
                std::cout << "response payload:" << finished_request.resp_payload << std::endl;
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

void Client_Interface::update_content_length(Request_Data& data)
{
    if (data.req_payload->size())
    {
        std::string content_length = "content-length";
        //data.req_headers.erase(content_length);
        data.shadow_req_headers[content_length] = std::to_string(data.req_payload->size());
    }
}

void Client_Interface::parse_and_save_cookies(Request_Data& finished_request)
{
    if (finished_request.resp_headers.find("Set-Cookie") != finished_request.resp_headers.end())
    {
        auto new_cookies = Cookie::parse_cookie_string(finished_request.resp_headers["Set-Cookie"],
                                                       *finished_request.authority, *finished_request.schema);
        for (auto& cookie : new_cookies)
        {
            if (Cookie::is_cookie_acceptable(cookie))
            {
                finished_request.saved_cookies[cookie.cookie_key] = std::move(cookie);
            }
        }
    }
}


void Client_Interface::populate_request_from_config_template(Request_Data& new_request,
                                                             size_t scenario_index,
                                                             size_t index_in_config_template)
{
    auto& request_template = config->json_config_schema.scenarios[scenario_index].requests[index_in_config_template];

    new_request.method = &request_template.method;
    new_request.schema = &request_template.schema;
    new_request.authority = &request_template.authority;
    new_request.string_collection.emplace_back(reassemble_str_with_variable(config, scenario_index,
                                                                            index_in_config_template,
                                                                            request_template.tokenized_payload,
                                                                            new_request.user_id));
    new_request.req_payload = &(new_request.string_collection.back());
    new_request.req_headers = &request_template.headers_in_map;
    new_request.expected_status_code = request_template.expected_status_code;
    new_request.delay_before_executing_next = request_template.delay_before_executing_next;
}

void Client_Interface::move_cookies_to_new_request(Request_Data& finished_request, Request_Data& new_request)
{
    new_request.saved_cookies.swap(finished_request.saved_cookies);
}


void Client_Interface::terminate_sub_clients()
{
    for (auto& sub_client : dest_clients)
    {
        if (sub_client.second != this && sub_client.second->session)
        {
            sub_client.second->terminate_session();
        }
    }
}

bool Client_Interface::is_test_finished()
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


void Client_Interface::update_this_in_dest_client_map()
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

void Client_Interface::init_lua_states()
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
            if (request.luaScript.size())
            {
                luaL_dostring(L, request.luaScript.c_str());
            }
            requests_lua_states.push_back(L);
        }
        lua_states.push_back(requests_lua_states);
    }
}


void Client_Interface::slice_user_id()
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
                auto tokens_per_client = ((scenario.variable_range_end - scenario.variable_range_start) / (config->nclients));
                if (tokens_per_client == 0)
                {
                    if (worker->id == 0)
                    {
                        std::cerr << "Error: number of user IDs is smaller than number of clients, cannot continue" << std::endl;
                        exit(EXIT_FAILURE);
                    }
                    else
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                        return;
                    }
                }
                auto tokens_left = ((scenario.variable_range_end - scenario.variable_range_start) % (config->nclients));
                scenario_data.req_variable_value_start = scenario.variable_range_start +
                                                         (this_client_id.my_id * tokens_per_client) +
                                                         std::min(this_client_id.my_id, tokens_left);
                scenario_data.req_variable_value_end = scenario_data.req_variable_value_start +
                                                       tokens_per_client +
                                                       (this_client_id.my_id >= tokens_left ? 0 : 1);

                scenario_data.curr_req_variable_value = scenario_data.req_variable_value_start;

                if (config->verbose)
                {
                    std::cerr << ", client Id:" << this_client_id.my_id
                              << ", scenario index: " << index
                              << ", variable id start:" << scenario_data.req_variable_value_start
                              << ", variable id end:" << scenario_data.req_variable_value_end
                              << std::endl;
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


bool Client_Interface::rps_mode()
{
    return (rps > 0.0);
}

void Client_Interface::update_scenario_based_stats(size_t scenario_index, size_t request_index, bool success,
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


size_t Client_Interface::get_index_of_next_scenario_to_run()
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
            totalWeight += ((scenario.weight * common_multiple) / scenario.requests.size());
            schedule_map[totalWeight] = scenario_index;
        }
        return schedule_map;
    };

    static thread_local auto schedule_map = init_schedule_map(config, find_common_multiple(init_size_vec(config)));
    static thread_local auto total_weight = (schedule_map.rbegin()->first);
    static thread_local std::random_device                  randDev;
    static thread_local std::mt19937_64                     generator(randDev());
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

void Client_Interface::submit_ping()
{
    session->submit_ping();
    signal_write();
}

void Client_Interface::produce_request_cookie_header(Request_Data& req_to_be_sent)
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
        for (auto& cookie : cookie_vec)
        {
            cookies_from_config.insert(cookie.cookie_key);
        }
    }
    const std::string cookie_delimeter = "; ";
    std::string cookies_to_append;
    for (auto& cookie : req_to_be_sent.saved_cookies)
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

bool Client_Interface::update_request_with_lua(lua_State* L, const Request_Data& finished_request,
                                               Request_Data& request_to_send)
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
        for (auto& header : * (request_to_send.req_headers))
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
                    request_to_send.authority = &(request_to_send.string_collection.back());
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


void Client_Interface::terminate_session()
{
    session->terminate();
    // http1 session needs writecb to tear down session.
    signal_write();
}

void Client_Interface::reset_timeout_requests()
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

void Client_Interface::on_rps_timer()
{
    reset_timeout_requests();
    assert(!config->timing_script);

    if (req_left == 0)
    {
        stop_rps_timer();
        return;
    }

    auto now = std::chrono::steady_clock::now();
    auto d = now - rps_duration_started;
    auto n = static_cast<size_t>(round(std::chrono::duration<double>(d).count() * config->rps));
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
void Client_Interface::process_timedout_streams()
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

void Client_Interface::on_stream_close(int32_t stream_id, bool success, bool final)
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

int Client_Interface::connection_made()
{
    auto ret = select_protocol_and_allocate_session();
    if (ret != 0)
    {
        return ret;
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

    start_request_delay_execution_timer();

    if (rps_mode())
    {
        start_rps_timer();
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

    if (authority != preferred_authority && config->json_config_schema.connect_back_to_preferred_host)
    {
        start_connect_to_preferred_host_timer();
    }

    return 0;
}

void Client_Interface::on_data_chunk(int32_t stream_id, const uint8_t* data, size_t len)
{
    auto request = requests_awaiting_response.find(stream_id);
    if (request != requests_awaiting_response.end())
    {
        request->second.resp_payload.append((const char*)data, len);
    }
    if (config->verbose)
    {
        std::string str((const char*)data, len);
        std::cout << "received data: " << std::endl << str << std::endl;
    }
}

void Client_Interface::resume_delayed_request_execution()
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

RequestStat* Client_Interface::get_req_stat(int32_t stream_id)
{
    auto it = streams.find(stream_id);
    if (it == std::end(streams))
    {
        return nullptr;
    }

    return &(*it).second.req_stat;
}

void Client_Interface::on_request_start(int32_t stream_id)
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
    bool stats_eligible = (worker->current_phase == Phase::MAIN_DURATION
                           || worker->current_phase == Phase::MAIN_DURATION_GRACEFUL_SHUTDOWN);
    streams.insert(std::make_pair(stream_id, Stream(scenario_index, request_index, stats_eligible)));
    auto curr_timepoint = std::chrono::steady_clock::now();
    stream_timestamp.insert(std::make_pair(curr_timepoint, stream_id));
}

void Client_Interface::record_request_time(RequestStat* req_stat)
{
    req_stat->request_time = std::chrono::steady_clock::now();
    req_stat->request_wall_time = std::chrono::system_clock::now();
}

Request_Data Client_Interface::get_request_to_submit()
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

void Client_Interface::on_header(int32_t stream_id, const uint8_t* name, size_t namelen,
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

        on_status_code(stream_id, status);
    }
}

Config* Client_Interface::get_config()
{
    return config;
}
Stats& Client_Interface::get_stats()
{
    return worker->stats;
}


Client_Interface* Client_Interface::get_controller_client()
{
    return parent_client;
}

size_t& Client_Interface::get_current_req_index()
{
    return reqidx;
}

std::map<int32_t, Request_Data>& Client_Interface::requests_waiting_for_response()
{
    return requests_awaiting_response;
}

bool Client_Interface::is_final()
{
    return final;
}
void Client_Interface::set_final(bool val)
{
    final = val;
}
size_t Client_Interface::get_req_left()
{
    return req_left;
}

Client_Interface* Client_Interface::find_or_create_dest_client(Request_Data& request_to_send)
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


bool Client_Interface::is_controller_client()
{
    return (parent_client == nullptr);
}

Request_Data Client_Interface::prepare_first_request()
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
        if (controller->runtime_scenario_data[scenario_index].curr_req_variable_value >=
            controller->runtime_scenario_data[scenario_index].req_variable_value_end)
        {
            std::cerr << "user id (variable_value) wrapped, start over from range start" << ", scenario index: " << scenario_index
                      << std::endl;
            controller->runtime_scenario_data[scenario_index].curr_req_variable_value =
                controller->runtime_scenario_data[scenario_index].req_variable_value_start;
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
            std::cerr << "lua script failure for first request, cannot continue, exit" << std::endl;
            exit(EXIT_FAILURE);
        }
    }
    update_content_length(new_request);

    return new_request;
}

int Client_Interface::submit_request()
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
                start_conn_active_watcher(client.second);
            }
        }
    }

    return 0;
}

void Client_Interface::process_request_failure(int errCode)
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

void Client_Interface::print_app_info()
{
    if (worker->id == 0 && !worker->app_info_report_done)
    {
        worker->app_info_report_done = true;
        std::cerr << "Application protocol: " << selected_proto << std::endl;
    }
}

}
