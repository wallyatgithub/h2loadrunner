#ifndef H2LOAD_UTILS_H
#define H2LOAD_UTILS_H
#include <iostream>

#include "h2load.h"
#include "h2load_http1_session.h"
#include "h2load_http2_session.h"


std::unique_ptr<h2load::Worker> create_worker(uint32_t id, SSL_CTX *ssl_ctx,
                                      size_t nreqs, size_t nclients,
                                      size_t rate, size_t max_samples, h2load::Config& config);

int parse_header_table_size(uint32_t &dst, const char *opt,
                            const char *optarg);

void read_script_from_file(std::istream &infile,
                           std::vector<ev_tstamp> &timings,
                           std::vector<std::string> &uris);

void sampling_init(h2load::Sampling &smp, size_t max_samples);

void writecb(struct ev_loop *loop, ev_io *w, int revents);

void readcb(struct ev_loop *loop, ev_io *w, int revents);

// Called every rate_period when rate mode is being used
void rate_period_timeout_w_cb(struct ev_loop *loop, ev_timer *w, int revents);

// Called when the duration for infinite number of requests are over
void duration_timeout_cb(struct ev_loop *loop, ev_timer *w, int revents);

// Called when the warmup duration for infinite number of requests are over
void warmup_timeout_cb(struct ev_loop *loop, ev_timer *w, int revents);

void rps_cb(struct ev_loop *loop, ev_timer *w, int revents);

void stream_timeout_cb(struct ev_loop *loop, ev_timer *w, int revents);

// Called when an a connection has been inactive for a set period of time
// or a fixed amount of time after all requests have been made on a
// connection
void conn_timeout_cb(EV_P_ ev_timer *w, int revents);

bool check_stop_client_request_timeout(h2load::Client *client, ev_timer *w);

void client_request_timeout_cb(struct ev_loop *loop, ev_timer *w, int revents);

bool recorded(const std::chrono::steady_clock::time_point &t);

std::string get_reqline(const char *uri, const http_parser_url &u);

void print_server_tmp_key(SSL *ssl);

int get_ev_loop_flags();



// Returns percentage of number of samples within mean +/- sd.
double within_sd(const std::vector<double> &samples, double mean, double sd);

// Computes statistics using |samples|. The min, max, mean, sd, and
// percentage of number of samples within mean +/- sd are computed.
// If |sampling| is true, this computes sample variance.  Otherwise,
// population variance.
h2load::SDStat compute_time_stat(const std::vector<double> &samples,
                         bool sampling = false);

bool parse_base_uri(const StringRef &base_uri, h2load::Config& config);

// Use std::vector<std::string>::iterator explicitly, without that,
// http_parser_url u{} fails with clang-3.4.
std::vector<std::string> parse_uris(std::vector<std::string>::iterator first,
                                    std::vector<std::string>::iterator last, h2load::Config& config);

std::vector<std::string> read_uri_from_file(std::istream &infile);


h2load::SDStats
process_time_stats(const std::vector<std::unique_ptr<h2load::Worker>> &workers);

void resolve_host(h2load::Config& config);

#ifndef OPENSSL_NO_NEXTPROTONEG
int client_select_next_proto_cb(SSL *ssl, unsigned char **out,
                                unsigned char *outlen, const unsigned char *in,
                                unsigned int inlen, void *arg);
#endif // !OPENSSL_NO_NEXTPROTONEG

void populate_config_from_json(h2load::Config& config);

#endif