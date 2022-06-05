#ifndef ASIO_SERVER_HTTP1_HANDLER_H
#define ASIO_SERVER_HTTP1_HANDLER_H

#include "nghttp2_config.h"

#include <map>
#include <functional>
#include <string>
#include <mutex>

#include <boost/array.hpp>

#include <nghttp2/asio_http2_server.h>
#include "asio_server_base_handler.h"

#include "llhttp.h"

namespace nghttp2 {
namespace asio_http2 {
namespace server {

class stream;
class serve_mux;

class http1_handler : public std::enable_shared_from_this<http1_handler>, public base_handler {
public:
  http1_handler(boost::asio::io_service &io_service,
                boost::asio::ip::tcp::endpoint ep, connection_write writefun,
                serve_mux &mux,
                const H2Server_Config_Schema& conf);

  ~http1_handler();

  void call_on_request(stream &s);

  virtual int start();

  virtual bool should_stop() const;

  virtual int start_response(stream &s);

  virtual int submit_trailer(stream &s, header_map h);

  virtual void stream_error(int32_t stream_id, uint32_t error_code);

  virtual void resume(stream &s);

  virtual response* push_promise(boost::system::error_code &ec, stream &s,
                         std::string method, std::string raw_path_query,
                         header_map h);

  virtual void initiate_write();

  virtual void signal_write();

  virtual int on_read(const std::vector<uint8_t>& buffer, std::size_t len) {
    callback_guard cg(*this);

    

    return 0;
  }

  virtual int on_write(std::vector<uint8_t>& buffer, std::size_t &len) {
    callback_guard cg(*this);

    len = 0;

    return 0;
  }

private:
  llhttp_t http_parser;
};

} // namespace server
} // namespace asio_http2
} // namespace nghttp2

#endif // ASIO_SERVER_HTTP2_HANDLER_H

