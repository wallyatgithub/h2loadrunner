#ifndef ASIO_WORKER_H
#define ASIO_WORKER_H

#include <map>

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
                size_t rate, size_t max_samples, Config* config, boost::asio::io_service* external_ios = nullptr);

    virtual ~asio_worker();

    using query_type = std::pair<std::string, std::string>;
    using result_type = std::pair<std::vector<std::string>, std::vector<std::string>>;
    using result_with_ttl_type = std::pair<result_type, std::chrono::steady_clock::time_point>;

    virtual void run_event_loop();

    virtual std::shared_ptr<base_client> create_new_client(size_t req_todo, PROTO_TYPE proto_type = PROTO_UNSPECIFIED, const std::string& schema = "", const std::string& authority = "");
    virtual std::shared_ptr<base_client> create_new_sub_client(base_client* parent_client, size_t req_todo, const std::string& schema, const std::string& authority, PROTO_TYPE proto_type = PROTO_UNSPECIFIED);

    bool timer_common_check(boost::asio::steady_timer & timer, const boost::system::error_code & ec,
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

    virtual void stop_event_loop();

    boost::asio::io_service& get_io_context();

    void enqueue_user_timer(uint64_t ms_to_expire, std::function<void(void)>);

    void handle_tick_timer_timeout(const boost::system::error_code & ec);

    void start_tick_timer();

    void stop_tick_timer();

    void prepare_worker_stop();

    std::thread::id get_thread_id();

    void resolve_hostname(const std::string& hostname, const std::function<void(std::vector<std::string>&)>& cb_function);

    void update_tcp_resolver_cache(const std::pair<std::string, std::string>& query, const result_type& addresses);
    const result_type& get_from_tcp_resolver_cache(const std::pair<std::string, std::string>& query);

    void update_udp_resolver_cache(const std::pair<std::string, std::string>& query, const result_type& addresses);
    const result_type& get_from_udp_resolver_cache(const std::pair<std::string, std::string>& query);

private:
    void process_user_timers();
    void update_resolver_cache(const std::pair<std::string, std::string>& query, const result_type& addresses, std::map<query_type, result_with_ttl_type>& cache);
    const result_type& get_from_resolver_cache(const std::pair<std::string, std::string>& query, std::map<query_type, result_with_ttl_type>& cache);

    std::unique_ptr<boost::asio::io_service> internal_io_context;
    boost::asio::io_service& io_context;
    boost::asio::steady_timer rate_mode_period_timer;
    boost::asio::steady_timer warmup_timer;
    boost::asio::steady_timer duration_timer;
    boost::asio::steady_timer tick_timer;
    boost::asio::ssl::context ssl_ctx;
    boost::asio::ssl::context ssl_ctx_http1;
    boost::asio::ssl::context ssl_ctx_http2;
    boost::asio::ssl::context ssl_ctx_http3;
    std::multimap<std::chrono::steady_clock::time_point, std::function<void(void)>> user_timers;
    std::thread::id my_thread_id;
    boost::asio::ip::tcp::resolver async_resolver;
    std::map<query_type, result_with_ttl_type> tcp_resolver_cache;
    std::map<query_type, result_with_ttl_type> udp_resolver_cache;
};

}

#endif
