#include <algorithm>
#include <numeric>
#include <cctype>
#include <mutex>
#ifndef _WINDOWS
#include <execinfo.h>
#endif
#include <iomanip>
#include <iostream>
#include <fstream>
#include <string>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <boost/algorithm/string.hpp>

#ifdef USE_LIBEV
extern "C" {
#include <ares.h>
}
#include "libev_client.h"
#endif

extern "C" {
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
}

#include <fstream>
#include <fcntl.h>
#include "template.h"
#include "util.h"
#include "config_schema.h"
#include "tls.h"

#include <nghttp2/asio_httpx_server.h>

#include "h2load_utils.h"
#include "base_client.h"
#include "asio_worker.h"
#ifdef ENABLE_HTTP3
#  ifdef HAVE_LIBNGTCP2_CRYPTO_OPENSSL
#    include <ngtcp2/ngtcp2_crypto_openssl.h>
#  endif // HAVE_LIBNGTCP2_CRYPTO_OPENSSL
#  ifdef HAVE_LIBNGTCP2_CRYPTO_BORINGSSL
#    include <ngtcp2/ngtcp2_crypto_boringssl.h>
#  endif // HAVE_LIBNGTCP2_CRYPTO_BORINGSSL
#endif   // ENABLE_HTTP3


using namespace h2load;


std::shared_ptr<h2load::base_worker> create_worker(uint32_t id, SSL_CTX* ssl_ctx,
                                                   size_t nreqs, size_t nclients,
                                                   size_t rate, size_t max_samples, h2load::Config& config)
{
    std::stringstream rate_report;
    if (config.is_rate_mode() && nclients > rate)
    {
        rate_report << "Up to " << rate << " client(s) will be created every "
                    << util::duration_str(config.rate_period) << " ";
    }

    if (config.is_timing_based_mode())
    {
        std::cerr << "spawning thread #" << id << ": " << nclients
                  << " total client(s). Timing-based test with "
                  << config.warm_up_time << "s of warm-up time and "
                  << config.duration << "s of main duration for measurements."
                  << std::endl;
    }
    else
    {
        std::cerr << "spawning thread #" << id << ": " << nclients
                  << " total client(s). " << rate_report.str() << nreqs
                  << " total requests" << std::endl;
    }
#ifndef USE_LIBEV
    if (config.is_rate_mode())
    {
        return std::make_shared<asio_worker>(id, nreqs, nclients, rate,
                                             max_samples, &config);
    }
    else
    {
        // Here rate is same as client because the rate_timeout callback
        // will be called only once
        return std::make_shared<asio_worker>(id, nreqs, nclients, nclients,
                                             max_samples, &config);
    }
#else
    if (config.is_rate_mode())
    {
        return std::make_shared<libev_worker>(id, ssl_ctx, nreqs, nclients, rate,
                                              max_samples, &config);
    }
    else
    {
        // Here rate is same as client because the rate_timeout callback
        // will be called only once
        return std::make_shared<libev_worker>(id, ssl_ctx, nreqs, nclients, nclients,
                                              max_samples, &config);
    }
#endif
}

int parse_header_table_size(uint32_t& dst, const char* opt,
                            const char* optarg)
{
    auto n = util::parse_uint_with_unit(optarg);
    if (n == -1)
    {
        std::cerr << "--" << opt << ": Bad option value: " << optarg << std::endl;
        return -1;
    }
    if (n > std::numeric_limits<uint32_t>::max())
    {
        std::cerr << "--" << opt
                  << ": Value too large.  It should be less than or equal to "
                  << std::numeric_limits<uint32_t>::max() << std::endl;
        return -1;
    }

    dst = (int)n;

    return 0;
}

void read_script_from_file(std::istream& infile,
                           std::vector<double>& timings,
                           std::vector<std::string>& uris)
{
    std::string script_line;
    int line_count = 0;
    while (std::getline(infile, script_line))
    {
        line_count++;
        if (script_line.empty())
        {
            std::cerr << "Empty line detected at line " << line_count
                      << ". Ignoring and continuing." << std::endl;
            continue;
        }

        std::size_t pos = script_line.find("\t");
        if (pos == std::string::npos)
        {
            std::cerr << "Invalid line format detected, no tab character at line "
                      << line_count << ". \n\t" << script_line << std::endl;
            exit(EXIT_FAILURE);
        }

        const char* start = script_line.c_str();
        char* end;
        auto v = std::strtod(start, &end);

        errno = 0;
        if (v < 0.0 || !std::isfinite(v) || end == start || errno != 0)
        {
            auto error = errno;
            std::cerr << "Time value error at line " << line_count << ". \n\t"
                      << "value = " << script_line.substr(0, pos) << std::endl;
            if (error != 0)
            {
                std::cerr << "\t" << strerror(error) << std::endl;
            }
            exit(EXIT_FAILURE);
        }

        timings.push_back(v / 1000.0);
        uris.push_back(script_line.substr(pos + 1, script_line.size()));
    }
}

void sampling_init(h2load::Sampling& smp, size_t max_samples)
{
    smp.n = 0;
    smp.max_samples = max_samples;
}

#ifdef USE_LIBEV

void writecb(struct ev_loop* loop, ev_io* w, int revents)
{
    auto client = static_cast<libev_client*>(w->data);
    client->restart_timeout_timer();
    auto rv = client->do_write();
    if (rv == libev_client::ERR_CONNECT_FAIL)
    {
        client->disconnect();
        if (client->reconnect_to_alt_addr())
        {
            return;
        }
        // Try next address
        client->current_addr = nullptr;
        rv = client->connect();
        if (rv != 0)
        {
            client->fail();
            client->worker->free_client(client);
            return;
        }
        return;
    }
    if (rv != 0)
    {
        client->fail();
        if (client->reconnect_to_alt_addr())
        {
            return;
        }
        client->worker->free_client(client);
    }
}

void readcb(struct ev_loop* loop, ev_io* w, int revents)
{
    auto client = static_cast<libev_client*>(w->data);
    client->restart_timeout_timer();
    if (client->do_read() != 0)
    {
        if (client->try_again_or_fail() == 0)
        {
            return;
        }
        if (client->reconnect_to_alt_addr())
        {
            return;
        }
        client->worker->free_client(client);
        return;
    }
    writecb(loop, &client->wev, revents);
    // client->disconnect() and client->fail() may be called
}

// Called every rate_period when rate mode is being used
void rate_period_timeout_w_cb(struct ev_loop* loop, ev_timer* w, int revents)
{
    auto worker = static_cast<libev_worker*>(w->data);
    worker->rate_period_timeout_handler();
}

// Called when the duration for infinite number of requests are over
void duration_timeout_cb(struct ev_loop* loop, ev_timer* w, int revents)
{
    auto worker = static_cast<libev_worker*>(w->data);

    worker->duration_timeout_handler();
    //ev_break (EV_A_ EVBREAK_ALL);
}

// Called when the warmup duration for infinite number of requests are over
void warmup_timeout_cb(struct ev_loop* loop, ev_timer* w, int revents)
{
    auto worker = static_cast<libev_worker*>(w->data);
    worker->warmup_timeout_handler();
}

void rps_cb(struct ev_loop* loop, ev_timer* w, int revents)
{
    auto client = static_cast<libev_client*>(w->data);
    client->on_rps_timer();
}

void stream_timeout_cb(struct ev_loop* loop, ev_timer* w, int revents)
{
    auto client = static_cast<libev_client*>(w->data);
    client->reset_timeout_requests();
}

void client_connection_timeout_cb(struct ev_loop* loop, ev_timer* w, int revents)
{
    auto client = static_cast<libev_client*>(w->data);
    client->call_connected_callbacks(false);
    client->connection_timeout_handler();
}

void delayed_request_cb(struct ev_loop* loop, ev_timer* w, int revents)
{
    auto client = static_cast<libev_client*>(w->data);
    client->resume_delayed_request_execution();
}

// Called when an a connection has been inactive for a set period of time
// or a fixed amount of time after all requests have been made on a
// connection
void conn_activity_timeout_cb(EV_P_ ev_timer* w, int revents)
{
    auto client = static_cast<libev_client*>(w->data);
    client->conn_activity_timeout_handler();
}

void client_request_timeout_cb(struct ev_loop* loop, ev_timer* w, int revents)
{
    auto client = static_cast<libev_client*>(w->data);
    client->timing_script_timeout_handler();
}

int get_ev_loop_flags()
{
    if (ev_supported_backends() & ~ev_recommended_backends() & EVBACKEND_KQUEUE)
    {
        return ev_recommended_backends() | EVBACKEND_KQUEUE;
    }

    return 0;
}

void ping_w_cb(struct ev_loop* loop, ev_timer* w, int revents)
{
    auto client = static_cast<libev_client*>(w->data);
    client->submit_ping();
}

void ares_addrinfo_query_callback(void* arg, int status, int timeouts, struct ares_addrinfo* res)
{
    libev_client* client = static_cast<libev_client*>(arg);

    if (status == ARES_SUCCESS)
    {
        if (client->ares_address)
        {
            ares_freeaddrinfo(client->ares_address);
        }
        client->next_addr = nullptr;
        client->current_addr = nullptr;
        client->ares_address = res;
        client->connect();
        ares_freeaddrinfo(client->ares_address);
        client->ares_address = nullptr;
    }
    else
    {
        client->fail();
    }
}

void ares_io_cb(struct ev_loop* loop, struct ev_io* watcher, int revents)
{
    libev_client* client = static_cast<libev_client*>(watcher->data);
    ares_process_fd(client->channel,
                    revents & EV_READ ? watcher->fd : ARES_SOCKET_BAD,
                    revents & EV_WRITE ? watcher->fd : ARES_SOCKET_BAD);
}

void reconnect_to_used_host_cb(struct ev_loop* loop, ev_timer* w, int revents)
{
    auto client = static_cast<libev_client*>(w->data);
    ev_timer_stop(loop, w);
    client->reconnect_to_used_host();
}

void ares_addrinfo_query_callback_for_probe(void* arg, int status, int timeouts, struct ares_addrinfo* res)
{
    libev_client* client = static_cast<libev_client*>(arg);
    if (status == ARES_SUCCESS)
    {
        client->probe_address(res);
        ares_freeaddrinfo(res);
    }
}

void connect_to_prefered_host_cb(struct ev_loop* loop, ev_timer* w, int revents)
{
    auto client = static_cast<libev_client*>(w->data);
    if (CLIENT_CONNECTED != client->state)
    {
        ev_timer_stop(loop, w); // reconnect will connect to preferred host first
    }
    else if (client->authority == client->preferred_authority && CLIENT_CONNECTED == client->state)
    {
        ev_timer_stop(loop, w); // already connected to preferred host
    }
    else // connected, but not to preferred host, so check if preferred host is up for connection
    {
        client->probe_and_connect_to(client->schema, client->preferred_authority);
    }
}

void probe_writecb(struct ev_loop* loop, ev_io* w, int revents)
{
    auto client = static_cast<libev_client*>(w->data);
    ev_io_stop(loop, w);
    if (util::check_socket_connected(client->probe_skt_fd))
    {
        client->on_prefered_host_up();
    }
}

void ares_socket_state_cb(void* data, int s, int read, int write)
{
    libev_client* client = static_cast<libev_client*>(data);
    auto worker = static_cast<libev_worker*>(client->worker);
    if (read != 0 || write != 0)
    {
        if (client->ares_io_watchers.find(s) == client->ares_io_watchers.end())
        {
            ev_io watcher;
            watcher.data = client;
            client->ares_io_watchers[s] = watcher;
            ev_init(&client->ares_io_watchers[s], ares_io_cb);
        }
        ev_io_set(&client->ares_io_watchers[s], s, (read ? EV_READ : 0) | (write ? EV_WRITE : 0));
        ev_io_start(worker->loop, &client->ares_io_watchers[s]);
    }
    else if (client->ares_io_watchers.find(s) != client->ares_io_watchers.end())
    {
        ev_io_stop(worker->loop, &client->ares_io_watchers[s]);
        client->ares_io_watchers.erase(s);
    }
}

#ifdef ENABLE_HTTP3
void quic_pkt_timeout_cb(struct ev_loop* loop, ev_timer* w, int revents)
{
    auto c = static_cast<libev_client*>(w->data);
    if (c->quic_pkt_timeout() != 0)
    {
        c->fail();
        c->worker->free_client(c);
        return;
    }
}

#endif

#endif

bool recorded(const std::chrono::steady_clock::time_point& t)
{
    return std::chrono::steady_clock::duration::zero() != t.time_since_epoch();
}

std::string get_reqline(const char* uri, const http_parser_url& u)
{
    std::string reqline;

    if (util::has_uri_field(u, UF_PATH))
    {
        reqline = util::get_uri_field(uri, u, UF_PATH).str();
    }
    else
    {
        reqline = "/";
    }

    if (util::has_uri_field(u, UF_QUERY))
    {
        reqline += '?';
        reqline += util::get_uri_field(uri, u, UF_QUERY);
    }

    return reqline;
}


void print_server_tmp_key(SSL* ssl)
{
    // libressl does not have SSL_get_server_tmp_key
#if OPENSSL_VERSION_NUMBER >= 0x10002000L && defined(SSL_get_server_tmp_key)
    EVP_PKEY* key;

    if (!SSL_get_server_tmp_key(ssl, &key))
    {
        return;
    }

    auto key_del = defer(EVP_PKEY_free, key);

    std::cerr << "Server Temp Key: ";

    auto pkey_id = EVP_PKEY_id(key);
    switch (pkey_id)
    {
        case EVP_PKEY_RSA:
            std::cerr << "RSA " << EVP_PKEY_bits(key) << " bits" << std::endl;
            break;
        case EVP_PKEY_DH:
            std::cerr << "DH " << EVP_PKEY_bits(key) << " bits" << std::endl;
            break;
        case EVP_PKEY_EC:
        {
            auto ec = EVP_PKEY_get1_EC_KEY(key);
            auto ec_del = defer(EC_KEY_free, ec);
            auto nid = EC_GROUP_get_curve_name(EC_KEY_get0_group(ec));
            auto cname = EC_curve_nid2nist(nid);
            if (!cname)
            {
                cname = OBJ_nid2sn(nid);
            }

            std::cerr << "ECDH " << cname << " " << EVP_PKEY_bits(key) << " bits"
                      << std::endl;
            break;
        }
        default:
            std::cerr << OBJ_nid2sn(pkey_id) << " " << EVP_PKEY_bits(key) << " bits"
                      << std::endl;
            break;
    }
#endif // OPENSSL_VERSION_NUMBER >= 0x10002000L
}



// Returns percentage of number of samples within mean +/- sd.
double within_sd(const std::vector<double>& samples, double mean, double sd)
{
    if (samples.size() == 0)
    {
        return 0.0;
    }
    auto lower = mean - sd;
    auto upper = mean + sd;
    auto m = std::count_if(
                 std::begin(samples), std::end(samples),
                 [&lower, &upper](double t)
    {
        return lower <= t && t <= upper;
    });
    return (m / static_cast<double>(samples.size())) * 100;
}

// Computes statistics using |samples|. The min, max, mean, sd, and
// percentage of number of samples within mean +/- sd are computed.
// If |sampling| is true, this computes sample variance.  Otherwise,
// population variance.
h2load::SDStat compute_time_stat(const std::vector<double>& samples,
                                 bool sampling)
{
    if (samples.empty())
    {
        return {0.0, 0.0, 0.0, 0.0, 0.0};
    }
    // standard deviation calculated using Rapid calculation method:
    // https://en.wikipedia.org/wiki/Standard_deviation#Rapid_calculation_methods
    double a = 0, q = 0;
    size_t n = 0;
    double sum = 0;
    auto res = SDStat {std::numeric_limits<double>::max(),
                       std::numeric_limits<double>::min()
                      };
    for (const auto& t : samples)
    {
        ++n;
        res.min = std::min(res.min, t);
        res.max = std::max(res.max, t);
        sum += t;

        auto na = a + (t - a) / n;
        q += (t - a) * (t - na);
        a = na;
    }

    assert(n > 0);
    res.mean = sum / n;
    res.sd = sqrt(q / (sampling && n > 1 ? n - 1 : n));
    res.within_sd = within_sd(samples, res.mean, res.sd);

    return res;
}

bool parse_base_uri(const StringRef& base_uri, h2load::Config& config)
{
    http_parser_url u {};
    if (http_parser_parse_url(base_uri.c_str(), base_uri.size(), 0, &u) != 0 ||
        !util::has_uri_field(u, UF_SCHEMA) || !util::has_uri_field(u, UF_HOST))
    {
        return false;
    }

    config.scheme = util::get_uri_field(base_uri.c_str(), u, UF_SCHEMA).str();
    config.host = util::get_uri_field(base_uri.c_str(), u, UF_HOST).str();
    config.default_port = util::get_default_port(base_uri.c_str(), u);
    if (util::has_uri_field(u, UF_PORT))
    {
        config.port = u.port;
    }
    else
    {
        config.port = config.default_port;
    }

    return true;
}

// Use std::vector<std::string>::iterator explicitly, without that,
// http_parser_url u{} fails with clang-3.4.
std::vector<std::string> parse_uris(std::vector<std::string>::iterator first,
                                    std::vector<std::string>::iterator last, h2load::Config& config)
{
    std::vector<std::string> reqlines;

    if (first == last)
    {
        std::cerr << "no URI available" << std::endl;
        exit(EXIT_FAILURE);
    }

    if (!config.has_base_uri())
    {

        if (!parse_base_uri(StringRef {*first}, config))
        {
            std::cerr << "invalid URI: " << *first << std::endl;
            exit(EXIT_FAILURE);
        }

        config.base_uri = *first;
    }

    for (; first != last; ++first)
    {
        http_parser_url u {};

        auto uri = (*first).c_str();

        if (http_parser_parse_url(uri, (*first).size(), 0, &u) != 0)
        {
            std::cerr << "invalid URI: " << uri << std::endl;
            exit(EXIT_FAILURE);
        }

        reqlines.push_back(get_reqline(uri, u));
    }

    return reqlines;
}

std::vector<std::string> read_uri_from_file(std::istream& infile)
{
    std::vector<std::string> uris;
    std::string line_uri;
    while (std::getline(infile, line_uri))
    {
        uris.push_back(line_uri);
    }

    return uris;
}

h2load::SDStats
process_time_stats(const std::vector<std::shared_ptr<h2load::base_worker>>& workers)
{
    auto request_times_sampling = false;
    auto client_times_sampling = false;
    size_t nrequest_times = 0;
    size_t nclient_times = 0;
    for (const auto& w : workers)
    {
        nrequest_times += w->stats.req_stats.size();
        request_times_sampling = w->request_times_smp.n > w->stats.req_stats.size();

        nclient_times += w->stats.client_stats.size();
        client_times_sampling = w->client_smp.n > w->stats.client_stats.size();
    }

    std::vector<double> request_times;
    request_times.reserve(nrequest_times);

    std::vector<double> connect_times, ttfb_times, rps_values;
    connect_times.reserve(nclient_times);
    ttfb_times.reserve(nclient_times);
    rps_values.reserve(nclient_times);

    for (const auto& w : workers)
    {
        for (const auto& req_stat : w->stats.req_stats)
        {
            if (!req_stat.completed)
            {
                continue;
            }
            request_times.push_back(
                std::chrono::duration_cast<std::chrono::duration<double>>(
                    req_stat.stream_close_time - req_stat.request_time)
                .count());
        }

        const auto& stat = w->stats;

        for (const auto& cstat : stat.client_stats)
        {
            if (recorded(cstat.client_start_time) &&
                recorded(cstat.client_end_time))
            {
                auto t = std::chrono::duration_cast<std::chrono::duration<double>>(
                             cstat.client_end_time - cstat.client_start_time)
                         .count();
                if (t > 1e-9)
                {
                    rps_values.push_back(cstat.req_success / t);
                }
            }

            // We will get connect event before FFTB.
            if (!recorded(cstat.connect_start_time) ||
                !recorded(cstat.connect_time))
            {
                continue;
            }

            connect_times.push_back(
                std::chrono::duration_cast<std::chrono::duration<double>>(
                    cstat.connect_time - cstat.connect_start_time)
                .count());

            if (!recorded(cstat.ttfb))
            {
                continue;
            }

            ttfb_times.push_back(
                std::chrono::duration_cast<std::chrono::duration<double>>(
                    cstat.ttfb - cstat.connect_start_time)
                .count());
        }
    }

    return {compute_time_stat(request_times, request_times_sampling),
            compute_time_stat(connect_times, client_times_sampling),
            compute_time_stat(ttfb_times, client_times_sampling),
            compute_time_stat(rps_values, client_times_sampling)
           };
}

bool is_null_destination(h2load::Config& config)
{
    return (!config.base_uri_unix && config.connect_to_host.empty() && config.host.empty());
}

void resolve_host(h2load::Config& config)
{
#ifndef _WINDOWS
    if (config.base_uri_unix)
    {
        auto res = std::make_unique<addrinfo>();
        res->ai_family = config.unix_addr.sun_family;
        res->ai_socktype = SOCK_STREAM;
        res->ai_addrlen = sizeof(config.unix_addr);
        res->ai_addr =
            static_cast<struct sockaddr*>(static_cast<void*>(&config.unix_addr));

        config.addrs = res.release();
        return;
    };
#endif
    int rv;
    addrinfo hints {}, *res;

    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = 0;
    hints.ai_flags = AI_ADDRCONFIG;

    const auto& resolve_host =
        config.connect_to_host.empty() ? config.host : config.connect_to_host;
    auto port =
        config.connect_to_port == 0 ? config.port : config.connect_to_port;

    rv =
        getaddrinfo(resolve_host.c_str(), util::utos(port).c_str(), &hints, &res);
    if (rv != 0)
    {
        std::cerr << "getaddrinfo() failed: " << gai_strerror(rv) << std::endl;
        exit(EXIT_FAILURE);
    }
    if (res == nullptr)
    {
        std::cerr << "No address returned" << std::endl;
        exit(EXIT_FAILURE);
    }
    config.addrs = res;
}

#ifndef OPENSSL_NO_NEXTPROTONEG
int client_select_next_proto_cb(SSL* ssl, unsigned char** out,
                                unsigned char* outlen, const unsigned char* in,
                                unsigned int inlen, void* arg)
{
    h2load::Config* config = static_cast<h2load::Config*>(arg);
    if (util::select_protocol(const_cast<const unsigned char**>(out), outlen, in,
                              inlen, config->npn_list))
    {
        return SSL_TLSEXT_ERR_OK;
    }

    // OpenSSL will terminate handshake with fatal alert if we return
    // NOACK.  So there is no way to fallback.
    return SSL_TLSEXT_ERR_NOACK;
}
#endif // !OPENSSL_NO_NEXTPROTONEG

void populate_config_from_json(h2load::Config& config)
{
    config.scheme = config.json_config_schema.schema;
    config.host = config.json_config_schema.host;
    config.port = config.json_config_schema.port;
    config.default_port = (config.scheme == "https" ? 443 : 80);
    config.ciphers = config.json_config_schema.ciphers;
    config.conn_active_timeout = config.json_config_schema.connection_active_timeout;
    config.conn_inactivity_timeout = config.json_config_schema.connection_inactivity_timeout;
    config.duration = config.json_config_schema.duration;
    config.encoder_header_table_size = config.json_config_schema.encoder_header_table_size;
    config.header_table_size = config.json_config_schema.header_table_size;
    config.max_concurrent_streams = config.json_config_schema.max_concurrent_streams;
    config.nclients = config.json_config_schema.clients;
    if (config.json_config_schema.no_tls_proto == "h2c")
    {
        config.no_tls_proto = h2load::Config::PROTO_HTTP2;
    }
    else
    {
        config.no_tls_proto = h2load::Config::PROTO_HTTP1_1;
    }
    config.npn_list = util::parse_config_str_list(StringRef {config.json_config_schema.npn_list.c_str()});
    config.nreqs = config.json_config_schema.nreqs;
    config.nthreads = config.json_config_schema.threads;
    config.rate = config.json_config_schema.rate;
    config.rate_period = config.json_config_schema.rate_period;
    config.rps = config.json_config_schema.request_per_second;
    config.rps_file = config.json_config_schema.rps_file;
    config.stream_timeout_in_ms = config.json_config_schema.stream_timeout_in_ms;
    config.window_bits = config.json_config_schema.window_bits;
    config.connection_window_bits = config.json_config_schema.connection_window_bits;
    config.warm_up_time = config.json_config_schema.warm_up_time;
    config.max_frame_size = config.json_config_schema.max_frame_size;
    config.tls13_ciphers = config.json_config_schema.tls13_ciphers;
    config.groups = config.json_config_schema.groups;
    config.no_udp_gso = config.json_config_schema.no_udp_gso;
    config.max_udp_payload_size = config.json_config_schema.max_udp_payload_size;
    config.ktls = config.json_config_schema.ktls;
    config.qlog_file_base = config.json_config_schema.qlog_file_base;
}

void insert_customized_headers_to_Json_scenarios(h2load::Config& config)
{
    if (config.json_config_schema.scenarios.size() && config.custom_headers.size())
    {
        for (auto& header : config.custom_headers)
        {
            //std::string header_name = header.name;
            //util::inp_strlower(header_name);
            for (auto& scenario : config.json_config_schema.scenarios)
            {
                for (auto& request : scenario.requests)
                {
                    request.headers_in_map[header.name] = header.value;
                    request.additonalHeaders.emplace_back(header.name + ":" + header.value);
                }
            }
        }
    }
}

std::string get_tls_error_string()
{
    unsigned long   error_code = 0;
    char            error_code_string[2048];
    const char*      file = 0, *data = 0;
    int             line = 0, flags = 0;
    std::string     error_string;
    //pthread_t       tid = pthread_self();

    while ((error_code = ERR_get_error_line_data(&file, &line, &data, &flags)) != 0)
    {
        ERR_error_string_n(error_code, error_code_string,
                           sizeof(error_code_string));
        std::stringstream strm;
        strm << error_code_string << ":" << file << ":" << line << ":additional info...\"" << ((
                                                                                                   flags & ERR_TXT_STRING) ? data : "") << "\"\n";
        error_string += strm.str();
    }
    return error_string;
}

void printBacktrace()
{
#ifndef _WINDOWS
    void* buffer[64];
    int num = backtrace((void**) &buffer, 64);
    char** addresses = backtrace_symbols(buffer, num);
    for (int i = 0 ; i < num ; ++i)
    {
        fprintf(stderr, "[%2d]: %s\n", i, addresses[i]);
    }
    free(addresses);
#endif
}

uint64_t find_common_multiple(std::vector<size_t> input)
{
    std::set<size_t> unique_values;

    for (auto val : input)
    {
        unique_values.insert(val);
    }

    std::set<size_t> final_set;

    for (auto iter = unique_values.rbegin(); iter != unique_values.rend(); iter++)
    {
        auto val = *iter;
        auto find_multiple = [val](size_t val_in_set)
        {
            if ((val_in_set / val >= 1) && (val_in_set % val == 0))
            {
                return true;
            }
            return false;
        };
        if (std::find_if(final_set.begin(), final_set.end(), find_multiple) == final_set.end())
        {
            final_set.insert(val);
        }
    }

    uint64_t retVal = 1;
    for (auto val : final_set)
    {
        retVal *= val;
    }
    return retVal;
}

template<typename T>
std::string to_string_with_precision_3(const T a_value)
{
    std::ostringstream out;
    out.precision(3);
    out << std::fixed << a_value;
    return out.str();
}

size_t get_request_name_max_width(h2load::Config& config)
{
    size_t width = 0;
    for (size_t scenario_index = 0; scenario_index < config.json_config_schema.scenarios.size(); scenario_index++)
    {
        std::string req_name = std::string(config.json_config_schema.scenarios[scenario_index].name).append("_").append(
                                   std::to_string(config.json_config_schema.scenarios[scenario_index].requests.size()));
        if (req_name.size() > width)
        {
            width = req_name.size();
        }
    }
    return width;
}

void output_realtime_stats(h2load::Config& config,
                           std::vector<std::shared_ptr<h2load::base_worker>>& workers,
                           std::atomic<bool>& workers_stopped, std::stringstream& dataStream)
{
    std::vector<std::vector<size_t>> scenario_req_sent_till_now;
    std::vector<std::vector<size_t>> scenario_req_done_till_now;
    std::vector<std::vector<size_t>> scenario_req_success_till_now;
    std::vector<std::vector<size_t>> scenario_2xx_till_now;
    std::vector<std::vector<size_t>> scenario_3xx_till_now;
    std::vector<std::vector<size_t>> scenario_4xx_till_now;
    std::vector<std::vector<size_t>> scenario_5xx_till_now;
    for (size_t scenario_index = 0; scenario_index < config.json_config_schema.scenarios.size(); scenario_index++)
    {
        std::vector<size_t> req_vec(config.json_config_schema.scenarios[scenario_index].requests.size(), 0);
        scenario_req_sent_till_now.push_back(req_vec);
        scenario_req_done_till_now.push_back(req_vec);
        scenario_req_success_till_now.push_back(req_vec);
        scenario_2xx_till_now.push_back(req_vec);
        scenario_3xx_till_now.push_back(req_vec);
        scenario_4xx_till_now.push_back(req_vec);
        scenario_5xx_till_now.push_back(req_vec);
    }

    auto period_start = std::chrono::steady_clock::now();
    while (!workers_stopped)
    {
        auto scenario_req_sent_till_last_interval = scenario_req_sent_till_now;
        auto scenario_req_done_till_last_interval = scenario_req_done_till_now;
        auto scenario_req_success_till_last_interval = scenario_req_success_till_now;
        auto scenario_2xx_till_last_interval = scenario_2xx_till_now;
        auto scenario_3xx_till_last_interval = scenario_3xx_till_now;
        auto scenario_4xx_till_last_interval = scenario_4xx_till_now;
        auto scenario_5xx_till_last_interval = scenario_5xx_till_now;

        std::this_thread::sleep_for(std::chrono::milliseconds(config.json_config_schema.statistics_interval * 1000));

        auto now = std::chrono::system_clock::now();
        auto now_c = std::chrono::system_clock::to_time_t(now);
        auto period_end = std::chrono::steady_clock::now();
        auto period_duration = std::chrono::duration_cast<std::chrono::milliseconds>(period_end - period_start).count();;
        period_start = period_end;

        std::stringstream outputStream;

        static uint64_t counter = 0;
        if (counter % 10 == 0)
        {
            outputStream <<
                         "time, request, sent/s, done/s, success/s, (done/s)/(sent/s), (success/s)/(done/s), delta_2xx, 3xx, 4xx, 5xx, latency-min(ms), max, mean, sd, +/-sd, total-sent, total-done, total-success, done/sent(total), success/done(total)";
            outputStream << std::endl;
        }
        counter++;

        static size_t rps_width = 0;
        static size_t total_req_width = 0;
        static size_t percentage_width = 8;
        static size_t latency_width = 5;
        static size_t request_name_width = get_request_name_max_width(config);

        auto latency_stats = produce_requests_latency_stats(workers);

        for (size_t scenario_index = 0; scenario_index < config.json_config_schema.scenarios.size(); scenario_index++)
        {
            for (size_t request_index = 0; request_index < config.json_config_schema.scenarios[scenario_index].requests.size();
                 request_index++)
            {
                scenario_req_sent_till_now[scenario_index][request_index] = 0;
                scenario_req_done_till_now[scenario_index][request_index] = 0;
                scenario_req_success_till_now[scenario_index][request_index] = 0;
                scenario_2xx_till_now[scenario_index][request_index] = 0;
                scenario_3xx_till_now[scenario_index][request_index] = 0;
                scenario_4xx_till_now[scenario_index][request_index] = 0;
                scenario_5xx_till_now[scenario_index][request_index] = 0;
                for (auto& w : workers)
                {
                    auto& s = *(w->scenario_stats[scenario_index][request_index]);
                    scenario_req_sent_till_now[scenario_index][request_index] += s.req_started;
                    scenario_req_done_till_now[scenario_index][request_index] += s.req_done;
                    scenario_req_success_till_now[scenario_index][request_index] += s.req_status_success;
                    scenario_2xx_till_now[scenario_index][request_index] += s.status[2];
                    scenario_3xx_till_now[scenario_index][request_index] += s.status[3];
                    scenario_4xx_till_now[scenario_index][request_index] += s.status[4];
                    scenario_5xx_till_now[scenario_index][request_index] += s.status[5];
                }
                size_t delta_RPS_sent = scenario_req_sent_till_now[scenario_index][request_index] -
                                        scenario_req_sent_till_last_interval[scenario_index][request_index];
                size_t delta_RPS_done = scenario_req_done_till_now[scenario_index][request_index] -
                                        scenario_req_done_till_last_interval[scenario_index][request_index];
                size_t delta_RPS_success = scenario_req_success_till_now[scenario_index][request_index] -
                                           scenario_req_success_till_last_interval[scenario_index][request_index];
                size_t request_delta_2xx = scenario_2xx_till_now[scenario_index][request_index] -
                                           scenario_2xx_till_last_interval[scenario_index][request_index];
                size_t request_delta_3xx = scenario_3xx_till_now[scenario_index][request_index] -
                                           scenario_3xx_till_last_interval[scenario_index][request_index];
                size_t request_delta_4xx = scenario_4xx_till_now[scenario_index][request_index] -
                                           scenario_4xx_till_last_interval[scenario_index][request_index];
                size_t request_delta_5xx = scenario_5xx_till_now[scenario_index][request_index] -
                                           scenario_5xx_till_last_interval[scenario_index][request_index];

                outputStream
                        << std::put_time(std::localtime(&now_c), "%F %T")
                        << ", " << std::left << std::setw(request_name_width) << std::string(
                            config.json_config_schema.scenarios[scenario_index].name).append("_").append(std::to_string(request_index))
                        << ", " << std::left << std::setw(rps_width) << round((double)(1000 * delta_RPS_sent) / period_duration)
                        << ", " << std::left << std::setw(rps_width) << round((double)(1000 * delta_RPS_done) / period_duration)
                        << ", " << std::left << std::setw(rps_width) << round((double)(1000 * delta_RPS_success) / period_duration)
                        << ", " << std::left << std::setw(percentage_width) << to_string_with_precision_3(delta_RPS_sent ? (((
                                                                                                                                 double)delta_RPS_done / delta_RPS_sent) * 100) : 0).append("%")
                        << ", " << std::left << std::setw(percentage_width) << to_string_with_precision_3(delta_RPS_done ? (((
                                                                                                                                 double)delta_RPS_success / delta_RPS_done) * 100) : 0).append("%")
                        << ", " << std::left << std::setw(total_req_width) << request_delta_2xx
                        << ", " << std::left << std::setw(total_req_width) << request_delta_3xx
                        << ", " << std::left << std::setw(total_req_width) << request_delta_4xx
                        << ", " << std::left << std::setw(total_req_width) << request_delta_5xx
                        << ", " << std::left << std::setw(latency_width) << util::format_duration_to_mili_second(
                            latency_stats[scenario_index][request_index].min)
                        << ", " << std::left << std::setw(latency_width) << util::format_duration_to_mili_second(
                            latency_stats[scenario_index][request_index].max)
                        << ", " << std::left << std::setw(latency_width) << util::format_duration_to_mili_second(
                            latency_stats[scenario_index][request_index].mean)
                        << ", " << std::left << std::setw(latency_width) << util::format_duration_to_mili_second(
                            latency_stats[scenario_index][request_index].sd)
                        << ", " << std::left << std::setw(percentage_width) << to_string_with_precision_3(
                            latency_stats[scenario_index][request_index].within_sd).append("%")
                        << ", " << std::left << std::setw(total_req_width) << scenario_req_sent_till_now[scenario_index][request_index]
                        << ", " << std::left << std::setw(total_req_width) << scenario_req_done_till_now[scenario_index][request_index]
                        << ", " << std::left << std::setw(total_req_width) << scenario_req_success_till_now[scenario_index][request_index]
                        << ", " << std::left << std::setw(percentage_width) << to_string_with_precision_3(
                            scenario_req_sent_till_now[scenario_index][request_index] ? (((double)
                                                                                          scenario_req_done_till_now[scenario_index][request_index] / scenario_req_sent_till_now[scenario_index][request_index]) *
                                                                                         100) : 0).append("%")
                        << ", " << std::left << std::setw(percentage_width) << to_string_with_precision_3(
                            scenario_req_done_till_now[scenario_index][request_index] ? (((double)
                                                                                          scenario_req_success_till_now[scenario_index][request_index] /
                                                                                          scenario_req_done_till_now[scenario_index][request_index]) * 100) : 0).append("%")
                        ;
                outputStream << std::endl;
            }
        }

        auto accumulate_2d_vector = [](std::vector<std::vector<size_t>>& two_d_vec)
        {
            size_t sum = 0;
            for (auto& one_d_vec : two_d_vec)
            {
                sum += std::accumulate(one_d_vec.begin(), one_d_vec.end(), 0);
            }
            return sum;
        };
        auto total_req_sent = accumulate_2d_vector(scenario_req_sent_till_now);
        auto total_req_done = accumulate_2d_vector(scenario_req_done_till_now);
        auto total_req_success = accumulate_2d_vector(scenario_req_success_till_now);
        auto total_2xx = accumulate_2d_vector(scenario_2xx_till_now);
        auto total_3xx = accumulate_2d_vector(scenario_3xx_till_now);
        auto total_4xx = accumulate_2d_vector(scenario_4xx_till_now);
        auto total_5xx = accumulate_2d_vector(scenario_5xx_till_now);

        auto total_req_sent_till_last_interval = accumulate_2d_vector(scenario_req_sent_till_last_interval);
        auto total_req_done_till_last_interval = accumulate_2d_vector(scenario_req_done_till_last_interval);
        auto total_req_success_till_last_interval = accumulate_2d_vector(scenario_req_success_till_last_interval);
        auto total_2xx_till_last_interval = accumulate_2d_vector(scenario_2xx_till_last_interval);
        auto total_3xx_till_last_interval = accumulate_2d_vector(scenario_3xx_till_last_interval);
        auto total_4xx_till_last_interval = accumulate_2d_vector(scenario_4xx_till_last_interval);
        auto total_5xx_till_last_interval = accumulate_2d_vector(scenario_5xx_till_last_interval);

        auto delta_RPS_sent = total_req_sent - total_req_sent_till_last_interval;
        auto delta_RPS_done = total_req_done - total_req_done_till_last_interval;
        auto delta_RPS_success = total_req_success - total_req_success_till_last_interval;
        auto delta_2xx = total_2xx - total_2xx_till_last_interval;
        auto delta_3xx = total_3xx - total_3xx_till_last_interval;
        auto delta_4xx = total_4xx - total_4xx_till_last_interval;
        auto delta_5xx = total_5xx - total_5xx_till_last_interval;

        outputStream
                << std::put_time(std::localtime(&now_c), "%F %T")
                << ", " << std::left << std::setw(request_name_width) << "All_Requests"
                << ", " << std::left << std::setw(rps_width) << round((double)(1000 * delta_RPS_sent) / period_duration)
                << ", " << std::left << std::setw(rps_width) << round((double)(1000 * delta_RPS_done) / period_duration)
                << ", " << std::left << std::setw(rps_width) << round((double)(1000 * delta_RPS_success) / period_duration)
                << ", " << std::left << std::setw(percentage_width) << to_string_with_precision_3(delta_RPS_sent ? (((
                                                                                                                         double)delta_RPS_done / delta_RPS_sent) * 100) : 0).append("%")
                << ", " << std::left << std::setw(percentage_width) << to_string_with_precision_3(delta_RPS_done ? (((
                                                                                                                         double)delta_RPS_success / delta_RPS_done) * 100) : 0).append("%")
                << ", " << std::left << std::setw(total_req_width) << delta_2xx
                << ", " << std::left << std::setw(total_req_width) << delta_3xx
                << ", " << std::left << std::setw(total_req_width) << delta_4xx
                << ", " << std::left << std::setw(total_req_width) << delta_5xx
                << ", " << std::left << std::setw(latency_width) << util::format_duration_to_mili_second(
                    latency_stats[config.json_config_schema.scenarios.size()][0].min)
                << ", " << std::left << std::setw(latency_width) << util::format_duration_to_mili_second(
                    latency_stats[config.json_config_schema.scenarios.size()][0].max)
                << ", " << std::left << std::setw(latency_width) << util::format_duration_to_mili_second(
                    latency_stats[config.json_config_schema.scenarios.size()][0].mean)
                << ", " << std::left << std::setw(latency_width) << util::format_duration_to_mili_second(
                    latency_stats[config.json_config_schema.scenarios.size()][0].sd)
                << ", " << std::left << std::setw(percentage_width) << to_string_with_precision_3(
                    latency_stats[config.json_config_schema.scenarios.size()][0].within_sd).append("%")
                << ", " << std::left << std::setw(total_req_width) << total_req_sent
                << ", " << std::left << std::setw(total_req_width) << total_req_done
                << ", " << std::left << std::setw(total_req_width) << total_req_success
                << ", " << std::left << std::setw(percentage_width) << to_string_with_precision_3(total_req_sent ? (((
                                                                                                                         double)total_req_done / total_req_sent) * 100) : 0).append("%")
                << ", " << std::left << std::setw(percentage_width) << to_string_with_precision_3(total_req_done ? (((
                                                                                                                         double)total_req_success / total_req_done) * 100) : 0).append("%")
                ;
        outputStream << std::endl;
        if (config.json_config_schema.statistics_file.size())
        {
            static std::ofstream log_file(config.json_config_schema.statistics_file);
            log_file << outputStream.str();
            log_file.flush();
        }
        else
        {
            std::cout << outputStream.str();
        }

        rps_width = std::to_string(delta_RPS_sent).size() > rps_width ? std::to_string(delta_RPS_sent).size() : rps_width;
        total_req_width = std::to_string(total_req_sent).size() > total_req_width ? std::to_string(
                              total_req_sent).size() : total_req_width;

        dataStream.str(outputStream.str());
    }
}


std::vector<std::vector<h2load::SDStat>>
                                      produce_requests_latency_stats(const std::vector<std::shared_ptr<h2load::base_worker>>& workers)
{
    auto request_times_sampling = false;
    size_t nrequest_times = 0;
    auto& config = workers[0]->config;
    for (const auto& w : workers)
    {
        nrequest_times += w->stats.req_stats.size();
        request_times_sampling = w->request_times_smp.n > w->stats.req_stats.size();
    }
    std::map<size_t, std::map<size_t, std::vector<double>>> request_times;

    for (const auto& w : workers)
    {
        for (const auto& req_stat : w->stats.req_stats)
        {
            if (!req_stat.completed)
            {
                continue;
            }
            request_times[req_stat.scenario_index][req_stat.request_index].push_back(
                std::chrono::duration_cast<std::chrono::duration<double>>(
                    req_stat.stream_close_time - req_stat.request_time)
                .count());
        }
    }
    std::vector<std::vector<h2load::SDStat>> stats;

    for (size_t scenario_index = 0; scenario_index < config->json_config_schema.scenarios.size(); scenario_index++)
    {
        std::vector<h2load::SDStat> requests_stats;
        for (size_t request_index = 0; request_index < config->json_config_schema.scenarios[scenario_index].requests.size();
             request_index++)
        {
            requests_stats.push_back(compute_time_stat(request_times[scenario_index][request_index], request_times_sampling));
        }
        stats.push_back(std::move(requests_stats));
    }

    std::vector<double> all_request_times;
    for (auto& items : request_times)
    {
        for (auto& item : items.second)
        {
            all_request_times.insert(all_request_times.end(),
                                     std::make_move_iterator(item.second.begin()),
                                     std::make_move_iterator(item.second.end()));
        }
    }
    std::vector<SDStat> requests_stats;
    requests_stats.push_back(compute_time_stat(all_request_times, request_times_sampling));
    stats.push_back(std::move(requests_stats));
    return stats;
}


void load_file_content(std::string& source)
{
    if (source.size())
    {
        std::ifstream f(source);
        if (f.good())
        {
            std::string dest((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            source = dest;
        }
    }
}

void load_generic_variables_from_csv_file(Scenario& scenario)
{
    const std::string utf8_bom = {char(0xEF), char(0xBB), char(0xBF)};
    std::ifstream infile(scenario.user_variables_input_file);
    if (!infile)
    {
        std::cerr << "cannot open file: " << scenario.user_variables_input_file << std::endl;
        return;
    }
    std::string line;
    std::getline(infile, line);
    std::vector<std::string> variable_names;
    if (line.find(utf8_bom) == 0)
    {
        line = line.substr(utf8_bom.size(), std::string::npos);
    }

    std::stringstream lineStream(line);
    std::string cell;
    while (std::getline(lineStream, cell, ','))
    {
        boost::trim(cell);
        variable_names.push_back(cell);
    }
    std::vector<std::vector<std::string>> variable_values;
    while (std::getline(infile, line))
    {
        if (line.size() && line[line.size() - 1] == '\r')
        {
            line = line.substr(0, line.size() - 1);
        }
        std::vector<std::string> row;
        std::stringstream lineStream(line);
        std::string cell;
        while (std::getline(lineStream, cell, ','))
        {
            row.push_back(cell);
        }
        if (row.size() < variable_names.size())
        {
            for (size_t i = 0; i < variable_names.size() - row.size(); i++)
            {
                row.push_back("");
            }
        }
        else
        {
            for (size_t i = 0; i < row.size() - variable_names.size(); i++)
            {
                row.pop_back();
            }
        }
        variable_values.push_back(std::move(row));
    }
    for (size_t index = 0; index < variable_names.size(); index++)
    {
        std::vector<std::vector<std::string>> one_column(1, std::vector<std::string>(0));
        for (auto& each_row : variable_values)
        {
            one_column[0].push_back(each_row[index]);
        }

        Generic_Variable generic_var(variable_names[index], one_column);
        scenario.variable_manager.insert(generic_var);
    }
}

void transform_old_style_variable(h2load::Config& config)
{
    auto transform_variable = [](std::string & target, const std::string & var_name)
    {
        auto tokenized_target = tokenize_string(target, var_name);
        target.clear();
        for (size_t i = 0; i < (tokenized_target.size() - 1); i++)
        {
            target.append(tokenized_target[i]);
            target.append("${").append(var_name).append("}");
        }
        target.append(tokenized_target.back());
    };

    for (auto& scenario : config.json_config_schema.scenarios)
    {
        if (scenario.variable_name_in_path_and_data.size())
        {
            for (auto& request : scenario.requests)
            {
                transform_variable(request.uri.input, scenario.variable_name_in_path_and_data);
                load_file_content(request.payload);
                transform_variable(request.payload, scenario.variable_name_in_path_and_data);
            }
            if (scenario.user_id_list_file.size())
            {
                Two_Dimensioning_Variable two_d_v(scenario.variable_name_in_path_and_data, scenario.user_id_list_file);
                scenario.two_dim_variables.push_back(two_d_v);
            }
            else
            {
                Range_Based_Variable rv(scenario.variable_name_in_path_and_data, scenario.variable_range_start,
                                        scenario.variable_range_end);
                scenario.range_based_variables.push_back(rv);
            }
            scenario.variable_name_in_path_and_data.clear();
            scenario.variable_range_start = 0;
            scenario.variable_range_end = 0;
            scenario.user_id_list_file.clear();
        }
    }
}

void split_string_and_var(const std::string& source, String_With_Variables_In_Between& result,
                          const Variable_Manager& variable_manager)
{
    size_t offset = 0;
    size_t var_start = 0;
    size_t var_end = 0;
    size_t var_unique_id;

    while (variable_present(source, offset, var_start, var_end))
    {
        auto segment = source.substr(offset, var_start - offset);
        auto var_name = source.substr(var_start + 2, (var_end - (var_start + 2)));
        result.string_segments.push_back(segment);
        if (variable_manager.get_gen_var_id(var_name, var_unique_id))
        {
            result.variable_ids_in_between.push_back(var_unique_id);
        }
        else
        {
            std::cerr << "variable ${" << var_name << "} appeared in" << std::endl << source << std::endl <<
                      "but it has no definition on how to get the value" << std::endl;
            exit(1);
        }
        offset = var_end + 1;
    }
    result.string_segments.push_back(source.substr(offset, std::string::npos));
}


void process_variables(h2load::Config& config)
{
    transform_old_style_variable(config);

    for (auto& scenario : config.json_config_schema.scenarios)
    {
        // TODO:  do not rely on insertion order
        for (auto& request : scenario.requests)
        {
            for (auto& response_value_regex_picker : request.response_value_regex_pickers)
            {
                size_t unique_id_of_variable;
                if (!scenario.variable_manager.get_gen_var_id(response_value_regex_picker.save_to_variable_name, unique_id_of_variable))
                {
                    Generic_Variable var(response_value_regex_picker.save_to_variable_name, 0, 0);
                    scenario.variable_manager.insert(var);
                    scenario.variable_manager.get_gen_var_id(response_value_regex_picker.save_to_variable_name, unique_id_of_variable);
                }
                request.actual_regex_value_pickers.emplace_back(Regex_Picker(response_value_regex_picker, unique_id_of_variable));
            }
        }

        // TODO: find some better design to avoid using this number_of_variables_from_value_pickers
        scenario.number_of_variables_from_value_pickers = scenario.variable_manager.get_number_of_generic_variables();

        for (auto& r : scenario.range_based_variables)
        {
            Generic_Variable generic_var(r.variable_name, r.variable_range_start, r.variable_range_end);
            scenario.variable_manager.insert(generic_var);
        }

        for (auto& dv : scenario.two_dim_variables)
        {
            auto raw_data = read_csv_file(dv.id_list_file_name);
            if (raw_data.empty())
            {
                std::cerr << "cannot read user IDs from: " << dv.id_list_file_name << std::endl;
                exit(EXIT_FAILURE);
            }
            size_t max_size = std::max_element(std::begin(raw_data),
                                               std::end(raw_data),
                                               [](const std::vector<std::string>& lhs,
                                                  const std::vector<std::string>& rhs)
            {
                return lhs.size() < rhs.size();
            })->size();
            std::cout << "max size:" << max_size << std::endl;
            std::vector<std::vector<std::string>> two_d_vals(max_size, std::vector<std::string>(0));
            for (size_t i = 0; i < raw_data.size(); i++)
            {
                for (size_t j = 0; j < raw_data[i].size(); j++)
                {
                    two_d_vals[j].emplace_back(raw_data[i][j]);
                }
            }

            Generic_Variable generic_var(dv.variable_name, two_d_vals);
            scenario.variable_manager.insert(generic_var);
        }

        if (scenario.user_variables_input_file.size())
        {
            load_generic_variables_from_csv_file(scenario);
        }

        for (auto& request : scenario.requests)
        {
            auto iter = uri_action_map.find(request.uri.typeOfAction);
            if (iter != uri_action_map.end())
            {
                request.uri.uri_action = iter->second;
            }
            else
            {
                std::cerr << request.uri.typeOfAction << " is invalid" << std::endl;
                exit(1);
            }

            size_t start;
            size_t end;
            std::string path;
            if (request.uri.uri_action == INPUT_URI && !variable_present(request.uri.input, 0, start, end))
            {
                http_parser_url u {};
                if (http_parser_parse_url(request.uri.input.c_str(), request.uri.input.size(), 0, &u) != 0)
                {
                    std::cerr << "invalid URI given: " << request.uri.input << std::endl;
                    std::cerr << "Please check this request: " << std::endl << staticjson::to_pretty_json_string(request) << std::endl;
                    exit(EXIT_FAILURE);
                }
                path = get_reqline(request.uri.input.c_str(), u);
                if (util::has_uri_field(u, UF_SCHEMA) && util::has_uri_field(u, UF_HOST))
                {
                    request.schema = util::get_uri_field(request.uri.input.c_str(), u, UF_SCHEMA).str();
                    util::inp_strlower(request.schema);
                    request.authority = util::get_uri_field(request.uri.input.c_str(), u, UF_HOST).str();
                    util::inp_strlower(request.authority);
                    if (util::has_uri_field(u, UF_PORT))
                    {
                        request.authority.append(":").append(util::utos(u.port));
                    }
                }
            }
            else if (variable_present(request.uri.input, 0, start, end))
            {
                path = request.uri.input;
                request.uri.uri_action = INPUT_WITH_VARIABLE;
            }

            split_string_and_var(path, request.tokenized_path_with_vars, scenario.variable_manager);

            split_string_and_var(request.payload, request.tokenized_payload_with_vars, scenario.variable_manager);

            for (auto& header_with_value : request.additonalHeaders)
            {
                size_t t = header_with_value.find(":", 1);
                if ((t == std::string::npos) ||
                    (header_with_value[0] == ':' && 1 == t))
                {
                    std::cerr << "invalid header, no name: " << header_with_value << std::endl;
                    continue;
                }
                std::string header_name = header_with_value.substr(0, t);
                std::string header_value = header_with_value.substr(t + 1);
                /*
                header_value.erase(header_value.begin(), std::find_if(header_value.begin(), header_value.end(),
                                                                      [](unsigned char ch)
                {
                    return !std::isspace(ch);
                }));
                */

                if (header_value.empty())
                {
                    std::cerr << "invalid header - no value: " << header_with_value
                              << std::endl;
                    continue;
                }
                size_t start, end;
                if (variable_present(header_name, 0, start, end) || variable_present(header_value, 0, start, end))
                {
                    String_With_Variables_In_Between name_result;
                    split_string_and_var(header_name, name_result, scenario.variable_manager);
                    String_With_Variables_In_Between value_result;
                    split_string_and_var(header_value, value_result, scenario.variable_manager);
                    request.headers_with_variable.emplace_back(std::make_pair(name_result, value_result));
                    continue;
                }
                request.headers_in_map[header_name] = header_value;
            }
        }
    }
}

void post_process_json_config_schema(h2load::Config& config)
{
    if (config.json_config_schema.host.size() && config.json_config_schema.host[0] == '['
        && config.json_config_schema.host[config.json_config_schema.host.size() - 1] == ']')
    {
        config.json_config_schema.host = config.json_config_schema.host.substr(1, config.json_config_schema.host.size() - 2);
    }

    for (auto& host_item : config.json_config_schema.load_share_hosts)
    {
        auto& host = host_item.host;
        if (host.size() && host[0] == '[' && host[host.size() - 1] == ']')
        {
            host = host.substr(1, host.size() - 2);
        }
        util::inp_strlower(host);
    }

    util::inp_strlower(config.json_config_schema.host);
    util::inp_strlower(config.json_config_schema.schema);

    process_variables(config);

    for (auto& scenario : config.json_config_schema.scenarios)
    {
        for (auto i = 0; i < scenario.requests.size(); i++)
        {
            auto& request = scenario.requests[i];

            for (auto& schema_header_match : request.response_match.header_match)
            {
                request.response_match_rules.emplace_back(Match_Rule(schema_header_match));
            }

            for (auto& schema_payload_match : request.response_match.payload_match)
            {
                request.response_match_rules.emplace_back(Match_Rule(schema_payload_match));
            }

            load_file_content(request.luaScript);

            if (request.luaScript.size())
            {
                lua_State* L = luaL_newstate();
                luaL_openlibs(L);
                luaL_dostring(L, request.luaScript.c_str());
                lua_getglobal(L, make_request);
                if (lua_isfunction(L, -1))
                {
                    request.make_request_function_present = true;
                }
                else
                {
                    std::cerr << scenario.name<<"_"<<i<<": lua script provided, but function " << make_request <<" is either malformed, or not present"<<std::endl;
                }
                lua_settop(L, 0);
                lua_getglobal(L, validate_response);
                if (lua_isfunction(L, -1))
                {
                    request.validate_response_function_present = true;
                }
                else
                {
                    std::cerr << scenario.name<<"_"<<i<<": lua script provided, but function " << validate_response <<" is either malformed, or not present"<<std::endl;
                }
                lua_settop(L, 0);
                lua_close(L);
            }
        }
    }
    load_file_content(config.json_config_schema.ca_cert);
    load_file_content(config.json_config_schema.client_cert);
    load_file_content(config.json_config_schema.private_key);
    static std::map<std::string, uint32_t> cc_algo = {{"RENO", 0}, {"CUBIC", 1}, {"BBR", 2}, {"BBR2", 3}};
    config.cc_algo = cc_algo[config.json_config_schema.quic_congestion_control_algorithm];
}

std::vector<std::vector<std::string>> read_csv_file(const std::string& csv_file_name)
{
    std::vector<std::vector<std::string>> result;
    std::ifstream infile(csv_file_name);
    if (!infile)
    {
        std::cerr << "cannot open file: " << csv_file_name << std::endl;
        return result;
    }
    std::string line;
    std::getline(infile, line); // remove first row which is column name;
    while (std::getline(infile, line))
    {
        if (line.size() && line[line.size() - 1] == '\r')
        {
            line = line.substr(0, line.size() - 1);
        }
        std::vector<std::string> row;
        std::stringstream lineStream(line);
        std::string cell;
        while (std::getline(lineStream, cell, ','))
        {
            row.push_back(cell);
        }
        result.push_back(row);
    }

    return result;
}

void rpsUpdateFunc(std::atomic<bool>& workers_stopped, h2load::Config& config)
{
    while (!config.rps_file.empty() && !workers_stopped)
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        std::ifstream file;
        file.open(config.rps_file);
        std::string line;
        if (file)
        {
            std::getline(file, line);
            file.close();
            char* end;
            auto v = std::strtod(line.c_str(), &end);
            if (end == line.c_str() || *end != '\0' || !std::isfinite(v) ||
                1. / v < 1e-6)
            {
                std::cerr << "--rps: Invalid value, skip: " << line << std::endl;
            }
            else if (v != config.rps)
            {
                config.rps = v;
            }
        }
    }
};

void integrated_http2_server(std::stringstream& dataStream, h2load::Config& config)
{
    uint32_t serverPort = config.json_config_schema.builtin_server_port;
    std::cerr << "builtin server listening at port: " << serverPort << std::endl;
    H2Server_Config_Schema config_schema;
    config_schema.no_tls_proto = "http/1.1";
    config_schema.config_post_process();

    nghttp2::asio_http2::server::asio_httpx_server server(config_schema);
    boost::system::error_code ec;
    server.num_threads(1);
    server.handle("/stat", [&](const nghttp2::asio_http2::server::asio_server_request & req,
                               nghttp2::asio_http2::server::asio_server_response & res,
                               uint64_t handler_id, int32_t stream_id)
    {
        nghttp2::asio_http2::header_map headers;
        nghttp2::asio_http2::header_value hdr_val;
        hdr_val.sensitive = false;
        std::string payload = dataStream.str();
        hdr_val.value = std::to_string(payload.size());
        headers.insert(std::make_pair("Content-Length", hdr_val));
        res.write_head(200, std::move(headers));
        res.end(payload);
    });
    server.handle("/config", [&](const nghttp2::asio_http2::server::asio_server_request & req,
                                 nghttp2::asio_http2::server::asio_server_response & res,
                                 uint64_t handler_id, int32_t stream_id)
    {
        std::string raw_query = req.uri().raw_query;
        std::string replyMsg = "rps updated to ";

        std::vector<std::string> tokens = tokenize_string(raw_query, "&");
        auto rps_it = std::find_if(tokens.begin(), tokens.end(), [](std::string e)
        {
            return (e.find("rps") != std::string::npos);
        });
        if (rps_it != tokens.end())
        {
            std::vector<std::string> rps_token = tokenize_string(*rps_it, "=");
            if (rps_token.size() == 2)
            {
                std::string rps = rps_token[1];
                char* end;
                auto v = std::strtod(rps.c_str(), &end);
                if (end == rps.c_str() || *end != '\0' || !std::isfinite(v) ||
                    1. / v < 1e-6)
                {
                    replyMsg = "Invalid rps given";
                }
                else if (v != config.rps)
                {
                    config.rps = v;
                    replyMsg.append(std::to_string(config.rps));
                }
            }
        }

        nghttp2::asio_http2::header_map headers;
        res.write_head(200, std::move(headers));
        res.end(std::move(replyMsg));
    });
    if (server.listen_and_serve(ec, std::string("0.0.0.0"), std::to_string(serverPort)))
    {
        std::cerr << "http2 server start error: " << ec.message() << std::endl;
    }
};

void print_extended_stats_summary(const h2load::Stats& stats, h2load::Config& config,
                                  const std::vector<std::shared_ptr<h2load::base_worker>>& workers)
{
    if (config.json_config_schema.scenarios.size())
    {
        std::stringstream colStream;
        colStream <<
                  "request, traffic-percentage, total-req-sent, total-req-done, total-req-success, total-2xx-resp, 3xx, 4xx, 5xx, latency-min(ms), max, mean, sd, +/-sd";
        std::cerr << colStream.str() << std::endl;
        auto latency_stats = produce_requests_latency_stats(workers);
        size_t request_name_width = get_request_name_max_width(config);
        static size_t percentage_width = 8;
        static size_t latency_width = 5;

        for (size_t scenario_index = 0; scenario_index < config.json_config_schema.scenarios.size(); scenario_index++)
        {
            for (size_t request_index = 0; request_index < config.json_config_schema.scenarios[scenario_index].requests.size();
                 request_index++)
            {
                size_t req_sent = 0;
                size_t req_done = 0;
                size_t req_success = 0;
                size_t resp_2xx = 0;
                size_t resp_3xx = 0;
                size_t resp_4xx = 0;
                size_t resp_5xx = 0;
                for (auto& w : workers)
                {
                    auto& s = *(w->scenario_stats[scenario_index][request_index]);
                    req_sent += s.req_started;
                    req_done += s.req_done;
                    req_success += s.req_status_success;
                    resp_2xx += s.status[2];
                    resp_3xx += s.status[3];
                    resp_4xx += s.status[4];
                    resp_5xx += s.status[5];
                }

                std::stringstream dataStream;
                dataStream << std::left << std::setw(request_name_width) << std::string(
                               config.json_config_schema.scenarios[scenario_index].name).append("_").append(std::to_string(request_index))
                           << ", " << std::left << std::setw(percentage_width) << to_string_with_precision_3(stats.req_done ? (double)(
                                                                                                                 req_done * 100) / stats.req_done : 0).append("%")
                           << ", " << req_sent
                           << ", " << req_done
                           << ", " << req_success
                           << ", " << resp_2xx
                           << ", " << resp_3xx
                           << ", " << resp_4xx
                           << ", " << resp_5xx
                           << ", " << std::left << std::setw(latency_width) << util::format_duration_to_mili_second(
                               latency_stats[scenario_index][request_index].min)
                           << ", " << std::left << std::setw(latency_width) << util::format_duration_to_mili_second(
                               latency_stats[scenario_index][request_index].max)
                           << ", " << std::left << std::setw(latency_width) << util::format_duration_to_mili_second(
                               latency_stats[scenario_index][request_index].mean)
                           << ", " << std::left << std::setw(latency_width) << util::format_duration_to_mili_second(
                               latency_stats[scenario_index][request_index].sd)
                           << ", " << std::left << std::setw(latency_width) << to_string_with_precision_3(
                               latency_stats[scenario_index][request_index].within_sd).append("%");
                ;
                std::cerr << dataStream.str() << std::endl;
            }
        }
    }
}

void load_ca_cert(SSL_CTX* ctx, const std::string& pem_content)
{
    std::stringstream strm;
    strm << "/tmp/cacert" << ::getpid() << ".pem";
    std::string fileName = strm.str();
    std::ofstream tmpFile;
    tmpFile.open(fileName);
    tmpFile << pem_content << std::flush;
    tmpFile.close();
    if (!SSL_CTX_load_verify_locations(ctx, fileName.c_str(), NULL))
    {
        std::cerr << "SSL_CTX_load_verify_locations failed: " << get_tls_error_string() << std::endl;
    }
    std::remove(fileName.c_str());
}

void load_cert(SSL_CTX* ctx, const std::string& pem_content)
{
    std::stringstream strm;
    strm << "/tmp/cert" << ::getpid() << ".pem";
    std::string fileName = strm.str();
    std::ofstream tmpFile;
    tmpFile.open(fileName);
    tmpFile << pem_content << std::flush;
    tmpFile.close();
    if (!SSL_CTX_use_certificate_chain_file(ctx, fileName.c_str()))
    {
        std::cerr << "SSL_CTX_use_certificate_chain_file failed" << get_tls_error_string() << std::endl;
    }
    std::remove(fileName.c_str());
}

void load_private_key(SSL_CTX* ctx, const std::string& pem_content)
{
    std::stringstream strm;
    strm << "/tmp/priKey" << ::getpid() << ".pem";
    std::string fileName = strm.str();
    std::ofstream tmpFile;
    tmpFile.open(fileName);
    tmpFile << pem_content << std::flush;
    tmpFile.close();
    if (!SSL_CTX_use_PrivateKey_file(ctx, fileName.c_str(), SSL_FILETYPE_PEM))
    {
        std::cerr << "SSL_CTX_use_PrivateKey_file failed" << get_tls_error_string() << std::endl;
    }
    std::remove(fileName.c_str());
}

bool check_key_cert_consistency(SSL_CTX* ctx)
{
    if (SSL_CTX_check_private_key(ctx) != 1)
    {
        std::cerr << "SSL_CTX_check_private_key failed" << get_tls_error_string() << std::endl;
        return false;
    }
    return true;
}

void set_cert_verification_mode(SSL_CTX* ctx, uint32_t certificate_verification_mode)
{
    int mode = SSL_VERIFY_NONE;
    switch (certificate_verification_mode)
    {
        case 0:
        {
            mode = SSL_VERIFY_NONE;
            break;
        }
        case 1:
        {
            mode = SSL_VERIFY_PEER;
            break;
        }
        default:
        {
            mode = SSL_VERIFY_NONE;
        }
    }
    SSL_CTX_set_verify(ctx, mode, NULL);
}

void SSL_CTX_keylog_cb_func_cb(const SSL* ssl, const char* line)
{
    void* p = SSL_get_ex_data(ssl, SSL_EXT_DATA_INDEX_KEYLOG_FILE);
    if (p)
    {
        std::ofstream log_file((char*)p, std::ios_base::app);
        log_file << line << std::endl;
    }
}

void setup_SSL_CTX(SSL_CTX* ssl_ctx, Config& config)
{
    auto ssl_opts = (SSL_OP_ALL & ~SSL_OP_DONT_INSERT_EMPTY_FRAGMENTS) |
                    SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 | SSL_OP_NO_COMPRESSION |
                    SSL_OP_NO_SESSION_RESUMPTION_ON_RENEGOTIATION;

    SSL_CTX_set_options(ssl_ctx, ssl_opts);
    SSL_CTX_set_mode(ssl_ctx, SSL_MODE_AUTO_RETRY);
    SSL_CTX_set_mode(ssl_ctx, SSL_MODE_RELEASE_BUFFERS);
    if (config.json_config_schema.tls_keylog_file.size())
    {
        SSL_CTX_set_keylog_callback(ssl_ctx, SSL_CTX_keylog_cb_func_cb);
    }

    if (config.json_config_schema.client_cert.size() && config.json_config_schema.private_key.size())
    {
        load_cert(ssl_ctx, config.json_config_schema.client_cert);
        load_private_key(ssl_ctx, config.json_config_schema.private_key);
        check_key_cert_consistency(ssl_ctx);
    }
    if (config.json_config_schema.ca_cert.size())
    {
        load_ca_cert(ssl_ctx, config.json_config_schema.ca_cert);
    }

    set_cert_verification_mode(ssl_ctx, config.json_config_schema.cert_verification_mode);

    auto max_tls_version = nghttp2::tls::NGHTTP2_TLS_MAX_VERSION;
    if (config.json_config_schema.max_tls_version == "TLSv1.2")
    {
        max_tls_version = TLS1_2_VERSION;
    }

    if (config.is_quic())
    {
#ifdef ENABLE_HTTP3
#  ifdef HAVE_LIBNGTCP2_CRYPTO_OPENSSL
        if (ngtcp2_crypto_openssl_configure_client_context(ssl_ctx) != 0)
        {
            std::cerr << "ngtcp2_crypto_openssl_configure_client_context failed"
                      << std::endl;
            exit(EXIT_FAILURE);
        }
#  endif // HAVE_LIBNGTCP2_CRYPTO_OPENSSL
#  ifdef HAVE_LIBNGTCP2_CRYPTO_BORINGSSL
        if (ngtcp2_crypto_boringssl_configure_client_context(ssl_ctx) != 0)
        {
            std::cerr << "ngtcp2_crypto_boringssl_configure_client_context failed"
                      << std::endl;
            exit(EXIT_FAILURE);
        }
#  endif // HAVE_LIBNGTCP2_CRYPTO_BORINGSSL
#endif   // ENABLE_HTTP3
    }
    else if (nghttp2::tls::ssl_ctx_set_proto_versions(
                 ssl_ctx, nghttp2::tls::NGHTTP2_TLS_MIN_VERSION,
                 max_tls_version) != 0)
    {
        std::cerr << "Could not set TLS versions" << std::endl;
        exit(EXIT_FAILURE);
    }

    if (SSL_CTX_set_cipher_list(ssl_ctx, config.ciphers.c_str()) == 0)
    {
        std::cerr << "SSL_CTX_set_cipher_list with " << config.ciphers
                  << " failed: " << ERR_error_string(ERR_get_error(), nullptr)
                  << std::endl;
        exit(EXIT_FAILURE);
    }

#if OPENSSL_1_1_1_API && !defined(OPENSSL_IS_BORINGSSL)
    if (SSL_CTX_set_ciphersuites(ssl_ctx, config.tls13_ciphers.c_str()) == 0)
    {
        std::cerr << "SSL_CTX_set_ciphersuites with " << config.tls13_ciphers
                  << " failed: " << ERR_error_string(ERR_get_error(), nullptr)
                  << std::endl;
        exit(EXIT_FAILURE);
    }
#endif // OPENSSL_1_1_1_API && !defined(OPENSSL_IS_BORINGSSL)

#if OPENSSL_1_1_1_API && !defined(OPENSSL_IS_BORINGSSL)
    if (SSL_CTX_set1_groups_list(ssl_ctx, config.groups.c_str()) != 1)
    {
        std::cerr << "SSL_CTX_set1_groups_list failed" << std::endl;
        exit(EXIT_FAILURE);
    }
#else  // !(OPENSSL_1_1_1_API && !defined(OPENSSL_IS_BORINGSSL))
    if (SSL_CTX_set1_curves_list(ssl_ctx, config.groups.c_str()) != 1)
    {
        std::cerr << "SSL_CTX_set1_curves_list failed" << std::endl;
        exit(EXIT_FAILURE);
    }
#endif // !(OPENSSL_1_1_1_API && !defined(OPENSSL_IS_BORINGSSL))

#ifndef OPENSSL_NO_NEXTPROTONEG
    SSL_CTX_set_next_proto_select_cb(ssl_ctx, client_select_next_proto_cb,
                                     &config);
#endif // !OPENSSL_NO_NEXTPROTONEG

#if OPENSSL_VERSION_NUMBER >= 0x10002000L
    std::vector<unsigned char> proto_list;
    for (const auto& proto : config.npn_list)
    {
        std::copy_n(proto.c_str(), proto.size(), std::back_inserter(proto_list));
    }

    SSL_CTX_set_alpn_protos(ssl_ctx, proto_list.data(), proto_list.size());
#endif // OPENSSL_VERSION_NUMBER >= 0x10002000L
}

bool is_it_an_ipv6_address(const std::string& address)
{
    struct addrinfo hint, *res = nullptr;
    bool retCode = false;

    memset(&hint, '\0', sizeof hint);
    hint.ai_family = PF_UNSPEC;
    hint.ai_flags = AI_NUMERICHOST;

    if ((getaddrinfo(address.c_str(), nullptr, &hint, &res) == 0) && res)
    {
        if (res->ai_family == AF_INET6)
        {
            retCode = true;
        }
        freeaddrinfo(res);
    }
    return retCode;
}

void process_delayed_scenario(h2load::Config& config)
{
    std::map<size_t, std::pair<uint32_t, uint32_t>> delayed_scenarios;

    for (size_t index = 0; index < config.json_config_schema.scenarios.size(); index++)
    {
        Scenario& scenario = config.json_config_schema.scenarios[index];
        if (scenario.interval_to_wait_before_start)
        {
            delayed_scenarios.insert(std::make_pair(index, std::make_pair(scenario.interval_to_wait_before_start,
                                                                          scenario.weight)));
            scenario.weight = 0;
        }
    }

    if (delayed_scenarios.size())
    {
        std::chrono::steady_clock::time_point process_start_time_point = std::chrono::steady_clock::now();
        auto config_ptr = &config;
        auto update_scenario = [config_ptr, process_start_time_point](std::map<size_t, std::pair<uint32_t, uint32_t>>& sce)
        {
            auto ms_since_start = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                                        process_start_time_point).count();
            auto it = sce.begin();
            while (it != sce.end())
            {
                if (it->second.first <= ms_since_start)
                {
                    config_ptr->json_config_schema.scenarios[it->first].weight = it->second.second;
                    it = sce.erase(it);
                    config_ptr->json_config_schema.config_update_sequence_number++;
                }
                else
                {
                    ++it;
                }
            }
            if (sce.empty())
            {
                return false;
            }
            else
            {
                return true;
            }
        };

        auto thread_body = [update_scenario, delayed_scenarios]()
        {
            auto delayed_scenarios_local_copy = delayed_scenarios;
            while (update_scenario(delayed_scenarios_local_copy))
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        };
        std::thread update_weight(thread_body);
        update_weight.detach();
    }
}

bool variable_present(const std::string& source, size_t start_offset, size_t& var_start, size_t& var_end)
{
    if (start_offset >= (source.size() - 1))
    {
        return false;
    }
    var_start = source.find("${", start_offset);
    var_end = source.find("}", start_offset);
    if ((var_start == std::string::npos) ||
        (var_end == std::string::npos) ||
        (var_end < var_start))
    {
        return false;
    }
    return true;
}

void split_string(const std::string& source, String_With_Variables_In_Between& result,
                  const std::map<std::string, size_t>& var_id_map)
{
    size_t offset = 0;
    size_t var_start = 0;
    size_t var_end = 0;

    while (variable_present(source, offset, var_start, var_end))
    {
        auto segment = source.substr(offset, var_start - offset);
        auto var_name = source.substr(var_start + 2, (var_end - (var_start + 2)));
        result.string_segments.push_back(segment);
        auto iter = var_id_map.find(var_name);
        assert(iter != var_id_map.end());
        result.variable_ids_in_between.push_back(iter->second);

        offset = var_end + 1;
    }
    result.string_segments.push_back(source.substr(offset, std::string::npos));

}

uint64_t current_timestamp_nanoseconds()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>
           (std::chrono::steady_clock::now().time_since_epoch()).count();
}


