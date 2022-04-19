#include "h2load.h"
#include "h2load_utils.h"
#include "libev_worker.h"
#include "h2load_Config.h"
#include "libev_client.h"


namespace h2load
{

libev_worker::libev_worker(uint32_t id, SSL_CTX* ssl_ctx, size_t nreq_todo, size_t nclients,
           size_t rate, size_t max_samples, Config* config):
           base_worker(id, nreq_todo, nclients, rate, max_samples, config),
           loop(ev_loop_new(get_ev_loop_flags())),
           ssl_ctx(ssl_ctx)
{
  init_timers();
}


libev_worker::~libev_worker()
{
    ev_timer_stop(loop, &rate_mode_period_watcher);
    ev_timer_stop(loop, &duration_watcher);
    ev_timer_stop(loop, &warmup_watcher);
    ev_loop_destroy(loop);
}

std::shared_ptr<base_client> libev_worker::create_new_client(size_t req_todo)
{
    return std::make_shared<libev_client>(next_client_id++, this, req_todo, (config));
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

}
