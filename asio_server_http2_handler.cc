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
#include "asio_server_http2_handler.h"

#include <iostream>

#include "asio_common.h"
#include "asio_server_serve_mux.h"
#include "asio_server_stream.h"
#include "asio_server_request.h"
#include "asio_server_response.h"
#include "http2.h"
#include "util.h"
#include "template.h"
#include "H2Server_Config_Schema.h"

namespace nghttp2 {

namespace asio_http2 {

namespace server {

namespace {
int stream_error(nghttp2_session *session, int32_t stream_id,
                 uint32_t error_code) {
  return nghttp2_submit_rst_stream(session, NGHTTP2_FLAG_NONE, stream_id,
                                   error_code);
}
} // namespace

namespace {
int on_begin_headers_callback(nghttp2_session *session,
                              const nghttp2_frame *frame, void *user_data) {
  auto handler = static_cast<http2_handler *>(user_data);

  if (frame->hd.type != NGHTTP2_HEADERS ||
      frame->headers.cat != NGHTTP2_HCAT_REQUEST) {
    return 0;
  }

  handler->create_stream(frame->hd.stream_id);

  return 0;
}
} // namespace

namespace {
int on_header_callback(nghttp2_session *session, const nghttp2_frame *frame,
                       const uint8_t *name, size_t namelen,
                       const uint8_t *value, size_t valuelen, uint8_t flags,
                       void *user_data) {
  auto handler = static_cast<http2_handler *>(user_data);
  auto stream_id = frame->hd.stream_id;

  if (frame->hd.type != NGHTTP2_HEADERS ||
      frame->headers.cat != NGHTTP2_HCAT_REQUEST) {
    return 0;
  }

  auto strm = handler->find_stream(stream_id);
  if (!strm) {
    return 0;
  }

  auto &req = strm->request();
  auto &uref = req.uri();

  switch (nghttp2::http2::lookup_token(name, namelen)) {
  case nghttp2::http2::HD__METHOD:
    req.method(std::string(value, value + valuelen));
    break;
  case nghttp2::http2::HD__SCHEME:
    uref.scheme.assign(value, value + valuelen);
    break;
  case nghttp2::http2::HD__AUTHORITY:
    uref.host.assign(value, value + valuelen);
    break;
  case nghttp2::http2::HD__PATH:
    split_path(uref, value, value + valuelen);
    break;
  case nghttp2::http2::HD_HOST:
    if (uref.host.empty()) {
      uref.host.assign(value, value + valuelen);
    }
  // fall through
  default:
    if (req.header_buffer_size() + namelen + valuelen > 64_k) {
      nghttp2_submit_rst_stream(session, NGHTTP2_FLAG_NONE, frame->hd.stream_id,
                                NGHTTP2_INTERNAL_ERROR);
      break;
    }
    req.update_header_buffer_size(namelen + valuelen);

    req.header().emplace(std::string(name, name + namelen),
                         header_value{std::string(value, value + valuelen),
                                      (flags & NGHTTP2_NV_FLAG_NO_INDEX) != 0});
  }

  return 0;
}
} // namespace

namespace {
int on_frame_recv_callback(nghttp2_session *session, const nghttp2_frame *frame,
                           void *user_data) {
  auto handler = static_cast<http2_handler *>(user_data);
  auto strm = handler->find_stream(frame->hd.stream_id);

  switch (frame->hd.type) {
  case NGHTTP2_DATA:
    if (!strm) {
      break;
    }

    if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) {
      strm->request().call_on_data(nullptr, 0);
    }

    if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM)
    {
        handler->call_on_request(*strm);
    }

    break;
  case NGHTTP2_HEADERS: {
    if (!strm || frame->headers.cat != NGHTTP2_HCAT_REQUEST) {
      break;
    }

    auto &req = strm->request();
    req.remote_endpoint(handler->remote_endpoint());

    if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) {
      strm->request().call_on_data(nullptr, 0);
    }

    if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM)
    {
        handler->call_on_request(*strm);
    }

    break;
  }
  }

  return 0;
}
} // namespace

namespace {
int on_data_chunk_recv_callback(nghttp2_session *session, uint8_t flags,
                                int32_t stream_id, const uint8_t *data,
                                size_t len, void *user_data) {
  auto handler = static_cast<http2_handler *>(user_data);
  auto strm = handler->find_stream(stream_id);

  if (!strm) {
    return 0;
  }

  strm->request().payload().append((const char*)data, len);

  strm->request().call_on_data(data, len);

  return 0;
}

} // namespace

namespace {
int on_stream_close_callback(nghttp2_session *session, int32_t stream_id,
                             uint32_t error_code, void *user_data) {
  auto handler = static_cast<http2_handler *>(user_data);

  auto strm = handler->find_stream(stream_id);
  if (!strm) {
    return 0;
  }

  strm->response().call_on_close(error_code);

  handler->close_stream(stream_id);

  return 0;
}
} // namespace

namespace {
int on_frame_send_callback(nghttp2_session *session, const nghttp2_frame *frame,
                           void *user_data) {
  auto handler = static_cast<http2_handler *>(user_data);

  if (frame->hd.type != NGHTTP2_PUSH_PROMISE) {
    return 0;
  }

  auto strm = handler->find_stream(frame->push_promise.promised_stream_id);

  if (!strm) {
    return 0;
  }

  auto &res = strm->response();
  res.push_promise_sent();

  return 0;
}
} // namespace

namespace {
int on_frame_not_send_callback(nghttp2_session *session,
                               const nghttp2_frame *frame, int lib_error_code,
                               void *user_data) {
  if (frame->hd.type != NGHTTP2_HEADERS) {
    return 0;
  }

  // Issue RST_STREAM so that stream does not hang around.
  nghttp2_submit_rst_stream(session, NGHTTP2_FLAG_NONE, frame->hd.stream_id,
                            NGHTTP2_INTERNAL_ERROR);

  return 0;
}
} // namespace

http2_handler::http2_handler(boost::asio::io_service &io_service,
                             boost::asio::ip::tcp::endpoint ep,
                             connection_write writefun, serve_mux &mux,
                             const H2Server_Config_Schema& conf)
    : base_handler(io_service, ep, writefun, mux, conf),
      session_(nullptr),
      buf_(nullptr),
      buflen_(0)
{
}

http2_handler::~http2_handler() {
  for (auto &p : streams_) {
    auto &strm = p.second;
    strm->response().call_on_close(NGHTTP2_INTERNAL_ERROR);
  }
  nghttp2_session_del(session_);
}

int http2_handler::start() {
  int rv;

  nghttp2_session_callbacks *callbacks;
  rv = nghttp2_session_callbacks_new(&callbacks);
  if (rv != 0) {
    return -1;
  }

  auto cb_del = defer(nghttp2_session_callbacks_del, callbacks);

  nghttp2_session_callbacks_set_on_begin_headers_callback(
      callbacks, on_begin_headers_callback);
  nghttp2_session_callbacks_set_on_header_callback(callbacks,
                                                   on_header_callback);
  nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks,
                                                       on_frame_recv_callback);
  nghttp2_session_callbacks_set_on_data_chunk_recv_callback(
      callbacks, on_data_chunk_recv_callback);
  nghttp2_session_callbacks_set_on_stream_close_callback(
      callbacks, on_stream_close_callback);
  nghttp2_session_callbacks_set_on_frame_send_callback(callbacks,
                                                       on_frame_send_callback);
  nghttp2_session_callbacks_set_on_frame_not_send_callback(
      callbacks, on_frame_not_send_callback);

  nghttp2_option* opt;

  rv = nghttp2_option_new(&opt);
  assert(rv == 0);
  if (config.encoder_header_table_size != NGHTTP2_DEFAULT_HEADER_TABLE_SIZE)
  {
      nghttp2_option_set_max_deflate_dynamic_table_size(
          opt, config.encoder_header_table_size);
  }

  rv = nghttp2_session_server_new2(&session_, callbacks, this, opt);

  nghttp2_option_del(opt);

  if (rv != 0) {
    return -1;
  }

  nghttp2_settings_entry ent{NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, config.max_concurrent_streams};

  std::array<nghttp2_settings_entry, 3> iv;
  size_t niv = 2;
  iv[0].settings_id = NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE;
  iv[0].value = (1 << config.window_bits) - 1;

  iv[1].settings_id = NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS;
  iv[1].value = config.max_concurrent_streams;

  if (config.header_table_size != NGHTTP2_DEFAULT_HEADER_TABLE_SIZE)
  {
      iv[niv].settings_id = NGHTTP2_SETTINGS_HEADER_TABLE_SIZE;
      iv[niv].value = config.header_table_size;
      ++niv;
  }

  nghttp2_submit_settings(session_, NGHTTP2_FLAG_NONE, iv.data(), niv);

  nghttp2_session_set_local_window_size(session_, NGHTTP2_FLAG_NONE, 0,
                                        (1 << config.connection_window_bits) - 1);

  return 0;
}

void http2_handler::call_on_request(asio_server_stream &strm) {
  auto cb = mux_.handler(strm.request());
  cb(strm.request(), strm.response(), strm.handler()->get_handler_id(), strm.get_stream_id());
}

bool http2_handler::should_stop() const {
  return !nghttp2_session_want_read(session_) &&
         !nghttp2_session_want_write(session_);
}

int http2_handler::start_response(asio_server_stream &strm) {
  int rv;

  auto &res = strm.response();
  auto &header = res.header();
  auto nva = std::vector<nghttp2_nv>();
  nva.reserve(2 + header.size());
  auto status = util::utos(res.status_code());
  auto date = http_date();
  nva.push_back(nghttp2::http2::make_nv_ls(":status", status));
  nva.push_back(nghttp2::http2::make_nv_ls("date", date));
  for (auto &hd : header) {
    nva.push_back(nghttp2::http2::make_nv(hd.first, hd.second.value,
                                          hd.second.sensitive));
  }

  nghttp2_data_provider *prd_ptr = nullptr, prd;
  auto &req = strm.request();
  if (::nghttp2::http2::expect_response_body(req.method(), res.status_code())) {
    prd.source.ptr = &strm;
    prd.read_callback = [](nghttp2_session *session, int32_t stream_id,
                           uint8_t *buf, size_t length, uint32_t *data_flags,
                           nghttp2_data_source *source,
                           void *user_data) -> ssize_t {
      auto &strm = *static_cast<asio_server_stream *>(source->ptr);
      return strm.response().call_read(buf, length, data_flags);
    };
    prd_ptr = &prd;
  }
  rv = nghttp2_submit_response(session_, strm.get_stream_id(), nva.data(),
                               nva.size(), prd_ptr);

  if (rv != 0) {
    return -1;
  }

  signal_write();

  return 0;
}

int http2_handler::submit_trailer(asio_server_stream &strm, header_map h) {
  int rv;
  auto nva = std::vector<nghttp2_nv>();
  nva.reserve(h.size());
  for (auto &hd : h) {
    nva.push_back(nghttp2::http2::make_nv(hd.first, hd.second.value,
                                          hd.second.sensitive));
  }

  rv = nghttp2_submit_trailer(session_, strm.get_stream_id(), nva.data(),
                              nva.size());

  if (rv != 0) {
    return -1;
  }

  signal_write();

  return 0;
}


void http2_handler::signal_write() {
    if (!inside_callback_ && !write_signaled_) {
      write_signaled_ = true;
      auto self = shared_from_this();
      io_service_.post([self]() { self->initiate_write(); });
    }
}

void http2_handler::initiate_write() {
  write_signaled_ = false;
  writefun_();
}

void http2_handler::stream_error(int32_t stream_id, uint32_t error_code) {
  ::nghttp2::asio_http2::server::stream_error(session_, stream_id, error_code);
  signal_write();
}

void http2_handler::resume(asio_server_stream &strm) {
  nghttp2_session_resume_data(session_, strm.get_stream_id());
  signal_write();
}

asio_server_response* http2_handler::push_promise(boost::system::error_code &ec,
                                      asio_server_stream &strm, std::string method,
                                      std::string raw_path_query,
                                      header_map h) {
  int rv;

  ec.clear();

  auto &req = strm.request();

  auto nva = std::vector<nghttp2_nv>();
  nva.reserve(4 + h.size());
  nva.push_back(nghttp2::http2::make_nv_ls(":method", method));
  nva.push_back(nghttp2::http2::make_nv_ls(":scheme", req.uri().scheme));
  nva.push_back(nghttp2::http2::make_nv_ls(":authority", req.uri().host));
  nva.push_back(nghttp2::http2::make_nv_ls(":path", raw_path_query));

  for (auto &hd : h) {
    nva.push_back(nghttp2::http2::make_nv(hd.first, hd.second.value,
                                          hd.second.sensitive));
  }

  rv = nghttp2_submit_push_promise(session_, NGHTTP2_FLAG_NONE,
                                   strm.get_stream_id(), nva.data(), nva.size(),
                                   nullptr);

  if (rv < 0) {
    ec = make_error_code(static_cast<nghttp2_error>(rv));
    return nullptr;
  }

  auto promised_strm = create_stream(rv);
  auto &promised_req = promised_strm->request();
  promised_req.header(std::move(h));
  promised_req.method(std::move(method));

  auto &uref = promised_req.uri();
  uref.scheme = req.uri().scheme;
  uref.host = req.uri().host;
  split_path(uref, std::begin(raw_path_query), std::end(raw_path_query));

  auto &promised_res = promised_strm->response();
  promised_res.pushed(true);

  signal_write();

  return &promised_strm->response();
}


} // namespace server

} // namespace asio_http2

} // namespace nghttp2
