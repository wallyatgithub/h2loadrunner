#include "h2load.h"
#include "h2load_utils.h"
#include "libev_worker.h"
#include "h2load_Config.h"
#include "libev_client.h"


namespace h2load
{

libev_worker::libev_worker(uint32_t id, size_t nreq_todo, size_t nclients,
           size_t rate, size_t max_samples, Config* config):
           base_worker(id, nreq_todo, nclients, rate, max_samples, config),
           loop(ev_loop_new(get_ev_loop_flags()))
{
  init_timers();
  ssl_ctx_http1 = SSL_CTX_new(SSLv23_client_method());
  ssl_ctx_http2 = SSL_CTX_new(SSLv23_client_method());
  ssl_ctx_http3 = SSL_CTX_new(SSLv23_client_method());
  ssl_ctx = SSL_CTX_new(SSLv23_client_method());
  setup_SSL_CTX(ssl_ctx_http1, *config, std::set<std::string>{HTTP1_ALPN});
  setup_SSL_CTX(ssl_ctx_http2, *config, std::set<std::string>{HTTP2_ALPN, HTTP1_ALPN});
  setup_SSL_CTX(ssl_ctx_http3, *config, std::set<std::string>{HTTP3_ALPN});
  setup_SSL_CTX(ssl_ctx, *config);
}


libev_worker::~libev_worker()
{
    ev_timer_stop(loop, &rate_mode_period_watcher);
    ev_timer_stop(loop, &duration_watcher);
    ev_timer_stop(loop, &warmup_watcher);
    ev_loop_destroy(loop);
    SSL_CTX_free(ssl_ctx_http1);
    SSL_CTX_free(ssl_ctx_http2);
    SSL_CTX_free(ssl_ctx_http3);
    SSL_CTX_free(ssl_ctx);
}

std::shared_ptr<base_client> libev_worker::create_new_client(size_t req_todo, PROTO_TYPE proto_type, const std::string& schema, const std::string& authority)
{
    auto ctx = ssl_ctx;
    switch (proto_type)
    {
        case PROTO_HTTP1:
            ctx = ssl_ctx_http1;
            break;
        case PROTO_HTTP2:
            ctx = ssl_ctx_http2;
            break;
        case PROTO_HTTP3:
            ctx = ssl_ctx_http3;
            break;
        case PROTO_UNSPECIFIED:
            ctx = ssl_ctx;
            break;

        default:
            std::cerr<<"invalid protol"<<std::endl;
            abort();
    }
    return std::make_shared<libev_client>(next_client_id++, this, req_todo, (config), ctx, nullptr, schema, authority);
}

std::shared_ptr<base_client> libev_worker::create_new_sub_client(base_client* parent_client, size_t req_todo, const std::string& schema, const std::string& authority, PROTO_TYPE proto_type)
{
    auto ctx = ssl_ctx;
    switch (proto_type)
    {
        case PROTO_HTTP1:
            ctx = ssl_ctx_http1;
            break;
        case PROTO_HTTP2:
            ctx = ssl_ctx_http2;
            break;
        case PROTO_HTTP3:
            ctx = ssl_ctx_http3;
            break;
        case PROTO_UNSPECIFIED:
            ctx = ssl_ctx;
            break;

        default:
            std::cerr<<"invalid protol"<<std::endl;
            abort();
    }
    return std::make_shared<libev_client>(parent_client->id, this, req_todo, (config), ctx, parent_client, schema, authority, proto_type);
}

void libev_worker::init_timers()
{
    ev_timer_init(&rate_mode_period_watcher, rate_period_timeout_w_cb, 0.,
                  config->rate_period);
    rate_mode_period_watcher.data = this;

    ev_timer_init(&duration_watcher, duration_timeout_cb, config->duration, 0.);
    duration_watcher.data = this;

    ev_timer_init(&warmup_watcher, warmup_timeout_cb, config->warm_up_time, 0.);
    warmup_watcher.data = this;
}

void libev_worker::start_rate_mode_period_timer()
{
    ev_timer_again(loop, &rate_mode_period_watcher);
}

void libev_worker::start_graceful_stop_timer()
{
    ev_timer_init(&duration_watcher, duration_timeout_cb, ((double)config->stream_timeout_in_ms / 1000), 0.);
    duration_watcher.data = this;
    start_duration_timer();
}

void libev_worker::start_warmup_timer()
{
    ev_timer_start(loop, &warmup_watcher);
}

void libev_worker::start_duration_timer()
{
    ev_timer_start(loop, &duration_watcher);
}

void libev_worker::stop_rate_mode_period_timer()
{
    ev_timer_stop(loop, &rate_mode_period_watcher);
}
void libev_worker::stop_warmup_timer()
{
    ev_timer_stop(loop, &warmup_watcher);
}

void libev_worker::stop_duration_timer()
{
    ev_timer_stop(loop, &duration_watcher);
}

void libev_worker::run_event_loop()
{
    ev_run(loop, 0);
}

void libev_worker::stop_event_loop()
{
    ev_break(loop, EVBREAK_ALL);
}


}
