#ifndef BASE_WORKER_H
#define BASE_WORKER_H


#include <vector>
#include <map>
#include <set>


#ifdef USE_LIBEV
#include "memchunk.h"
#endif
#include "h2load_stats.h"
#include "h2load_Config.h"
#include "base_client.h"


#include <memory>
#include "template.h"
#include "h2load.h"

namespace h2load
{

class base_worker
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
    std::vector<base_client*> clients_to_collect_stats;
    std::map<base_client*, std::shared_ptr<base_client>> managed_clients;
    // This is only active when there is not a bounded number of requests
    // specified

    std::map<PROTO_TYPE, std::map<std::string, std::set<base_client*>>> client_pool;
    std::map<size_t, base_client*> client_ids;
    std::mt19937 randgen;

    base_worker(uint32_t id, size_t nreq_todo, size_t nclients,
                     size_t rate, size_t max_samples, Config* config);
    virtual ~base_worker();

    virtual void start_rate_mode_period_timer() = 0;
    virtual void start_warmup_timer() = 0;
    virtual void start_duration_timer() = 0;
    virtual void stop_rate_mode_period_timer() = 0;
    virtual void stop_warmup_timer() = 0;
    virtual void stop_duration_timer() = 0;
    virtual void run_event_loop() = 0;
    virtual std::shared_ptr<base_client> create_new_client(size_t req_todo, PROTO_TYPE proto_type = PROTO_UNSPECIFIED, const std::string& schema = "", const std::string& authority = "") = 0;
    virtual std::shared_ptr<base_client> create_new_sub_client(base_client* parent_client, size_t req_todo, const std::string& schema, const std::string& authority, PROTO_TYPE proto_type = PROTO_UNSPECIFIED) = 0;
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
    void free_client(base_client*);
    void check_in_client(std::shared_ptr<base_client>);
    void check_out_client(base_client*);

    std::map<PROTO_TYPE, std::map<std::string, std::set<base_client*>>>& get_client_pool();

    std::map<size_t, base_client*>& get_client_ids();

    size_t get_number_of_active_clients();

    std::shared_ptr<base_client> get_shared_ptr_of_client(base_client* client);

};

}

#endif

