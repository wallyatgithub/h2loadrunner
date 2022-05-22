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


using Request_Preprocessor = std::function<void(std::map<std::string, std::string, ci_less>& headers,
                                                std::string& payload,
                                                std::string& orig_dst,
                                                std::string& proto)>;

static Request_Preprocessor dummy_req_pre_processor = {};

using Request_Sent_cb = std::function<void(int32_t, h2load::base_client*)>;


Request_Sent_cb await_response_request_sent_cb_generator(lua_State *L);


extern "C"
{

int make_connection(lua_State *L);

int send_http_request(lua_State *L);

int send_http_request_and_await_response(lua_State *L);

int forward_http_request_and_await_response(lua_State *L);

int send_grpc_request_and_await_response(lua_State *L);

int await_response(lua_State *L);

int setup_parallel_test(lua_State *L);

int sleep_for_ms(lua_State *L);

int time_since_epoch(lua_State *L);

int start_server(lua_State *L);

int stop_server(lua_State *L);

int register_service_handler(lua_State *L);

int send_response(lua_State *L);

int wait_for_message(lua_State *L);

int resolve_hostname(lua_State *L);


}

int _send_http_request(lua_State *L, Request_Preprocessor request_preprocessor, std::function<void(int32_t, h2load::base_client*)> request_sent_callback);

void load_and_run_lua_script(const std::vector<std::string>& lua_scripts, h2load::Config& config);

h2load::asio_worker* get_worker(lua_State *L);

/*
 * return code:
 * 0: successfully injected std::function to worker thread, thus the caller needs to yield
 */
int32_t _make_connection(lua_State *L, const std::string& uri, std::function<void(bool)> connected_callback,
                                const std::string& proto);

int lua_resume_wrapper (lua_State *L, int nargs);

void update_orig_dst_and_proto(std::map<std::string, std::string, ci_less>& headers, std::string& payload,
                                         std::string& orig_dst,
                                         std::string& proto);

int lua_resume_if_yielded(lua_State *L, int nargs);

void register_functions_to_lua(lua_State *L);

void register_3rd_party_lib_func_to_lua(lua_State *L);

void init_new_lua_state(lua_State* L);

bool is_coroutine_with_unique_id(lua_State* L);

bool is_coroutine_to_be_returned_to_pool(lua_State* L);

struct Lua_State_Data
{
    int64_t unique_id_within_group = 0;
};

struct Host_Resolution_Data
{
    std::vector<lua_State*> lua_sates_await_result;
    std::vector<std::string> ip_addresses;
    std::chrono::steady_clock::time_point expire_time_point;
};

struct Data_Per_Worker_Thread
{
    std::map<lua_State*, int> coroutine_references;
    std::map<std::string, Host_Resolution_Data> host_resolution_data;
    std::vector<lua_State*> lua_coroutine_pools;
    std::shared_ptr<lua_State> lua_main_states_per_worker;
    std::map<lua_State*, Lua_State_Data> lua_state_data;
};

struct Lua_Group_Config
{
    explicit Lua_Group_Config():
        number_of_workers(1),
        number_of_client_to_same_host_in_one_worker(1),
        number_of_parallel_lua_coroutines(1),
        config_initialized(false),
        number_of_finished_coroutins(0),
        server_running(false)
    {
        data_per_worker_thread.resize(number_of_workers);
    };

    size_t number_of_parallel_lua_coroutines;
    size_t number_of_client_to_same_host_in_one_worker;
    size_t number_of_workers;
    bool config_initialized;
    std::string lua_script;
    size_t number_of_finished_coroutins;
    std::vector<Data_Per_Worker_Thread> data_per_worker_thread;
    std::vector<std::shared_ptr<h2load::asio_worker>> workers;
    std::vector<boost::asio::io_service::work> works;
    h2load::Config config_template;
    bool server_running;
    std::string server_id;
};

Lua_Group_Config& get_lua_group_config(size_t group_id);

Data_Per_Worker_Thread& get_runtime_data(lua_State* L);

std::mutex& get_lua_config_mutex(lua_State* L);

Lua_State_Data& get_lua_state_data(lua_State* L);

void start_test_group(size_t group_id);

void load_service_script_into_lua_states(size_t group_id, const std::string& server_id);

void invoke_service_hanlder(lua_State *L, std::string lua_function_name,
                                   boost::asio::io_service* ios,
                                   uint64_t handler_id,
                                   int32_t stream_id,
                                   const std::multimap<std::string, std::string>& req_headers,
                                   const std::string& payload);

void setup_test_group(size_t group_id);

void init_workers(size_t group_id);

void stop_workers(size_t number_of_groups);

void set_group_id(lua_State* L, size_t group_id);

void set_worker_index(lua_State* L, size_t worker_index);

size_t get_group_id(lua_State* L);

size_t get_worker_index(lua_State* L);

void set_server_id(lua_State* L, std::string server_id);

std::string get_server_id(lua_State* L);

void set_passive(lua_State* L);

bool is_passive(lua_State* L);

bool is_test_finished(size_t number_of_test_groups);

void enter_c_function(lua_State* L);

void format_length_prefixed_message(std::string& payload);

/*
 * this function would return or yield, depending on the outcome of the lua c function
 */
uint64_t leave_c_function(lua_State* L);

extern "C"
{
// functions from pb.so
LUALIB_API int luaopen_pb_io(lua_State *L);
LUALIB_API int luaopen_pb_conv(lua_State *L);
LUALIB_API int luaopen_pb_buffer(lua_State *L);
LUALIB_API int luaopen_pb_slice(lua_State *L);
LUALIB_API int luaopen_pb(lua_State *L);
LUALIB_API int luaopen_pb_unsafe(lua_State *L);
}

#endif
