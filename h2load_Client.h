#ifndef H2LOAD_CLIENT_H
#define H2LOAD_CLIENT_H


#include <vector>
#include <unordered_map>
#include <deque>

#include <list>

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
#include "h2load_utils.h"

static std::string emptyString;

namespace h2load
{

struct Request_Data
{
    std::string* schema;
    std::string* authority;
    std::string* req_payload;
    std::string* path;
    uint64_t user_id;
    std::string* method;
    std::map<std::string, std::string, ci_less>* req_headers;
    std::map<std::string, std::string, ci_less> shadow_req_headers;
    std::string resp_payload;
    std::map<std::string, std::string, ci_less> resp_headers;
    uint16_t status_code;
    uint16_t expected_status_code;
    uint32_t delay_before_executing_next;
    std::map<std::string, Cookie, std::greater<std::string>> saved_cookies;
    size_t next_request_idx;
    std::shared_ptr<TransactionStat> transaction_stat;
    std::vector<std::string> string_collection;
    explicit Request_Data():
    schema(&emptyString),
    authority(&emptyString),
    req_payload(&emptyString),
    path(&emptyString),
    method(&emptyString)
    {
        user_id = 0;
        status_code = 0;
        expected_status_code = 0;
        delay_before_executing_next = 0;
        next_request_idx = 0;
        transaction_stat = nullptr;
        string_collection.reserve(12); // (path, authority, method, schema, payload, xx) * 2
    };

    friend std::ostream& operator<<(std::ostream& o, const Request_Data& request_data)
    {
        o << "Request_Data: { "<<std::endl
          << "schema:" << *request_data.schema<<std::endl
          << "authority:" << *request_data.authority<<std::endl
          << "req_payload:" << *request_data.req_payload<<std::endl
          << "path:" << *request_data.path<<std::endl
          << "user_id:" << request_data.user_id<<std::endl
          << "method:" << *request_data.method<<std::endl
          << "expected_status_code:" << request_data.expected_status_code<<std::endl
          << "delay_before_executing_next:" << request_data.delay_before_executing_next<<std::endl;

        for (auto& it: *(request_data.req_headers))
        {
            o << "request header name from template: "<<it.first<<", header value: " <<it.second<<std::endl;
        }
        for (auto& it: request_data.shadow_req_headers)
        {
            o << "updated request header name: "<<it.first<<", header value: " <<it.second<<std::endl;
        }

        o << "response status code:" << request_data.status_code<<std::endl;
        o << "resp_payload:" << request_data.resp_payload<<std::endl;
        for (auto& it: request_data.resp_headers)
        {
            o << "response header name: "<<it.first<<", header value: " <<it.second<<std::endl;
        }
        o << "next request index: "<<request_data.next_request_idx<<std::endl;

        for (auto& it: request_data.saved_cookies)
        {
            o << "cookie name: "<<it.first<<", cookie content: " <<it.second<<std::endl;
        }

        o << "}"<<std::endl;
        return o;
    };

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
    ev_timer restart_client_watcher;
    Config* config;
    uint64_t curr_req_variable_value;
    std::deque<Request_Data> requests_to_submit;
    std::multimap<std::chrono::steady_clock::time_point, Request_Data> delayed_requests_to_submit;
    std::map<int32_t, Request_Data> requests_awaiting_response;
    std::vector<lua_State*> lua_states;
    std::map<std::string, Client*> dest_clients;
    Client* parent_client;
    std::string schema;
    std::string authority;
    std::string preferred_authority;
    ares_channel channel;
    std::map<int, ev_io> ares_io_watchers;
    ev_timer release_ancestor_watcher;
    ev_timer delayed_request_watcher;
    uint64_t req_variable_value_start;
    uint64_t req_variable_value_end;
    size_t totalTrans_till_last_check = 0;
    std::chrono::time_point<std::chrono::steady_clock> timestamp_of_last_tps_check;
    size_t total_leading_Req_till_last_check = 0;
    std::chrono::time_point<std::chrono::steady_clock> timestamp_of_last_rps_check;
    double rps;
    ev_timer adaptive_traffic_watcher;
    std::deque<std::string> candidate_addresses;
    std::deque<std::string> used_addresses;
    ev_timer delayed_reconnect_watcher;
    static std::atomic<uint32_t> client_unique_id;
    ev_timer connect_to_preferred_host_watcher;
    ev_io probe_wev;
    int probe_skt_fd;
    std::function<void()> write_clear_callback;

    enum { ERR_CONNECT_FAIL = -100 };

    Client(uint32_t id, Worker* worker, size_t req_todo, Config* conf,
             Client* parent = nullptr, const std::string& dest_schema = "",
             const std::string& dest_authority = "");
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
    Request_Data prepare_first_request();
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

    int resolve_fqdn_and_connect(const std::string& schema, const std::string& authority,
                                           ares_addrinfo_callback callback = ares_addrinfo_query_callback);
    int connect_to_host(const std::string& schema, const std::string& authority);

    bool any_request_to_submit();

    void terminate_sub_clients();

    void substitute_ancestor(Client* ancestor);

    void enqueue_request(Request_Data& finished_request, Request_Data&& new_request);

    std::map<std::string, Client*>::const_iterator get_client_serving_first_request();

    double calc_tps();
    double calc_rps();

    bool rps_mode();
    double adjust_traffic_needed();
    void switch_to_non_rps_mode();
    void switch_mode(double new_rps);
    bool is_leading_request(Request_Data& request);
    void mark_response_success_or_failure(int32_t stream_id);
    uint64_t get_total_pending_streams();

    bool is_controller_client();
    Client* get_controller_client();
    void transfer_controllership();

    bool reconnect_to_alt_addr();

    void init_timer_watchers();

    bool is_test_finished();

    bool probe_address(ares_addrinfo* ares_addr);

    int write_clear_with_callback();

    void update_this_in_dest_client_map();

    bool should_reconnect_on_disconnect();

};

class Submit_Requet_Wrapper
{
public:
  Client* client;

  Submit_Requet_Wrapper(Client* this_client, Client* next_client)
  {
      if (next_client != this_client && next_client)
      {
          client = next_client;
      }
      else
      {
          client = nullptr;
      }
  };
  ~Submit_Requet_Wrapper()
  {
      if (client &&
          !client->rps_mode() &&
          client->state == CLIENT_CONNECTED)
      {
          client->submit_request();
          client->signal_write();
      }

  };
};


}
#endif
