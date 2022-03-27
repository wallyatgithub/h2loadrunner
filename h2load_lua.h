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

void load_and_run_lua_script(const std::vector<std::string>& lua_scripts, h2load::Config& config);

h2load::asio_worker* get_worker(lua_State *L);

int32_t _make_connection(lua_State *L, const std::string& uri, std::function<void(bool)> connected_callback);

int lua_resume_wrapper (lua_State *L, int nargs);

void register_functions_to_lua(lua_State *L);

void init_new_lua_state(lua_State* L);

void stop_workers();

struct Lua_Group_Config
{
    explicit Lua_Group_Config():
        number_of_workers(1),
        number_of_client_to_same_host_in_one_worker(1),
        number_of_lua_coroutines(1),
        config_initialized(false),
        number_of_finished_coroutins(0)
    {
    };

    size_t number_of_lua_coroutines;
    size_t number_of_client_to_same_host_in_one_worker;
    size_t number_of_workers;
    bool config_initialized;
    std::string lua_script;
    size_t number_of_finished_coroutins;
    std::map<lua_State*, int> coroutine_references;
    std::vector<std::shared_ptr<lua_State>> lua_states_for_each_worker;
    std::vector<std::shared_ptr<h2load::asio_worker>> workers;
    std::vector<boost::asio::io_service::work> works;
    h2load::Config config_template;
    std::function<void()> group_start_entry;
};

struct Lua_State_Data
{
    size_t group_id = 0;
    size_t worker_id = 0;
    size_t unique_id_within_group = 0;
    bool started_from_worker_thread = false;
};

Lua_Group_Config& get_lua_group_config(lua_State* L);

std::mutex& get_lua_config_mutex(lua_State* L);

Lua_State_Data& get_lua_state_data(lua_State* L);

std::map<lua_State*, Lua_State_Data>& get_lua_state_data_repository();

void setup_test_group(size_t group_id);

void init_workers(size_t group_id);

void stop_workers(size_t number_of_groups);

bool to_be_restarted_in_worker_thread(lua_State* L);

bool is_running_in_worker_thread(lua_State* L);

/*
#define force_in_worker_thread_if_not_yet(L) \
if (to_be_restarted_in_worker_thread(L)) \
{ \
    return lua_yield(L, 0); \
}
*/

#endif
