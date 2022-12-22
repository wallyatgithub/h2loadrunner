/*
 * nghttp2 - HTTP/2 C Library
 *
 * Copyright (c) 2019 nghttp2 contributors
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
#include "h2load_http3_session.h"

#include <iostream>

#include <ngtcp2/ngtcp2.h>
#include "base_worker.h"

#include "h2load.h"

namespace h2load
{

Http3Session::Http3Session(base_client* client)
    : client_(client), conn_(nullptr), npending_request_(0), reqidx_(0) {}

Http3Session::~Http3Session()
{
    nghttp3_conn_del(conn_);
}

void Http3Session::on_connect() {}

int Http3Session::submit_request()
{
    if (npending_request_)
    {
        ++npending_request_;
        return 0;
    }

    auto config = client_->worker->config;
    reqidx_ = client_->get_controller_client()->reqidx;

    if (++client_->get_controller_client()->reqidx == config->nva.size())
    {
        client_->get_controller_client()->reqidx = 0;
    }

    auto stream_id = submit_request_internal();
    if (stream_id < 0)
    {
        if (stream_id == NGTCP2_ERR_STREAM_ID_BLOCKED)
        {
            ++npending_request_;
            return 0;
        }
        return -1;
    }

    return 0;
}

namespace
{
nghttp3_ssize read_data(nghttp3_conn* conn, int64_t stream_id, nghttp3_vec* vec,
                        size_t veccnt, uint32_t* pflags, void* user_data,
                        void* stream_user_data)
{
    auto s = static_cast<Http3Session*>(user_data);

    s->read_data(vec, veccnt, pflags);

    return 1;
}
} // namespace

namespace
{
nghttp3_ssize _read_data(nghttp3_conn* conn, int64_t stream_id, nghttp3_vec* vec,
                         size_t veccnt, uint32_t* pflags, void* user_data,
                         void* stream_user_data)
{
    auto s = static_cast<Http3Session*>(user_data);

    s->_read_data(stream_id, vec, veccnt, pflags);

    return 1;
}
} // namespace

void Http3Session::_read_data(int64_t stream_id, nghttp3_vec* vec, size_t veccnt,
                              uint32_t* pflags)
{
    assert(veccnt > 0);
    auto config = client_->worker->config;

    auto& request = client_->get_request_response_data(stream_id);
    static std::string empty_str;
    std::string& stream_buffer = request ? *(request->req_payload) : empty_str;

    if (config->verbose)
    {
        std::cout << "sending data:" << stream_buffer << std::endl;
    }

    vec[0].base = (uint8_t*)(stream_buffer.c_str());
    vec[0].len = stream_buffer.size();
    *pflags |= NGHTTP3_DATA_FLAG_EOF;
}

void Http3Session::read_data(nghttp3_vec* vec, size_t veccnt,
                             uint32_t* pflags)
{
    assert(veccnt > 0);

    auto config = client_->worker->config;

    vec[0].base = (uint8_t*)(config->payload_data.c_str());
    vec[0].len = config->payload_data.size();
    *pflags |= NGHTTP3_DATA_FLAG_EOF;
}

int64_t Http3Session::submit_request_internal()
{
    auto config = client_->worker->config;

    if (config->json_config_schema.scenarios.size())
    {
        return _submit_request();
    }

    int rv;
    int64_t stream_id;

    auto& nva = config->nva[reqidx_];

    rv = ngtcp2_conn_open_bidi_stream(client_->quic.conn, &stream_id, nullptr);
    if (rv != 0)
    {
        return rv;
    }

    nghttp3_data_reader dr{};
    dr.read_data = h2load::read_data;

    rv = nghttp3_conn_submit_request(
             conn_, stream_id, reinterpret_cast<nghttp3_nv*>(nva.data()), nva.size(),
             config->data_length ? &dr : nullptr, nullptr);
    if (rv != 0)
    {
        return rv;
    }

    client_->on_request_start(stream_id);
    auto req_stat = client_->get_req_stat(stream_id);
    assert(req_stat);
    client_->record_request_time(req_stat);

    return stream_id;
}

int Http3Session::on_read(const uint8_t* data, size_t len)
{
    return -1;
}

int Http3Session::on_write()
{
    return -1;
}

void Http3Session::terminate()
{
    client_->request_connection_close();
}

size_t Http3Session::max_concurrent_streams()
{
    return (size_t)client_->worker->config->max_concurrent_streams;
}

namespace
{
int stream_close(nghttp3_conn* conn, int64_t stream_id, uint64_t app_error_code,
                 void* user_data, void* stream_user_data)
{
    auto s = static_cast<Http3Session*>(user_data);
    if (s->stream_close(stream_id, app_error_code) != 0)
    {
        return NGHTTP3_ERR_CALLBACK_FAILURE;
    }
    return 0;
}
} // namespace

int Http3Session::stream_close(int64_t stream_id, uint64_t app_error_code)
{
    if (!ngtcp2_is_bidi_stream(stream_id))
    {
        assert(!ngtcp2_conn_is_local_stream(client_->quic.conn, stream_id));
        ngtcp2_conn_extend_max_streams_uni(client_->quic.conn, 1);
    }
    client_->on_stream_close(stream_id, app_error_code == NGHTTP3_H3_NO_ERROR);
    return 0;
}

namespace
{
int recv_data(nghttp3_conn* conn, int64_t stream_id, const uint8_t* data,
              size_t datalen, void* user_data, void* stream_user_data)
{
    auto s = static_cast<Http3Session*>(user_data);
    s->recv_data(stream_id, data, datalen);
    return 0;
}
} // namespace

void Http3Session::recv_data(int64_t stream_id, const uint8_t* data,
                             size_t datalen)
{
    client_->record_ttfb();
    client_->worker->stats.bytes_body += datalen;
    client_->on_data_chunk(stream_id, data, datalen);
    consume(stream_id, datalen);
}

namespace
{
int deferred_consume(nghttp3_conn* conn, int64_t stream_id, size_t nconsumed,
                     void* user_data, void* stream_user_data)
{
    auto s = static_cast<Http3Session*>(user_data);
    s->consume(stream_id, nconsumed);
    return 0;
}
} // namespace

void Http3Session::consume(int64_t stream_id, size_t nconsumed)
{
    ngtcp2_conn_extend_max_stream_offset(client_->quic.conn, stream_id,
                                         nconsumed);
    ngtcp2_conn_extend_max_offset(client_->quic.conn, nconsumed);
}

namespace
{
int begin_headers(nghttp3_conn* conn, int64_t stream_id, void* user_data,
                  void* stream_user_data)
{
    auto s = static_cast<Http3Session*>(user_data);
    s->begin_headers(stream_id);
    return 0;
}
} // namespace

void Http3Session::begin_headers(int64_t stream_id)
{
    auto payloadlen = nghttp3_conn_get_frame_payload_left(conn_, stream_id);
    assert(payloadlen > 0);

    client_->worker->stats.bytes_head += payloadlen;
    client_->on_header_frame_begin(stream_id, 0);
}

namespace
{
int recv_header(nghttp3_conn* conn, int64_t stream_id, int32_t token,
                nghttp3_rcbuf* name, nghttp3_rcbuf* value, uint8_t flags,
                void* user_data, void* stream_user_data)
{
    auto s = static_cast<Http3Session*>(user_data);
    auto k = nghttp3_rcbuf_get_buf(name);
    auto v = nghttp3_rcbuf_get_buf(value);
    s->recv_header(stream_id, &k, &v);
    return 0;
}
} // namespace

void Http3Session::recv_header(int64_t stream_id, const nghttp3_vec* name,
                               const nghttp3_vec* value)
{
    client_->on_header(stream_id, name->base, name->len, value->base, value->len);
    client_->worker->stats.bytes_head_decomp += name->len + value->len;
}

namespace
{
int stop_sending(nghttp3_conn* conn, int64_t stream_id, uint64_t app_error_code,
                 void* user_data, void* stream_user_data)
{
    auto s = static_cast<Http3Session*>(user_data);
    if (s->stop_sending(stream_id, app_error_code) != 0)
    {
        return NGHTTP3_ERR_CALLBACK_FAILURE;
    }
    return 0;
}
} // namespace

int Http3Session::stop_sending(int64_t stream_id, uint64_t app_error_code)
{
    auto rv = ngtcp2_conn_shutdown_stream_read(client_->quic.conn, stream_id,
                                               app_error_code);
    if (rv != 0)
    {
        std::cerr << "ngtcp2_conn_shutdown_stream_read: " << ngtcp2_strerror(rv)
                  << std::endl;
        return -1;
    }
    return 0;
}

namespace
{
int reset_stream(nghttp3_conn* conn, int64_t stream_id, uint64_t app_error_code,
                 void* user_data, void* stream_user_data)
{
    auto s = static_cast<Http3Session*>(user_data);
    if (s->reset_stream(stream_id, app_error_code) != 0)
    {
        return NGHTTP3_ERR_CALLBACK_FAILURE;
    }
    return 0;
}
} // namespace

int Http3Session::reset_stream(int64_t stream_id, uint64_t app_error_code)
{
    auto rv = ngtcp2_conn_shutdown_stream_write(client_->quic.conn, stream_id,
                                                app_error_code);
    if (rv != 0)
    {
        std::cerr << "ngtcp2_conn_shutdown_stream_write: " << ngtcp2_strerror(rv)
                  << std::endl;
        return -1;
    }
    return 0;
}

int Http3Session::close_stream(int64_t stream_id, uint64_t app_error_code)
{
    auto rv = nghttp3_conn_close_stream(conn_, stream_id, app_error_code);
    switch (rv)
    {
        case 0:
            return 0;
        case NGHTTP3_ERR_STREAM_NOT_FOUND:
            if (!ngtcp2_is_bidi_stream(stream_id))
            {
                assert(!ngtcp2_conn_is_local_stream(client_->quic.conn, stream_id));
                ngtcp2_conn_extend_max_streams_uni(client_->quic.conn, 1);
            }
            return 0;
        default:
            return -1;
    }
}

int Http3Session::shutdown_stream_read(int64_t stream_id)
{
    auto rv = nghttp3_conn_shutdown_stream_read(conn_, stream_id);
    if (rv != 0)
    {
        return -1;
    }
    return 0;
}

int Http3Session::extend_max_local_streams()
{
    auto config = client_->worker->config;

    for (; npending_request_; --npending_request_)
    {
        auto stream_id = submit_request_internal();
        if (stream_id < 0)
        {
            if (stream_id == NGTCP2_ERR_STREAM_ID_BLOCKED)
            {
                return 0;
            }
            return -1;
        }

        if (++reqidx_ == config->nva.size())
        {
            reqidx_ = 0;
        }
    }

    return 0;
}

int Http3Session::init_conn()
{
    int rv;

    assert(conn_ == nullptr);

    if (ngtcp2_conn_get_max_local_streams_uni(client_->quic.conn) < 3)
    {
        return -1;
    }

    nghttp3_callbacks callbacks
    {
        nullptr, // acked_stream_data
        h2load::stream_close,
        h2load::recv_data,
        h2load::deferred_consume,
        h2load::begin_headers,
        h2load::recv_header,
        nullptr, // end_headers
        nullptr, // begin_trailers
        h2load::recv_header,
        nullptr, // end_trailers
        h2load::stop_sending,
        nullptr, // end_stream
        h2load::reset_stream,
        nullptr, // shutdown
    };

    auto config = client_->worker->config;

    nghttp3_settings settings;
    nghttp3_settings_default(&settings);
    settings.qpack_max_dtable_capacity = config->header_table_size;
    settings.qpack_blocked_streams = 100;

    auto mem = nghttp3_mem_default();

    rv = nghttp3_conn_client_new(&conn_, &callbacks, &settings, mem, this);
    if (rv != 0)
    {
        std::cerr << "nghttp3_conn_client_new: " << nghttp3_strerror(rv)
                  << std::endl;
        return -1;
    }

    int64_t ctrl_stream_id;

    rv =
        ngtcp2_conn_open_uni_stream(client_->quic.conn, &ctrl_stream_id, nullptr);
    if (rv != 0)
    {
        std::cerr << "ngtcp2_conn_open_uni_stream: " << ngtcp2_strerror(rv)
                  << std::endl;
        return -1;
    }

    rv = nghttp3_conn_bind_control_stream(conn_, ctrl_stream_id);
    if (rv != 0)
    {
        std::cerr << "nghttp3_conn_bind_control_stream: " << nghttp3_strerror(rv)
                  << std::endl;
        return -1;
    }

    int64_t qpack_enc_stream_id, qpack_dec_stream_id;

    rv = ngtcp2_conn_open_uni_stream(client_->quic.conn, &qpack_enc_stream_id,
                                     nullptr);
    if (rv != 0)
    {
        std::cerr << "ngtcp2_conn_open_uni_stream: " << ngtcp2_strerror(rv)
                  << std::endl;
        return -1;
    }

    rv = ngtcp2_conn_open_uni_stream(client_->quic.conn, &qpack_dec_stream_id,
                                     nullptr);
    if (rv != 0)
    {
        std::cerr << "ngtcp2_conn_open_uni_stream: " << ngtcp2_strerror(rv)
                  << std::endl;
        return -1;
    }

    rv = nghttp3_conn_bind_qpack_streams(conn_, qpack_enc_stream_id,
                                         qpack_dec_stream_id);
    if (rv != 0)
    {
        std::cerr << "nghttp3_conn_bind_qpack_streams: " << nghttp3_strerror(rv)
                  << std::endl;
        return -1;
    }

    return 0;
}

ssize_t Http3Session::read_stream(uint32_t flags, int64_t stream_id,
                                  const uint8_t* data, size_t datalen)
{
    auto nconsumed = nghttp3_conn_read_stream(
                         conn_, stream_id, data, datalen, flags & NGTCP2_STREAM_DATA_FLAG_FIN);
    if (nconsumed < 0)
    {
        std::cerr << "nghttp3_conn_read_stream: " << nghttp3_strerror(nconsumed)
                  << std::endl;
        ngtcp2_connection_close_error_set_application_error(
            &client_->quic.last_error,
            nghttp3_err_infer_quic_app_error_code(nconsumed), nullptr, 0);
        return -1;
    }
    return nconsumed;
}

ssize_t Http3Session::write_stream(int64_t& stream_id, int& fin,
                                   nghttp3_vec* vec, size_t veccnt)
{
    auto sveccnt =
        nghttp3_conn_writev_stream(conn_, &stream_id, &fin, vec, veccnt);
    if (sveccnt < 0)
    {
        ngtcp2_connection_close_error_set_application_error(
            &client_->quic.last_error,
            nghttp3_err_infer_quic_app_error_code(sveccnt), nullptr, 0);
        return -1;
    }
    return sveccnt;
}

void Http3Session::block_stream(int64_t stream_id)
{
    nghttp3_conn_block_stream(conn_, stream_id);
}

void Http3Session::shutdown_stream_write(int64_t stream_id)
{
    nghttp3_conn_shutdown_stream_write(conn_, stream_id);
}

int Http3Session::add_write_offset(int64_t stream_id, size_t ndatalen)
{
    auto rv = nghttp3_conn_add_write_offset(conn_, stream_id, ndatalen);
    if (rv != 0)
    {
        ngtcp2_connection_close_error_set_application_error(
            &client_->quic.last_error, nghttp3_err_infer_quic_app_error_code(rv),
            nullptr, 0);
        return -1;
    }
    return 0;
}

int Http3Session::add_ack_offset(int64_t stream_id, size_t datalen)
{
    auto rv = nghttp3_conn_add_ack_offset(conn_, stream_id, datalen);
    if (rv != 0)
    {
        ngtcp2_connection_close_error_set_application_error(
            &client_->quic.last_error, nghttp3_err_infer_quic_app_error_code(rv),
            nullptr, 0);
        return -1;
    }
    return 0;
}


int Http3Session::_submit_request()
{
    int64_t stream_id;

    auto config = client_->worker->config;

    if (ngtcp2_conn_open_bidi_stream(client_->quic.conn, &stream_id, nullptr) == NGTCP2_ERR_STREAM_ID_BLOCKED)
    {
        ++npending_request_;
        return 0;
    }

    nghttp3_data_reader dr{};
    dr.read_data = h2load::_read_data;

    std::vector<nghttp2_nv> http2_nvs;
    auto ptr = std::move(client_->get_request_to_submit());
    auto& data = *ptr;
    if (data.is_empty())
    {
        return -1;
    }

    http2_nvs.reserve(data.req_headers_from_config->size() + data.req_headers_of_individual.size() + 4);

    http2_nvs.emplace_back(http2::make_nv(path_header, *data.path, false));
    http2_nvs.emplace_back(http2::make_nv(scheme_header, *data.schema, false));
    http2_nvs.emplace_back(http2::make_nv(authority_header, *data.authority, false));
    http2_nvs.emplace_back(http2::make_nv(method_header, *data.method, false));

    for (auto& header : *data.req_headers_from_config)
    {
        /*
            if (reserved_headers.count(header.first))
            {
                continue;
            }
        */

        if (data.req_headers_of_individual.count(header.first))
        {
            continue;
        }

        http2_nvs.emplace_back(http2::make_nv(header.first, header.second, false));
    }

    for (auto& header : data.req_headers_of_individual)
    {
        /*
            if (reserved_headers.count(header.first))
            {
                continue;
            }
        */

        http2_nvs.emplace_back(http2::make_nv(header.first, header.second, false));
    }

    if (config->verbose)
    {
        //std::cout<<std::endl<<"Dump request to send: "<<std::endl<<data<<std::endl;
        std::cout << "sending headers:" << std::endl;
        for (auto nv : http2_nvs)
        {
            std::string name((const char*)nv.name, nv.namelen);
            std::string value((const char*)nv.value, nv.valuelen);
            std::cout << name << ":" << value << std::endl;
        }
    }

    auto rv = nghttp3_conn_submit_request(
                  conn_, stream_id, (nghttp3_nv*)http2_nvs.data(), http2_nvs.size(),
                  data.req_payload->size() ? &dr : nullptr, nullptr);
    if (rv != 0)
    {
        return rv;
    }

    curr_stream_id = stream_id;
    client_->on_request_start(stream_id, ptr);
    auto req_stat = client_->get_req_stat(stream_id);
    assert(req_stat);
    client_->record_request_time(req_stat);
    return 0;
}

void Http3Session::submit_ping() // TODO:
{
    //nghttp2_submit_ping(session_, NGHTTP2_FLAG_NONE, NULL);
}

void Http3Session::reset_stream(int64_t stream_id)
{
    ngtcp2_conn_shutdown_stream(client_->quic.conn, stream_id, NGHTTP3_H3_REQUEST_CANCELLED);

    client_->on_stream_close(stream_id, false);
}


} // namespace h2load
