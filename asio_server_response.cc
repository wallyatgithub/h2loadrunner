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
#include "asio_server_response.h"

#include "asio_server_stream.h"
#include "asio_server_request.h"
#include "asio_server_base_handler.h"
#include "asio_common.h"

#include "http2.h"

namespace nghttp2 {
namespace asio_http2 {
namespace server {

asio_server_response::asio_server_response()
    : strm_(nullptr),
      generator_cb_(deferred_generator()),
      status_code_(200),
      state_(response_state::INITIAL),
      pushed_(false),
      push_promise_sent_(false) {}

unsigned int asio_server_response::status_code() const { return status_code_; }

void asio_server_response::write_head(unsigned int status_code, header_map h) {
  if (state_ != response_state::INITIAL) {
    return;
  }

  status_code_ = status_code;
  header_ = std::move(h);

  state_ = response_state::HEADER_DONE;

  if (pushed_ && !push_promise_sent_) {
    return;
  }
  if (debug_mode)
  {
    std::cerr<<"======outgoing http response message begin======"<<std::endl<<std::flush;
    std::cerr<<"status code: "<<status_code<<std::endl<<std::flush;
    std::cerr<<"headers: "<<std::endl<<std::flush;
    for (auto& header: h)
    {
        std::cerr<<header.first<<": "<<header.second.value<<std::endl<<std::flush;
    }
  }
  start_response();
}

void asio_server_response::end(std::string data) {
  if (debug_mode)
  {
    if (data.size())
    {
      std::cerr<<"http response body:"<<std::endl<<std::flush;
      std::cerr<<data<<std::endl<<std::flush;
    }
    std::cerr<<"======outgoing http response message end======"<<std::endl<<std::endl<<std::flush;
  }

  payload_size_ = data.size();
  end(string_generator(std::move(data)));
}
void asio_server_response::send_data_no_eos(std::string data)
{
    payload_size_ = data.size();
    end(string_generator(std::move(data)));
}

void asio_server_response::end(generator_cb cb) {
  if (state_ == response_state::BODY_STARTED) {
    return;
  }

  generator_cb_ = std::move(cb);

  if (state_ == response_state::INITIAL) {
    write_head(status_code_);
  } else {
    // generator_cb is changed, start writing in case it is deferred.
    auto handler = strm_->handler();
    handler->resume(*strm_);
  }

  state_ = response_state::BODY_STARTED;
}

void asio_server_response::write_trailer(header_map h)
{
    trailers_ = std::move(h);
    if (!generator_cb_)
    {
        send_trailer();
    }
}

void asio_server_response::send_trailer()
{
    if (trailers_.size())
    {
        auto handler = strm_->handler();
        handler->submit_trailer(*strm_, std::move(trailers_));
    }
}


void asio_server_response::start_response() {
  auto handler = strm_->handler();

  auto &req = strm_->request();

  if (!::nghttp2::http2::expect_response_body(req.method(), status_code_)) {
    state_ = response_state::BODY_STARTED;
  }

  if (handler->start_response(*strm_) != 0) {
    handler->stream_error(strm_->get_stream_id(), NGHTTP2_INTERNAL_ERROR);
    return;
  }
}

size_t asio_server_response::get_payload_size()
{
    return payload_size_;
}

void asio_server_response::on_close(close_cb cb) { close_cb_ = std::move(cb); }

void asio_server_response::call_on_close(uint32_t error_code) {
  if (close_cb_) {
    close_cb_(error_code);
  }
}

void asio_server_response::cancel(uint32_t error_code) {
  auto handler = strm_->handler();
  handler->stream_error(strm_->get_stream_id(), error_code);
}

asio_server_response* asio_server_response::push(boost::system::error_code &ec, std::string method,
                              std::string raw_path_query, header_map h) const {
  auto handler = strm_->handler();
  return handler->push_promise(ec, *strm_, std::move(method),
                               std::move(raw_path_query), std::move(h));
}

void asio_server_response::resume() {
  auto handler = strm_->handler();
  handler->resume(*strm_);
}

boost::asio::io_service &asio_server_response::io_service() {
  return strm_->handler()->io_service();
}

void asio_server_response::pushed(bool f) { pushed_ = f; }

void asio_server_response::push_promise_sent() {
  if (push_promise_sent_) {
    return;
  }
  push_promise_sent_ = true;
  if (state_ == response_state::INITIAL) {
    return;
  }
  start_response();
}

const header_map &asio_server_response::header() const { return header_; }

const header_map &asio_server_response::trailers() const { return trailers_; }

void asio_server_response::stream(class asio_server_stream *s) { strm_ = s; }

generator_cb::result_type
asio_server_response::call_read(uint8_t *data, std::size_t len, uint32_t *data_flags)
{
    auto retCode = 0;
    if (generator_cb_)
    {
        retCode = generator_cb_(data, len, data_flags);
        if (*data_flags & NGHTTP2_DATA_FLAG_EOF)
        {
            auto dummy = std::move(generator_cb_);
        }
    }
    else
    {
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
    }

    if (*data_flags & NGHTTP2_DATA_FLAG_EOF && trailers_.size())
    {
        *data_flags |= NGHTTP2_DATA_FLAG_NO_END_STREAM;
        send_trailer();
    }
    return retCode;
}

} // namespace server
} // namespace asio_http2
} // namespace nghttp2
