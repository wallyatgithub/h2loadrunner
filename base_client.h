#ifndef BASE_CLIENT_H
#define BASE_CLIENT_H

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
//#include "base_worker.h"
#include "h2load_session.h"
#include "h2load.h"

namespace h2load
{

struct Config;
struct RequestStat;
class base_worker;

class Unique_Id
{
public:
    static std::atomic<uint64_t> client_unique_id;
    uint64_t my_id;
    Unique_Id();
};


class Stream_Callback_Data
{
public:
    int64_t stream_id = 0;
    std::function<void(void)> response_callback;
    std::string resp_payload;
    std::vector<std::map<std::string, std::string, ci_less>> resp_headers;
    bool response_available = false;
    bool resp_trailer_present = false;
};

using time_point_in_seconds_double =
    std::chrono::time_point<std::chrono::steady_clock, std::chrono::duration< double >>;

class base_client
{
public:
    base_client(uint32_t id, base_worker* wrker, size_t req_todo, Config* conf,
                SSL_CTX* ssl_ctx, base_client* parent = nullptr, const std::string& dest_schema = "",
                const std::string& dest_authority = "", PROTO_TYPE proto = PROTO_UNSPECIFIED);
    virtual ~base_client();
    virtual size_t push_data_to_output_buffer(const uint8_t* data, size_t length) = 0;
    virtual void signal_write() = 0;
    virtual bool any_pending_data_to_write() = 0;
    virtual std::shared_ptr<base_client> create_dest_client(const std::string& dst_sch,
                                                            const std::string& dest_authority,
                                                            PROTO_TYPE proto = PROTO_UNSPECIFIED) = 0;


    virtual void start_conn_active_watcher() = 0;
    virtual int connect_to_host(const std::string& schema, const std::string& authority) = 0;
    virtual void disconnect() = 0;
    virtual void clear_default_addr_info() = 0;
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
    virtual void start_delayed_reconnect_timer() = 0;
    virtual void probe_and_connect_to(const std::string& schema, const std::string& authority) = 0;
    virtual void setup_graceful_shutdown() = 0;
    virtual bool is_write_signaled() = 0;
    virtual void stop_delayed_execution_timer() = 0;

    bool is_disconnected();
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
    void process_submit_request_failure(int errCode = -1);

    void process_timedout_streams();
    void process_abandoned_streams();
    void mark_requests_to_send_fail();
    void timeout();
    void terminate_session();
    void on_header(int64_t stream_id, const uint8_t* name, size_t namelen,
                   const uint8_t* value, size_t valuelen);
    void record_ttfb();
    void on_data_chunk(int64_t stream_id, const uint8_t* data, size_t len);
    void on_stream_close(int64_t stream_id, bool success, bool final = false);
    RequestStat* get_req_stat(int64_t stream_id);
    void record_request_time(RequestStat* req_stat);
    std::map<int64_t, Request_Data>& requests_waiting_for_response();
    size_t& get_current_req_index();
    void on_request_start(int64_t stream_id);
    Request_Data get_request_to_submit();
    void on_status_code(int64_t stream_id, uint16_t status);
    bool is_final();
    void set_final(bool val);
    void cluster_on_connected();
    Config* get_config();
    Stats& get_stats();

    uint64_t get_total_pending_streams();
    base_client* get_controller_client();
    base_client* find_or_create_dest_client(Request_Data& request_to_send);
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
    void populate_request_from_config_template(Request_Data& new_request,
                                               size_t scenario_index,
                                               size_t request_index);

    void terminate_sub_clients();
    bool is_test_finished();
    void update_this_in_dest_client_map();

    void submit_ping();
    size_t get_index_of_next_scenario_to_run();
    void update_scenario_based_stats(size_t scenario_index, size_t request_index, bool success, bool status_success);
    bool rps_mode();
    void slice_var_ids();
    void init_lua_states();
    void init_connection_targert();
    void log_failed_request(const h2load::Config& config, const h2load::Request_Data& failed_req, int64_t stream_id);
    bool validate_response_with_lua(lua_State* L, const Request_Data& finished_request);
    void record_stream_close_time(int64_t stream_id);
    void brief_log_to_file(int64_t stream_id, bool success);
    void enqueue_request(Request_Data& finished_request, Request_Data&& new_request);
    void inc_status_counter_and_validate_response(int64_t stream_id);
    bool should_reconnect_on_disconnect();
    int select_protocol_and_allocate_session();
    void report_tls_info();
    bool get_host_and_port_from_authority(const std::string& schema, const std::string& authority, std::string& host,
                                          std::string& port);
    void call_connected_callbacks(bool success);
    void install_connected_callback(std::function<void(bool, h2load::base_client*)> callback);
    void queue_stream_for_user_callback(int64_t stream_id);
    void process_stream_user_callback(int64_t stream_id);
    void on_header_frame_begin(int64_t stream_id, uint8_t flags);
    void pass_response_to_lua(int64_t stream_id, lua_State* L);
    uint64_t get_client_unique_id();
    void set_prefered_authority(const std::string& authority);
    void run_post_response_action(Request_Data& finished_request);
    void run_pre_request_action(Request_Data& new_request);
    std::string assemble_string(const String_With_Variables_In_Between& source, size_t scenario_index, size_t req_index,
                                Scenario_Data_Per_User& scenario_data_per_user);
    bool parse_uri_and_poupate_request(const std::string& uri, Request_Data& new_request);
    void sanitize_request(Request_Data& new_request);
    size_t get_number_of_request_left();
    void dec_number_of_request_left();

#ifdef ENABLE_HTTP3
    // QUIC
    int quic_init(const sockaddr* local_addr, socklen_t local_addrlen,
                  const sockaddr* remote_addr, socklen_t remote_addrlen);
    void quic_free();
    int quic_handshake_completed();
    int quic_recv_stream_data(uint32_t flags, int64_t stream_id,
                              const uint8_t* data, size_t datalen);
    int quic_acked_stream_data_offset(int64_t stream_id, size_t datalen);
    int quic_stream_close(int64_t stream_id, uint64_t app_error_code);
    int quic_stream_reset(int64_t stream_id, uint64_t app_error_code);
    int quic_stream_stop_sending(int64_t stream_id, uint64_t app_error_code);
    int quic_extend_max_local_streams();
    void quic_write_qlog(const void* data, size_t datalen);
    int quic_make_http3_session();

    void request_connection_close();

    bool is_quic();

    void clean_up_this_in_dest_client_map();

    PROTO_TYPE get_proto_type();

#endif // ENABLE_HTTP3

    base_worker* worker;
    ClientStat cstat;
    std::multimap<std::chrono::steady_clock::time_point, int64_t> stream_timestamp;
    std::unordered_map<int64_t, Stream> streams;
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
    std::map<int64_t, Request_Data> requests_awaiting_response;
    std::vector<std::vector<lua_State*>> lua_states;
    std::map<PROTO_TYPE, std::map<std::string, base_client*>> dest_clients;
    std::shared_ptr<base_client> parent_client;
    std::string schema;
    std::string authority;
    std::string preferred_authority;
    double rps;
    std::deque<std::string> candidate_addresses;
    std::deque<std::string> used_addresses;
    Unique_Id this_client_id;
    std::function<bool()> write_clear_callback;
    std::vector<Scenario_Data_Per_Client> scenario_data_per_connection;
    time_point_in_seconds_double rps_duration_started;
    SSL* ssl;
    SSL_CTX* ssl_context;
    std::vector<std::function<void(bool, h2load::base_client*)>> connected_callbacks;
    std::map<int32_t, Stream_Callback_Data> stream_user_callback_queue;
    size_t req_inflight_of_all_clients = 0;
#ifdef ENABLE_HTTP3
    struct Quic
    {
        ngtcp2_crypto_conn_ref conn_ref;
        ngtcp2_conn* conn = nullptr;
        ngtcp2_connection_close_error last_error;
        bool close_requested = false;
        FILE* qlog_file = nullptr;

        struct
        {
            bool send_blocked = false;
            size_t num_blocked = 0;
            size_t num_blocked_sent = 0;
            struct
            {
                Address remote_addr;
                const uint8_t* data = nullptr;
                size_t datalen = 0;
                size_t gso_size = 0;
            } blocked[2];
            std::unique_ptr<uint8_t[]> data;
        } tx;
        void init()
        {
            conn = nullptr;
            close_requested = false;
            qlog_file = nullptr;
            tx.send_blocked = false;
            tx.num_blocked = 0;
            tx.num_blocked_sent = 0;
            for (int i = 0; i < 2; i++)
            {
                tx.blocked[i].data = nullptr;
                tx.blocked[i].datalen = 0;
                tx.blocked[i].gso_size = 0;
            }
        };
    } ;
    Quic quic; // TODO: make this unique_ptr and reallocate during reconnect
#endif // ENABLE_HTTP3
    std::string tls_keylog_file_name;
    PROTO_TYPE proto_type;
    URI_SCHEMA http_schema;
};

}
#endif

