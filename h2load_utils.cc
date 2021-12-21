#include <algorithm>
#include <cctype>
#include <execinfo.h>
#include <iomanip>

#include <string>
#include <openssl/err.h>
#include <openssl/ssl.h>

extern "C" {
#include <ares.h>
}

#include <fstream>
#include <unistd.h>
#include <fcntl.h>
#include "template.h"
#include "util.h"
#include "config_schema.h"


#include "h2load_utils.h"
#include "h2load_Client.h"


using namespace h2load;

std::unique_ptr<h2load::Worker> create_worker(uint32_t id, SSL_CTX* ssl_ctx,
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
        std::cout << "spawning thread #" << id << ": " << nclients
                  << " total client(s). Timing-based test with "
                  << config.warm_up_time << "s of warm-up time and "
                  << config.duration << "s of main duration for measurements."
                  << std::endl;
    }
    else
    {
        std::cout << "spawning thread #" << id << ": " << nclients
                  << " total client(s). " << rate_report.str() << nreqs
                  << " total requests" << std::endl;
    }

    if (config.is_rate_mode())
    {
        return std::make_unique<Worker>(id, ssl_ctx, nreqs, nclients, rate,
                                        max_samples, &config);
    }
    else
    {
        // Here rate is same as client because the rate_timeout callback
        // will be called only once
        return std::make_unique<Worker>(id, ssl_ctx, nreqs, nclients, nclients,
                                        max_samples, &config);
    }
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

    dst = n;

    return 0;
}

void read_script_from_file(std::istream& infile,
                           std::vector<ev_tstamp>& timings,
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

void writecb(struct ev_loop* loop, ev_io* w, int revents)
{
    auto client = static_cast<Client*>(w->data);
    client->restart_timeout();
    auto rv = client->do_write();
    if (rv == Client::ERR_CONNECT_FAIL)
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
            delete client;
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
        delete client;
    }
}

void readcb(struct ev_loop* loop, ev_io* w, int revents)
{
    auto client = static_cast<Client*>(w->data);
    client->restart_timeout();
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
        delete client;
        return;
    }
    writecb(loop, &client->wev, revents);
    // client->disconnect() and client->fail() may be called
}

// Called every rate_period when rate mode is being used
void rate_period_timeout_w_cb(struct ev_loop* loop, ev_timer* w, int revents)
{
    auto worker = static_cast<Worker*>(w->data);
    auto nclients_per_second = worker->rate;
    auto conns_remaining = worker->nclients - worker->nconns_made;
    auto nclients = std::min(nclients_per_second, conns_remaining);

    for (size_t i = 0; i < nclients; ++i)
    {
        auto req_todo = worker->nreqs_per_client;
        if (worker->nreqs_rem > 0)
        {
            ++req_todo;
            --worker->nreqs_rem;
        }
        auto client =
            std::make_unique<Client>(worker->next_client_id++, worker, req_todo, (worker->config));

        ++worker->nconns_made;

        if (client->connect() != 0)
        {
            std::cerr << "client could not connect to host" << std::endl;
            client->fail();
        }
        else
        {
            if (worker->config->is_timing_based_mode())
            {
                worker->clients.push_back(client.release());
            }
            else
            {
                client.release();
            }
        }
        worker->report_rate_progress();
    }
    if (!worker->config->is_timing_based_mode())
    {
        if (worker->nconns_made >= worker->nclients)
        {
            ev_timer_stop(worker->loop, w);
        }
    }
    else
    {
        // To check whether all created clients are pushed correctly
        if (worker->nclients != worker->clients.size())
        {
            std::cout << "client not started successfully, exit" << worker->id << std::endl;
            exit(EXIT_FAILURE);
        }
    }
}

// Called when the duration for infinite number of requests are over
void duration_timeout_cb(struct ev_loop* loop, ev_timer* w, int revents)
{
    auto worker = static_cast<Worker*>(w->data);

    worker->current_phase = Phase::DURATION_OVER;

    std::cout << "Main benchmark duration is over for thread #" << worker->id
              << ". Stopping all clients." << std::endl;
    worker->stop_all_clients();
    std::cout << "Stopped all clients for thread #" << worker->id << std::endl;
    //ev_break (EV_A_ EVBREAK_ALL);
}

// Called when the warmup duration for infinite number of requests are over
void warmup_timeout_cb(struct ev_loop* loop, ev_timer* w, int revents)
{
    auto worker = static_cast<Worker*>(w->data);

    std::cout << "Warm-up phase is over for thread #" << worker->id << "."
              << std::endl;
    std::cout << "Main benchmark duration is started for thread #" << worker->id
              << "." << std::endl;
    assert(worker->stats.req_started == 0);
    assert(worker->stats.req_done == 0);

    for (auto client : worker->clients)
    {
        if (client)
        {
            assert(client->req_todo == 0);
            assert(client->req_left == 1);
            assert(client->req_inflight == 0);
            assert(client->req_started == 0);
            assert(client->req_done == 0);

            client->record_client_start_time();
            client->clear_connect_times();
            client->record_connect_start_time();
        }
    }

    worker->current_phase = Phase::MAIN_DURATION;

    ev_timer_start(worker->loop, &worker->duration_watcher);
}

void rps_cb(struct ev_loop* loop, ev_timer* w, int revents)
{
    auto client = static_cast<Client*>(w->data);
    auto& session = client->session;
    client->reset_timeout_requests();
    assert(!client->config->timing_script);

    if (client->req_left == 0)
    {
        ev_timer_stop(loop, w);
        return;
    }

    auto now = ev_now(loop);
    auto d = now - client->rps_duration_started;
    auto n = static_cast<size_t>(round(d * client->config->rps));
    client->rps_req_pending = n; // += n; do not accumulate to avoid burst of load
    client->rps_duration_started = now - d + static_cast<double>(n) / client->config->rps;

    if (client->rps_req_pending == 0)
    {
        return;
    }

    auto nreq = session->max_concurrent_streams() - client->streams.size();
    if (nreq == 0)
    {
        return;
    }

    nreq = client->config->is_timing_based_mode() ? std::max(nreq, client->req_left)
           : std::min(nreq, client->req_left);
    nreq = std::min(nreq, client->rps_req_pending);

    for (; nreq > 0; --nreq)
    {
        auto retCode = client->submit_request();
        if (retCode != 0)
        {
            break;
        }
        client->rps_req_inflight++;
        client->rps_req_pending--;
    }
    // client->signal_write(); // submit_request already calls signal_write()
}

void stream_timeout_cb(struct ev_loop* loop, ev_timer* w, int revents)
{
    auto client = static_cast<Client*>(w->data);
    auto& session = client->session;
    client->reset_timeout_requests();
}

void client_connection_timeout_cb(struct ev_loop* loop, ev_timer* w, int revents)
{
    auto client = static_cast<Client*>(w->data);
    ev_timer_stop(loop, w);
    client->fail();
    client->reconnect_to_alt_addr();
    //ev_break (EV_A_ EVBREAK_ALL);
}

void delayed_request_cb(struct ev_loop* loop, ev_timer* w, int revents)
{
    auto client = static_cast<Client*>(w->data);
    std::chrono::steady_clock::time_point curr_time_point = std::chrono::steady_clock::now();
    auto barrier = client->delayed_requests_to_submit.upper_bound(curr_time_point);
    auto it = client->delayed_requests_to_submit.begin();
    while (it != barrier)
    {
        client->requests_to_submit.emplace_back(std::move(it->second));
        it = client->delayed_requests_to_submit.erase(it);
    }
}

// Called when an a connection has been inactive for a set period of time
// or a fixed amount of time after all requests have been made on a
// connection
void conn_activity_timeout_cb(EV_P_ ev_timer* w, int revents)
{
    auto client = static_cast<Client*>(w->data);

    ev_timer_stop(client->worker->loop, &client->conn_inactivity_watcher);
    ev_timer_stop(client->worker->loop, &client->conn_active_watcher);

    if (util::check_socket_connected(client->fd))
    {
        client->timeout();
    }
}

bool check_stop_client_request_timeout(h2load::Client* client, ev_timer* w)
{
    if (client->req_left == 0)
    {
        // no more requests to make, stop timer
        ev_timer_stop(client->worker->loop, w);
        return true;
    }

    return false;
}

void client_request_timeout_cb(struct ev_loop* loop, ev_timer* w, int revents)
{
    auto client = static_cast<Client*>(w->data);
    client->reset_timeout_requests();

    if (client->streams.size() >= (size_t)client->config->max_concurrent_streams)
    {
        ev_timer_stop(client->worker->loop, w);
        return;
    }

    if (client->submit_request() != 0)
    {
        ev_timer_stop(client->worker->loop, w);
        return;
    }
    client->signal_write();

    if (check_stop_client_request_timeout(client, w))
    {
        return;
    }

    ev_tstamp duration =
        client->config->timings[client->reqidx] - client->config->timings[client->reqidx - 1];

    while (duration < 1e-9)
    {
        if (client->submit_request() != 0)
        {
            ev_timer_stop(client->worker->loop, w);
            return;
        }
        client->signal_write();
        if (check_stop_client_request_timeout(client, w))
        {
            return;
        }

        duration =
            client->config->timings[client->reqidx] - client->config->timings[client->reqidx - 1];
    }

    client->request_timeout_watcher.repeat = duration;
    ev_timer_again(client->worker->loop, &client->request_timeout_watcher);
}

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

    std::cout << "Server Temp Key: ";

    auto pkey_id = EVP_PKEY_id(key);
    switch (pkey_id)
    {
        case EVP_PKEY_RSA:
            std::cout << "RSA " << EVP_PKEY_bits(key) << " bits" << std::endl;
            break;
        case EVP_PKEY_DH:
            std::cout << "DH " << EVP_PKEY_bits(key) << " bits" << std::endl;
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

            std::cout << "ECDH " << cname << " " << EVP_PKEY_bits(key) << " bits"
                      << std::endl;
            break;
        }
        default:
            std::cout << OBJ_nid2sn(pkey_id) << " " << EVP_PKEY_bits(key) << " bits"
                      << std::endl;
            break;
    }
#endif // OPENSSL_VERSION_NUMBER >= 0x10002000L
}

int get_ev_loop_flags()
{
    if (ev_supported_backends() & ~ev_recommended_backends() & EVBACKEND_KQUEUE)
    {
        return ev_recommended_backends() | EVBACKEND_KQUEUE;
    }

    return 0;
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
process_time_stats(const std::vector<std::unique_ptr<h2load::Worker>>& workers)
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

void resolve_host(h2load::Config& config)
{
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
    config.stream_timeout_in_ms = config.json_config_schema.stream_timeout_in_ms;
    config.window_bits = config.json_config_schema.window_bits;
    config.connection_window_bits = config.json_config_schema.connection_window_bits;
    config.warm_up_time = config.json_config_schema.warm_up_time;
//    config.variable_range_slicing = config.json_config_schema.variable_range_slicing;
//    config.req_variable_start = config.json_config_schema.variable_range_start;
//    config.req_variable_end = config.json_config_schema.variable_range_end;
    //close(config.log_fd);
    //config.log_fd = open(config.json_config_schema.log_file.c_str(), O_WRONLY | O_CREAT | O_APPEND,
    //                     S_IRUSR | S_IWUSR | S_IRGRP);
}

void insert_customized_headers_to_Json_scenarios(h2load::Config& config)
{
    for (auto& header : config.custom_headers)
    {
        //std::string header_name = header.name;
        //util::inp_strlower(header_name);
        for (auto& scenario: config.json_config_schema.scenarios)
        {
            for (auto& request : scenario.requests)
            {
                request.headers_in_map[header.name] = header.value;
                request.additonalHeaders.emplace_back(header.name + ":" + header.value);
            }
        }
    }
}

std::vector<std::string> tokenize_string(const std::string& source, const std::string& delimeter)
{
    std::vector<std::string> retVec;
    size_t start = 0;
    size_t delimeter_len = delimeter.length();
    if (!delimeter.empty())
    {
        size_t pos = source.find(delimeter, start);
        while (pos != std::string::npos) {
            retVec.emplace_back(source.substr(start, (pos-start)));
            start = pos + delimeter_len;
            pos = source.find(delimeter, start);
        }
        retVec.emplace_back(source.substr(start, std::string::npos));
    }
    else
    {
        retVec.emplace_back(source);
    }
    return retVec;
}

void tokenize_path_and_payload_for_fast_var_replace(h2load::Config& config)
{
  for (auto& scenario : config.json_config_schema.scenarios)
  {
      assert(scenario.requests.size());
      for (auto& request : scenario.requests)
      {
          request.tokenized_path = tokenize_string(request.path, scenario.variable_name_in_path_and_data);
          request.tokenized_payload = tokenize_string(request.payload, scenario.variable_name_in_path_and_data);
      }
  }
}

std::string reassemble_str_with_variable(const std::vector<std::string>& tokenized_source,
                                                    uint64_t variable_value, size_t full_var_length)
{
    std::string retStr = tokenized_source[0];

    if (tokenized_source.size() > 1)
    {
        std::string curr_var_value_str = std::to_string(variable_value);
        std::string padding;
        padding.reserve(full_var_length - curr_var_value_str.size());
        for (size_t i = 0; i < full_var_length - curr_var_value_str.size(); i++)
        {
            padding.append("0");
        }
        curr_var_value_str.insert(0, padding);

        std::string variable;
        for (size_t i = 1; i < tokenized_source.size(); i++)
        {
            retStr.append(curr_var_value_str);
            retStr.append(tokenized_source[i]);
        }
    }
    return retStr;
}

void ping_w_cb(struct ev_loop* loop, ev_timer* w, int revents)
{
    auto client = static_cast<Client*>(w->data);
    client->submit_ping();
}

void ares_addrinfo_query_callback(void* arg, int status, int timeouts, struct ares_addrinfo* res) 
{
  Client* client = static_cast<Client*>(arg);

  if (status == ARES_SUCCESS)
  {
      if (client->ares_addr)
      {
          ares_freeaddrinfo(client->ares_addr);
      }
      client->next_addr = nullptr;
      client->current_addr = nullptr;
      client->ares_addr = res;
      client->connect();
      ares_freeaddrinfo(client->ares_addr);
      client->ares_addr = nullptr;
  }
  else
  {
      client->fail();
  }
}

void ares_io_cb(struct ev_loop *loop, struct ev_io *watcher, int revents)
{
    Client* client = static_cast<Client*>(watcher->data);
    ares_process_fd(client->channel,
                    revents & EV_READ ? watcher->fd : ARES_SOCKET_BAD,
                    revents & EV_WRITE ? watcher->fd : ARES_SOCKET_BAD);
}


void ares_socket_state_cb(void *data, int s, int read, int write)
{
    Client* client = static_cast<Client*>(data);
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
        ev_io_start(client->worker->loop, &client->ares_io_watchers[s]);
    }
    else if (client->ares_io_watchers.find(s) != client->ares_io_watchers.end())
    {
        ev_io_stop(client->worker->loop, &client->ares_io_watchers[s]);
        client->ares_io_watchers.erase(s);
    }
}

void normalize_request_templates(h2load::Config* config)
{
    for (auto& scenario : config->json_config_schema.scenarios)
    {
        for (auto& request : scenario.requests)
        {
            if (request.uri.typeOfAction != "input")
            {
                continue;
            }
            bool uri_updated = false;
            if (request.schema.empty())
            {
                request.schema = config->scheme;
                uri_updated = true;
            }
            if (request.authority.empty())
            {
                if (config->port != config->default_port)
                {
                    request.authority = config->host + ":" + util::utos(config->port);
                }
                else
                {
                    request.authority = config->host;
                }
                uri_updated = true;
            }
            if (uri_updated)
            {
                // for output to user to let user know the actual scenario to execute
                request.uri.input = request.schema + "://" + request.authority + request.path;
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
    pthread_t       tid = pthread_self();

    while ((error_code = ERR_get_error_line_data(&file, &line, &data, &flags)) != 0)
    {
        ERR_error_string_n(error_code, error_code_string,
                           sizeof(error_code_string));
        std::stringstream strm;
        strm << "tid==" << tid << ":" << error_code_string << ":" << file << ":" << line << ":additional info...\"" << ((
          flags & ERR_TXT_STRING) ? data : "") << "\"\n";
        error_string += strm.str();
    }
    return error_string;
}


void reconnect_to_used_host_cb(struct ev_loop* loop, ev_timer* w, int revents)
{
    auto client = static_cast<Client*>(w->data);
    ev_timer_stop(loop, w);
    if (CLIENT_CONNECTED == client->state)
    {
        return;
    }
    if (client->used_addresses.size())
    {
        client->authority = std::move(client->used_addresses.front());
        client->used_addresses.pop_front();
        std::cerr<<"switch to used host: "<<client->authority<<std::endl;
        client->resolve_fqdn_and_connect(client->schema, client->authority);
    }
    else
    {
        std::cerr<<"retry current host: "<<client->authority<<std::endl;
        client->resolve_fqdn_and_connect(client->schema, client->authority);
    }
}

void ares_addrinfo_query_callback_for_probe(void* arg, int status, int timeouts, struct ares_addrinfo* res)
{
  Client* client = static_cast<Client*>(arg);
  if (status == ARES_SUCCESS)
  {
      client->probe_address(res);
      ares_freeaddrinfo(res);
  }
}

void connect_to_prefered_host_cb(struct ev_loop* loop, ev_timer* w, int revents)
{
    auto client = static_cast<Client*>(w->data);
    if (CLIENT_CONNECTED != client->state)
    {
        ev_timer_stop(loop, w); // reconnect will connect to preferred host first
    }
    else if (client->authority == client->preferred_authority && CLIENT_CONNECTED == client->state)
    {
        ev_timer_stop(loop, w); // already to preferred host, either attempt in progress, or connected
    }
    else // connected, but not to preferred host, so check if preferred host is up for connection
    {
        client->resolve_fqdn_and_connect(client->schema, client->preferred_authority,
                                         ares_addrinfo_query_callback_for_probe);
    }
}

void probe_writecb(struct ev_loop* loop, ev_io* w, int revents)
{
    auto client = static_cast<Client*>(w->data);
    ev_io_stop(loop, w);
    if (util::check_socket_connected(client->probe_skt_fd))
    {
        std::cerr<<"preferred host is up: "<<client->preferred_authority<<std::endl;
        if (client->authority != client->preferred_authority && client->state == CLIENT_CONNECTED)
        {
            std::cerr<<"switch back to preferred host: "<<client->preferred_authority<<std::endl;
            client->disconnect();
            client->authority = client->preferred_authority;
            client->resolve_fqdn_and_connect(client->schema, client->authority);
        }
    }
}

void printBacktrace()
{
    void *buffer[64];
    int num = backtrace((void**) &buffer, 64);
    char **addresses = backtrace_symbols(buffer, num);
    for( int i = 0 ; i < num ; ++i ) {
        fprintf(stderr, "[%2d]: %s\n", i, addresses[i]);
    }
    free(addresses);
}

uint64_t find_common_multiple(std::vector<size_t> input)
{
    std::set<size_t> unique_values;

    for (auto val: input)
    {
        unique_values.insert(val);
    }

    std::set<size_t> final_set;

    for (auto iter = unique_values.rbegin(); iter!= unique_values.rend(); iter++)
    {
        auto val = *iter;
        auto find_multiple = [val](size_t val_in_set)
        {
            if ((val_in_set/val >= 1) && (val_in_set%val == 0))
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
    for (auto val: final_set)
    {
        retVal *= val;
    }
    return retVal;
}

template<typename T>
std::string to_string_with_precision_2(const T a_value)
{
    std::ostringstream out;
    out.precision(2);
    out << std::fixed << a_value;
    return out.str();
}

void output_realtime_stats(h2load::Config& config,
                                   std::vector<std::unique_ptr<h2load::Worker>>& workers,
                                   std::atomic<bool>& workers_stopped, std::stringstream& DatStream)
{
    std::vector<std::vector<size_t>> scenario_req_sent_till_now;
    std::vector<std::vector<size_t>> scenario_resp_received_till_now;
    std::vector<std::vector<size_t>> scenario_req_success_till_now;
    std::vector<std::vector<size_t>> scenario_3xx_till_now;
    std::vector<std::vector<size_t>> scenario_4xx_till_now;
    std::vector<std::vector<size_t>> scenario_5xx_till_now;
    for (size_t scenario_index = 0; scenario_index < config.json_config_schema.scenarios.size(); scenario_index++)
    {
        std::vector<size_t> req_vec(config.json_config_schema.scenarios[scenario_index].requests.size(), 0);
        scenario_req_sent_till_now.push_back(req_vec);
        scenario_resp_received_till_now.push_back(req_vec);
        scenario_req_success_till_now.push_back(req_vec);
        scenario_3xx_till_now.push_back(req_vec);
        scenario_4xx_till_now.push_back(req_vec);
        scenario_5xx_till_now.push_back(req_vec);
    }

    auto period_start = std::chrono::steady_clock::now();
    while (!workers_stopped)
    {
        auto scenario_req_sent_till_last_interval = scenario_req_sent_till_now;
        auto scenario_resp_received_till_last_interval = scenario_resp_received_till_now;
        auto scenario_req_success_till_last_interval = scenario_req_success_till_now;
        auto scenario_3xx_till_last_interval = scenario_3xx_till_now;
        auto scenario_4xx_till_last_interval = scenario_4xx_till_now;
        auto scenario_5xx_till_last_interval = scenario_5xx_till_now;

        std::this_thread::sleep_for(std::chrono::milliseconds(config.json_config_schema.statistics_interval * 1000));

        auto now = std::chrono::system_clock::now();
        auto now_c = std::chrono::system_clock::to_time_t(now);
        auto period_end = std::chrono::steady_clock::now();
        auto period_duration = std::chrono::duration_cast<std::chrono::milliseconds>(period_end - period_start).count();;
        period_start = period_end;

        static uint64_t counter = 0;
        if (counter % 10 == 0)
        {
            std::stringstream colStream;
            colStream << "time, scenario, request-index, sent/s, recv/s, success/s, (recv/s)/(sent/s), (success/s)/(recv/s), 3xx/s, 4xx/s, 5xx/s, latency-min(ms), max, mean, sd, +/-sd, total-sent, total-recv, total-success, recv/sent(total), success/recv(total)";
            std::cout<<colStream.str()<<std::endl;
        }
        counter++;

        static uint16_t rps_width = 0;
        static uint16_t total_req_width = 0;
        static uint16_t percentage_width = 7;
        static uint16_t scenario_name_width = 25;
        static uint16_t request_id_width = 3;
        static uint16_t latency_width = 5;

        auto latency_stats = produce_requests_latency_stats(workers);

        for (size_t scenario_index = 0; scenario_index < config.json_config_schema.scenarios.size(); scenario_index++)
        {
            for (size_t request_index = 0; request_index < config.json_config_schema.scenarios[scenario_index].requests.size(); request_index++)
            {
                scenario_req_sent_till_now[scenario_index][request_index] = 0;
                scenario_resp_received_till_now[scenario_index][request_index] = 0;
                scenario_req_success_till_now[scenario_index][request_index] = 0;
                scenario_3xx_till_now[scenario_index][request_index] = 0;
                scenario_4xx_till_now[scenario_index][request_index] = 0;
                scenario_5xx_till_now[scenario_index][request_index] = 0;
                for (auto& w : workers)
                {
                    auto& s = *(w->scenario_stats[scenario_index][request_index]);
                    scenario_req_sent_till_now[scenario_index][request_index] += s.req_started;
                    scenario_resp_received_till_now[scenario_index][request_index] += s.req_done;
                    scenario_req_success_till_now[scenario_index][request_index] += s.req_status_success;
                    scenario_3xx_till_now[scenario_index][request_index] += s.status[3];
                    scenario_4xx_till_now[scenario_index][request_index] += s.status[4];
                    scenario_5xx_till_now[scenario_index][request_index] += s.status[5];
                }
                size_t delta_RPS_sent = scenario_req_sent_till_now[scenario_index][request_index] - scenario_req_sent_till_last_interval[scenario_index][request_index];
                size_t delta_RPS_received = scenario_resp_received_till_now[scenario_index][request_index] - scenario_resp_received_till_last_interval[scenario_index][request_index];
                size_t delta_RPS_success = scenario_req_success_till_now[scenario_index][request_index] - scenario_req_success_till_last_interval[scenario_index][request_index];
                size_t delta_RPS_3xx = scenario_3xx_till_now[scenario_index][request_index] - scenario_3xx_till_last_interval[scenario_index][request_index];
                size_t delta_RPS_4xx = scenario_4xx_till_now[scenario_index][request_index] - scenario_4xx_till_last_interval[scenario_index][request_index];
                size_t delta_RPS_5xx = scenario_5xx_till_now[scenario_index][request_index] - scenario_5xx_till_last_interval[scenario_index][request_index];

                std::stringstream ScenarioDatStream;
                ScenarioDatStream
                    << std::put_time(std::localtime(&now_c), "%F %T")
                    << ", " << std::left << std::setw(scenario_name_width)<< config.json_config_schema.scenarios[scenario_index].name
                    << ", " << std::left << std::setw(request_id_width)<< request_index
                    << ", " << std::left << std::setw(rps_width) << round((double)(1000*delta_RPS_sent)/period_duration)
                    << ", " << std::left << std::setw(rps_width) << round((double)(1000*delta_RPS_received)/period_duration)
                    << ", " << std::left << std::setw(rps_width) << round((double)(1000*delta_RPS_success)/period_duration)
                    << ", " << std::left << std::setw(percentage_width) << to_string_with_precision_2(delta_RPS_sent?(((double)delta_RPS_received / delta_RPS_sent) * 100):0).append( "%")
                    << ", " << std::left << std::setw(percentage_width) << to_string_with_precision_2(delta_RPS_received?(((double)delta_RPS_success / delta_RPS_received) * 100):0).append("%")
                    << ", " << std::left << std::setw(rps_width) << delta_RPS_3xx
                    << ", " << std::left << std::setw(rps_width) << delta_RPS_4xx
                    << ", " << std::left << std::setw(rps_width) << delta_RPS_5xx
                    << ", " << std::left << std::setw(latency_width)<<util::format_duration_to_mili_second(latency_stats[scenario_index][request_index].min)
                    << ", " << std::left << std::setw(latency_width)<<util::format_duration_to_mili_second(latency_stats[scenario_index][request_index].max)
                    << ", " << std::left << std::setw(latency_width)<<util::format_duration_to_mili_second(latency_stats[scenario_index][request_index].mean)
                    << ", " << std::left << std::setw(latency_width)<<util::format_duration_to_mili_second(latency_stats[scenario_index][request_index].sd)
                    << ", " << std::left << std::setw(percentage_width) << to_string_with_precision_2(latency_stats[scenario_index][request_index].within_sd).append( "%")
                    << ", " << std::left << std::setw(total_req_width) << scenario_req_sent_till_now[scenario_index][request_index]
                    << ", " << std::left << std::setw(total_req_width) << scenario_resp_received_till_now[scenario_index][request_index]
                    << ", " << std::left << std::setw(total_req_width) << scenario_req_success_till_now[scenario_index][request_index]
                    << ", " << std::left << std::setw(percentage_width) << to_string_with_precision_2(scenario_req_sent_till_now[scenario_index][request_index]?(((double)scenario_resp_received_till_now[scenario_index][request_index] / scenario_req_sent_till_now[scenario_index][request_index]) * 100):0).append( "%")
                    << ", " << std::left << std::setw(percentage_width) << to_string_with_precision_2(scenario_resp_received_till_now[scenario_index][request_index]?(((double)scenario_req_success_till_now[scenario_index][request_index] / scenario_resp_received_till_now[scenario_index][request_index]) * 100):0).append( "%")
                ;
                std::cout<<ScenarioDatStream.str()<<std::endl;
            }
        }

        auto accumulate_2d_vector = [](std::vector<std::vector<size_t>>& two_d_vec)
        {
            size_t sum = 0;
            for (auto& one_d_vec: two_d_vec)
            {
                sum += std::accumulate(one_d_vec.begin(), one_d_vec.end(), 0);
            }
            return sum;
        };
        auto total_req_sent = accumulate_2d_vector(scenario_req_sent_till_now);
        auto total_resp_recv = accumulate_2d_vector(scenario_resp_received_till_now);
        auto total_resp_success = accumulate_2d_vector(scenario_req_success_till_now);
        auto total_3xx = accumulate_2d_vector(scenario_3xx_till_now);
        auto total_4xx = accumulate_2d_vector(scenario_4xx_till_now);
        auto total_5xx = accumulate_2d_vector(scenario_5xx_till_now);

        auto total_req_sent_till_last_interval = accumulate_2d_vector(scenario_req_sent_till_last_interval);
        auto total_resp_recv_till_last_interval = accumulate_2d_vector(scenario_resp_received_till_last_interval);
        auto total_resp_success_till_last_interval = accumulate_2d_vector(scenario_req_success_till_last_interval);
        auto total_3xx_till_last_interval = accumulate_2d_vector(scenario_3xx_till_last_interval);
        auto total_4xx_till_last_interval = accumulate_2d_vector(scenario_4xx_till_last_interval);
        auto total_5xx_till_last_interval = accumulate_2d_vector(scenario_5xx_till_last_interval);

        auto delta_RPS_sent = total_req_sent - total_req_sent_till_last_interval;
        auto delta_RPS_received = total_resp_recv - total_resp_recv_till_last_interval;
        auto delta_RPS_success = total_resp_success - total_resp_success_till_last_interval;
        auto delta_RPS_3xx = total_3xx - total_3xx_till_last_interval;
        auto delta_RPS_4xx = total_4xx - total_4xx_till_last_interval;
        auto delta_RPS_5xx = total_5xx - total_5xx_till_last_interval;

        DatStream.str(std::string());
        DatStream 
            << std::put_time(std::localtime(&now_c), "%F %T")
            << ", " << std::left << std::setw(scenario_name_width) << "All-Scenarios"
            << ", " << std::left << std::setw(request_id_width) << 0
            << ", " << std::left << std::setw(rps_width) << round((double)(1000*delta_RPS_sent)/period_duration)
            << ", " << std::left << std::setw(rps_width) << round((double)(1000*delta_RPS_received)/period_duration)
            << ", " << std::left << std::setw(rps_width) << round((double)(1000*delta_RPS_success)/period_duration)
            << ", " << std::left << std::setw(percentage_width) << to_string_with_precision_2(delta_RPS_sent?(((double)delta_RPS_received / delta_RPS_sent) * 100):0).append("%")
            << ", " << std::left << std::setw(percentage_width) << to_string_with_precision_2(delta_RPS_received?(((double)delta_RPS_success / delta_RPS_received) * 100):0).append("%")
            << ", " << std::left << std::setw(rps_width) << delta_RPS_3xx
            << ", " << std::left << std::setw(rps_width) << delta_RPS_4xx
            << ", " << std::left << std::setw(rps_width) << delta_RPS_5xx
            << ", " << std::left << std::setw(latency_width)<<util::format_duration_to_mili_second(latency_stats[config.json_config_schema.scenarios.size()][0].min)
            << ", " << std::left << std::setw(latency_width)<<util::format_duration_to_mili_second(latency_stats[config.json_config_schema.scenarios.size()][0].max)
            << ", " << std::left << std::setw(latency_width)<<util::format_duration_to_mili_second(latency_stats[config.json_config_schema.scenarios.size()][0].mean)
            << ", " << std::left << std::setw(latency_width)<<util::format_duration_to_mili_second(latency_stats[config.json_config_schema.scenarios.size()][0].sd)
            << ", " << std::left << std::setw(percentage_width) << to_string_with_precision_2(latency_stats[config.json_config_schema.scenarios.size()][0].within_sd).append( "%")
            << ", " << std::left << std::setw(total_req_width) << total_req_sent
            << ", " << std::left << std::setw(total_req_width) << total_resp_recv
            << ", " << std::left << std::setw(total_req_width) << total_resp_success
            << ", " << std::left << std::setw(percentage_width) << to_string_with_precision_2(total_req_sent?(((double)total_resp_recv / total_req_sent) * 100):0).append("%")
            << ", " << std::left << std::setw(percentage_width) << to_string_with_precision_2(total_resp_recv?(((double)total_resp_success / total_resp_recv) * 100):0).append("%")
        ;
        std::cout<<DatStream.str()<<std::endl;

        rps_width = std::to_string(delta_RPS_sent).size() > rps_width ? std::to_string(delta_RPS_sent).size() : rps_width;
        total_req_width = std::to_string(total_req_sent).size() > total_req_width ? std::to_string(total_req_sent).size(): total_req_width;

    }
}

std::vector<std::vector<h2load::SDStat>>
produce_requests_latency_stats(const std::vector<std::unique_ptr<h2load::Worker>>& workers)
{
    auto request_times_sampling = false;
    size_t nrequest_times = 0;
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

    for (size_t scenario_index = 0; scenario_index < request_times.size(); scenario_index++)
    {
        std::vector<h2load::SDStat> requests_stats;
        for (size_t request_index = 0; request_index < request_times[scenario_index].size(); request_index++)
        {
            requests_stats.push_back(compute_time_stat(request_times[scenario_index][request_index], request_times_sampling));
        }
        stats.push_back(std::move(requests_stats));
    }

    std::vector<double> all_request_times;
    for (auto& items: request_times)
    {
        for (auto& item: items.second)
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

