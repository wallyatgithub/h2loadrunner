#ifndef ASIO_WORKER_H
#define ASIO_WORKER_H

#ifdef _WINDOWS
#include <sdkddkver.h>
#endif
#include <chrono>

#include <boost/asio.hpp>
#include <boost/noncopyable.hpp>
#include <boost/asio/ssl.hpp>

#include "base_worker.h"

namespace h2load
{

class asio_worker: public h2load::base_worker, private boost::noncopyable
{
public:

    asio_worker(uint32_t id, size_t nreq_todo, size_t nclients,
                size_t rate, size_t max_samples, Config* config);

    virtual void run_event_loop();

    virtual std::shared_ptr<base_client> create_new_client(size_t req_todo);

    bool timer_common_check(boost::asio::deadline_timer & timer, const boost::system::error_code & ec,
                            void (asio_worker:: * handler)(const boost::system::error_code&));

    virtual void start_rate_mode_period_timer();

    virtual void stop_rate_mode_period_timer();

    virtual void handle_rate_mode_period_timer_timeout(const boost::system::error_code & ec);

    virtual void start_warmup_timer();

    virtual void stop_warmup_timer();

    virtual void handle_warmup_timer_timeout(const boost::system::error_code & ec);

    virtual void start_duration_timer();

    virtual void handle_duration_timer_timeout(const boost::system::error_code & ec);

    virtual void stop_duration_timer();

    virtual void start_graceful_stop_timer();

    boost::asio::io_service& get_io_context();

    void enqueue_user_timer(uint64_t ms_to_expire, std::function<void(void)>);

    void handle_tick_timer_timeout(const boost::system::error_code & ec);

    void start_tick_timer();

    void stop_tick_timer();

    void prepare_worker_stop();

    std::thread::id get_thread_id();

    void resolve_hostname(const std::string& hostname, const std::function<void(std::vector<std::string>&)>& cb_function);

private:

    void process_user_timers();

    boost::asio::io_service io_context;
    boost::asio::deadline_timer rate_mode_period_timer;
    boost::asio::deadline_timer warmup_timer;
    boost::asio::deadline_timer duration_timer;
    boost::asio::deadline_timer tick_timer;
    boost::asio::ssl::context ssl_ctx;
    std::multimap<std::chrono::steady_clock::time_point, std::function<void(void)>> user_timers;
    std::thread::id my_thread_id;
    boost::asio::ip::tcp::resolver async_resolver;

};

}

#endif
