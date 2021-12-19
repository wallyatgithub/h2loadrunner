#ifndef H2LOAD_CONFIG_H
#define H2LOAD_CONFIG_H


#include <vector>
#include <atomic>

#include <ev.h>

#include "config_schema.h"

namespace h2load
{

struct Config
{
    std::vector<std::vector<nghttp2_nv>> nva;
    std::vector<std::string> h1reqs;
    std::vector<ev_tstamp> timings;
    nghttp2::Headers custom_headers;
    std::string scheme;
    std::string host;
    std::string connect_to_host;
    std::string ifile;
    std::string ciphers;
    // length of upload data
    int64_t data_length;
    addrinfo* addrs;
    size_t nreqs;
    size_t nclients;
    size_t nthreads;
    // The maximum number of concurrent streams per session.
    ssize_t max_concurrent_streams;
    size_t window_bits;
    size_t connection_window_bits;
    // rate at which connections should be made
    size_t rate;
    ev_tstamp rate_period;
    // amount of time for main measurements in timing-based test
    ev_tstamp duration;
    // amount of time to wait before starting measurements in timing-based test
    ev_tstamp warm_up_time;
    // amount of time to wait for activity on a given connection
    ev_tstamp conn_active_timeout;
    // amount of time to wait after the last request is made on a connection
    ev_tstamp conn_inactivity_timeout;
    enum { PROTO_HTTP2, PROTO_HTTP1_1 } no_tls_proto;
    uint32_t header_table_size;
    uint32_t encoder_header_table_size;
    // file descriptor for upload data
    int data_fd;
    // file descriptor to write per-request stats to.
    int log_fd;
    uint16_t port;
    uint16_t default_port;
    uint16_t connect_to_port;
    bool verbose;
    bool timing_script;
    std::string base_uri;
    // true if UNIX domain socket is used.  In this case, base_uri is
    // not used in usual way.
    bool base_uri_unix;
    // used when UNIX domain socket is used (base_uri_unix is true).
    sockaddr_un unix_addr;
    // list of supported NPN/ALPN protocol strings in the order of
    // preference.
    std::vector<std::string> npn_list;
    // The number of request per second for each client.
    std::atomic<double> rps;
//    uint64_t req_variable_start;
//    uint64_t req_variable_end;
//    std::string req_variable_name;
    // this is the name of the header, identifying resource created by server
    // h2load can send update/delete to operate the resource
//    std::string crud_resource_header_name;
//    std::string crud_create_method;
//    std::string crud_read_method;
//    std::string crud_update_method;
//    std::string crud_delete_method;
//    std::string crud_create_data_file_name;
//    std::string crud_update_data_file_name;
    uint16_t stream_timeout_in_ms;
    std::string rps_file;
    Config_Schema json_config_schema;
    std::vector<std::string> reqlines;
//    bool variable_range_slicing;

    Config();
    ~Config();

    bool is_rate_mode() const;
    bool is_timing_based_mode() const;
    bool has_base_uri() const;
    bool rps_enabled() const;
};

}
#endif
