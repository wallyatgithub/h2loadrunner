#ifndef H2LOAD_WORKER_H
#define H2LOAD_WORKER_H


#include <vector>
#include <ev.h>
#include <openssl/ssl.h>

#ifdef USE_LIBEV
#include "memchunk.h"
#endif
#include "h2load_stats.h"
#include "h2load_Config.h"
#include "base_worker.h"


#include "memory"
#include "template.h"
#include "h2load.h"

namespace h2load
{

class libev_worker: public base_worker
{
public:
    struct ev_loop* loop;
    MemchunkPool mcpool;
    SSL_CTX* ssl_ctx_http1;
    SSL_CTX* ssl_ctx_http2;
    SSL_CTX* ssl_ctx_http3;
    SSL_CTX* ssl_ctx;
    ev_timer rate_mode_period_watcher;
    ev_timer duration_watcher;
    ev_timer warmup_watcher;

    libev_worker(uint32_t id, size_t nreq_todo, size_t nclients,
           size_t rate, size_t max_samples, Config* config);
    virtual ~libev_worker();
    libev_worker(libev_worker&& o) = default;

    virtual void start_rate_mode_period_timer();
    virtual void start_warmup_timer();
    virtual void start_duration_timer();
    virtual void stop_rate_mode_period_timer();
    virtual void stop_warmup_timer();
    virtual void stop_duration_timer();
    virtual void run_event_loop();
    virtual void start_graceful_stop_timer();
    virtual std::shared_ptr<base_client> create_new_client(size_t req_todo, PROTO_TYPE proto_type = PROTO_UNSPECIFIED, const std::string& schema = "", const std::string& authority = "");
    virtual std::shared_ptr<base_client> create_new_sub_client(base_client* parent_client, size_t req_todo, const std::string& schema, const std::string& authority, PROTO_TYPE proto_type = PROTO_UNSPECIFIED);
    virtual void stop_event_loop();

    void init_timers();

};

}

#endif
