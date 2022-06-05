#include "asio_server_http1_handler.h"

#include <iostream>

#include "asio_common.h"
#include "asio_server_serve_mux.h"
#include "asio_server_stream.h"
#include "asio_server_request_impl.h"
#include "asio_server_response_impl.h"
#include "http2.h"
#include "util.h"
#include "template.h"
#include "H2Server_Config_Schema.h"

namespace nghttp2 {

namespace asio_http2 {

namespace server {

http1_handler::http1_handler(boost::asio::io_service &io_service,
                             boost::asio::ip::tcp::endpoint ep,
                             connection_write writefun, serve_mux &mux,
                             const H2Server_Config_Schema& conf)
    : base_handler(io_service, ep, writefun, mux, conf)
{
}

http1_handler::~http1_handler() {
  for (auto &p : streams_) {
    auto &strm = p.second;
    strm->response().impl().call_on_close(NGHTTP2_INTERNAL_ERROR);
  }
}

int http1_handler::start() {
  
  return 0;
}

void http1_handler::call_on_request(stream &strm) {
  auto cb = mux_.handler(strm.request().impl());
  cb(strm.request(), strm.response(), strm.handler()->get_handler_id(), strm.get_stream_id());
}

bool http1_handler::should_stop() const {
  return true;
}

int http1_handler::start_response(stream &strm) {
  int rv;

  
  return 0;
}

int http1_handler::submit_trailer(stream &strm, header_map h) {

  return 0;
}


void http1_handler::signal_write() {
    if (!inside_callback_ && !write_signaled_) {
      write_signaled_ = true;
      auto self = shared_from_this();
      io_service_.post([self]() { self->initiate_write(); });
    }
}

void http1_handler::initiate_write() {
  write_signaled_ = false;
  writefun_();
}

void http1_handler::stream_error(int32_t stream_id, uint32_t error_code) {
}

void http1_handler::resume(stream &strm) {
}

response *http1_handler::push_promise(boost::system::error_code &ec,
                                      stream &strm, std::string method,
                                      std::string raw_path_query,
                                      header_map h) {
    return nullptr;
}


} // namespace server

} // namespace asio_http2

} // namespace nghttp2

