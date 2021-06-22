#ifndef H2LOAD_WORKER_H
#define H2LOAD_WORKER_H


#include <vector>
#include <ev.h>
#include <openssl/ssl.h>

#include "memchunk.h"
#include "h2load_stats.h"


namespace h2load
{

struct Client;

struct Worker
{
    MemchunkPool mcpool;
    Stats stats;
    Sampling request_times_smp;
    Sampling client_smp;
    struct ev_loop* loop;
    SSL_CTX* ssl_ctx;
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
    ev_timer timeout_watcher;
    // The next client ID this worker assigns
    uint32_t next_client_id;
    // Keeps track of the current phase (for timing-based experiment) for the
    // worker
    Phase current_phase;
    // We need to keep track of the clients in order to stop them when needed
    std::vector<Client*> clients;
    // This is only active when there is not a bounded number of requests
    // specified
    ev_timer duration_watcher;
    ev_timer warmup_watcher;

    Worker(uint32_t id, SSL_CTX* ssl_ctx, size_t nreq_todo, size_t nclients,
           size_t rate, size_t max_samples, Config* config);
    ~Worker();
    Worker(Worker&& o) = default;
    void run();
    void sample_req_stat(RequestStat* req_stat);
    void sample_client_stat(ClientStat* cstat);
    void report_progress();
    void report_rate_progress();
    // This function calls the destructors of all the clients.
    void stop_all_clients();
    // This function frees a client from the list of clients for this Worker.
    void free_client(Client*);
};

}

#endif
