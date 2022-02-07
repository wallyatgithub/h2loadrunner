#ifndef WORKER_INTERFACE_H
#define WORKER_INTERFACE_H


#include <vector>
#include <map>


#include "memchunk.h"
#include "h2load_stats.h"
#include "h2load_Config.h"
#include "Client_Interface.h"


#include <memory>
#include "template.h"
#include "h2load.h"

namespace h2load
{

class Worker_Interface
{
public:
    Stats stats;
    std::vector<std::vector<std::unique_ptr<Stats>>> scenario_stats;
    Sampling request_times_smp;
    Sampling client_smp;
    Config* config;
    size_t progress_interval;
    uint32_t id;
    bool tls_info_report_done;
    bool app_info_report_done;
    size_t nconns_made;
    // number of clients this worker handles
    size_t nclients;
    // number of requests each client issues
    size_t nreqs_per_client;
    // at most nreqs_rem clients get an extra request
    size_t nreqs_rem;
    size_t rate;
    // maximum number of samples in this worker thread
    size_t max_samples;
    // The next client ID this worker assigns
    uint32_t next_client_id;
    // Keeps track of the current phase (for timing-based experiment) for the
    // worker
    Phase current_phase;
    // We need to keep track of the clients in order to stop them when needed
    std::vector<Client_Interface*> clients;
    std::map<Client_Interface*, std::shared_ptr<Client_Interface>> managed_clients;
    // This is only active when there is not a bounded number of requests
    // specified

    Worker_Interface(uint32_t id, size_t nreq_todo, size_t nclients,
                     size_t rate, size_t max_samples, Config* config);
    virtual ~Worker_Interface();

    virtual void start_rate_mode_period_timer() = 0;
    virtual void start_warmup_timer() = 0;
    virtual void start_duration_timer() = 0;
    virtual void stop_rate_mode_period_timer() = 0;
    virtual void stop_warmup_timer() = 0;
    virtual void stop_duration_timer() = 0;
    virtual void run_event_loop() = 0;
    virtual std::shared_ptr<Client_Interface> create_new_client(size_t req_todo) = 0;
    virtual void start_graceful_stop_timer() = 0;

    void rate_period_timeout_handler();
    void warmup_timeout_handler();
    void duration_timeout_handler();
    void run();
    void sample_req_stat(RequestStat* req_stat);
    void sample_client_stat(ClientStat* cstat);
    void report_progress();
    void report_rate_progress();
    // This function calls the destructors of all the clients.
    void stop_all_clients();
    // This function frees a client from the list of clients for this Worker.
    void free_client(Client_Interface*);
    void check_in_client(std::shared_ptr<Client_Interface>);
    void check_out_client(Client_Interface*);
};

}

#endif

