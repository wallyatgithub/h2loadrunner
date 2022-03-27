#include <algorithm>
#include <numeric>
#include <cctype>
#include <mutex>

#include <iomanip>
#include <iostream>
#include <fstream>
#include <string>
#include <openssl/err.h>
#include <openssl/ssl.h>

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
#include "Client_Interface.h"

#include "asio_worker.h"
#include "h2load_lua.h"

std::map<lua_State*, Lua_State_Data>& get_lua_state_data_repository()
{
    static std::map<lua_State*, Lua_State_Data> lua_states_data;
    return lua_states_data;
}

Lua_State_Data& get_lua_state_data(lua_State* L)
{
    return get_lua_state_data_repository()[L];
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
    for (auto& coroutine: lua_group_config.coroutine_references)
    {
        auto lua_state = coroutine.first;
        work_id_to_lua_states[get_lua_state_data(lua_state).worker_id].push_back(lua_state);
    }

    init_workers(group_id);

    for (auto it: work_id_to_lua_states)
    {
        auto worker_index = it.first;
        std::vector<lua_State*> lua_states_to_start_for_worker_x = it.second;
        auto start_lua_states = [lua_states_to_start_for_worker_x]()
        {
            for (auto L: lua_states_to_start_for_worker_x)
            {
                get_lua_state_data(L).started_from_worker_thread = true;
                lua_resume_wrapper(L, 0);
            }
        };
        lua_group_config.workers[worker_index]->get_io_context().post(start_lua_states);
    }
}

bool to_be_restarted_in_worker_thread(lua_State* L)
{
    if (get_lua_state_data(L).started_from_worker_thread)
    {
        return false;
    }
    else
    {
        auto group_id = get_lua_state_data(L).group_id;
        auto& lua_group_config = get_lua_group_config(group_id);
        auto worker_thread = get_worker(L);
        lua_settop(L, 0);
        luaL_loadstring(L, lua_group_config.lua_script.c_str());
        auto restart_coroutine = [L]()
        {
            get_lua_state_data(L).started_from_worker_thread = true;
            lua_resume_wrapper(L, 0);
        };
        get_worker(L)->get_io_context().post(restart_coroutine);
        return true;
    }
}


void setup_test_group(size_t group_id)
{
    auto& lua_group_config = get_lua_group_config(group_id);
    std::cerr << "number of workers: " << lua_group_config.number_of_workers
              << ", nummber of coroutines: " << lua_group_config.number_of_lua_coroutines<<std::endl;
    // create one main state for each worker
    for (int i = 0; i < lua_group_config.number_of_workers; i++)
    {
        auto lua_state = std::shared_ptr<lua_State>(luaL_newstate(), &lua_close);
        init_new_lua_state(lua_state.get());
        get_lua_state_data(lua_state.get()).group_id = group_id;
        get_lua_state_data(lua_state.get()).worker_id = 0;
        get_lua_state_data(lua_state.get()).unique_id_within_group = lua_group_config.number_of_lua_coroutines;
        get_lua_state_data(lua_state.get()).started_from_worker_thread = false;
        lua_group_config.lua_states_for_each_worker.push_back(lua_state);
    }

    for (int i = 0; i < lua_group_config.number_of_lua_coroutines; i++)
    {
        auto worker_index = i % lua_group_config.number_of_workers;
        auto parent_lua_state = lua_group_config.lua_states_for_each_worker[worker_index].get();
        if (!lua_checkstack(parent_lua_state, 1))
        {
            std::cerr<<"no enough space in stack"<<std::endl;
            exit(1);
        }
        lua_State* cL = lua_newthread(parent_lua_state);
        get_lua_state_data(cL).group_id = group_id;
        get_lua_state_data(cL).worker_id = worker_index;
        get_lua_state_data(cL).unique_id_within_group = i;
        lua_group_config.coroutine_references[cL] = luaL_ref(parent_lua_state, LUA_REGISTRYINDEX);
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
    auto group_id = get_lua_state_data(L).group_id;
    auto& lua_group_config = get_lua_group_config(group_id);
    if ((number_of_coroutines > 0) &&
        (lua_group_config.config_initialized == false) &&
        (lua_group_config.lua_script.size() > 0))
    {
        lua_group_config.config_initialized = true;
        lua_group_config.number_of_lua_coroutines = number_of_coroutines;
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
        };
        worker_ptr->get_io_context().post(stop_user_timer_service);
        // use std::move to destroy work
        auto dummy = std::move(get_lua_group_config(group_index).works[i]);
    }
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
        get_lua_state_data(L).group_id = i;
        get_lua_state_data(L).worker_id = 0;
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
            // setup_parallel_test might be called, but not necessarily
        }
        else
        {
            std::cerr<<"error running lua script:"<<std::endl<<lua_scripts[i]<<std::endl;
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
    auto all_coroutines_finished = [number_of_groups]()
    {
        size_t total_number_coroutines = 0;
        size_t total_number_finished = 0;
        for (int i = 0; i < number_of_groups; i++)
        {
            total_number_coroutines += get_lua_group_config(i).number_of_lua_coroutines;
            total_number_finished += get_lua_group_config(i).number_of_finished_coroutins;
        }
        return (total_number_coroutines == total_number_finished);
    };
    while (!all_coroutines_finished())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
    std::cerr<<"all coroutines finished"<<std::endl;
    for (auto& L: bootstrap_lua_states)
    {
        lua_close(L);
    }
    bootstrap_lua_states.clear();
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
}

bool is_running_in_worker_thread(lua_State* L)
{
    auto& lua_group_config = get_lua_group_config(get_lua_state_data(L).group_id);
    auto worker_thread = lua_group_config.workers[get_lua_state_data(L).worker_id];
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
        conf.json_config_schema.interval_to_send_ping = 5;

        Request request;
        Scenario scenario;
        scenario.requests.push_back(request);
        conf.json_config_schema.scenarios.push_back(scenario);
        conf.json_config_schema.connection_retry_on_disconnect = true;
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
    size_t group_id = get_lua_state_data(L).group_id;
    if (get_lua_group_config(group_id).workers.size() == 0)
    {
        init_workers(group_id);
    }
    return get_lua_group_config(group_id).workers[get_lua_state_data(L).worker_id].get();
}

int32_t _make_connection(lua_State *L, const std::string& uri, std::function<void(bool, h2load::Client_Interface*)> connected_callback)
{
    auto worker = get_worker(L);
    http_parser_url u {};
    if (http_parser_parse_url(uri.c_str(), uri.size(), 0, &u) != 0 ||
        !util::has_uri_field(u, UF_SCHEMA) || !util::has_uri_field(u, UF_HOST))
    {
        return -1;
    }
    auto group_id = get_lua_state_data(L).group_id;
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
            clients[base_uri].push_back(client);
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
            auto client = clients[base_uri][client_index];

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
                connected_callback(true, client.get());
            }
        }
    };
    worker->get_io_context().post(run_inside_worker);
    return 0;
}

int make_connection(lua_State *L)
{
    int top = lua_gettop(L);
    if ((top == 1)&&(lua_type(L, -1) == LUA_TSTRING))
    {
        size_t len;
        const char* str = lua_tolstring(L, -1, &len);
        std::string base_uri(str, len);
        lua_pop(L, 1);

        auto connected_callback = [L](bool success, h2load::Client_Interface* client)
        {
            lua_pushinteger(L, success ? client->get_client_unique_id() : -1);
            lua_resume_wrapper(L, 1);
        };
        if (_make_connection(L, base_uri, connected_callback) == 0)
        {
            return lua_yield(L, 0);
        }
    }
    else
    {
        for (size_t i= 0; i < top; i++)
        {
            lua_pop(L, 1);
        }
    }
    lua_pushinteger(L, -1);
    return 1;
}

int send_http_request(lua_State *L)
{
    auto request_sent = [L](int32_t stream_id, h2load::Client_Interface* client)
    {
        client->queue_stream_for_user_callback(stream_id);
        lua_pushinteger(L, client->get_client_unique_id());
        lua_pushinteger(L, stream_id);
        lua_resume_wrapper(L, 2);
    };
    return _send_http_request(L, request_sent);
}

int send_http_request_and_await_response(lua_State *L)
{
    auto request_sent = [L](int32_t stream_id, h2load::Client_Interface* client)
    {
        client->queue_stream_for_user_callback(stream_id);
        client->pass_response_to_lua(stream_id, L);
    };
    return _send_http_request(L, request_sent);
}

int sleep_for_ms(lua_State *L)
{
    uint64_t ms_to_sleep = 0;
    int top = lua_gettop(L);
    if (top == 1)
    {
        ms_to_sleep = lua_tointeger(L, -1);
        lua_pop(L, 1);
    }
    if (ms_to_sleep == 0)
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
    else
    {
        for (size_t i = 0; i < top; i++)
        {
            lua_pop(L, 1);
        }
    }
    auto worker = get_worker(L);

    auto run_inside_worker = [worker, client_unique_id, stream_id, L]()
    {
        auto& clients = worker->get_client_pool();
        for (auto& clients_to_same_host: clients)
        {
            for (auto& client: clients_to_same_host.second)
            {
                if (client->get_client_unique_id() == client_unique_id)
                {
                    client->pass_response_to_lua(stream_id, L);
                    return;
                }
            }
        }
        lua_createtable(L, 0, 1);
        lua_pushlstring(L, "", 0);
        lua_resume_wrapper(L, 2);
    };

    if (client_unique_id >= 0 && stream_id > 0)
    {
        worker->get_io_context().post(run_inside_worker);
        return lua_yield(L, 0);
    }
    else
    {
        lua_createtable(L, 0, 1);
        lua_pushlstring(L, "", 0);
        return 2;
    }
}


int _send_http_request(lua_State *L, std::function<void(int32_t, h2load::Client_Interface*)> request_sent_callback)
{
    auto retCode = 0;
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
                        retCode = -1;
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
                retCode = -1;
                break;
            }
        }
        lua_pop(L, 1);
    }

    if (retCode == 0)
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

        auto connected_callback = [payload, schema, authority, method, path, headers, request_sent_callback](bool success, h2load::Client_Interface* client)
        {
            if (!success)
            {
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
        if (_make_connection(L, base_uri, connected_callback) != 0)
        {
            retCode = -1;
        }
    }
    if (retCode == 0)
    {
        return lua_yield(L, 0);
    }
    return 0;
}

int lua_resume_wrapper(lua_State *L, int nargs)
{
    auto retCode = lua_resume(L, nargs);
    if (LUA_YIELD != retCode)
    {
      auto group_id = get_lua_state_data(L).group_id;
      std::lock_guard<std::mutex> guard(get_lua_group_config_mutex(group_id));
      auto& lua_group_config = get_lua_group_config(group_id);
      lua_group_config.number_of_finished_coroutins++;
      if (lua_group_config.coroutine_references.count(L))
      {
          auto worker_index = get_lua_state_data(L).worker_id;
          auto parent_lua_state = lua_group_config.lua_states_for_each_worker[worker_index].get();
          luaL_unref(parent_lua_state, LUA_REGISTRYINDEX, lua_group_config.coroutine_references[L]);
          lua_group_config.coroutine_references.erase(L);
          lua_gc(parent_lua_state, LUA_GCCOLLECT, 0);
      }
    }
    return retCode;
}


