/*
 * nghttp2 - HTTP/2 C Library
 *
 * Copyright (c) 2014 Tatsuhiro Tsujikawa
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include "nghttp2_config.h"

#include <nghttp2/asio_httpx_server.h>

#include "asio_httpx_server_impl.h"
#include "asio_server.h"
#include "template.h"

namespace nghttp2 {

namespace asio_http2 {

namespace server {

asio_httpx_server::asio_httpx_server(const H2Server_Config_Schema& conf) : impl_(std::make_unique<asio_httpx_server_impl>(conf)) {}

asio_httpx_server::~asio_httpx_server() {}

asio_httpx_server::asio_httpx_server(asio_httpx_server &&other) noexcept : impl_(std::move(other.impl_)) {}

asio_httpx_server &asio_httpx_server::operator=(asio_httpx_server &&other) noexcept {
  if (this == &other) {
    return *this;
  }

  impl_ = std::move(other.impl_);

  return *this;
}

boost::system::error_code asio_httpx_server::listen_and_serve(boost::system::error_code &ec,
                                                  const std::string &address,
                                                  const std::string &port,
                                                  bool asynchronous) {
  return impl_->listen_and_serve(ec, nullptr, address, port, asynchronous);
}

boost::system::error_code asio_httpx_server::listen_and_serve(
    boost::system::error_code &ec, boost::asio::ssl::context &tls_context,
    const std::string &address, const std::string &port, bool asynchronous) {
  return impl_->listen_and_serve(ec, &tls_context, address, port, asynchronous);
}

void asio_httpx_server::num_threads(size_t num_threads) { impl_->num_threads(num_threads); }

void asio_httpx_server::backlog(int backlog) { impl_->backlog(backlog); }

void asio_httpx_server::tls_handshake_timeout(const boost::posix_time::time_duration &t) {
  impl_->tls_handshake_timeout(t);
}

void asio_httpx_server::read_timeout(const boost::posix_time::time_duration &t) {
  impl_->read_timeout(t);
}

bool asio_httpx_server::handle(std::string pattern, request_cb cb) {
  return impl_->handle(std::move(pattern), std::move(cb));
}

void asio_httpx_server::stop() { impl_->stop(); }

void asio_httpx_server::join() { return impl_->join(); }

const std::vector<std::shared_ptr<boost::asio::io_service>> &
asio_httpx_server::io_services() const {
  return impl_->io_services();
}

std::vector<int> asio_httpx_server::ports() const { return impl_->ports(); }

} // namespace server

} // namespace asio_http2

} // namespace nghttp2
