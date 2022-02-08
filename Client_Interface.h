#ifndef CLIENT_INTERFACE_H
#define CLIENT_INTERFACE_H

#include <map>
#include <string>
#include <chrono>

extern "C" {
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
}
#include <openssl/ssl.h>

#include "http2.h"


#include "config_schema.h"
#include "h2load_stats.h"
//#include "Worker_Interface.h"
#include "h2load_session.h"
#include "h2load.h"

namespace h2load
{

class Config;
class RequestStat;
class Worker_Interface;

class Unique_Id
{
public:
    static std::atomic<uint64_t> client_unique_id;
    uint64_t my_id;
    Unique_Id();
};

using time_point_in_seconds_double =
    std::chrono::time_point<std::chrono::steady_clock, std::chrono::duration< double >>;

class Client_Interface
{
public:
    Client_Interface(uint32_t id, Worker_Interface* wrker, size_t req_todo, Config* conf,
                     Client_Interface* parent = nullptr, const std::string& dest_schema = "",
                     const std::string& dest_authority = "");
    virtual ~Client_Interface() {}
    virtual size_t push_data_to_output_buffer(const uint8_t* data, size_t length) = 0;
    virtual void signal_write() = 0;
    virtual bool any_pending_data_to_write() = 0;
    virtual std::shared_ptr<Client_Interface> create_dest_client(const std::string& dst_sch,
                                                                 const std::string& dest_authority) = 0;


    virtual void start_conn_active_watcher() = 0;
    virtual int connect_to_host(const std::string& schema, const std::string& authority) = 0;
    virtual void disconnect() = 0;
    virtual void clear_default_addr_info() = 0;
    virtual void setup_connect_with_async_fqdn_lookup() = 0;
    virtual void feed_timing_script_request_timeout_timer() = 0;
    virtual void graceful_restart_connection() = 0;
    virtual void restart_timeout_timer() = 0;
    virtual void start_rps_timer() = 0;
    virtual void start_stream_timeout_timer() = 0;
    virtual void start_connect_to_preferred_host_timer() = 0;
    virtual void start_timing_script_request_timeout_timer(double duration) = 0;
    virtual void stop_timing_script_request_timeout_timer() = 0;
    virtual void stop_rps_timer() = 0;
    virtual void start_request_delay_execution_timer() = 0;
    virtual void conn_activity_timeout_handler() = 0;
    virtual void start_connect_timeout_timer() = 0;
    virtual void stop_connect_timeout_timer() = 0;
    virtual void start_warmup_timer() = 0;
    virtual void stop_warmup_timer() = 0;
    virtual void start_conn_inactivity_watcher() = 0;
    virtual void stop_conn_inactivity_timer() = 0;
    virtual int make_async_connection() = 0;
    virtual int do_connect() = 0;
    virtual void start_delayed_reconnect_timer() = 0;
    virtual void probe_and_connect_to(const std::string& schema, const std::string& authority) = 0;
    virtual void setup_graceful_shutdown() = 0;

    int connect();
    void cleanup_due_to_disconnect();
    void final_cleanup();
    void init_req_left();
    void reconnect_to_used_host();
    void on_prefered_host_up();
    bool reconnect_to_alt_addr();
    void try_new_connection();
    void connection_timeout_handler();
    void timing_script_timeout_handler();
    void on_rps_timer();
    void resume_delayed_request_execution();

    void print_app_info();
    int connection_made();
    void fail();
    // Call this function when do_read() returns -1.  This function
    // tries to connect to the remote host again if it is requested.  If
    // so, this function returns 0, and this object should be retained.
    // Otherwise, this function returns -1, and this object should be
    // deleted.
    int try_again_or_fail();
    void process_request_failure(int errCode = -1);

    void process_timedout_streams();
    void process_abandoned_streams();
    void timeout();
    void terminate_session();
    void on_header(int32_t stream_id, const uint8_t* name, size_t namelen,
                   const uint8_t* value, size_t valuelen);
    void record_ttfb();
    void on_data_chunk(int32_t stream_id, const uint8_t* data, size_t len);
    void on_stream_close(int32_t stream_id, bool success, bool final = false);
    RequestStat* get_req_stat(int32_t stream_id);
    void record_request_time(RequestStat* req_stat);
    std::map<int32_t, Request_Data>& requests_waiting_for_response();
    size_t& get_current_req_index();
    void on_request_start(int32_t stream_id);
    Request_Data get_request_to_submit();
    void on_status_code(int32_t stream_id, uint16_t status);
    bool is_final();
    void set_final(bool val);
    size_t get_req_left();

    Config* get_config();
    Stats& get_stats();

    uint64_t get_total_pending_streams();
    Client_Interface* get_controller_client();
    Client_Interface* find_or_create_dest_client(Request_Data& request_to_send);
    bool is_controller_client();
    int submit_request();
    Request_Data prepare_first_request();

    void reset_timeout_requests();

    void record_connect_start_time();
    void record_connect_time();
    void clear_connect_times();
    void record_client_start_time();
    void record_client_end_time();

    bool prepare_next_request(Request_Data& data);
    void update_content_length(Request_Data& data);
    bool update_request_with_lua(lua_State* L, const Request_Data& finished_request, Request_Data& request_to_send);
    void produce_request_cookie_header(Request_Data& req_to_be_sent);
    void parse_and_save_cookies(Request_Data& finished_request);
    void move_cookies_to_new_request(Request_Data& finished_request, Request_Data& new_request);
    void populate_request_from_config_template(Request_Data& new_request,
                                               size_t scenario_index,
                                               size_t index_in_config_template);

    void terminate_sub_clients();
    bool is_test_finished();
    void update_this_in_dest_client_map();

    void submit_ping();
    size_t get_index_of_next_scenario_to_run();
    void update_scenario_based_stats(size_t scenario_index, size_t request_index, bool success, bool status_success);
    bool rps_mode();
    void slice_user_id();
    void init_lua_states();
    void init_connection_targert();
    void log_failed_request(const h2load::Config& config, const h2load::Request_Data& failed_req, int32_t stream_id);
    bool validate_response_with_lua(lua_State* L, const Request_Data& finished_request);
    void record_stream_close_time(int32_t stream_id);
    void brief_log_to_file(int32_t stream_id, bool success);
    void enqueue_request(Request_Data& finished_request, Request_Data&& new_request);
    void inc_status_counter_and_validate_response(int32_t stream_id);
    bool should_reconnect_on_disconnect();
    int select_protocol_and_allocate_session();
    void report_tls_info();


    Worker_Interface* worker;
    ClientStat cstat;
    std::multimap<std::chrono::steady_clock::time_point, int32_t> stream_timestamp;
    std::unordered_map<int32_t, Stream> streams;
    std::unique_ptr<Session> session;
    ClientState state;
    size_t reqidx;
    // The number of requests this client has to issue.
    size_t req_todo;
    // The number of requests left to issue
    size_t req_left;
    // The number of requests currently have started, but not abandoned
    // or finished.
    size_t req_inflight;
    // The number of requests this client has issued so far.
    size_t req_started;
    // The number of requests this client has done so far.
    size_t req_done;
    // The client id per worker
    uint32_t id;
    std::string selected_proto;
    bool new_connection_requested;
    // true if the current connection will be closed, and no more new
    // request cannot be processed.
    bool final;
    size_t rps_req_pending;
    // The number of in-flight streams.  req_inflight has similar value
    // but it only measures requests made during Phase::MAIN_DURATION.
    // rps_req_inflight measures the number of requests in all phases,
    // and it is only used if --rps is given.
    size_t rps_req_inflight;
    Config* config;
    std::deque<Request_Data> requests_to_submit;
    std::multimap<std::chrono::steady_clock::time_point, Request_Data> delayed_requests_to_submit;
    std::map<int32_t, Request_Data> requests_awaiting_response;
    std::vector<std::vector<lua_State*>> lua_states;
    std::map<std::string, Client_Interface*> dest_clients;
    Client_Interface* parent_client;
    std::string schema;
    std::string authority;
    std::string preferred_authority;
    double rps;
    std::deque<std::string> candidate_addresses;
    std::deque<std::string> used_addresses;
    Unique_Id this_client_id;
    std::function<void()> write_clear_callback;
    std::vector<Runtime_Scenario_Data> runtime_scenario_data;
    time_point_in_seconds_double rps_duration_started;
    SSL* ssl;
};

}
#endif

