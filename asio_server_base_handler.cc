#include "asio_server_base_handler.h"

#include <iostream>

#include "asio_common.h"
#include "asio_server_serve_mux.h"
#include "asio_server_stream.h"
#include "asio_server_request.h"
#include "asio_server_response.h"
#include "http2.h"
#include "util.h"
#include "template.h"
#include "H2Server_Config_Schema.h"

namespace nghttp2
{

namespace asio_http2
{

namespace server
{

thread_local std::atomic<uint64_t> base_handler::handler_unique_id(0);
thread_local std::map<uint64_t, base_handler*> base_handler::alive_handlers;
thread_local std::map<uint64_t, boost::asio::io_service*> base_handler::handler_io_service;

base_handler::base_handler(boost::asio::io_service& io_service,
                           boost::asio::ip::tcp::endpoint ep,
                           connection_write writefun, serve_mux& mux,
                           const H2Server_Config_Schema& conf)
    : writefun_(writefun),
      mux_(mux),
      io_service_(io_service),
      remote_ep_(ep),
      inside_callback_(false),
      write_signaled_(false),
      tstamp_cached_(time(nullptr)),
      formatted_date_(util::http_date(tstamp_cached_)),
      this_handler_id(handler_unique_id++),
      config(conf)
{
    alive_handlers[this_handler_id] = this;
    handler_io_service[this_handler_id] = &io_service_;
}

base_handler::~base_handler()
{
    alive_handlers.erase(this_handler_id);
    handler_io_service.erase(this_handler_id);
}

void base_handler::reset_writefun()
{
    auto tmp = std::move(writefun_);
}

base_handler* base_handler::find_handler(uint64_t handler_id)
{
    auto it = alive_handlers.find(handler_id);
    if (it != alive_handlers.end())
    {
        return it->second;
    }
    return nullptr;
}

boost::asio::io_service* base_handler::find_io_service(uint64_t handler_id)
{
    auto it = handler_io_service.find(handler_id);
    if (it != handler_io_service.end())
    {
        return it->second;
    }
    return nullptr;
}

uint64_t base_handler::get_handler_id()
{
    return this_handler_id;
}

const std::string& base_handler::http_date()
{
    auto t = time(nullptr);
    if (t != tstamp_cached_)
    {
        tstamp_cached_ = t;
        formatted_date_ = util::http_date(t);
    }
    return formatted_date_;
}

asio_server_stream* base_handler::create_stream(int32_t stream_id)
{
    auto p =
        streams_.emplace(stream_id, std::make_unique<asio_server_stream>(this, stream_id));
    assert(p.second);
    return (*p.first).second.get();
}

void base_handler::close_stream(int32_t stream_id)
{
    streams_.erase(stream_id);
}

asio_server_stream* base_handler::find_stream(int32_t stream_id)
{
    auto i = streams_.find(stream_id);
    if (i == std::end(streams_))
    {
        return nullptr;
    }

    return (*i).second.get();
}

void base_handler::enter_callback()
{
    assert(!inside_callback_);
    inside_callback_ = true;
}

void base_handler::leave_callback()
{
    assert(inside_callback_);
    inside_callback_ = false;
}

boost::asio::io_service& base_handler::io_service()
{
    return io_service_;
}

const boost::asio::ip::tcp::endpoint& base_handler::remote_endpoint()
{
    return remote_ep_;
}

callback_guard::callback_guard(base_handler& h) : handler(h)
{
    handler.enter_callback();
}

callback_guard::~callback_guard()
{
    handler.leave_callback();
}


} // namespace server

} // namespace asio_http2

} // namespace nghttp2

