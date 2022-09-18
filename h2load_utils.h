#ifndef H2LOAD_UTILS_H
#define H2LOAD_UTILS_H
#include <iostream>
#include <atomic>
#include <set>
#include <thread>
#include <vector>
#include <sstream>
#include <stdlib.h>

#include <openssl/ssl.h>

#ifdef USE_LIBEV
extern "C" {
#include <ares.h>
}
#include <ev.h>
#include "memchunk.h"
#endif


#include "http2.h"
#include "template.h"

#include "h2load.h"
#include "h2load_Config.h"
#ifdef USE_LIBEV
#include "libev_worker.h"
#endif
#include "asio_worker.h"

#include "h2load_Cookie.h"
#include "h2load_stats.h"


#include "h2load_http1_session.h"
#include "h2load_http2_session.h"

namespace h2load
{
class libev_client;
}
std::shared_ptr<h2load::base_worker> create_worker(uint32_t id, SSL_CTX* ssl_ctx,
                                                   size_t nreqs, size_t nclients,
                                                   size_t rate, size_t max_samples, h2load::Config& config);

int parse_header_table_size(uint32_t& dst, const char* opt,
                            const char* optarg);

void read_script_from_file(std::istream& infile,
                           std::vector<double>& timings,
                           std::vector<std::string>& uris);

void sampling_init(h2load::Sampling& smp, size_t max_samples);

bool recorded(const std::chrono::steady_clock::time_point& t);

std::string get_reqline(const char* uri, const http_parser_url& u);

void print_server_tmp_key(SSL* ssl);

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
process_time_stats(const std::vector<std::shared_ptr<h2load::base_worker>>& workers);

void resolve_host(h2load::Config& config);

#ifndef OPENSSL_NO_NEXTPROTONEG
int client_select_next_proto_cb(SSL* ssl, unsigned char** out,
                                unsigned char* outlen, const unsigned char* in,
                                unsigned int inlen, void* arg);
#endif // !OPENSSL_NO_NEXTPROTONEG


void populate_config_from_json(h2load::Config& config);

void insert_customized_headers_to_Json_scenarios(h2load::Config& config);

std::vector<h2load::Cookie> parse_cookie_string(const std::string& cookie_string, const std::string& origin_authority,
                                                const std::string& origin_schema);
#ifdef USE_LIBEV

int get_ev_loop_flags();

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

void client_request_timeout_cb(struct ev_loop* loop, ev_timer* w, int revents);

void client_connection_timeout_cb(struct ev_loop* loop, ev_timer* w, int revents);

void ares_addrinfo_query_callback(void* arg, int status, int timeouts, struct ares_addrinfo* res);

void ares_socket_state_cb(void* data, int s, int read, int write);

void ares_io_cb(struct ev_loop* loop, struct ev_io* watcher, int revents);

void delayed_request_cb(struct ev_loop* loop, ev_timer* w, int revents);

void reconnect_to_used_host_cb(struct ev_loop* loop, ev_timer* w, int revents);

void ares_addrinfo_query_callback_for_probe(void* arg, int status, int timeouts, struct ares_addrinfo* res);

void connect_to_prefered_host_cb(struct ev_loop* loop, ev_timer* w, int revents);

void probe_writecb(struct ev_loop* loop, ev_io* w, int revents);

#endif

std::string get_tls_error_string();

void printBacktrace();

uint64_t find_common_multiple(std::vector<size_t> input);

std::vector<std::vector<h2load::SDStat>>
                                      produce_requests_latency_stats(const std::vector<std::shared_ptr<h2load::base_worker>>& workers);

void output_realtime_stats(h2load::Config& config, std::vector<std::shared_ptr<h2load::base_worker>>& workers,
                           std::atomic<bool>& workers_stopped, std::stringstream& DatStream);

template<typename T>
std::string to_string_with_precision_3(const T a_value);

size_t get_request_name_max_width(h2load::Config& config);

void post_process_json_config_schema(h2load::Config& config);

std::vector<std::vector<std::string>> read_csv_file(const std::string& csv_file_name);

void rpsUpdateFunc(std::atomic<bool>& workers_stopped, h2load::Config& config);

void integrated_http2_server(std::stringstream& DatStream, h2load::Config& config);

void print_extended_stats_summary(const h2load::Stats& stats, h2load::Config& config,
                                  const std::vector<std::shared_ptr<h2load::base_worker>>& workers);

void load_ca_cert(SSL_CTX* ctx, const std::string& pem_content);

void load_cert(SSL_CTX* ctx, const std::string& pem_content);

void load_private_key(SSL_CTX* ctx, const std::string& pem_content);

bool check_key_cert_consistency(SSL_CTX* ctx);

void set_cert_verification_mode(SSL_CTX* ctx, uint32_t certificate_verification_mode);

void setup_SSL_CTX(SSL_CTX* ssl_ctx, h2load::Config& config);

bool is_it_an_ipv6_address(const std::string& address);

bool is_null_destination(h2load::Config& config);

void process_delayed_scenario(h2load::Config& config);

void split_string(const std::string& source, String_With_Variables_In_Between& result,
                  const std::map<std::string, size_t>& var_id_map);

bool variable_present(const std::string& source, size_t start_offset, size_t& var_start, size_t& var_end);

void process_variables(h2load::Config& config);

void split_string_and_var(const std::string& source, String_With_Variables_In_Between& result,
                          const Variable_Manager& variable_manager);

void transform_old_style_variable(h2load::Config& config);

void load_generic_variables_from_csv_file(Scenario& scenario);

void load_file_content(std::string& source);

#endif
