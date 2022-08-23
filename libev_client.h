#ifndef H2LOAD_CLIENT_H
#define H2LOAD_CLIENT_H


#include <vector>
#include <unordered_map>
#include <deque>

#include <list>

#include <ev.h>

extern "C" {
#include <ares.h>
}

#include "memchunk.h"
#include "template.h"

#include "h2load_Config.h"
#include "libev_worker.h"
#include "h2load_session.h"
#include "h2load_stats.h"
#include "h2load_Cookie.h"
#include "h2load_utils.h"
#include "base_client.h"


namespace h2load
{

class libev_client: public base_client
{
public:
    enum { ERR_CONNECT_FAIL = -100 };

    libev_client(uint32_t id, libev_worker* wrker, size_t req_todo, Config* conf,
                 libev_client* parent = nullptr, const std::string& dest_schema = "",
                 const std::string& dest_authority = "");
    virtual ~libev_client();
    virtual size_t push_data_to_output_buffer(const uint8_t* data, size_t length);
    virtual void signal_write() ;
    virtual bool any_pending_data_to_write();
    virtual void start_conn_active_watcher();
    virtual std::shared_ptr<base_client> create_dest_client(const std::string& dst_sch,
                                                            const std::string& dest_authority);
    virtual int connect_to_host(const std::string& schema, const std::string& authority);
    virtual void disconnect();
    virtual void clear_default_addr_info();
    virtual void setup_connect_with_async_fqdn_lookup();
    virtual void feed_timing_script_request_timeout_timer();
    virtual void graceful_restart_connection();
    virtual void restart_timeout_timer();
    virtual void start_rps_timer();
    virtual void start_stream_timeout_timer();
    virtual void start_connect_to_preferred_host_timer();
    virtual void start_timing_script_request_timeout_timer(double duration);
    virtual void stop_timing_script_request_timeout_timer();
    virtual void stop_rps_timer();
    virtual void start_request_delay_execution_timer();
    virtual void conn_activity_timeout_handler();
    virtual void start_connect_timeout_timer();
    virtual void stop_connect_timeout_timer();

    virtual void start_warmup_timer();
    virtual void stop_warmup_timer();
    virtual void start_conn_inactivity_watcher();
    virtual void stop_conn_inactivity_timer();
    virtual int make_async_connection();
    virtual int do_connect();
    virtual void start_delayed_reconnect_timer();
    virtual void probe_and_connect_to(const std::string& schema, const std::string& authority);
    virtual void setup_graceful_shutdown();
    virtual bool is_write_signaled();

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

    int resolve_fqdn_and_connect(const std::string& schema, const std::string& authority,
                                 ares_addrinfo_callback callback = ares_addrinfo_query_callback);

    void init_timer_watchers();

    bool probe_address(ares_addrinfo* ares_address);

    int write_clear_with_callback();
    void restore_connectfn();
    int connect_with_async_fqdn_lookup();
    void init_ares();

    template<class T>
    int make_socket(T* addr);

#ifdef ENABLE_HTTP3
    int read_quic();
    int write_quic();
    int write_udp(const sockaddr* addr, socklen_t addrlen, const uint8_t* data,
                  size_t datalen, size_t gso_size);
    void quic_restart_pkt_timer();

    void setup_quic_pkt_timer();
    void quic_close_connection();
    void on_send_blocked(const ngtcp2_addr& remote_addr, const uint8_t* data,
                         size_t datalen, size_t gso_size);
    int send_blocked_packet();

    ev_timer pkt_timer;
    
    int quic_pkt_timeout();

#endif

    DefaultMemchunks wb;
    ev_io wev;
    ev_io rev;
    std::function<int(libev_client&)> readfn, writefn;
    std::function<int(libev_client&)> connectfn;
    ev_timer request_timeout_watcher;
    addrinfo* next_addr;
    // Address for the current address.  When try_new_connection() is
    // used and current_addr is not nullptr, it is used instead of
    // trying next address though next_addr.  To try new address, set
    // nullptr to current_addr before calling connect().
    addrinfo* current_addr;
    ares_addrinfo* ares_address;
    int fd;
    ev_timer conn_active_watcher;
    ev_timer conn_inactivity_watcher;
    // rps_watcher is a timer to invoke callback periodically to
    // generate a new request.
    ev_timer rps_watcher;
    ev_timer stream_timeout_watcher;
    ev_timer connection_timeout_watcher;

    // The number of requests allowed by rps, but limited by stream
    // concurrency.
    ev_timer send_ping_watcher;
    ares_channel channel;
    std::map<int, ev_io> ares_io_watchers;
    ev_timer delayed_request_watcher;
    ev_timer delayed_reconnect_watcher;
    ev_timer connect_to_preferred_host_watcher;
    ev_io probe_wev;
    int probe_skt_fd;
};

class Submit_Requet_Wrapper
{
public:
    libev_client* client;

    Submit_Requet_Wrapper(libev_client* this_client, libev_client* next_client)
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
