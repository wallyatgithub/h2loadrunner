#include <algorithm>
#include <numeric>
#include <cctype>
#include <mutex>
#include <iterator>
#include <future>

#include <iomanip>
#include <iostream>
#include <fstream>
#include <string>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <boost/asio.hpp>

extern "C" {
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
}
#ifndef LUA_OK
#define LUA_OK 0
#endif

#include <fstream>
#include <fcntl.h>

#include "h2load_utils.h"
#include "base_client.h"

#include "asio_worker.h"
#include "h2load_lua.h"
#include "asio_util.h"


const static std::string worker_index_str = "worker_index";
const static std::string group_index_str = "group_index";
const static std::string server_id_str = "server_id";
const static std::string inactive_state = "inactive_state";


void set_group_id(lua_State* L, size_t group_id)
{
    lua_pushlightuserdata(L, (void *)group_index_str.c_str());
    lua_pushinteger(L, group_id);
    lua_rawset(L, LUA_REGISTRYINDEX);
}

void set_worker_index(lua_State* L, size_t worker_index)
{
    lua_pushlightuserdata(L, (void *)worker_index_str.c_str());
    lua_pushinteger(L, worker_index);
    lua_rawset(L, LUA_REGISTRYINDEX);
}

size_t get_group_id(lua_State* L)
{
    size_t group_id = 0;
    auto top_before = lua_gettop(L);
    lua_pushlightuserdata(L, (void *)group_index_str.c_str());
    lua_gettable(L, LUA_REGISTRYINDEX);
    group_id = lua_tonumber(L, -1);
    lua_settop(L, top_before);
    return group_id;
}

size_t get_worker_index(lua_State* L)
{
    auto top_before = lua_gettop(L);
    lua_pushlightuserdata(L, (void *)worker_index_str.c_str());
    lua_gettable(L, LUA_REGISTRYINDEX);
    auto worker_index = lua_tonumber(L, -1);
    lua_settop(L, top_before);
    return worker_index;
}

void set_server_id(lua_State* L, std::string server_id)
{
    lua_pushlightuserdata(L, (void *)server_id_str.c_str());
    lua_pushlstring(L, server_id.c_str(), server_id.size());
    lua_rawset(L, LUA_REGISTRYINDEX);
}

std::string get_server_id(lua_State* L)
{
    std::string server_id;
    auto top_before = lua_gettop(L);
    lua_pushlightuserdata(L, (void *)server_id_str.c_str());
    lua_gettable(L, LUA_REGISTRYINDEX);
    size_t len = 0;
    const char* id = lua_tolstring(L, -1, &len);
    server_id.assign(id, len);
    lua_settop(L, top_before);
    return server_id;
}

void set_passive(lua_State* L)
{
    lua_pushlightuserdata(L, (void *)inactive_state.c_str());
    lua_pushboolean(L, 1);
    lua_rawset(L, LUA_REGISTRYINDEX);
}

bool is_passive(lua_State* L)
{
    auto top_before = lua_gettop(L);
    lua_pushlightuserdata(L, (void *)inactive_state.c_str());
    lua_gettable(L, LUA_REGISTRYINDEX);
    auto inactive = lua_toboolean(L, -1);
    lua_settop(L, top_before);
    return inactive;
}

Lua_State_Data& get_lua_state_data(lua_State* L)
{
    auto group_id = get_group_id(L);
    auto worker_id = get_worker_index(L);
    return get_lua_group_config(group_id).lua_state_data[worker_id][L];
}

std::mutex& get_lua_group_config_mutex(size_t group_id)
{
    static std::vector<std::unique_ptr<std::mutex>> lua_group_config_mutexes;
    auto min_required_size = group_id + 1;
    if (min_required_size > lua_group_config_mutexes.size())
    {
        for (int i = 0; i < (min_required_size - lua_group_config_mutexes.size()); i++)
        {
            lua_group_config_mutexes.emplace_back(std::make_unique<std::mutex>());
        }
    }
    return *lua_group_config_mutexes[group_id].get();
}

Lua_Group_Config& get_lua_group_config(size_t group_id)
{
    static std::vector<Lua_Group_Config> lua_group_configs;
    auto min_required_size = group_id + 1;
    if (min_required_size > lua_group_configs.size())
    {
        lua_group_configs.resize(min_required_size);
    }
    return lua_group_configs[group_id];
}

void start_test_group(size_t group_id)
{
    auto& lua_group_config = get_lua_group_config(group_id);

    if (lua_group_config.coroutine_references.empty())
    {
        return;
    }
    std::map<size_t, std::vector<lua_State*>> work_id_to_lua_states;
    for (auto& vec: lua_group_config.coroutine_references)
    {
        for (auto& coroutine: vec)
        {
            auto lua_state = coroutine.first;
            work_id_to_lua_states[get_worker_index(lua_state)].push_back(lua_state);
        }
    }

    for (auto it: work_id_to_lua_states)
    {
        auto worker_index = it.first;
        std::vector<lua_State*> lua_states_to_start_for_worker_x = it.second;
        auto start_lua_states = [lua_states_to_start_for_worker_x]()
        {
            for (auto L: lua_states_to_start_for_worker_x)
            {
                lua_resume_wrapper(L, 0);
            }
        };
        lua_group_config.workers[worker_index]->get_io_context().post(start_lua_states);
    }
}

void setup_test_group(size_t group_id)
{
    auto& lua_group_config = get_lua_group_config(group_id);
    std::cerr << "number of workers: " << lua_group_config.number_of_workers
              << ", nummber of coroutines: " << lua_group_config.number_of_parallel_lua_coroutines
              << ", nummber of parallel connections to same host: "
              << lua_group_config.number_of_client_to_same_host_in_one_worker
              <<std::endl;
    // create one main state for each worker
    lua_group_config.coroutine_references.clear();
    lua_group_config.lua_coroutine_pools.clear();
    for (int i = 0; i < lua_group_config.number_of_workers; i++)
    {
        auto lua_state = std::shared_ptr<lua_State>(luaL_newstate(), &lua_close);
        init_new_lua_state(lua_state.get());
        set_group_id(lua_state.get(), group_id);
        set_worker_index(lua_state.get(), i);
        get_lua_state_data(lua_state.get()).unique_id_within_group = lua_group_config.number_of_parallel_lua_coroutines;
        lua_group_config.lua_main_states_per_worker.push_back(lua_state);
        std::map<lua_State*, int> coroutine_reference_map;
        lua_group_config.coroutine_references.push_back(coroutine_reference_map);
        std::vector<lua_State*> coroutine_pool;
        lua_group_config.lua_coroutine_pools.push_back(coroutine_pool);
    }
    init_workers(group_id);

    for (int i = 0; i < lua_group_config.number_of_parallel_lua_coroutines; i++)
    {
        auto worker_index = i % lua_group_config.number_of_workers;
        auto parent_lua_state = lua_group_config.lua_main_states_per_worker[worker_index].get();
        if (!lua_checkstack(parent_lua_state, 1))
        {
            std::cerr<<"no enough space in stack"<<std::endl;
            exit(1);
        }
        lua_State* cL = lua_newthread(parent_lua_state);
        get_lua_state_data(cL).unique_id_within_group = i;
        lua_group_config.coroutine_references[worker_index][cL] = luaL_ref(parent_lua_state, LUA_REGISTRYINDEX);
        luaL_loadstring(cL, lua_group_config.lua_script.c_str());
    }
}

int setup_parallel_test(lua_State *L)
{
    size_t number_of_coroutines = 0;
    size_t number_of_client = 0;
    size_t number_of_workers = 0;

    int top = lua_gettop(L);
    if (top == 3)
    {
        number_of_coroutines = lua_tointeger(L, -1);
        lua_pop(L, 1);
        number_of_client = lua_tointeger(L, -1);
        lua_pop(L, 1);
        number_of_workers = lua_tointeger(L, -1);
        lua_pop(L, 1);
    }
    auto group_id = get_group_id(L);
    auto& lua_group_config = get_lua_group_config(group_id);
    if ((number_of_coroutines > 0) &&
        (lua_group_config.config_initialized == false) &&
        (lua_group_config.lua_script.size() > 0))
    {
        lua_group_config.config_initialized = true;
        lua_group_config.number_of_parallel_lua_coroutines = number_of_coroutines;
        lua_group_config.number_of_client_to_same_host_in_one_worker = number_of_client;
        lua_group_config.number_of_workers = number_of_workers;
        setup_test_group(group_id);
        return lua_yield(L, 0);
    }
    else
    {
      lua_pushinteger(L, get_lua_state_data(L).unique_id_within_group);
      return 1;
    }
}

void init_new_lua_state(lua_State* L)
{
    luaL_openlibs(L);
    lua_register(L, "make_connection", make_connection);
    lua_register(L, "send_http_request", send_http_request);
    lua_register(L, "await_response", await_response);
    lua_register(L, "send_http_request_and_await_response", send_http_request_and_await_response);
    lua_register(L, "setup_parallel_test", setup_parallel_test);
    lua_register(L, "sleep_for_ms", sleep_for_ms);
    lua_register(L, "time_since_epoch", time_since_epoch);
    lua_register(L, "start_server", start_server);
    lua_register(L, "stop_server", stop_server);
    lua_register(L, "register_service_handler", register_service_handler);
    lua_register(L, "send_response", send_response);
    lua_register(L, "wait_for_message", wait_for_message);
}


void stop_workers(size_t number_of_groups)
{
    for (int group_index = 0; group_index < number_of_groups; group_index++)
    for (int i = 0; i < get_lua_group_config(group_index).workers.size(); i++)
    {
        auto worker_ptr = get_lua_group_config(group_index).workers[i].get();
        auto stop_user_timer_service = [worker_ptr]()
        {
            worker_ptr->stop_tick_timer();
            worker_ptr->stop_all_clients();
        };
        worker_ptr->get_io_context().post(stop_user_timer_service);
        // use std::move to destroy work
        auto dummy = std::move(get_lua_group_config(group_index).works[i]);
    }
}

bool is_test_finished(size_t number_of_test_groups)
{
    auto all_coroutines_finished = [](size_t group_id)
    {
        auto& lua_group_config = get_lua_group_config(group_id);
        return (lua_group_config.number_of_parallel_lua_coroutines == lua_group_config.number_of_finished_coroutins);
    };

    auto server_stopped = [](size_t group_id)
    {
        if (get_lua_group_config(group_id).server_running)
        {
            return false;
        }
        return true;
    };

    for (int i = 0; i < number_of_test_groups; i++)
    {
        if (!all_coroutines_finished(i) || !server_stopped(i))
        {
            return false;
        }
    }
    return true;
}


void load_and_run_lua_script(const std::vector<std::string>& lua_scripts, h2load::Config& config)
{
    std::vector<lua_State*> bootstrap_lua_states;

    // init all config and mutex to avoid runtime vector reallocation
    for (size_t i = 0; i < lua_scripts.size(); i++)
    {
        get_lua_group_config(i);
        get_lua_group_config_mutex(i);
    }

    // Use bootstrap L for each group to call setup_parallel_test
    for (size_t i = 0; i < lua_scripts.size(); i++)
    {
        get_lua_group_config(i).lua_script = lua_scripts[i];
        get_lua_group_config(i).config_template = config;
        lua_State* L = luaL_newstate();
        init_new_lua_state(L);
        bootstrap_lua_states.push_back(L);
        set_group_id(L, i);
        set_worker_index(L, 0);
        get_lua_state_data(L).unique_id_within_group = 0;
        luaL_loadstring(L, lua_scripts[i].c_str());
        // if setup_parallel_test is not called, meaning no new coroutine is created,
        // then bootstrap L is to execute the script directly for each group
        auto retCode = lua_resume_wrapper(L, 0);
        if (LUA_OK == retCode)
        {
            // setup_parallel_test is not called, script done
        }
        else if (LUA_YIELD == retCode)
        {
            // setup_parallel_test might have been called
        }
        else
        {
            size_t len;
            std::string error_str;
            auto str_ptr = lua_tolstring(L, -2, &len);
            error_str.assign(str_ptr, len);
            lua_pop(L, 1);
            std::cerr<<"error running lua script:"<<std::endl<<error_str<<std::endl<<lua_scripts[i]<<std::endl;
        }
    }

    for (size_t i = 0; i < lua_scripts.size(); i++)
    {
        // if setup_parallel_test is not called
        // coroutine_references is empty for that group
        // start_test_group will do nothing
        start_test_group(i);
    }
    size_t number_of_groups = lua_scripts.size();
    while (!is_test_finished(number_of_groups))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
    std::cerr<<"test finished"<<std::endl;
    for (auto& L: bootstrap_lua_states)
    {
        lua_close(L);
    }
    bootstrap_lua_states.clear();
    stop_workers(number_of_groups);
    // to let all workers clean up and quit
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
}

bool is_running_in_worker_thread(lua_State* L)
{
    auto& lua_group_config = get_lua_group_config(get_group_id(L));
    auto worker_thread = lua_group_config.workers[get_worker_index(L)];
    auto curr_thread_id = std::this_thread::get_id();
    auto worker_thread_id = worker_thread->get_thread_id();
    return (curr_thread_id == worker_thread_id);
}

void init_workers(size_t group_id)
{
    auto& lua_group_config = get_lua_group_config(group_id);
    if (lua_group_config.workers.size())
    {
        return;
    }
    static h2load::Config conf;
    auto init_config = [&lua_group_config]()
    {
        conf.ciphers = lua_group_config.config_template.ciphers;
        conf.max_concurrent_streams = lua_group_config.config_template.max_concurrent_streams;
        conf.window_bits = lua_group_config.config_template.window_bits;
        conf.connection_window_bits = lua_group_config.config_template.connection_window_bits;
        conf.conn_active_timeout = lua_group_config.config_template.conn_active_timeout;
        conf.conn_inactivity_timeout = lua_group_config.config_template.conn_inactivity_timeout;
        conf.no_tls_proto = lua_group_config.config_template.no_tls_proto;
        conf.header_table_size = lua_group_config.config_template.header_table_size;
        conf.encoder_header_table_size = lua_group_config.config_template.encoder_header_table_size;
        conf.verbose = lua_group_config.config_template.verbose;
        conf.npn_list = lua_group_config.config_template.npn_list;
        conf.stream_timeout_in_ms = lua_group_config.config_template.stream_timeout_in_ms;
        conf.json_config_schema.ca_cert = lua_group_config.config_template.json_config_schema.ca_cert;
        conf.json_config_schema.client_cert = lua_group_config.config_template.json_config_schema.client_cert;
        conf.json_config_schema.private_key = lua_group_config.config_template.json_config_schema.private_key;
        conf.json_config_schema.cert_verification_mode = lua_group_config.config_template.json_config_schema.cert_verification_mode;
        conf.json_config_schema.max_tls_version = lua_group_config.config_template.json_config_schema.max_tls_version;
        // conf.json_config_schema.interval_to_send_ping = 5;
        // conf.json_config_schema.connection_retry_on_disconnect = true;

        Request request;
        Scenario scenario;
        scenario.requests.push_back(request);
        conf.json_config_schema.scenarios.push_back(scenario);
        return true;
    };
    static auto init_config_ret_code = init_config();

    for (int i = 0; i < lua_group_config.number_of_workers; i++)
    {
        lua_group_config.workers.emplace_back(std::make_shared<h2load::asio_worker>(0, 0xFFFFFFFF, 1, 0, 1000, &conf));
    }
    for (int i = 0; i < lua_group_config.workers.size(); i++)
    {
        lua_group_config.works.emplace_back(lua_group_config.workers[i]->get_io_context());
    }

    auto thread_func = [](h2load::asio_worker* worker_ptr)
    {
        worker_ptr->run_event_loop();
    };
    for (int i = 0; i < lua_group_config.workers.size(); i++)
    {
        auto worker_ptr = lua_group_config.workers[i].get();
        std::thread worker_thread(thread_func, worker_ptr);
        worker_thread.detach();
        auto start_user_timer_service = [worker_ptr]()
        {
            worker_ptr->start_tick_timer();
        };
        worker_ptr->get_io_context().post(start_user_timer_service);
    }
}

h2load::asio_worker* get_worker(lua_State *L)
{
    size_t group_id = get_group_id(L);
    if (get_lua_group_config(group_id).workers.size() == 0)
    {
        init_workers(group_id);
    }
    return get_lua_group_config(group_id).workers[get_worker_index(L)].get();
}

int32_t _make_connection(lua_State *L, const std::string& uri, std::function<void(bool, h2load::base_client*)> connected_callback)
{
    auto worker = get_worker(L);
    http_parser_url u {};
    if (http_parser_parse_url(uri.c_str(), uri.size(), 0, &u) != 0 ||
        !util::has_uri_field(u, UF_SCHEMA) || !util::has_uri_field(u, UF_HOST))
    {
        std::cout<<"invalid uri:"<<uri<<std::endl;
        connected_callback(false, nullptr);
        return -1;
    }
    auto group_id = get_group_id(L);
    auto clients_needed = get_lua_group_config(group_id).number_of_client_to_same_host_in_one_worker;
    auto run_inside_worker = [uri, connected_callback, worker, clients_needed]()
    {
        http_parser_url u {};
        if (http_parser_parse_url(uri.c_str(), uri.size(), 0, &u) != 0 ||
            !util::has_uri_field(u, UF_SCHEMA) || !util::has_uri_field(u, UF_HOST))
        {
            return connected_callback(false, nullptr);
        }
        std::string schema = util::get_uri_field(uri.c_str(), u, UF_SCHEMA).str();
        std::string authority = util::get_uri_field(uri.c_str(), u, UF_HOST).str();
        auto port = util::get_default_port(uri.c_str(), u);
        if (util::has_uri_field(u, UF_PORT))
        {
            port = u.port;
        }
        authority.append(":").append(std::to_string(port));
        auto client_id = worker->next_client_id;
        std::string base_uri = schema;
        base_uri.append("://").append(authority);
        auto& clients = worker->get_client_pool();
        if (clients[base_uri].size() < clients_needed)
        {
            auto client = worker->create_new_client(0xFFFFFFFF);
            worker->check_in_client(client);
            // pre-mature insert to block excessive client creation during test start
            clients[base_uri].insert(client.get());
            client->install_connected_callback(connected_callback);
            client->set_prefered_authority(authority);
            client->connect_to_host(schema, authority);
        }
        else
        {
            thread_local static std::random_device rand_dev;
            thread_local static std::mt19937 generator(rand_dev());
            thread_local static std::uniform_int_distribution<uint64_t>  distr(0, clients_needed - 1);
            auto client_index = distr(generator);
            auto iter = clients[base_uri].begin();
            std::advance(iter, client_index);
            auto client = *iter;

            if (h2load::CLIENT_IDLE == client->state)
            {
                client->install_connected_callback(connected_callback);
                client->connect_to_host(schema, authority);
            }
            else if (h2load::CLIENT_CONNECTING == client->state)
            {
                client->install_connected_callback(connected_callback);
            }
            else
            {
                connected_callback(true, client);
            }
        }
    };
    worker->get_io_context().post(run_inside_worker);
    return 0;
}

int make_connection(lua_State *L)
{
    auto connected_callback = [L](bool success, h2load::base_client* client)
    {
        lua_pushinteger(L, success ? client->get_client_unique_id() : -1);
        lua_resume_if_yielded(L, 1);
    };

    std::string base_uri;
    if ((lua_gettop(L) == 1)&&(lua_type(L, -1) == LUA_TSTRING))
    {
        size_t len;
        const char* str = lua_tolstring(L, -1, &len);
        base_uri.assign(str, len);
        lua_pop(L, 1);
    }
    else
    {
        std::cerr<<"invalid argument: "<<__FUNCTION__<<std::endl;
    }
    lua_settop(L, 0);

    if (base_uri.size() && (_make_connection(L, base_uri, connected_callback) == 0))
    {
        return lua_yield(L, 0);
    }

    if (lua_gettop(L) == 0)
    {
        connected_callback(false, nullptr);
    }
    return lua_gettop(L);
}

int send_http_request(lua_State *L)
{
    auto request_sent = [L](int32_t stream_id, h2load::base_client* client)
    {
        if (stream_id && client)
        {
            client->queue_stream_for_user_callback(stream_id);
            lua_pushinteger(L, client->get_client_unique_id());
            lua_pushinteger(L, stream_id);
        }
        else
        {
            lua_pushinteger(L, -1);
            lua_pushinteger(L, -1);
        }
        lua_resume_if_yielded(L, 2);
    };
    return _send_http_request(L, request_sent);
}

int send_http_request_and_await_response(lua_State *L)
{
    auto request_sent = [L](int32_t stream_id, h2load::base_client* client)
    {
        if (stream_id > 0 && client)
        {
            client->queue_stream_for_user_callback(stream_id);
            client->pass_response_to_lua(stream_id, L);
        }
        else
        {
            lua_createtable(L, 0, 1);
            lua_pushlstring(L, "", 0);
            lua_pushinteger(L, -1);
            lua_resume_if_yielded(L, 3);
        }
    };
    return _send_http_request(L, request_sent);
}

int time_since_epoch(lua_State *L)
{
    auto curr_time_point = std::chrono::steady_clock::now();
    auto ms_since_epoch = std::chrono::duration_cast<std::chrono::milliseconds>(curr_time_point.time_since_epoch()).count();
    lua_pushinteger(L, ms_since_epoch);
    return 1;
}

int sleep_for_ms(lua_State *L)
{
    int64_t ms_to_sleep = 0;
    std::string stack;
    int top = lua_gettop(L);
    if (top == 1)
    {
        ms_to_sleep = lua_tointeger(L, -1);
        lua_pop(L, 1);
    }
    lua_settop(L, 0);

    if (ms_to_sleep <= 0)
    {
        return 0;
    }

    auto wakeup_me = [L]()
    {
        lua_resume_wrapper(L, 0);
    };

    auto worker = get_worker(L);
    auto run_in_worker = [worker, wakeup_me, ms_to_sleep]()
    {
        worker->enqueue_user_timer(ms_to_sleep, wakeup_me);
    };
    worker->get_io_context().post(run_in_worker);
    return lua_yield(L, 0);
}

int await_response(lua_State *L)
{
    uint64_t client_unique_id = -1;
    int32_t stream_id = -1;

    int top = lua_gettop(L);
    if (top == 2)
    {
        stream_id = lua_tointeger(L, -1);
        lua_pop(L, 1);
        client_unique_id = lua_tointeger(L, -1);
        lua_pop(L, 1);
    }
    lua_settop(L, 0);

    auto worker = get_worker(L);

    auto retrieve_response_cb = [worker, client_unique_id, stream_id, L]()
    {
        auto client_iter = worker->get_client_ids().find(client_unique_id);
        if (client_iter != worker->get_client_ids().end())
        {
            client_iter->second->pass_response_to_lua(stream_id, L);
            return;
        }
        lua_createtable(L, 0, 1);
        lua_pushlstring(L, "", 0);
        lua_resume_if_yielded(L, 2);
    };

    bool argument_error = (client_unique_id < 0 || stream_id <= 0);
    if (!argument_error)
    {
        worker->get_io_context().post(retrieve_response_cb);
        return lua_yield(L, 0);
    }

    if (lua_gettop(L) == 0)
    {
        lua_createtable(L, 0, 1);
        lua_pushlstring(L, "", 0);
    }
    return lua_gettop(L);
}


int _send_http_request(lua_State *L, std::function<void(int32_t, h2load::base_client*)> request_sent_callback)
{
    auto argument_error = false;
    std::string payload;
    std::map<std::string, std::string, ci_less> headers;
    static std::map<std::string, std::string, ci_less> dummyHeaders;
    int top = lua_gettop(L);
    for (int i = 0; i < top; i++)
    {
        switch (lua_type(L, -1))
        {
            case LUA_TSTRING:
            {
                size_t len;
                const char* str = lua_tolstring(L, -1, &len);
                payload.assign(str, len);
                break;
            }
            case LUA_TTABLE:
            {
                lua_pushnil(L);
                while (lua_next(L, -2) != 0)
                {
                    size_t len;
                    /* uses 'key' (at index -2) and 'value' (at index -1) */
                    if ((LUA_TSTRING != lua_type(L, -2)) || (LUA_TSTRING != lua_type(L, -1)))
                    {
                        std::cerr << __FUNCTION__<< ": invalid http header" << std::endl;
                        argument_error = true;
                        break;
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
                break;
            }
            default:
            {
                std::cerr << __FUNCTION__<<": invalid parameter passed in" << std::endl;
                argument_error = true;
                break;
            }
        }
        lua_pop(L, 1);
    }
    lua_settop(L, 0);

    if (!argument_error)
    {
        std::string schema = headers[h2load::scheme_header];
        headers.erase(h2load::scheme_header);
        std::string authority = headers[h2load::authority_header];
        headers.erase(h2load::authority_header);
        std::string method = headers[h2load::method_header];
        headers.erase(h2load::method_header);
        std::string path = headers[h2load::path_header];
        headers.erase(h2load::path_header);
        std::string base_uri = schema;
        base_uri.append("://").append(authority);
        h2load::asio_worker* worker;
        worker = get_worker(L);

        auto connected_callback = [payload, schema, authority, method, path, headers, request_sent_callback](bool success, h2load::base_client* client)
        {
            if (!success)
            {
                request_sent_callback(-1, nullptr);
                return;
            }
            h2load::Request_Data request_to_send;
            request_to_send.request_sent_callback = request_sent_callback;
            request_to_send.string_collection.emplace_back(payload);
            request_to_send.req_payload = &(request_to_send.string_collection.back());
            request_to_send.string_collection.emplace_back(method);
            request_to_send.method = &(request_to_send.string_collection.back());
            request_to_send.string_collection.emplace_back(path);
            request_to_send.path = &(request_to_send.string_collection.back());
            request_to_send.string_collection.emplace_back(authority);
            request_to_send.authority = &(request_to_send.string_collection.back());
            request_to_send.string_collection.emplace_back(schema);
            request_to_send.schema = &(request_to_send.string_collection.back());
            request_to_send.shadow_req_headers = std::move(headers);
            request_to_send.req_headers = &dummyHeaders;
            client->requests_to_submit.emplace_back(std::move(request_to_send));
            client->submit_request();
        };
        if (_make_connection(L, base_uri, connected_callback) == 0)
        {
            return lua_yield(L, 0);
        }
    }

    if (lua_gettop(L) == 0)
    {
        request_sent_callback(-1, nullptr);
    }
    return lua_gettop(L);
}

int lua_resume_if_yielded(lua_State *L, int nargs)
{
    if (LUA_YIELD == lua_status(L))
    {
        return lua_resume_wrapper(L, nargs);
    }
    else
    {
        return nargs;
    }
}

bool is_coroutine_with_unique_id(lua_State* L)
{
    auto group_id = get_group_id(L);
    auto& lua_group_config = get_lua_group_config(group_id);
    auto worker_index = get_worker_index(L);
    if (get_lua_group_config(group_id).lua_state_data[worker_index].count(L))
    {
        return true;
    }
    return false;
}

bool is_coroutine_to_be_returned_to_pool(lua_State* L)
{
    return (is_passive(L));
}

int lua_resume_wrapper(lua_State *L, int nargs)
{
    auto retCode = lua_resume(L, nargs);
    if (LUA_YIELD != retCode)
    {
        auto group_id = get_group_id(L);
        auto& lua_group_config = get_lua_group_config(group_id);
        auto worker_index = get_worker_index(L);
        if (is_coroutine_with_unique_id(L))
        {
              std::lock_guard<std::mutex> guard(get_lua_group_config_mutex(group_id));
              lua_group_config.number_of_finished_coroutins++;
        }
        if (lua_group_config.coroutine_references[worker_index].count(L))
        {
            if (is_coroutine_to_be_returned_to_pool(L))
            {
                lua_group_config.lua_coroutine_pools[worker_index].push_back(L);
            }
            else
            {
                auto parent_lua_state = lua_group_config.lua_main_states_per_worker[worker_index].get();
                luaL_unref(parent_lua_state, LUA_REGISTRYINDEX, lua_group_config.coroutine_references[worker_index][L]);
                lua_group_config.coroutine_references[worker_index].erase(L);
                lua_gc(parent_lua_state, LUA_GCCOLLECT, 0);
                lua_group_config.lua_state_data[worker_index].erase(L);
            }
        }
    }
    return retCode;
}

H2Server_Config_Schema config_schema;

int start_server(lua_State *L)
{
    std::string config_file_name;
    int top = lua_gettop(L);
    for (int i = 0; i < top; i++)
    {
        switch (lua_type(L, -1))
        {
            case LUA_TSTRING:
            {
                size_t len;
                const char* str = lua_tolstring(L, -1, &len);
                config_file_name.assign(str, len);
                break;
            }
            default:
            {
                std::cerr << __FUNCTION__<<": invalid parameter passed in" << std::endl;
                break;
            }
        }
        lua_pop(L, 1);
    }
    lua_settop(L, 0);

    if (is_passive(L))
    {
        std::string server_id = get_lua_group_config(get_group_id(L)).server_id;
        lua_pushlstring(L, server_id.c_str(), server_id.size());
        return 1;
    }

    std::promise<void> ready_promise;

    auto thread_func = [config_file_name, &ready_promise]()
    {
        auto init_cbk = [&ready_promise]()
        {
            ready_promise.set_value();
        };
        start_server(config_file_name, false, init_cbk);
    };
    std::thread serverThread(thread_func);
    auto bootstrap_thread_id = serverThread.get_id();
    serverThread.detach();
    std::stringstream ss;
    ss<<bootstrap_thread_id;
    lua_pushlstring(L, ss.str().c_str(), ss.str().size());
    ready_promise.get_future().wait();
    get_lua_group_config(get_group_id(L)).server_id = ss.str();
    get_lua_group_config(get_group_id(L)).server_running = true;
    return 1;
}

int stop_server(lua_State *L)
{
    int top = lua_gettop(L);
    if ((top == 1) && lua_type(L, -1) == LUA_TSTRING)
    {
        size_t len;
        const char* str = lua_tolstring(L, -1, &len);
        std::string server_thread_hash;
        server_thread_hash.assign(str, len);
        stop_server(server_thread_hash);
    }
    lua_settop(L, 0);
    get_lua_group_config(get_group_id(L)).server_id.clear();
    auto group_id = get_group_id(L);
    get_lua_group_config(get_group_id(L)).server_running = false;
    return 0;
}

void load_service_script_into_lua_states(size_t group_id, const std::string& server_id)
{
    auto& lua_group_config = get_lua_group_config(group_id);
    for (size_t index = 0; index < lua_group_config.lua_main_states_per_worker.size(); index++)
    {
        lua_State* parent_lua_state = lua_group_config.lua_main_states_per_worker[index].get();
        set_passive(parent_lua_state);
        //set_server_id(parent_lua_state, server_id);
        luaL_dostring(parent_lua_state, lua_group_config.lua_script.c_str());
    }
}

void invoke_service_hanlder(lua_State *L, std::string lua_function_name,
                                                                   boost::asio::io_service* ios,
                                                                   uint64_t handler_id,
                                                                   int32_t stream_id,
                                                                   const std::multimap<std::string, std::string>& req_headers,
                                                                   const std::string& payload)
{
    auto& lua_group_config = get_lua_group_config(get_group_id(L));
    lua_State* cL = nullptr;
    if (lua_group_config.lua_coroutine_pools[get_worker_index(L)].size())
    {
        cL = lua_group_config.lua_coroutine_pools[get_worker_index(L)].back();
        lua_group_config.lua_coroutine_pools[get_worker_index(L)].pop_back();
    }
    if (!cL)
    {
        cL = lua_newthread(L);
        lua_group_config.coroutine_references[get_worker_index(L)][cL] = luaL_ref(L, LUA_REGISTRYINDEX);
        lua_settop(L, 0);
    }

    lua_getglobal(cL, lua_function_name.c_str());
    if (lua_isfunction(cL, -1))
    {
        lua_createtable(cL, 0, 3);
        lua_pushlstring(cL, "ios", 3);
        lua_pushlightuserdata(cL, ios);
        lua_rawset(cL, -3);
        lua_pushlstring(cL, "hid", 3);
        lua_pushinteger(cL, handler_id);
        lua_rawset(cL, -3);
        lua_pushlstring(cL, "sid", 3);
        lua_pushinteger(cL, stream_id);
        lua_rawset(cL, -3);

        std::map<std::string, std::string> headers;
        for (auto& header : req_headers)
        {
            if (headers.count(header.first))
            {
                headers[header.first].append(";").append(header.second);
            }
            else
            {
                headers[header.first] = header.second;
            }
        }
        lua_createtable(cL, 0, headers.size());
        for (auto& header : headers)
        {
            lua_pushlstring(cL, header.first.c_str(), header.first.size());
            lua_pushlstring(cL, header.second.c_str(), header.second.size());
            lua_rawset(cL, -3);
        }
        lua_pushlstring(cL, payload.c_str(), payload.size());
        lua_resume_wrapper(cL, 3);
    }
}

int register_service_handler(lua_State *L)
{
    std::string lua_function_name;
    std::string service_name;
    std::string server_thread_hash;
    size_t number_of_client = 1;
    if ((lua_gettop(L) == 4))
    {
        number_of_client = lua_tointeger(L, -1);
        lua_pop(L, 1);
    }
    if ((lua_gettop(L) == 3))
    {
        size_t len;
        const char* str = lua_tolstring(L, -1, &len);
        lua_pop(L, 1);
        lua_function_name.assign(str, len);
        str = lua_tolstring(L, -1, &len);
        service_name.assign(str, len);
        lua_pop(L, 1);
        str = lua_tolstring(L, -1, &len);
        server_thread_hash.assign(str, len);
        lua_pop(L, 1);
    }
    lua_settop(L, 0);
    if (is_passive(L))
    {
        return 0;
    }

    if (service_name.empty() || lua_function_name.empty())
    {
        return 0;
    }
    auto group_id = get_group_id(L);
    auto number_of_thread_in_server = get_H2Server_match_Instances(server_thread_hash).size();
    auto& lua_group_config = get_lua_group_config(group_id);
    if (!lua_group_config.config_initialized)
    {
        lua_group_config.config_initialized = true;
        lua_group_config.number_of_parallel_lua_coroutines = 0;
        lua_group_config.number_of_client_to_same_host_in_one_worker = number_of_client;
        lua_group_config.number_of_workers = number_of_thread_in_server;
        setup_test_group(group_id);

        // TODO: is there any way to locate a function without luaL_dostring which is to run the whole script?
        load_service_script_into_lua_states(group_id, server_thread_hash);
    }

    for (size_t index = 0; index < lua_group_config.number_of_workers; index++)
    {
        lua_State* parent_lua_state = lua_group_config.lua_main_states_per_worker[index].get();
        h2load::asio_worker* worker = get_worker(parent_lua_state);

        auto request_processor = [parent_lua_state, lua_function_name, worker](boost::asio::io_service* ios,
                                                    uint64_t handler_id,
                                                    int32_t stream_id,
                                                    const std::multimap<std::string, std::string>& req_headers,
                                                    const std::string& payload)
        {
            auto run_in_worker = std::bind(invoke_service_hanlder,
                                           parent_lua_state, lua_function_name,
                                           ios, handler_id, stream_id, req_headers,
                                           payload);
            worker->get_io_context().post(run_in_worker);
            return true;
        };
        install_request_callback(server_thread_hash, index, service_name, request_processor);
    }

    return 0;
}

int send_response(lua_State *L)
{
    std::string payload;
    std::map<std::string, std::string> response_headers;
    boost::asio::io_service* ios = nullptr;
    uint64_t handler_id = 0;
    int32_t stream_id = 0;
    int top = lua_gettop(L);
    for (int i = 0; i < top; i++)
    {
        switch (lua_type(L, -1))
        {
            case LUA_TSTRING:
            {
                size_t len;
                const char* str = lua_tolstring(L, -1, &len);
                payload.assign(str, len);
                break;
            }
            case LUA_TTABLE:
            {
                lua_pushnil(L);
                while (lua_next(L, -2) != 0)
                {
                    size_t len;
                    /* uses 'key' (at index -2) and 'value' (at index -1) */
                    const char* k = lua_tolstring(L, -2, &len);
                    std::string key(k, len);

                    if (LUA_TSTRING == lua_type(L, -1))
                    {
                        const char* v = lua_tolstring(L, -1, &len);
                        std::string value(v, len);
                        //util::inp_strlower(key);
                        response_headers[key] = value;
                    }
                    else if (LUA_TLIGHTUSERDATA == lua_type(L, -1))
                    {
                        ios = static_cast<boost::asio::io_service*>(lua_touserdata(L, -1));
                    }
                    else if (LUA_TNUMBER == lua_type(L, -1))
                    {
                        if (key == "hid")
                        {
                            handler_id = lua_tointeger(L, -1);
                        }
                        else if (key == "sid")
                        {
                            stream_id= lua_tointeger(L, -1);
                        }
                        else
                        {
                            std::cout<<"invalid key:"<<key<<std::endl;;
                        }
                    }
                    else
                    {
                        std::cout<<"invalid value:"<<lua_type(L, -1)<<std::endl;;
                    }
                    /* removes 'value'; keeps 'key' for next iteration */
                    lua_pop(L, 1);
                }
                break;
            }
        }
        lua_pop(L, 1);
    }
    send_response_from_another_thread(ios, handler_id, stream_id, response_headers, payload);
    return 0;
}

int wait_for_message(lua_State *L)
{
    return lua_yield(L, 0);
}

