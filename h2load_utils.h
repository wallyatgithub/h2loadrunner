#ifndef H2LOAD_UTILS_H
#define H2LOAD_UTILS_H
#include <iostream>
#include <atomic>
#include <set>
#include <thread>
#include <vector>
#include <sstream>
#include <stdlib.h>

#include <ev.h>
#include <openssl/ssl.h>
extern "C" {
#include <ares.h>
}

#include "http2.h"
#include "template.h"
#include "memchunk.h"

#include "h2load.h"
#include "h2load_Config.h"
#include "h2load_Worker.h"
#include "h2load_Cookie.h"
#include "h2load_stats.h"

#include "h2load_http1_session.h"
#include "h2load_http2_session.h"


std::unique_ptr<h2load::Worker> create_worker(uint32_t id, SSL_CTX* ssl_ctx,
                                              size_t nreqs, size_t nclients,
                                              size_t rate, size_t max_samples, h2load::Config& config);

int parse_header_table_size(uint32_t& dst, const char* opt,
                            const char* optarg);

void read_script_from_file(std::istream& infile,
                           std::vector<ev_tstamp>& timings,
                           std::vector<std::string>& uris);

void sampling_init(h2load::Sampling& smp, size_t max_samples);

void writecb(struct ev_loop* loop, ev_io* w, int revents);

void readcb(struct ev_loop* loop, ev_io* w, int revents);

// Called every rate_period when rate mode is being used
void rate_period_timeout_w_cb(struct ev_loop* loop, ev_timer* w, int revents);

// Called when the duration for infinite number of requests are over
void duration_timeout_cb(struct ev_loop* loop, ev_timer* w, int revents);

// Called when the warmup duration for infinite number of requests are over
void warmup_timeout_cb(struct ev_loop* loop, ev_timer* w, int revents);

void rps_cb(struct ev_loop* loop, ev_timer* w, int revents);

void ping_w_cb(struct ev_loop* loop, ev_timer* w, int revents);

void stream_timeout_cb(struct ev_loop* loop, ev_timer* w, int revents);

// Called when an a connection has been inactive for a set period of time
// or a fixed amount of time after all requests have been made on a
// connection
void conn_activity_timeout_cb(EV_P_ ev_timer* w, int revents);

bool check_stop_client_request_timeout(h2load::Client* client, ev_timer* w);

void client_request_timeout_cb(struct ev_loop* loop, ev_timer* w, int revents);

bool recorded(const std::chrono::steady_clock::time_point& t);

std::string get_reqline(const char* uri, const http_parser_url& u);

void print_server_tmp_key(SSL* ssl);

int get_ev_loop_flags();



// Returns percentage of number of samples within mean +/- sd.
double within_sd(const std::vector<double>& samples, double mean, double sd);

// Computes statistics using |samples|. The min, max, mean, sd, and
// percentage of number of samples within mean +/- sd are computed.
// If |sampling| is true, this computes sample variance.  Otherwise,
// population variance.
h2load::SDStat compute_time_stat(const std::vector<double>& samples,
                                 bool sampling = false);

bool parse_base_uri(const StringRef& base_uri, h2load::Config& config);

// Use std::vector<std::string>::iterator explicitly, without that,
// http_parser_url u{} fails with clang-3.4.
std::vector<std::string> parse_uris(std::vector<std::string>::iterator first,
                                    std::vector<std::string>::iterator last, h2load::Config& config);

std::vector<std::string> read_uri_from_file(std::istream& infile);


h2load::SDStats
process_time_stats(const std::vector<std::unique_ptr<h2load::Worker>>& workers);

void resolve_host(h2load::Config& config);

#ifndef OPENSSL_NO_NEXTPROTONEG
int client_select_next_proto_cb(SSL* ssl, unsigned char** out,
                                unsigned char* outlen, const unsigned char* in,
                                unsigned int inlen, void* arg);
#endif // !OPENSSL_NO_NEXTPROTONEG


void populate_config_from_json(h2load::Config& config);

void insert_customized_headers_to_Json_scenarios(h2load::Config& config);

void tokenize_path_and_payload_for_fast_var_replace(h2load::Config& config);

std::vector<std::string> tokenize_string(const std::string& source, const std::string& delimeter);

std::string reassemble_str_with_variable(h2load::Config* config,
                                                    size_t scenario_index,
                                                    size_t request_index,
                                                    const std::vector<std::string>& tokenized_source,
                                                    uint64_t variable_value);

std::vector<h2load::Cookie> parse_cookie_string(const std::string& cookie_string, const std::string& origin_authority, const std::string& origin_schema);

void client_connection_timeout_cb(struct ev_loop* loop, ev_timer* w, int revents);

void ares_addrinfo_query_callback(void* arg, int status, int timeouts, struct ares_addrinfo* res);

void ares_socket_state_cb(void *data, int s, int read, int write);

void ares_io_cb(struct ev_loop *loop, struct ev_io *watcher, int revents);

void delayed_request_cb(struct ev_loop* loop, ev_timer* w, int revents);

void normalize_request_templates(h2load::Config* config);

std::string get_tls_error_string();

void reconnect_to_used_host_cb(struct ev_loop* loop, ev_timer* w, int revents);

void ares_addrinfo_query_callback_for_probe(void* arg, int status, int timeouts, struct ares_addrinfo* res);

void connect_to_prefered_host_cb(struct ev_loop* loop, ev_timer* w, int revents);

void probe_writecb(struct ev_loop* loop, ev_io* w, int revents);

void printBacktrace();

uint64_t find_common_multiple(std::vector<size_t> input);

std::vector<std::vector<h2load::SDStat>>
produce_requests_latency_stats(const std::vector<std::unique_ptr<h2load::Worker>>& workers);

void output_realtime_stats(h2load::Config& config, std::vector<std::unique_ptr<h2load::Worker>>& workers, std::atomic<bool>& workers_stopped, std::stringstream& DatStream);

template<typename T>
std::string to_string_with_precision_3(const T a_value);

size_t get_request_name_max_width (h2load::Config& config);

void post_process_json_config_schema(h2load::Config& config);

std::vector<std::vector<std::string>> read_csv_file(const std::string& csv_file_name);


#endif
