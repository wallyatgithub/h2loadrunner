#include "h2load.h"
#include "h2load_utils.h"
#include "h2load_Worker.h"
#include "h2load_Config.h"
#include "h2load_Client.h"



std::random_device rd;

std::mt19937 gen(rd());


namespace h2load
{


Worker::Worker(uint32_t id, SSL_CTX* ssl_ctx, size_t req_todo, size_t nclients,
               size_t rate, size_t max_samples, Config* config)
    : stats(req_todo, nclients),
      loop(ev_loop_new(get_ev_loop_flags())),
      ssl_ctx(ssl_ctx),
      config(config),
      id(id),
      tls_info_report_done(false),
      app_info_report_done(false),
      nconns_made(0),
      nclients(nclients),
      nreqs_per_client(req_todo / nclients),
      nreqs_rem(req_todo % nclients),
      rate(rate),
      max_samples(max_samples),
      next_client_id(0)
{
    if (!config->is_rate_mode() && !config->is_timing_based_mode())
    {
        progress_interval = std::max(static_cast<size_t>(1), req_todo / 10);
    }
    else
    {
        progress_interval = std::max(static_cast<size_t>(1), nclients / 10);
    }

    // Below timeout is not needed in case of timing-based benchmarking
    // create timer that will go off every rate_period
    ev_timer_init(&rate_mode_period_watcher, rate_period_timeout_w_cb, 0.,
                  config->rate_period);
    rate_mode_period_watcher.data = this;

    if (config->is_timing_based_mode())
    {
        stats.req_stats.reserve(std::max(req_todo, max_samples));
        stats.client_stats.reserve(std::max(nclients, max_samples));
    }
    else
    {
        stats.req_stats.reserve(std::min(req_todo, max_samples));
        stats.client_stats.reserve(std::min(nclients, max_samples));
    }

    sampling_init(request_times_smp, max_samples);
    sampling_init(client_smp, max_samples);

    ev_timer_init(&duration_watcher, duration_timeout_cb, config->duration, 0.);
    duration_watcher.data = this;

    ev_timer_init(&warmup_watcher, warmup_timeout_cb, config->warm_up_time, 0.);
    warmup_watcher.data = this;

    if (config->is_timing_based_mode())
    {
        current_phase = Phase::INITIAL_IDLE;
    }
    else
    {
        current_phase = Phase::MAIN_DURATION;
    }
    for (size_t scenario_index = 0; scenario_index < config->json_config_schema.scenarios.size(); scenario_index++)
    {
        std::vector<std::unique_ptr<Stats>> requests_stats;
        for (size_t request_index = 0; request_index < config->json_config_schema.scenarios[scenario_index].requests.size(); request_index++)
        {
            auto stat = std::make_unique<Stats>(req_todo, nclients);
            requests_stats.emplace_back(std::move(stat));
        }
        scenario_stats.push_back(std::move(requests_stats));
    }
}

Worker::~Worker()
{
    ev_timer_stop(loop, &rate_mode_period_watcher);
    ev_timer_stop(loop, &duration_watcher);
    ev_timer_stop(loop, &warmup_watcher);
    ev_loop_destroy(loop);
}

void Worker::stop_all_clients()
{
    for (auto client : clients)
    {
        if (client && client->session)
        {
            client->terminate_session();
            client->terminate_sub_clients();
        }
    }
}

void Worker::free_client(Client* deleted_client)
{
    for (size_t index = 0; index < clients.size(); index++)
    {
        if (clients[index] == deleted_client)
        {
            clients[index]->req_todo = clients[index]->req_done;
            stats.req_todo += clients[index]->req_todo;
            clients[index] = NULL;
            return;
        }
    }
}

void Worker::run()
{
    if (!config->is_rate_mode() && !config->is_timing_based_mode())
    {
        for (size_t i = 0; i < nclients; ++i)
        {
            auto req_todo = nreqs_per_client;
            if (nreqs_rem > 0)
            {
                ++req_todo;
                --nreqs_rem;
            }

            auto client = std::make_unique<Client>(next_client_id++, this, req_todo, config);
            if (client->do_connect() != 0)
            {
                std::cerr << "client could not connect to host" << std::endl;
                client->fail();
            }
            else
            {
                client.release();
            }
        }
    }
    else if (config->is_rate_mode())
    {
        ev_timer_again(loop, &rate_mode_period_watcher);

        // call callback so that we don't waste the first rate_period
        rate_period_timeout_w_cb(loop, &rate_mode_period_watcher, 0);
    }
    else
    {
        // call the callback to start for one single time
        rate_period_timeout_w_cb(loop, &rate_mode_period_watcher, 0);
    }
    ev_run(loop, 0);
}

namespace
{
template <typename Stats, typename Stat>
void sample(Sampling& smp, Stats& stats, Stat* s)
{
    ++smp.n;
    if (stats.size() < smp.max_samples)
    {
        stats.push_back(*s);
        return;
    }
    auto d = std::uniform_int_distribution<unsigned long>(0, smp.n - 1);
    auto i = d(gen);
    if (i < smp.max_samples)
    {
        stats[i] = *s;
    }
}
} // namespace

void Worker::sample_req_stat(RequestStat* req_stat)
{
    sample(request_times_smp, stats.req_stats, req_stat);
}

void Worker::sample_client_stat(ClientStat* cstat)
{
    sample(client_smp, stats.client_stats, cstat);
}

void Worker::report_progress()
{
    if (id != 0 || config->is_rate_mode() || stats.req_done % progress_interval ||
        config->is_timing_based_mode())
    {
        return;
    }

    std::cerr << "progress: " << stats.req_done * 100 / stats.req_todo << "% done"
              << std::endl;
}

void Worker::report_rate_progress()
{
    if (id != 0 || nconns_made % progress_interval)
    {
        return;
    }

    std::cerr << "progress: " << nconns_made * 100 / nclients
              << "% of clients started" << std::endl;
}

}
