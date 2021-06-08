#include "h2load_utils.h"

using namespace h2load;

void sampling_init(h2load::Sampling &smp, size_t max_samples) {
  smp.n = 0;
  smp.max_samples = max_samples;
}

void writecb(struct ev_loop *loop, ev_io *w, int revents) {
  auto client = static_cast<Client *>(w->data);
  client->restart_timeout();
  auto rv = client->do_write();
  if (rv == Client::ERR_CONNECT_FAIL) {
    client->disconnect();
    // Try next address
    client->current_addr = nullptr;
    rv = client->connect();
    if (rv != 0) {
      client->fail();
      client->worker->free_client(client);
      delete client;
      return;
    }
    return;
  }
  if (rv != 0) {
    client->fail();
    client->worker->free_client(client);
    delete client;
  }
}

void readcb(struct ev_loop *loop, ev_io *w, int revents) {
  auto client = static_cast<Client *>(w->data);
  client->restart_timeout();
  if (client->do_read() != 0) {
    if (client->try_again_or_fail() == 0) {
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
void rate_period_timeout_w_cb(struct ev_loop *loop, ev_timer *w, int revents) {
  auto worker = static_cast<Worker *>(w->data);
  auto nclients_per_second = worker->rate;
  auto conns_remaining = worker->nclients - worker->nconns_made;
  auto nclients = std::min(nclients_per_second, conns_remaining);

  for (size_t i = 0; i < nclients; ++i) {
    auto req_todo = worker->nreqs_per_client;
    if (worker->nreqs_rem > 0) {
      ++req_todo;
      --worker->nreqs_rem;
    }
    auto client =
        std::make_unique<Client>(worker->next_client_id++, worker, req_todo, (worker->config));

    ++worker->nconns_made;

    if (client->connect() != 0) {
      std::cerr << "client could not connect to host" << std::endl;
      client->fail();
    } else {
      if (worker->config->is_timing_based_mode()) {
        worker->clients.push_back(client.release());
      } else {
        client.release();
      }
    }
    worker->report_rate_progress();
  }
  if (!worker->config->is_timing_based_mode()) {
    if (worker->nconns_made >= worker->nclients) {
      ev_timer_stop(worker->loop, w);
    }
  } else {
    // To check whether all created clients are pushed correctly
    assert(worker->nclients == worker->clients.size());
  }
}

// Called when the duration for infinite number of requests are over
void duration_timeout_cb(struct ev_loop *loop, ev_timer *w, int revents) {
  auto worker = static_cast<Worker *>(w->data);

  worker->current_phase = Phase::DURATION_OVER;

  std::cout << "Main benchmark duration is over for thread #" << worker->id
            << ". Stopping all clients." << std::endl;
  worker->stop_all_clients();
  std::cout << "Stopped all clients for thread #" << worker->id << std::endl;
}

// Called when the warmup duration for infinite number of requests are over
void warmup_timeout_cb(struct ev_loop *loop, ev_timer *w, int revents) {
  auto worker = static_cast<Worker *>(w->data);

  std::cout << "Warm-up phase is over for thread #" << worker->id << "."
            << std::endl;
  std::cout << "Main benchmark duration is started for thread #" << worker->id
            << "." << std::endl;
  assert(worker->stats.req_started == 0);
  assert(worker->stats.req_done == 0);

  for (auto client : worker->clients) {
    if (client) {
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

void rps_cb(struct ev_loop *loop, ev_timer *w, int revents) {
  auto client = static_cast<Client *>(w->data);
  auto &session = client->session;
  client->reset_timeout_requests();
  assert(!client->config->timing_script);

  if (client->req_left == 0) {
    ev_timer_stop(loop, w);
    return;
  }

  auto now = ev_now(loop);
  auto d = now - client->rps_duration_started;
  auto n = static_cast<size_t>(round(d * client->config->rps));
  client->rps_req_pending += n;
  client->rps_duration_started = now - d + static_cast<double>(n) / client->config->rps;

  if (client->rps_req_pending == 0) {
    return;
  }

  auto nreq = session->max_concurrent_streams() - client->streams.size();
  if (nreq == 0) {
    return;
  }

  nreq = client->config->is_timing_based_mode() ? std::max(nreq, client->req_left)
                                       : std::min(nreq, client->req_left);
  nreq = std::min(nreq, client->rps_req_pending);

  client->rps_req_inflight += nreq;
  client->rps_req_pending -= nreq;

  for (; nreq > 0; --nreq) {
    auto retCode = client->submit_request();
    if (retCode != 0) {
      client->process_request_failure(retCode);
      break;
    }
  }
  client->signal_write();
}

void stream_timeout_cb(struct ev_loop *loop, ev_timer *w, int revents) {
  auto client = static_cast<Client *>(w->data);
  auto &session = client->session;
  client->reset_timeout_requests();

}

// Called when an a connection has been inactive for a set period of time
// or a fixed amount of time after all requests have been made on a
// connection
void conn_timeout_cb(EV_P_ ev_timer *w, int revents) {
  auto client = static_cast<Client *>(w->data);

  ev_timer_stop(client->worker->loop, &client->conn_inactivity_watcher);
  ev_timer_stop(client->worker->loop, &client->conn_active_watcher);

  if (util::check_socket_connected(client->fd)) {
    client->timeout();
  }
}

bool check_stop_client_request_timeout(h2load::Client *client, ev_timer *w) {
  if (client->req_left == 0) {
    // no more requests to make, stop timer
    ev_timer_stop(client->worker->loop, w);
    return true;
  }

  return false;
}

void client_request_timeout_cb(struct ev_loop *loop, ev_timer *w, int revents) {
  auto client = static_cast<Client *>(w->data);
  client->reset_timeout_requests();

  if (client->streams.size() >= (size_t)client->config->max_concurrent_streams) {
    ev_timer_stop(client->worker->loop, w);
    return;
  }

  if (client->submit_request() != 0) {
    ev_timer_stop(client->worker->loop, w);
    client->process_request_failure();
    return;
  }
  client->signal_write();

  if (check_stop_client_request_timeout(client, w)) {
    return;
  }

  ev_tstamp duration =
      client->config->timings[client->reqidx] - client->config->timings[client->reqidx - 1];

  while (duration < 1e-9) {
    if (client->submit_request() != 0) {
      ev_timer_stop(client->worker->loop, w);
      client->process_request_failure();
      return;
    }
    client->signal_write();
    if (check_stop_client_request_timeout(client, w)) {
      return;
    }

    duration =
        client->config->timings[client->reqidx] - client->config->timings[client->reqidx - 1];
  }

  client->request_timeout_watcher.repeat = duration;
  ev_timer_again(client->worker->loop, &client->request_timeout_watcher);
}

bool recorded(const std::chrono::steady_clock::time_point &t) {
  return std::chrono::steady_clock::duration::zero() != t.time_since_epoch();
}

std::string get_reqline(const char *uri, const http_parser_url &u) {
  std::string reqline;

  if (util::has_uri_field(u, UF_PATH)) {
    reqline = util::get_uri_field(uri, u, UF_PATH).str();
  } else {
    reqline = "/";
  }

  if (util::has_uri_field(u, UF_QUERY)) {
    reqline += '?';
    reqline += util::get_uri_field(uri, u, UF_QUERY);
  }

  return reqline;
}


void print_server_tmp_key(SSL *ssl) {
// libressl does not have SSL_get_server_tmp_key
#if OPENSSL_VERSION_NUMBER >= 0x10002000L && defined(SSL_get_server_tmp_key)
  EVP_PKEY *key;

  if (!SSL_get_server_tmp_key(ssl, &key)) {
    return;
  }

  auto key_del = defer(EVP_PKEY_free, key);

  std::cout << "Server Temp Key: ";

  auto pkey_id = EVP_PKEY_id(key);
  switch (pkey_id) {
  case EVP_PKEY_RSA:
    std::cout << "RSA " << EVP_PKEY_bits(key) << " bits" << std::endl;
    break;
  case EVP_PKEY_DH:
    std::cout << "DH " << EVP_PKEY_bits(key) << " bits" << std::endl;
    break;
  case EVP_PKEY_EC: {
    auto ec = EVP_PKEY_get1_EC_KEY(key);
    auto ec_del = defer(EC_KEY_free, ec);
    auto nid = EC_GROUP_get_curve_name(EC_KEY_get0_group(ec));
    auto cname = EC_curve_nid2nist(nid);
    if (!cname) {
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

int get_ev_loop_flags() {
  if (ev_supported_backends() & ~ev_recommended_backends() & EVBACKEND_KQUEUE) {
    return ev_recommended_backends() | EVBACKEND_KQUEUE;
  }

  return 0;
}



// Returns percentage of number of samples within mean +/- sd.
double within_sd(const std::vector<double> &samples, double mean, double sd) {
  if (samples.size() == 0) {
    return 0.0;
  }
  auto lower = mean - sd;
  auto upper = mean + sd;
  auto m = std::count_if(
      std::begin(samples), std::end(samples),
      [&lower, &upper](double t) { return lower <= t && t <= upper; });
  return (m / static_cast<double>(samples.size())) * 100;
}

// Computes statistics using |samples|. The min, max, mean, sd, and
// percentage of number of samples within mean +/- sd are computed.
// If |sampling| is true, this computes sample variance.  Otherwise,
// population variance.
h2load::SDStat compute_time_stat(const std::vector<double> &samples,
                         bool sampling) {
  if (samples.empty()) {
    return {0.0, 0.0, 0.0, 0.0, 0.0};
  }
  // standard deviation calculated using Rapid calculation method:
  // https://en.wikipedia.org/wiki/Standard_deviation#Rapid_calculation_methods
  double a = 0, q = 0;
  size_t n = 0;
  double sum = 0;
  auto res = SDStat{std::numeric_limits<double>::max(),
                    std::numeric_limits<double>::min()};
  for (const auto &t : samples) {
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

bool parse_base_uri(const StringRef &base_uri, h2load::Config& config) {
  http_parser_url u{};
  if (http_parser_parse_url(base_uri.c_str(), base_uri.size(), 0, &u) != 0 ||
      !util::has_uri_field(u, UF_SCHEMA) || !util::has_uri_field(u, UF_HOST)) {
    return false;
  }

  config.scheme = util::get_uri_field(base_uri.c_str(), u, UF_SCHEMA).str();
  config.host = util::get_uri_field(base_uri.c_str(), u, UF_HOST).str();
  config.default_port = util::get_default_port(base_uri.c_str(), u);
  if (util::has_uri_field(u, UF_PORT)) {
    config.port = u.port;
  } else {
    config.port = config.default_port;
  }

  return true;
}

// Use std::vector<std::string>::iterator explicitly, without that,
// http_parser_url u{} fails with clang-3.4.
std::vector<std::string> parse_uris(std::vector<std::string>::iterator first,
                                    std::vector<std::string>::iterator last, h2load::Config& config) {
  std::vector<std::string> reqlines;

  if (first == last) {
    std::cerr << "no URI available" << std::endl;
    exit(EXIT_FAILURE);
  }

  if (!config.has_base_uri()) {

    if (!parse_base_uri(StringRef{*first}, config)) {
      std::cerr << "invalid URI: " << *first << std::endl;
      exit(EXIT_FAILURE);
    }

    config.base_uri = *first;
  }

  for (; first != last; ++first) {
    http_parser_url u{};

    auto uri = (*first).c_str();

    if (http_parser_parse_url(uri, (*first).size(), 0, &u) != 0) {
      std::cerr << "invalid URI: " << uri << std::endl;
      exit(EXIT_FAILURE);
    }

    reqlines.push_back(get_reqline(uri, u));
  }

  return reqlines;
}

std::vector<std::string> read_uri_from_file(std::istream &infile) {
  std::vector<std::string> uris;
  std::string line_uri;
  while (std::getline(infile, line_uri)) {
    uris.push_back(line_uri);
  }

  return uris;
}


h2load::SDStats
process_time_stats(const std::vector<std::unique_ptr<h2load::Worker>> &workers) {
  auto request_times_sampling = false;
  auto client_times_sampling = false;
  size_t nrequest_times = 0;
  size_t nclient_times = 0;
  for (const auto &w : workers) {
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

  for (const auto &w : workers) {
    for (const auto &req_stat : w->stats.req_stats) {
      if (!req_stat.completed) {
        continue;
      }
      request_times.push_back(
          std::chrono::duration_cast<std::chrono::duration<double>>(
              req_stat.stream_close_time - req_stat.request_time)
              .count());
    }

    const auto &stat = w->stats;

    for (const auto &cstat : stat.client_stats) {
      if (recorded(cstat.client_start_time) &&
          recorded(cstat.client_end_time)) {
        auto t = std::chrono::duration_cast<std::chrono::duration<double>>(
                     cstat.client_end_time - cstat.client_start_time)
                     .count();
        if (t > 1e-9) {
          rps_values.push_back(cstat.req_success / t);
        }
      }

      // We will get connect event before FFTB.
      if (!recorded(cstat.connect_start_time) ||
          !recorded(cstat.connect_time)) {
        continue;
      }

      connect_times.push_back(
          std::chrono::duration_cast<std::chrono::duration<double>>(
              cstat.connect_time - cstat.connect_start_time)
              .count());

      if (!recorded(cstat.ttfb)) {
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
          compute_time_stat(rps_values, client_times_sampling)};
}

void resolve_host(h2load::Config& config) {
  if (config.base_uri_unix) {
    auto res = std::make_unique<addrinfo>();
    res->ai_family = config.unix_addr.sun_family;
    res->ai_socktype = SOCK_STREAM;
    res->ai_addrlen = sizeof(config.unix_addr);
    res->ai_addr =
        static_cast<struct sockaddr *>(static_cast<void *>(&config.unix_addr));

    config.addrs = res.release();
    return;
  };

  int rv;
  addrinfo hints{}, *res;

  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = 0;
  hints.ai_flags = AI_ADDRCONFIG;

  const auto &resolve_host =
      config.connect_to_host.empty() ? config.host : config.connect_to_host;
  auto port =
      config.connect_to_port == 0 ? config.port : config.connect_to_port;

  rv =
      getaddrinfo(resolve_host.c_str(), util::utos(port).c_str(), &hints, &res);
  if (rv != 0) {
    std::cerr << "getaddrinfo() failed: " << gai_strerror(rv) << std::endl;
    exit(EXIT_FAILURE);
  }
  if (res == nullptr) {
    std::cerr << "No address returned" << std::endl;
    exit(EXIT_FAILURE);
  }
  config.addrs = res;
}

#ifndef OPENSSL_NO_NEXTPROTONEG
int client_select_next_proto_cb(SSL *ssl, unsigned char **out,
                                unsigned char *outlen, const unsigned char *in,
                                unsigned int inlen, void *arg) {
  h2load::Config* config = static_cast<h2load::Config*>(arg);
  if (util::select_protocol(const_cast<const unsigned char **>(out), outlen, in,
                            inlen, config->npn_list)) {
    return SSL_TLSEXT_ERR_OK;
  }

  // OpenSSL will terminate handshake with fatal alert if we return
  // NOACK.  So there is no way to fallback.
  return SSL_TLSEXT_ERR_NOACK;
}
#endif // !OPENSSL_NO_NEXTPROTONEG

