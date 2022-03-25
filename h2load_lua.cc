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

std::mutex& get_lua_global_config_mutex()
{
    static std::mutex lua_global_config_mutex;
    return lua_global_config_mutex;
}

Lua_Global_Config& get_lua_global_config()
{
    static Lua_Global_Config lua_global_config;
    return lua_global_config;
}

int setup_parallel_test(lua_State *L)
{
    uint32_t number_of_coroutines = 0;
    uint32_t number_of_workers = 0;

    int top = lua_gettop(L);
    if (top == 2)
    {
        number_of_coroutines = lua_tointeger(L, -1);
        lua_pop(L, 1);
        number_of_workers = lua_tointeger(L, -1);
        lua_pop(L, 1);
    }
    if ((number_of_coroutines > 0) &&
        (get_lua_global_config().config_initialized == false) &&
        (get_lua_global_config().lua_script.size() > 0))
    {
        get_lua_global_config().config_initialized = true;
        get_lua_global_config().number_of_lua_coroutines = number_of_coroutines;
        get_lua_global_config().number_of_workers = number_of_workers;
        std::cerr<<"number of workers: "<<number_of_workers <<", nummber of coroutines: "<<number_of_coroutines<<std::endl;
        for (int i = 0; i < number_of_workers; i++)
        {
            auto lua_state = create_new_lua_state();
            get_lua_global_config().lua_states.push_back(lua_state);
        }
        for (int i = 0; i < number_of_coroutines; i++)
        {
            std::lock_guard<std::mutex> guard(get_lua_global_config_mutex());
            auto worker_index = i % get_lua_global_config().number_of_workers;
            auto parent_lua_state = get_lua_global_config().lua_states[worker_index];
            if (!lua_checkstack(parent_lua_state, 1))
            {
                std::cerr<<"no enough space in stack"<<std::endl;
                exit(1);
            }
            lua_State* cL = lua_newthread(parent_lua_state);
            get_lua_global_config().coroutine_ref_map[cL] = luaL_ref(parent_lua_state, LUA_REGISTRYINDEX);
            get_lua_global_config().coroutine_id_map[cL] = i;
            luaL_loadstring(cL, get_lua_global_config().lua_script.c_str());
        }
        for (auto& coroutine: get_lua_global_config().coroutine_id_map)
        {
            auto t = coroutine.first;
            auto start_coroutine = [t]()
            {
                lua_resume_wrapper(t, 0);
            };
            get_worker(coroutine.first)->get_io_context().post(start_coroutine);
        }
        return lua_yield(L, 0);
    }
    else
    {
      lua_pushinteger(L, get_lua_global_config().coroutine_id_map[L]);
      return 1;
    }
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
    get_worker(L)->enqueue_user_timer(ms_to_sleep, wakeup_me);
    return lua_yield(L, 0);
}

lua_State* create_new_lua_state()
{
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    lua_register(L, "make_connection", make_connection);
    lua_register(L, "send_http_request", send_http_request);
    lua_register(L, "await_response", await_response);
    lua_register(L, "send_http_request_and_await_response", send_http_request_and_await_response);
    lua_register(L, "setup_parallel_test", setup_parallel_test);
    lua_register(L, "sleep_for_ms", sleep_for_ms);
    return L;
}

void stop_workers()
{
    for (int i = 0; i < get_lua_global_config().workers.size(); i++)
    {
        auto worker_ptr = get_lua_global_config().workers[i].get();
        auto stop_user_timer_service = [worker_ptr]()
        {
            worker_ptr->stop_tick_timer();
        };
        worker_ptr->get_io_context().post(stop_user_timer_service);
    }
}

void load_and_run_lua_script(const std::string& lua_script, h2load::Config& config)
{
    get_lua_global_config().lua_script = lua_script;
    get_lua_global_config().config_template = config;
    auto L = create_new_lua_state();
    luaL_loadstring(L, lua_script.c_str());
    auto retCode = lua_resume_wrapper(L, 0);
    if (LUA_YIELD == retCode)
    {
        while (get_lua_global_config().number_of_lua_coroutines > get_lua_global_config().number_of_finished_coroutins)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        }
        std::cerr<<"all coroutines finished"<<std::endl;
    }
    else
    {
        if (LUA_OK != retCode)
        {
            std::cerr<<"error loading lua script"<<std::endl;
        }
    }
    lua_close(L);
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
}

h2load::asio_worker* get_worker(lua_State *L)
{
    static h2load::Config conf;
    auto init_config = []()
    {
        conf.ciphers = get_lua_global_config().config_template.ciphers;
        conf.max_concurrent_streams = get_lua_global_config().config_template.max_concurrent_streams;
        conf.window_bits = get_lua_global_config().config_template.window_bits;
        conf.connection_window_bits = get_lua_global_config().config_template.connection_window_bits;
        conf.conn_active_timeout = get_lua_global_config().config_template.conn_active_timeout;
        conf.conn_inactivity_timeout = get_lua_global_config().config_template.conn_inactivity_timeout;
        conf.no_tls_proto = get_lua_global_config().config_template.no_tls_proto;
        conf.header_table_size = get_lua_global_config().config_template.header_table_size;
        conf.encoder_header_table_size = get_lua_global_config().config_template.encoder_header_table_size;
        conf.verbose = get_lua_global_config().config_template.verbose;
        conf.npn_list = get_lua_global_config().config_template.npn_list;
        conf.stream_timeout_in_ms = get_lua_global_config().config_template.stream_timeout_in_ms;
        conf.json_config_schema.ca_cert = get_lua_global_config().config_template.json_config_schema.ca_cert;
        conf.json_config_schema.client_cert = get_lua_global_config().config_template.json_config_schema.client_cert;
        conf.json_config_schema.private_key = get_lua_global_config().config_template.json_config_schema.private_key;
        conf.json_config_schema.cert_verification_mode = get_lua_global_config().config_template.json_config_schema.cert_verification_mode;
        conf.json_config_schema.max_tls_version = get_lua_global_config().config_template.json_config_schema.max_tls_version;
      
        Request request;
        Scenario scenario;
        scenario.requests.push_back(request);
        conf.json_config_schema.scenarios.push_back(scenario);
        conf.json_config_schema.connection_retry_on_disconnect = true;
        return true;
    };
    static auto init_config_ret_code = init_config();

    auto init_workers = []()
    {
        for (int i = 0; i < get_lua_global_config().number_of_workers; i++)
        {
            get_lua_global_config().workers.emplace_back(std::make_unique<h2load::asio_worker>(0, 0xFFFFFFFF, 1, 0, 1000, &conf));
        }
        for (int i = 0; i < get_lua_global_config().workers.size(); i++)
        {
            get_lua_global_config().works.emplace_back(get_lua_global_config().workers[i]->get_io_context());
        }
        return true;
    };
    static auto init_workers_ret_code = init_workers();

    auto run_workers = []()
    {
        auto thread_func = [](h2load::asio_worker* worker_ptr)
        {
            worker_ptr->run_event_loop();
        };
        for (int i = 0; i < get_lua_global_config().workers.size(); i++)
        {
            auto worker_ptr = get_lua_global_config().workers[i].get();
            std::thread worker_thread(thread_func, worker_ptr);
            worker_thread.detach();
            auto start_user_timer_service = [worker_ptr]()
            {
                worker_ptr->start_tick_timer();
            };
            worker_ptr->get_io_context().post(start_user_timer_service);
        }
        return true;
    };
    static auto run_workers_ret_Code = run_workers();

    size_t worker_index =
      get_lua_global_config().coroutine_id_map.count(L) ? get_lua_global_config().coroutine_id_map[L]%get_lua_global_config().number_of_workers: 0;
    return get_lua_global_config().workers[worker_index].get();
}

int32_t _make_connection(lua_State *L, const std::string& uri, std::function<void(bool, h2load::Client_Interface*)> connected_callback)
{
    h2load::asio_worker* worker;
    worker = get_worker(L);
    http_parser_url u {};
    if (http_parser_parse_url(uri.c_str(), uri.size(), 0, &u) != 0 ||
        !util::has_uri_field(u, UF_SCHEMA) || !util::has_uri_field(u, UF_HOST))
    {
        return -1;
    }
    auto run_inside_worker = [uri, connected_callback, worker]()
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
        if (clients.count(base_uri) == 0)
        {
            clients[base_uri] = worker->create_new_client(0xFFFFFFFF);
            clients[base_uri]->install_connected_callback(connected_callback);
            clients[base_uri]->set_prefered_authority(authority);
            clients[base_uri]->connect_to_host(schema, authority);
        }
        else if (h2load::CLIENT_IDLE == clients[base_uri]->state)
        {
            clients[base_uri]->install_connected_callback(connected_callback);
            clients[base_uri]->connect_to_host(schema, authority);
        }
        else if (h2load::CLIENT_CONNECTING == clients[base_uri]->state)
        {
            clients[base_uri]->install_connected_callback(connected_callback);
        }
        else
        {
            connected_callback(true, clients[base_uri].get());
        }
    };
    worker->get_io_context().post(run_inside_worker);
    return 0;
}

int make_connection(lua_State *L)
{
    if (lua_type(L, -1) == LUA_TSTRING)
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
    return 0;
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
    if (client_unique_id >= 0 && stream_id > 0)
    {
        auto worker = get_worker(L);
        auto& clients = worker->get_client_pool();
        for (auto& client: clients)
        {
            if (client.second->get_client_unique_id() == client_unique_id)
            {
                if (client.second->pass_response_to_lua(stream_id, L))
                {
                   return lua_yield(L, 0);
                }
            }
        }
    }

    lua_createtable(L, 0, 1);
    lua_pushliteral(L, "nil");
    lua_pushliteral(L, "nil");
    lua_rawset(L, -3);
    lua_pushlstring(L, "", 0);
    return 2;
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
    return retCode;
}

int lua_resume_wrapper(lua_State *L, int nargs)
{
    auto retCode = lua_resume(L, nargs);
    if (LUA_OK == retCode)
    {
      std::lock_guard<std::mutex> guard(get_lua_global_config_mutex());
      get_lua_global_config().number_of_finished_coroutins++;
      if (get_lua_global_config().coroutine_id_map.count(L))
      {
          auto worker_index = get_lua_global_config().coroutine_id_map[L] % get_lua_global_config().number_of_workers;
          auto parent_lua_state = get_lua_global_config().lua_states[worker_index];
          luaL_unref(parent_lua_state, LUA_REGISTRYINDEX, get_lua_global_config().coroutine_ref_map[L]);
          get_lua_global_config().coroutine_ref_map.erase(L);
          lua_gc(parent_lua_state, LUA_GCCOLLECT, 0);
      }
    }
    return retCode;
}


