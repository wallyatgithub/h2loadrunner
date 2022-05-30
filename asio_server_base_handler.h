#ifndef ASIO_SERVER_BASE_HANDLER_H
#define ASIO_SERVER_BASE_HANDLER_H

#include "nghttp2_config.h"

#include <map>
#include <functional>
#include <string>
#include <mutex>

#include <boost/array.hpp>

#include <nghttp2/asio_http2_server.h>

namespace nghttp2 {
namespace asio_http2 {
namespace server {

class base_handler;
class stream;
class serve_mux;

using connection_write = std::function<void(void)>;

class base_handler : public std::enable_shared_from_this<base_handler> {
public:
  base_handler(boost::asio::io_service &io_service,
                boost::asio::ip::tcp::endpoint ep, connection_write writefun,
                serve_mux &mux,
                const H2Server_Config_Schema& conf);

  ~base_handler();

  virtual int start();

  virtual stream* create_stream(int32_t stream_id);
  virtual void close_stream(int32_t stream_id);
  virtual stream* find_stream(int32_t stream_id);

  virtual void call_on_request(stream &s);

  virtual bool should_stop() const;

  virtual int start_response(stream &s);

  virtual int submit_trailer(stream &s, header_map h);

  virtual void stream_error(int32_t stream_id, uint32_t error_code);

  virtual void initiate_write();

  virtual void enter_callback();
  virtual void leave_callback();

  virtual void resume(stream &s);

  virtual response* push_promise(boost::system::error_code &ec, stream &s,
                         std::string method, std::string raw_path_query,
                         header_map h);

  virtual void signal_write();

  boost::asio::io_service &io_service();

  const boost::asio::ip::tcp::endpoint &remote_endpoint();

  const std::string &http_date();
  static base_handler* find_handler(uint64_t handler_id);
  static boost::asio::io_service* find_io_service(uint64_t handler_id);

  uint64_t get_handler_id();

  virtual int on_read(const std::vector<uint8_t>& buffer, std::size_t len) = 0;

  virtual int on_write(std::vector<uint8_t>& buffer, std::size_t &len) = 0;

private:
  std::map<int32_t, std::shared_ptr<stream>> streams_;
  connection_write writefun_;
  serve_mux &mux_;
  boost::asio::io_service &io_service_;
  boost::asio::ip::tcp::endpoint remote_ep_;
  const uint8_t *buf_;
  std::size_t buflen_;
  bool inside_callback_;
  // true if we have pending on_write call.  This avoids repeated call
  // of io_service::post.
  bool write_signaled_;
  time_t tstamp_cached_;
  std::string formatted_date_;
  thread_local static std::atomic<uint64_t> handler_unique_id;
  thread_local static std::map<uint64_t, base_handler*> alive_handlers;
  thread_local static std::map<uint64_t, boost::asio::io_service*> handler_io_service;
  uint64_t this_handler_id;
  const H2Server_Config_Schema& config;
};

} // namespace server
} // namespace asio_http2
} // namespace nghttp2

#endif // ASIO_SERVER_HTTP2_HANDLER_H

