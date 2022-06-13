#ifndef ASIO_SERVER_HTTP1_HANDLER_H
#define ASIO_SERVER_HTTP1_HANDLER_H

#include "nghttp2_config.h"

#include <map>
#include <functional>
#include <string>
#include <mutex>
#include <set>

#include <boost/array.hpp>

#include <nghttp2/asio_httpx_server.h>
#include "asio_server_base_handler.h"

#include "llhttp.h"

namespace nghttp2
{
namespace asio_http2
{
namespace server
{

class asio_server_stream;
class serve_mux;

class http1_handler : public std::enable_shared_from_this<http1_handler>, public base_handler
{
public:
    http1_handler(boost::asio::io_service& io_service,
                  boost::asio::ip::tcp::endpoint ep, connection_write writefun,
                  serve_mux& mux,
                  const H2Server_Config_Schema& conf);

    ~http1_handler();

    void call_on_request(asio_server_stream& s);

    virtual int start();

    virtual bool should_stop() const;

    virtual int start_response(asio_server_stream& s);

    virtual int submit_trailer(asio_server_stream& s, header_map h);

    virtual void stream_error(int32_t stream_id, uint32_t error_code);

    virtual void resume(asio_server_stream& s);

    virtual asio_server_response* push_promise(boost::system::error_code& ec, asio_server_stream& s,
                                   std::string method, std::string raw_path_query,
                                   header_map h);

    virtual void initiate_write();

    virtual void signal_write();

    virtual int on_read(const std::vector<uint8_t>& buffer, std::size_t len);

    virtual int on_write(std::vector<uint8_t>& buffer, std::size_t& len);

    uint32_t request_count = 0;
    std::string schema;
    std::string host;
    std::string port;
    bool should_keep_alive = false;
    std::string curr_header_name;
    std::set<uint32_t> stream_ids_to_respond;

private:
    llhttp_t http_parser;
    std::set<uint32_t> get_consecutive_stream_ids_to_respond();

};

} // namespace server
} // namespace asio_http2
} // namespace nghttp2

#endif // ASIO_SERVER_HTTP2_HANDLER_H

