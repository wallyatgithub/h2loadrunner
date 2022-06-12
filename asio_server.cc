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
// We wrote this code based on the original code which has the
// following license:
//
// server.cpp
// ~~~~~~~~~~
//
// Copyright (c) 2003-2013 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#include "asio_server.h"

#include "asio_server_connection.h"
#include "asio_common.h"
#include "util.h"
#include "H2Server_Config_Schema.h"

namespace nghttp2 {
namespace asio_http2 {
namespace server {

server::server(std::size_t io_service_pool_size,
               const boost::posix_time::time_duration &tls_handshake_timeout,
               const boost::posix_time::time_duration &read_timeout,
               const H2Server_Config_Schema& conf)
    : io_service_pool_(io_service_pool_size),
      tls_handshake_timeout_(tls_handshake_timeout),
      read_timeout_(read_timeout),
      config(conf)
      {}

boost::system::error_code
server::listen_and_serve(boost::system::error_code &ec,
                         boost::asio::ssl::context *tls_context,
                         const std::string &address, const std::string &port,
                         int backlog, serve_mux &mux, bool asynchronous) {
  ec.clear();

  if (bind_and_listen(ec, address, port, backlog)) {
    return ec;
  }

  for (auto &acceptor : acceptors_) {
    if (tls_context) {
      start_accept(*tls_context, acceptor, mux);
    } else {
      start_accept(acceptor, mux);
    }
  }

  io_service_pool_.run(asynchronous);

  return ec;
}

boost::system::error_code server::bind_and_listen(boost::system::error_code &ec,
                                                  const std::string &address,
                                                  const std::string &port,
                                                  int backlog) {
  // Open the acceptor with the option to reuse the address (i.e.
  // SO_REUSEADDR).
  tcp::resolver resolver(io_service_pool_.get_io_service());
  tcp::resolver::query query(address, port);
  auto it = resolver.resolve(query, ec);
  if (ec) {
    return ec;
  }

  for (; it != tcp::resolver::iterator(); ++it) {
    tcp::endpoint endpoint = *it;
    auto acceptor = tcp::acceptor(io_service_pool_.get_io_service());

    if (acceptor.open(endpoint.protocol(), ec)) {
      continue;
    }

    acceptor.set_option(tcp::acceptor::reuse_address(true));

    if (acceptor.bind(endpoint, ec)) {
      continue;
    }

    if (acceptor.listen(
            backlog == -1 ? boost::asio::socket_base::max_connections : backlog,
            ec)) {
      continue;
    }

    acceptors_.push_back(std::move(acceptor));
  }

  if (acceptors_.empty()) {
    return ec;
  }

  // ec could have some errors since we may have failed to bind some
  // interfaces.
  ec.clear();

  return ec;
}

void server::start_accept(boost::asio::ssl::context &tls_context,
                          tcp::acceptor &acceptor, serve_mux &mux) {

  if (!acceptor.is_open()) {
    return;
  }

  auto new_connection = std::make_shared<connection<ssl_socket>>(
      mux, tls_handshake_timeout_, read_timeout_,
      io_service_pool_.get_io_service(), tls_context);

  acceptor.async_accept(
      new_connection->socket().lowest_layer(),
      [this, &tls_context, &acceptor, &mux,
       new_connection](const boost::system::error_code &e) {
        if (!e) {
          new_connection->socket().lowest_layer().set_option(
              tcp::no_delay(true));
          boost::asio::socket_base::receive_buffer_size rcv_option(config.skt_recv_buffer_size);
          new_connection->socket().lowest_layer().set_option(rcv_option);
          boost::asio::socket_base::receive_buffer_size snd_option(config.skt_send_buffer_size);
          new_connection->socket().lowest_layer().set_option(snd_option);
          new_connection->start_tls_handshake_deadline();
          auto& conf = config;
          new_connection->socket().async_handshake(
              boost::asio::ssl::stream_base::server,
              [new_connection, &conf](const boost::system::error_code &e) {
                if (e) {
                  new_connection->stop();
                  return;
                }
                auto proto = HTTP2;
                if (!tls_h2_negotiated(new_connection->socket())) {
                  proto = HTTP1_1;
                }
                new_connection->start(conf, proto);
              });
        }

        start_accept(tls_context, acceptor, mux);
      });
}

void server::start_accept(tcp::acceptor &acceptor, serve_mux &mux) {

  if (!acceptor.is_open()) {
    return;
  }

  auto new_connection = std::make_shared<connection<tcp::socket>>(
      mux, tls_handshake_timeout_, read_timeout_,
      io_service_pool_.get_io_service());

  acceptor.async_accept(
      new_connection->socket(), [this, &acceptor, &mux, new_connection](
                                    const boost::system::error_code &e) {
        if (!e) {
          new_connection->socket().set_option(tcp::no_delay(true));
          boost::asio::socket_base::receive_buffer_size rcv_option(config.skt_recv_buffer_size);
          new_connection->socket().set_option(rcv_option);
          boost::asio::socket_base::receive_buffer_size snd_option(config.skt_send_buffer_size);
          new_connection->socket().set_option(snd_option);
          new_connection->start_read_deadline();
          new_connection->start(config, config.no_tls_proto_enum);
        }
        if (acceptor.is_open()) {
          start_accept(acceptor, mux);
        }
      });
}

void server::stop() {
  for (auto &acceptor : acceptors_) {
    acceptor.close();
  }
  io_service_pool_.stop();
}

void server::join() { io_service_pool_.join(); }

const std::vector<std::shared_ptr<boost::asio::io_service>> &
server::io_services() const {
  return io_service_pool_.io_services();
}

const std::vector<int> server::ports() const {
  auto ports = std::vector<int>(acceptors_.size());
  auto index = 0;
  for (const auto &acceptor : acceptors_) {
    ports[index++] = acceptor.local_endpoint().port();
  }
  return ports;
}

} // namespace server
} // namespace asio_http2
} // namespace nghttp2
