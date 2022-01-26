#include "h2load.h"
#include "h2load_utils.h"
#include "h2load_Worker.h"
#include "h2load_Config.h"
#include "h2load_Client.h"


namespace h2load
{

Worker::Worker(uint32_t id, SSL_CTX* ssl_ctx, size_t nreq_todo, size_t nclients,
           size_t rate, size_t max_samples, Config* config):
           Worker_Interface(id, nreq_todo, nclients, rate, max_samples, config),
           loop(ev_loop_new(get_ev_loop_flags())),
           ssl_ctx(ssl_ctx)
{
  init_timers();
}


Worker::~Worker()
{
    ev_timer_stop(loop, &rate_mode_period_watcher);
    ev_timer_stop(loop, &duration_watcher);
    ev_timer_stop(loop, &warmup_watcher);
    ev_loop_destroy(loop);
}

std::unique_ptr<Client_Interface> Worker::create_new_client(size_t req_todo)
{
    return std::make_unique<Client>(next_client_id++, this, req_todo, (config));
}

void Worker::init_timers()
{
    ev_timer_init(&rate_mode_period_watcher, rate_period_timeout_w_cb, 0.,
                  config->rate_period);
    rate_mode_period_watcher.data = this;

    ev_timer_init(&duration_watcher, duration_timeout_cb, config->duration, 0.);
    duration_watcher.data = this;

    ev_timer_init(&warmup_watcher, warmup_timeout_cb, config->warm_up_time, 0.);
    warmup_watcher.data = this;
}

void Worker::start_rate_mode_period_timer()
{
    ev_timer_again(loop, &rate_mode_period_watcher);
}

void Worker::start_graceful_stop_timer()
{
    ev_timer_init(&duration_watcher, duration_timeout_cb, ((double)config->stream_timeout_in_ms / 1000), 0.);
    duration_watcher.data = this;
    start_duration_timer();
}

void Worker::start_warmup_timer()
{
    ev_timer_start(loop, &warmup_watcher);
}

void Worker::start_duration_timer()
{
    ev_timer_start(loop, &duration_watcher);
}

void Worker::stop_rate_mode_period_timer()
{
    ev_timer_stop(loop, &rate_mode_period_watcher);
}
void Worker::stop_warmup_timer()
{
    ev_timer_stop(loop, &warmup_watcher);
}

void Worker::stop_duration_timer()
{
    ev_timer_stop(loop, &duration_watcher);
}

void Worker::run_event_loop()
{
    ev_run(loop, 0);
}

}
