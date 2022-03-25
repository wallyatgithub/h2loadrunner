#ifndef H2LOAD_LUA_H
#define H2LOAD_LUA_H
#include <iostream>
#include <atomic>
#include <set>
#include <thread>
#include <vector>
#include <sstream>
#include <stdlib.h>
extern "C" {
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
}
#include "http2.h"
#include "template.h"

#include "h2load.h"
#include "h2load_Config.h"
#include "asio_worker.h"

extern "C"
{

int make_connection(lua_State *L);

int send_http_request(lua_State *L);

int send_http_request_and_await_response(lua_State *L);

int await_response(lua_State *L);

int setup_parallel_test(lua_State *L);

int sleep_for_ms(lua_State *L);
}

int _send_http_request(lua_State *L, std::function<void(int32_t, h2load::Client_Interface*)> request_sent_callback);

void load_and_run_lua_script(const std::string& lua_script, h2load::Config& config);

h2load::asio_worker* get_worker(lua_State *L);

int32_t _make_connection(lua_State *L, const std::string& uri, std::function<void(bool)> connected_callback);

int lua_resume_wrapper (lua_State *L, int nargs);

void register_functions_to_lua(lua_State *L);

lua_State* create_new_lua_state();

void stop_workers();

struct Lua_Global_Config
{
    explicit Lua_Global_Config():
        number_of_lua_coroutines(1),
        number_of_workers(1),
        config_initialized(false),
        number_of_finished_coroutins(0)
    {
    };

    uint32_t number_of_lua_coroutines;
    uint32_t number_of_workers;
    bool config_initialized;
    std::string lua_script;
    size_t number_of_finished_coroutins;
    std::map<lua_State*, size_t> coroutine_to_worker_map;
    std::map<lua_State*, size_t> coroutine_id_map;
    std::map<lua_State*, int> coroutine_ref_map;
    std::vector<lua_State*> lua_states;
    std::vector<std::unique_ptr<h2load::asio_worker>> workers;
    std::vector<boost::asio::io_service::work> works;
    h2load::Config config_template;
};

Lua_Global_Config& get_lua_global_config();

std::mutex& get_lua_global_config_mutex();


#endif
