#ifndef H2LOAD_CLIENT_H
#define H2LOAD_CLIENT_H


#include <vector>
#include <unordered_map>
#include <deque>

#include <ev.h>

extern "C" {
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
}
#include "http2.h"

#include "memchunk.h"
#include "template.h"

#include "h2load_Config.h"
#include "h2load_Worker.h"
#include "h2load_session.h"
#include "h2load_stats.h"
#include "h2load_Cookie.h"
#include <ares.h>


namespace h2load
{


struct Request_Data
{
    std::string schema;
    std::string authority;
    std::string req_payload;
    std::string path;
    uint64_t user_id;
    std::string method;
    std::map<std::string, std::string, ci_less> req_headers;
    std::string resp_payload;
    std::map<std::string, std::string, ci_less> resp_headers;
    uint16_t status_code;
    uint16_t expected_status_code;
    std::map<std::string, Cookie, std::greater<std::string>> saved_cookies;
    size_t next_request;
};

struct Client
{
    DefaultMemchunks wb;
    std::multimap<std::chrono::steady_clock::time_point, int32_t> stream_timestamp;
    std::unordered_map<int32_t, Stream> streams;
    ClientStat cstat;
    std::unique_ptr<Session> session;
    ev_io wev;
    ev_io rev;
    std::function<int(Client&)> readfn, writefn;
    Worker* worker;
    SSL* ssl;
    ev_timer request_timeout_watcher;
    addrinfo* next_addr;
    // Address for the current address.  When try_new_connection() is
    // used and current_addr is not nullptr, it is used instead of
    // trying next address though next_addr.  To try new address, set
    // nullptr to current_addr before calling connect().
    addrinfo* current_addr;
    ares_addrinfo* ares_addr;
    size_t reqidx;
    ClientState state;
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
    int fd;
    ev_timer conn_active_watcher;
    ev_timer conn_inactivity_watcher;
    std::string selected_proto;
    bool new_connection_requested;
    // true if the current connection will be closed, and no more new
    // request cannot be processed.
    bool final;
    // rps_watcher is a timer to invoke callback periodically to
    // generate a new request.
    ev_timer rps_watcher;
    ev_timer stream_timeout_watcher;
    ev_timer connection_timeout_watcher;
    // The timestamp that starts the period which contributes to the
    // next request generation.
    ev_tstamp rps_duration_started;
    // The number of requests allowed by rps, but limited by stream
    // concurrency.
    size_t rps_req_pending;
    // The number of in-flight streams.  req_inflight has similar value
    // but it only measures requests made during Phase::MAIN_DURATION.
    // rps_req_inflight measures the number of requests in all phases,
    // and it is only used if --rps is given.
    size_t rps_req_inflight;
    int32_t curr_stream_id;
    std::unique_ptr<Client> ancestor_to_release;
    ev_timer retart_client_watcher;
    Config* config;
    uint64_t curr_req_variable_value;
    std::deque<Request_Data> requests_to_submit;
    std::map<int32_t, Request_Data> requests_awaiting_response;
    std::vector<lua_State*> lua_states;
    std::map<std::string, Client*> dest_client;
    Client* parent_client;
    std::string schema;
    std::string authority;
    ares_channel channel;
    std::map<int, ev_io> ares_io_watchers;
    Client* next_to_run;

    enum { ERR_CONNECT_FAIL = -100 };

    Client(uint32_t id, Worker* worker, size_t req_todo, Config* conf,
             Client* initiating_client=nullptr, const std::string& dest_schema="",
             const std::string& dest_authority="");
    ~Client();
    template<class T>
    int make_socket(T* addr);
    int connect();
    void disconnect();
    void fail();
    // Call this function when do_read() returns -1.  This function
    // tries to connect to the remote host again if it is requested.  If
    // so, this function returns 0, and this object should be retained.
    // Otherwise, this function returns -1, and this object should be
    // deleted.
    int try_again_or_fail();
    void timeout();
    void restart_timeout();
    int submit_request();
    void process_request_failure(int errCode = -1);
    void process_timedout_streams();
    void process_abandoned_streams();
    void report_tls_info();
    void report_app_info();
    void terminate_session();
    // Asks client to create new connection, instead of just fail.
    void try_new_connection();

    int do_read();
    int do_write();

    // low-level I/O callback functions called by do_read/do_write
    int connected();
    int read_clear();
    int write_clear();
    int tls_handshake();
    int read_tls();
    int write_tls();

    int on_read(const uint8_t* data, size_t len);
    int on_write();

    int connection_made();

    void on_request_start(int32_t stream_id);
    void reset_timeout_requests();
    void on_header(int32_t stream_id, const uint8_t* name, size_t namelen,
                   const uint8_t* value, size_t valuelen);
    void on_status_code(int32_t stream_id, uint16_t status);
    // |success| == true means that the request/response was exchanged
    // |successfully, but it does not mean response carried successful
    // |HTTP status code.
    void on_stream_close(int32_t stream_id, bool success, bool final = false);

    void on_data_chunk(int32_t stream_id, const uint8_t* data, size_t len);

    // Returns RequestStat for |stream_id|.  This function must be
    // called after on_request_start(stream_id), and before
    // on_stream_close(stream_id, ...).  Otherwise, this will return
    // nullptr.
    RequestStat* get_req_stat(int32_t stream_id);
    void record_request_time(RequestStat* req_stat);
    void record_connect_start_time();
    void record_connect_time();
    void record_ttfb();
    void clear_connect_times();
    void record_client_start_time();
    void record_client_end_time();

    void signal_write();

    Request_Data get_request_to_submit();
    bool prepare_next_request(Request_Data& data);
    void replace_variable(std::string& input, const std::string& variable_name, uint64_t variable_value);
    void update_content_length(Request_Data& data);
    bool update_request_with_lua(lua_State* L, const Request_Data& finished_request, Request_Data& request_to_send);
    void produce_request_cookie_header(Request_Data& req_to_be_sent);
    void parse_and_save_cookies(Request_Data& finished_request);
    void move_cookies_to_new_request(Request_Data& finished_request, Request_Data& new_request);
    void populate_request_from_config_template(Request_Data& new_request,
                                                              size_t index_in_config_template);

    Client* find_or_create_dest_client(Request_Data& request_to_send);

    int resolve_fqdn_and_connect(const std::string& schema, const std::string& authority);
    int connect_to_host(const std::string& schema, const std::string& authority);

    bool any_request_to_submit();

};

}
#endif
