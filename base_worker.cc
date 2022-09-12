#include "h2load.h"
#include "h2load_utils.h"
#include "base_worker.h"
#include "h2load_Config.h"
#include "base_client.h"



std::random_device rd;

std::mt19937 gen(rd());


namespace h2load
{


base_worker::base_worker(uint32_t id, size_t req_todo, size_t nclients,
                                   size_t rate, size_t max_samples, Config* config)
    : stats(req_todo, nclients),
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
      next_client_id(0),
      randgen(util::make_mt19937())
{
    if (!config->is_rate_mode() && !config->is_timing_based_mode())
    {
        progress_interval = std::max(static_cast<size_t>(1), req_todo / 10);
    }
    else
    {
        progress_interval = std::max(static_cast<size_t>(1), nclients / 10);
    }

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
        for (size_t request_index = 0; request_index < config->json_config_schema.scenarios[scenario_index].requests.size();
             request_index++)
        {
            auto stat = std::make_unique<Stats>(req_todo, nclients);
            requests_stats.emplace_back(std::move(stat));
        }
        scenario_stats.push_back(std::move(requests_stats));
    }
}

base_worker::~base_worker()
{
}

void base_worker::stop_all_clients()
{
/*
    for (auto client : clients)
    {
        if (client && client->session)
        {
            client->setup_graceful_shutdown();
            client->terminate_session();
            client->terminate_sub_clients();
        }
    }
*/
    // client_pool has all the connected clients, including sub client
    for (auto& clients_set: client_pool)
    {
        for (auto& client: clients_set.second)
        {
            client->setup_graceful_shutdown();
            client->terminate_session();
        }
    }
}

void base_worker::free_client(base_client* deleted_client)
{
    if (!this)
    {
        return;
    }
    for (size_t index = 0; index < clients.size(); index++)
    {
        if (clients[index] == deleted_client)
        {
            clients[index]->req_todo = clients[index]->req_done;
            stats.req_todo += clients[index]->req_todo;
            clients[index] = NULL;
            break;
        }
    }
    check_out_client(deleted_client);
}

void base_worker::run()
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

            auto client = create_new_client(req_todo);
            if (client->do_connect() != 0)
            {
                std::cerr << "client could not connect to host" << std::endl;
                client->fail();
            }
            check_in_client(client);
        }
    }
    else if (config->is_rate_mode())
    {
        start_rate_mode_period_timer();
        // call callback so that we don't waste the first rate_period
        rate_period_timeout_handler();
    }
    else
    {
        // call the callback to start for one single time
        rate_period_timeout_handler();
    }
    run_event_loop();
}

void base_worker::rate_period_timeout_handler()
{
    auto nclients_per_second = rate;
    auto conns_remaining = nclients - nconns_made;
    auto nclients_to_create = std::min(nclients_per_second, conns_remaining);
    for (size_t i = 0; i < nclients_to_create; ++i)
    {
        auto req_todo = nreqs_per_client;
        if (nreqs_rem > 0)
        {
            ++req_todo;
            --nreqs_rem;
        }
        auto client = create_new_client(req_todo);

        ++nconns_made;

        if (client->do_connect() != 0)
        {
            std::cerr << "client could not connect to host" << std::endl;
            client->fail();
        }
        else
        {
            if (config->is_timing_based_mode())
            {
                clients.push_back(client.get());
            }
            check_in_client(client);
        }
        report_rate_progress();
    }
    if (nconns_made >= nclients)
    {
        stop_rate_mode_period_timer();
        // To check whether all created clients are pushed correctly
        if (config->is_timing_based_mode() && nclients != clients.size())
        {
            std::cerr << "client not started successfully, exit" << id << std::endl;
            exit(EXIT_FAILURE);
        }
    }
}
void base_worker::duration_timeout_handler()
{
    if (current_phase == Phase::MAIN_DURATION && config->json_config_schema.scenarios.size())
    {
        current_phase = Phase::MAIN_DURATION_GRACEFUL_SHUTDOWN;
        std::cerr << "Main benchmark duration is over for thread #" << id
                  << ". Entering graceful shutdown." << std::endl;
        start_graceful_stop_timer();
    }
    else
    {
        current_phase = Phase::DURATION_OVER;
        std::cerr << "Main benchmark duration is over for thread #" << id
                  << ". Stopping all clients." << std::endl;
        stop_all_clients();
        stop_rate_mode_period_timer();
        std::cerr << "Stopped all clients for thread #" << id << std::endl;
    }
}

void base_worker::warmup_timeout_handler()
{
    std::cerr << "Warm-up phase is over for thread #" << id << "."
              << std::endl;
    std::cerr << "Main benchmark duration is started for thread #" << id
              << "." << std::endl;
    assert(stats.req_started == 0);
    assert(stats.req_done == 0);

    for (auto client : clients)
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

    current_phase = Phase::MAIN_DURATION;

    start_duration_timer();
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

void base_worker::sample_req_stat(RequestStat* req_stat)
{
    sample(request_times_smp, stats.req_stats, req_stat);
}

void base_worker::sample_client_stat(ClientStat* cstat)
{
    sample(client_smp, stats.client_stats, cstat);
}

void base_worker::report_progress()
{
    if (id != 0 || config->is_rate_mode() || stats.req_done % progress_interval ||
        config->is_timing_based_mode())
    {
        return;
    }

    std::cerr << "progress: " << stats.req_done * 100 / stats.req_todo << "% done"
              << std::endl;
}

void base_worker::report_rate_progress()
{
    if (id != 0 || nconns_made % progress_interval)
    {
        return;
    }

    std::cerr << "progress: " << nconns_made * 100 / nclients
              << "% of clients started" << std::endl;
}

void base_worker::check_in_client(std::shared_ptr<base_client> client)
{
    managed_clients[client.get()] = client;
}

void base_worker::check_out_client(base_client* client)
{
    managed_clients.erase(client);
}

std::map<std::string, std::set<base_client*>>& base_worker::get_client_pool()
{
    return client_pool;
}

std::map<size_t, base_client*>& base_worker::get_client_ids()
{
    return client_ids;
}

}

