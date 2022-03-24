#ifdef _WINDOWS
#include <sdkddkver.h>
#endif
#include <boost/asio.hpp>
#include <boost/noncopyable.hpp>
#include <boost/asio/ssl.hpp>

#include "Worker_Interface.h"
#include "asio_client_connection.h"
#include "asio_worker.h"
#include "h2load_Config.h"

namespace h2load
{


void asio_worker::run_event_loop()
{
    io_context.run();
}

boost::asio::io_service& asio_worker::get_io_context()
{
    return io_context;
}

std::shared_ptr<Client_Interface> asio_worker::create_new_client(size_t req_todo)
{
    return std::make_shared<asio_client_connection>(io_context, next_client_id++, this, req_todo, (config), ssl_ctx);
}


asio_worker::asio_worker(uint32_t id, size_t nreq_todo, size_t nclients,
                         size_t rate, size_t max_samples, Config* config):
    Worker_Interface(id, nreq_todo, nclients, rate, max_samples, config),
    rate_mode_period_timer(io_context),
    warmup_timer(io_context),
    duration_timer(io_context),
    tick_timer(io_context),
    ssl_ctx(boost::asio::ssl::context::sslv23)
{
    setup_SSL_CTX(ssl_ctx.native_handle(), *config);
}

bool asio_worker::timer_common_check(boost::asio::deadline_timer& timer, const boost::system::error_code& ec,
                                     void (asio_worker::*handler)(const boost::system::error_code&))
{
    if (boost::asio::error::operation_aborted == ec)
    {
        return false;
    }

    if (timer.expires_at() >
        boost::asio::deadline_timer::traits_type::now())
    {
        timer.async_wait
        (
            [this, handler](const boost::system::error_code & ec)
        {
            (this->*handler)(ec);
        });
        return false;
    }
    return true;
}

void asio_worker::start_rate_mode_period_timer()
{
    rate_mode_period_timer.expires_from_now(boost::posix_time::millisec((int64_t)(config->rate_period * 1000)));
    rate_mode_period_timer.async_wait
    (
        [this](const boost::system::error_code & ec)
    {
        handle_rate_mode_period_timer_timeout(ec);
    });
}

void asio_worker::start_tick_timer()
{
    tick_timer.expires_from_now(boost::posix_time::millisec(100));
    tick_timer.async_wait
    (
        [this](const boost::system::error_code & ec)
    {
        handle_tick_timer_timeout(ec);
    });
}

void asio_worker::stop_tick_timer()
{
    tick_timer.cancel();
}

void asio_worker::stop_rate_mode_period_timer()
{
    rate_mode_period_timer.cancel();
}

void asio_worker::handle_tick_timer_timeout(const boost::system::error_code & ec)
{
    if (!timer_common_check(tick_timer, ec, &asio_worker::handle_tick_timer_timeout))
    {
        return;
    }
    process_user_timers();
    start_tick_timer();
}

void asio_worker::handle_rate_mode_period_timer_timeout(const boost::system::error_code& ec)
{
    if (!timer_common_check(rate_mode_period_timer, ec, &asio_worker::handle_rate_mode_period_timer_timeout))
    {
        return;
    }
    rate_period_timeout_handler();
    start_rate_mode_period_timer();
}

void asio_worker::start_warmup_timer()
{
    warmup_timer.expires_from_now(boost::posix_time::millisec((int64_t)(config->warm_up_time * 1000)));
    warmup_timer.async_wait
    (
        [this](const boost::system::error_code & ec)
    {
        handle_warmup_timer_timeout(ec);
    });
}

void asio_worker::stop_warmup_timer()
{
    warmup_timer.cancel();
}

void asio_worker::handle_warmup_timer_timeout(const boost::system::error_code& ec)
{
    if (!timer_common_check(warmup_timer, ec, &asio_worker::handle_warmup_timer_timeout))
    {
        return;
    }
    warmup_timeout_handler();
}

void asio_worker::start_duration_timer()
{
    duration_timer.expires_from_now(boost::posix_time::millisec((int64_t)(config->duration * 1000)));
    duration_timer.async_wait
    (
        [this](const boost::system::error_code & ec)
    {
        handle_duration_timer_timeout(ec);
    });
}

void asio_worker::handle_duration_timer_timeout(const boost::system::error_code& ec)
{
    if (!timer_common_check(duration_timer, ec, &asio_worker::handle_duration_timer_timeout))
    {
        return;
    }

    duration_timeout_handler();
}

void asio_worker::stop_duration_timer()
{
    duration_timer.cancel();
}

void asio_worker::start_graceful_stop_timer()
{
    duration_timer.expires_from_now(boost::posix_time::millisec(config->stream_timeout_in_ms));
    duration_timer.async_wait
    (
        [this](const boost::system::error_code & ec)
    {
        handle_duration_timer_timeout(ec);
    });
}

std::map<std::string, std::shared_ptr<h2load::Client_Interface>>& asio_worker::get_client_pool()
{
    return client_pool;
}

void asio_worker::enqueue_user_timer(uint64_t ms_to_expire, std::function<void(void)> callback)
{
    auto curr_timepoint = std::chrono::steady_clock::now();
    std::chrono::milliseconds timeout_duration(ms_to_expire);
    auto timeout_timepoint = curr_timepoint + timeout_duration;
    user_timers.insert(std::make_pair(timeout_timepoint, callback));
}

void asio_worker::process_user_timers()
{
    if (user_timers.empty())
    {
        return;
    }
    std::chrono::steady_clock::time_point curr_timepoint = std::chrono::steady_clock::now();
    auto stop_sign = user_timers.upper_bound(curr_timepoint);
    auto iter = user_timers.begin();
    while (iter != stop_sign)
    {
        iter->second();
        iter = user_timers.erase(iter);
    }
}

}


