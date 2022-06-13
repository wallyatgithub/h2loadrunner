/*
 * nghttp2 - HTTP/2 C Library
 *
 * Copyright (c) 2015 Tatsuhiro Tsujikawa
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
#ifndef ASIO_HTTP2_SERVER_H
#define ASIO_HTTP2_SERVER_H

#include <nghttp2/asio_http2.h>
#include "H2Server_Config_Schema.h"


namespace nghttp2 {

namespace asio_http2 {

namespace server {

class asio_server_request;
class asio_server_response;

// This is so called request callback.  Called every time request is
// received.  The life time of |request| and |response| objects end
// when callback set by response::on_close() is called.  After that,
// the application must not access to those objects.
typedef std::function<void(const asio_server_request&, asio_server_response&, uint64_t, int32_t)> request_cb;

class http2_impl;

class http2 {
public:
  http2(const H2Server_Config_Schema& conf);
  ~http2();

  http2(http2 &&other) noexcept;
  http2 &operator=(http2 &&other) noexcept;

  // Starts listening connection on given address and port and serves
  // incoming requests in cleartext TCP connection.  If |asynchronous|
  // is false, this function blocks forever unless there is an error.
  // If it is true, after server has started, this function returns
  // immediately, and the caller should call stop() and join() to
  // shutdown server gracefully.
  boost::system::error_code listen_and_serve(boost::system::error_code &ec,
                                             const std::string &address,
                                             const std::string &port,
                                             bool asynchronous = false);

  // Starts listening connection on given address and port and serves
  // incoming requests in SSL/TLS encrypted connection.  For
  // |asynchronous| parameter, see cleartext version
  // |listen_and_serve|.
  boost::system::error_code
  listen_and_serve(boost::system::error_code &ec,
                   boost::asio::ssl::context &tls_context,
                   const std::string &address, const std::string &port,
                   bool asynchronous = false);

  // Registers request handler |cb| with path pattern |pattern|.  This
  // function will fail and returns false if same pattern has been
  // already registered or |pattern| is empty string.  Otherwise
  // returns true.  The pattern match rule is the same as
  // net/http/ServeMux in golang.  Quoted from golang manual
  // (http://golang.org/pkg/net/http/#ServeMux):
  //
  //   Patterns name fixed, rooted paths, like "/favicon.ico", or
  //   rooted subtrees, like "/images/" (note the trailing
  //   slash). Longer patterns take precedence over shorter ones, so
  //   that if there are handlers registered for both "/images/" and
  //   "/images/thumbnails/", the latter handler will be called for
  //   paths beginning "/images/thumbnails/" and the former will
  //   receive requests for any other paths in the "/images/" subtree.
  //
  //   Note that since a pattern ending in a slash names a rooted
  //   subtree, the pattern "/" matches all paths not matched by other
  //   registered patterns, not just the URL with Path == "/".
  //
  //   Patterns may optionally begin with a host name, restricting
  //   matches to URLs on that host only. Host-specific patterns take
  //   precedence over general patterns, so that a handler might
  //   register for the two patterns "/codesearch" and
  //   "codesearch.google.com/" without also taking over requests for
  //   "http://www.google.com/".
  //
  // Just like ServeMux in golang, URL request path is sanitized and
  // if they contains . or .. elements, they are redirected to an
  // equivalent .- and ..-free URL.
  bool handle(std::string pattern, request_cb cb);

  // Sets number of native threads to handle incoming HTTP request.
  // It defaults to 1.
  void num_threads(size_t num_threads);

  // Sets the maximum length to which the queue of pending
  // connections.
  void backlog(int backlog);

  // Sets TLS handshake timeout, which defaults to 60 seconds.
  void tls_handshake_timeout(const boost::posix_time::time_duration &t);

  // Sets read timeout, which defaults to 60 seconds.
  void read_timeout(const boost::posix_time::time_duration &t);

  // Gracefully stop http2 server
  void stop();

  // Join on http2 server and wait for it to fully stop
  void join();

  // Get access to the io_service objects.
  const std::vector<std::shared_ptr<boost::asio::io_service>> &
  io_services() const;

  // Returns a vector with the ports in use
  std::vector<int> ports() const;

private:
  std::unique_ptr<http2_impl> impl_;
};

// Configures |tls_context| for server use.  This function sets couple
// of OpenSSL options (disables SSLv2 and SSLv3 and compression) and
// enables ECDHE ciphers.  NPN callback is also configured.
boost::system::error_code
configure_tls_context_easy(boost::system::error_code &ec,
                           boost::asio::ssl::context &tls_context,
                           bool mTLS);

// Returns request handler to do redirect to |uri| using
// |status_code|.  The |uri| appears in "location" header field as is.
request_cb redirect_handler(int status_code, std::string uri);

// Returns request handler to reply with given |status_code| and HTML
// including message about status code.
request_cb status_handler(int status_code);

} // namespace server

} // namespace asio_http2

} // namespace nghttp2

#endif // ASIO_HTTP2_SERVER_H
