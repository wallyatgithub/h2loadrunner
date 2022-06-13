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
#include "asio_server_request.h"

namespace nghttp2 {
namespace asio_http2 {
namespace server {

asio_server_request::asio_server_request() : strm_(nullptr), header_buffer_size_(0) {}

const header_map &asio_server_request::header() const { return header_; }

const std::string &asio_server_request::method() const { return method_; }

const uri_ref &asio_server_request::uri() const { return uri_; }

uri_ref &asio_server_request::uri() { return uri_; }

void asio_server_request::header(header_map h) { header_ = std::move(h); }

header_map &asio_server_request::header() { return header_; }

void asio_server_request::method(std::string arg) { method_ = std::move(arg); }

void asio_server_request::on_data(data_cb cb) { on_data_cb_ = std::move(cb); }

void asio_server_request::stream(class asio_server_stream *s) { strm_ = s; }

void asio_server_request::call_on_data(const uint8_t *data, std::size_t len) {
  if (on_data_cb_) {
    on_data_cb_(data, len);
  }
}

const boost::asio::ip::tcp::endpoint &asio_server_request::remote_endpoint() const {
  return remote_ep_;
}

void asio_server_request::remote_endpoint(boost::asio::ip::tcp::endpoint ep) {
  remote_ep_ = std::move(ep);
}

size_t asio_server_request::header_buffer_size() const { return header_buffer_size_; }

void asio_server_request::update_header_buffer_size(size_t len) {
  header_buffer_size_ += len;
}

std::string& asio_server_request::payload()
{
  return payload_;
}

const std::string& asio_server_request::unmutable_payload() const
{
  return payload_;
}


} // namespace server
} // namespace asio_http2
} // namespace nghttp2
