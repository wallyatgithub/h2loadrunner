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
#include "asio_httpx_server_impl.h"

#include <openssl/ssl.h>

#include "asio_server.h"
#include "util.h"
#include "tls.h"
#include "template.h"

namespace nghttp2 {

namespace asio_http2 {

namespace server {

asio_httpx_server_impl::asio_httpx_server_impl(const H2Server_Config_Schema& conf)
    : num_threads_(1),
      backlog_(-1),
      tls_handshake_timeout_(boost::posix_time::seconds(60)),
      read_timeout_(boost::posix_time::seconds(60)),
      config(conf){}

boost::system::error_code asio_httpx_server_impl::listen_and_serve(
    boost::system::error_code &ec, boost::asio::ssl::context *tls_context,
    const std::string &address, const std::string &port, bool asynchronous) {
  server_.reset(
      new server(num_threads_, tls_handshake_timeout_, read_timeout_, config));
  return server_->listen_and_serve(ec, tls_context, address, port, backlog_,
                                   mux_, asynchronous);
}

void asio_httpx_server_impl::num_threads(size_t num_threads) { num_threads_ = num_threads; }

void asio_httpx_server_impl::backlog(int backlog) { backlog_ = backlog; }

void asio_httpx_server_impl::tls_handshake_timeout(
    const boost::posix_time::time_duration &t) {
  tls_handshake_timeout_ = t;
}

void asio_httpx_server_impl::read_timeout(const boost::posix_time::time_duration &t) {
  read_timeout_ = t;
}

bool asio_httpx_server_impl::handle(std::string pattern, request_cb cb) {
  return mux_.handle(std::move(pattern), std::move(cb));
}

void asio_httpx_server_impl::stop() { return server_->stop(); }

void asio_httpx_server_impl::join() { return server_->join(); }

const std::vector<std::shared_ptr<boost::asio::io_service>> &
asio_httpx_server_impl::io_services() const {
  return server_->io_services();
}

std::vector<int> asio_httpx_server_impl::ports() const { return server_->ports(); }

} // namespace server

} // namespace asio_http2

} // namespace nghttp2
