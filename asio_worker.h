#ifndef ASIO_WORKER_H
#define ASIO_WORKER_H

#include <boost/asio.hpp>
#include <boost/bind.hpp>
#include <boost/noncopyable.hpp>
#include <boost/asio/ssl.hpp>

#include "Worker_Interface.h"
#include "asio_client_connection.h"
#include "h2load_Config.h"

namespace h2load
{

class asio_worker: public std::enable_shared_from_this<asio_worker>, public h2load::Worker_Interface,
    private boost::noncopyable
{
public:


    virtual void run_event_loop()
    {
        io_context.run();
    }

    virtual std::shared_ptr<Client_Interface> create_new_client(size_t req_todo)
    {
        return std::make_shared<asio_client_connection>(io_context, next_client_id++, this, req_todo, (config));
    }


    asio_worker(uint32_t id, size_t nreq_todo, size_t nclients,
                size_t rate, size_t max_samples, Config* config):
        Worker_Interface(id, nreq_todo, nclients, rate, max_samples, config),
        rate_mode_period_timer(io_context),
        warmup_timer(io_context),
        duration_timer(io_context)
    {
    }

    bool timer_common_check(boost::asio::deadline_timer& timer, const boost::system::error_code& ec, void (asio_worker::*handler)(const boost::system::error_code&))
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
                [this, handler](const boost::system::error_code& ec)
                {
                    (this->*handler)(ec);
                });
            return false;
        }
        return true;
    }

    virtual void start_rate_mode_period_timer()
    {
        rate_mode_period_timer.expires_from_now(boost::posix_time::millisec((int64_t)(config->rate_period * 1000)));
        rate_mode_period_timer.async_wait
          (
          [this](const boost::system::error_code& ec)
          {
              handle_rate_mode_period_timer_timeout(ec);
          });
    }

    virtual void stop_rate_mode_period_timer()
    {
        rate_mode_period_timer.cancel();
    }

    virtual void handle_rate_mode_period_timer_timeout(const boost::system::error_code& ec)
    {
        if (!timer_common_check(rate_mode_period_timer, ec, &asio_worker::handle_rate_mode_period_timer_timeout))
        {
            return;
        }
        rate_period_timeout_handler();
        start_rate_mode_period_timer();
    }

    virtual void start_warmup_timer()
    {
        warmup_timer.expires_from_now(boost::posix_time::millisec((int64_t)(config->warm_up_time * 1000)));
        warmup_timer.async_wait
          (
          [this](const boost::system::error_code& ec)
          {
              handle_warmup_timer_timeout(ec);
          });
    }

    virtual void stop_warmup_timer()
    {
        warmup_timer.cancel();
    }

    virtual void handle_warmup_timer_timeout(const boost::system::error_code& ec)
    {
        if (!timer_common_check(warmup_timer, ec, &asio_worker::handle_warmup_timer_timeout))
        {
            return;
        }
        warmup_timeout_handler();
    }

    virtual void start_duration_timer()
    {
        duration_timer.expires_from_now(boost::posix_time::millisec((int64_t)(config->duration * 1000)));
        duration_timer.async_wait
          (
          [this](const boost::system::error_code& ec)
          {
              handle_duration_timer_timeout(ec);
          });
    }

    virtual void handle_duration_timer_timeout(const boost::system::error_code& ec)
    {
        if (!timer_common_check(duration_timer, ec, &asio_worker::handle_duration_timer_timeout))
        {
            return;
        }

        duration_timeout_handler();
    }

    virtual void stop_duration_timer()
    {
        duration_timer.cancel();
    }

    virtual void start_graceful_stop_timer()
    {
        duration_timer.expires_from_now(boost::posix_time::millisec(config->stream_timeout_in_ms));
        duration_timer.async_wait
          (
          [this](const boost::system::error_code& ec)
          {
              handle_duration_timer_timeout(ec);
          });
    }


private:
    boost::asio::io_context io_context;

    boost::asio::deadline_timer rate_mode_period_timer;
    boost::asio::deadline_timer warmup_timer;
    boost::asio::deadline_timer duration_timer;


};

}

#endif
