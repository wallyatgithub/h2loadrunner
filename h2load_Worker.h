#ifndef H2LOAD_WORKER_H
#define H2LOAD_WORKER_H


#include <vector>
#include <ev.h>
#include <openssl/ssl.h>

#include "memchunk.h"
#include "h2load_stats.h"
#include "h2load_Config.h"
#include "Worker_Interface.h"


#include "memory"
#include "template.h"
#include "h2load.h"

namespace h2load
{

class Worker: public Worker_Interface
{
public:
    struct ev_loop* loop;
    MemchunkPool mcpool;
    SSL_CTX* ssl_ctx;
    ev_timer rate_mode_period_watcher;
    ev_timer duration_watcher;
    ev_timer warmup_watcher;

    Worker(uint32_t id, SSL_CTX* ssl_ctx, size_t nreq_todo, size_t nclients,
           size_t rate, size_t max_samples, Config* config);
    virtual ~Worker();
    Worker(Worker&& o) = default;

    virtual void start_rate_mode_period_timer();
    virtual void start_warmup_timer();
    virtual void start_duration_timer();
    virtual void stop_rate_mode_period_timer();
    virtual void stop_warmup_timer();
    virtual void stop_duration_timer();
    virtual void run_event_loop();
    virtual void start_graceful_stop_timer();
    virtual std::shared_ptr<Client_Interface> create_new_client(size_t req_todo);


    void init_timers();

};

}

#endif
