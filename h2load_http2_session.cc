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
#include "h2load_http2_session.h"

#include <cassert>
#include <cerrno>
#include <iostream>
#include <string>
#include <cstring>
#include <stdlib.h>
#include <regex>

#include "h2load.h"
#include "h2load_Config.h"
#include "base_worker.h"
#include "base_client.h"


#include "util.h"
#include "template.h"

using namespace nghttp2;

namespace h2load
{

Http2Session::Http2Session(base_client* client)
    : client_(client),
    session_(nullptr),
    curr_stream_id(0),
    config(client->get_config()),
    stats(client->get_stats()),
    request_map(client->requests_waiting_for_response())
{

}

Http2Session::~Http2Session()
{
    nghttp2_session_del(session_);
}

namespace
{
int on_header_callback(nghttp2_session* session, const nghttp2_frame* frame,
                       const uint8_t* name, size_t namelen,
                       const uint8_t* value, size_t valuelen, uint8_t flags,
                       void* user_data)
{
    auto client = static_cast<base_client*>(user_data);
    if (frame->hd.type != NGHTTP2_HEADERS ||
        (frame->headers.cat != NGHTTP2_HCAT_RESPONSE &&
         frame->headers.cat != NGHTTP2_HCAT_HEADERS))
    {
        return 0;
    }
    client->on_header(frame->hd.stream_id, name, namelen, value, valuelen);
    client->get_stats().bytes_head_decomp += namelen + valuelen;

    if (client->get_config()->verbose)
    {
        std::cout << "[stream_id=" << frame->hd.stream_id << "] ";
        std::cout.write(reinterpret_cast<const char*>(name), namelen);
        std::cout << ": ";
        std::cout.write(reinterpret_cast<const char*>(value), valuelen);
        std::cout << "\n";
    }

    return 0;
}
} // namespace

namespace
{
int on_frame_recv_callback(nghttp2_session* session, const nghttp2_frame* frame,
                           void* user_data)
{
    auto client = static_cast<base_client*>(user_data);
    if (frame->hd.type != NGHTTP2_HEADERS ||
        (frame->headers.cat != NGHTTP2_HCAT_RESPONSE &&
         frame->headers.cat != NGHTTP2_HCAT_HEADERS))
    {
        return 0;
    }

    client->get_stats().bytes_head +=
        frame->hd.length - frame->headers.padlen -
        ((frame->hd.flags & NGHTTP2_FLAG_PRIORITY) ? 5 : 0);
    if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM)
    {
        client->record_ttfb();
    }
    return 0;
}
} // namespace

namespace
{

int on_begin_header_callback(nghttp2_session *session,
                                       const nghttp2_frame *frame,
                                       void *user_data)
{
    auto client = static_cast<base_client*>(user_data);
    if (frame->hd.type != NGHTTP2_HEADERS ||
        (frame->headers.cat != NGHTTP2_HCAT_RESPONSE &&
         frame->headers.cat != NGHTTP2_HCAT_HEADERS))
    {
        return 0;
    }

    client->on_header_frame_begin(frame->hd.stream_id, frame->hd.flags);
    return 0;
}
}

namespace
{
int on_data_chunk_recv_callback(nghttp2_session* session, uint8_t flags,
                                int32_t stream_id, const uint8_t* data,
                                size_t len, void* user_data)
{
    auto client = static_cast<base_client*>(user_data);
    client->on_data_chunk(stream_id, data, len);
    client->record_ttfb();
    client->get_stats().bytes_body += len;
    return 0;
}
} // namespace

namespace
{
int on_stream_close_callback(nghttp2_session* session, int32_t stream_id,
                             uint32_t error_code, void* user_data)
{
    auto client = static_cast<base_client*>(user_data);
    client->on_stream_close(stream_id, error_code == NGHTTP2_NO_ERROR);
    return 0;
}
} // namespace

namespace
{
int before_frame_send_callback(nghttp2_session* session,
                               const nghttp2_frame* frame, void* user_data)
{
    if (frame->hd.type != NGHTTP2_HEADERS ||
        frame->headers.cat != NGHTTP2_HCAT_REQUEST)
    {
        return 0;
    }

    auto client = static_cast<base_client*>(user_data);
    auto req_stat = client->get_req_stat(frame->hd.stream_id);
    assert(req_stat);
    client->record_request_time(req_stat);

    return 0;
}
} // namespace

namespace
{
ssize_t file_read_callback(nghttp2_session* session, int32_t stream_id,
                           uint8_t* buf, size_t length, uint32_t* data_flags,
                           nghttp2_data_source* source, void* user_data)
{
    auto client = static_cast<base_client*>(user_data);
    auto req_stat = client->get_req_stat(stream_id);
    assert(req_stat);
    auto config = client->get_config();
    auto size_left = config->payload_data.size() - req_stat->data_offset;
    auto nread = size_left > length ? length : size_left;
    std::memcpy(buf, (uint8_t*)config->payload_data.c_str() + req_stat->data_offset, nread);

    req_stat->data_offset += nread;

    if (req_stat->data_offset == config->data_length)
    {
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
        return nread;
    }

    if (req_stat->data_offset > config->data_length || nread == 0)
    {
        return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
    }

    return nread;
}
} // namespace

ssize_t buffer_read_callback(nghttp2_session* session, int32_t stream_id,
                             uint8_t* buf, size_t length, uint32_t* data_flags,
                             nghttp2_data_source* source, void* user_data)
{
    auto client = static_cast<base_client*>(user_data);
    auto config = client->get_config();
    auto& request_map = client->requests_waiting_for_response();
    auto request = request_map.find(stream_id);
    assert(request != request_map.end());
    std::string& stream_buffer = *(request->second.req_payload);

    if (config->verbose)
    {
        std::cout << "sending data:" << stream_buffer << std::endl;
    }

    if (request->second.req_payload_cursor < stream_buffer.size())
    {
        if (length >= (stream_buffer.size() - request->second.req_payload_cursor))
        {
            memcpy(buf, (stream_buffer.c_str() + request->second.req_payload_cursor),
                        (stream_buffer.size() - request->second.req_payload_cursor));
            *data_flags |= NGHTTP2_DATA_FLAG_EOF;
            size_t buf_size = (stream_buffer.size() - request->second.req_payload_cursor);
            request->second.req_payload_cursor = stream_buffer.size();
            return buf_size;
        }
        else
        {
            memcpy(buf, stream_buffer.c_str(), length);
            request->second.req_payload_cursor += length;
            return length;
        }
    }
    else
    {
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
        return 0;
    }
}

namespace
{
ssize_t send_callback(nghttp2_session* session, const uint8_t* data,
                      size_t length, int flags, void* user_data)
{
    auto client = static_cast<base_client*>(user_data);

    return client->push_data_to_output_buffer(data, length);
}
} // namespace

void Http2Session::on_connect()
{
    int rv;

    // This is required with --disable-assert.
    (void)rv;

    nghttp2_session_callbacks* callbacks;

    nghttp2_session_callbacks_new(&callbacks);

    auto callbacks_deleter = defer(nghttp2_session_callbacks_del, callbacks);

    nghttp2_session_callbacks_set_on_begin_headers_callback(callbacks,
                                                         on_begin_header_callback);

    nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks,
                                                         on_frame_recv_callback);

    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(
        callbacks, on_data_chunk_recv_callback);

    nghttp2_session_callbacks_set_on_stream_close_callback(
        callbacks, on_stream_close_callback);

    nghttp2_session_callbacks_set_on_header_callback(callbacks,
                                                     on_header_callback);

    nghttp2_session_callbacks_set_before_frame_send_callback(
        callbacks, before_frame_send_callback);

    nghttp2_session_callbacks_set_send_callback(callbacks, send_callback);

    nghttp2_option* opt;

    rv = nghttp2_option_new(&opt);
    assert(rv == 0);

    //nghttp2_option_set_no_http_messaging(opt, 1);

    if (config->encoder_header_table_size != NGHTTP2_DEFAULT_HEADER_TABLE_SIZE)
    {
        nghttp2_option_set_max_deflate_dynamic_table_size(
            opt, config->encoder_header_table_size);
    }

    nghttp2_session_client_new2(&session_, callbacks, client_, opt);

    nghttp2_option_del(opt);

    std::array<nghttp2_settings_entry, 3> iv;
    size_t niv = 2;
    iv[0].settings_id = NGHTTP2_SETTINGS_ENABLE_PUSH;
    iv[0].value = 0;
    iv[1].settings_id = NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE;
    iv[1].value = (1 << config->window_bits) - 1;

    if (config->header_table_size != NGHTTP2_DEFAULT_HEADER_TABLE_SIZE)
    {
        iv[niv].settings_id = NGHTTP2_SETTINGS_HEADER_TABLE_SIZE;
        iv[niv].value = config->header_table_size;
        ++niv;
    }
    if (config->max_frame_size != 16_k)
    {
        iv[niv].settings_id = NGHTTP2_SETTINGS_MAX_FRAME_SIZE;
        iv[niv].value = config->max_frame_size;
        ++niv;
    }
    rv = nghttp2_submit_settings(session_, NGHTTP2_FLAG_NONE, iv.data(), niv);

    assert(rv == 0);

    auto connection_window = (1 << config->connection_window_bits) - 1;
    nghttp2_session_set_local_window_size(session_, NGHTTP2_FLAG_NONE, 0,
                                          connection_window);
    client_->signal_write();
}

int Http2Session::submit_request()
{
    if (nghttp2_session_check_request_allowed(session_) == 0)
    {
        return -1;
    }

    if (config->json_config_schema.scenarios.size())
    {
        return _submit_request();
    }

    auto& nva = config->nva[client_->get_current_req_index()++];

    if (client_->get_current_req_index() == config->nva.size())
    {
        client_->get_current_req_index() = 0;
    }

    nghttp2_data_provider prd {{0}, file_read_callback};

    auto stream_id =
        nghttp2_submit_request(session_, nullptr, nva.data(), nva.size(),
                               config->data_length ? &prd : nullptr, nullptr);
    if (stream_id < 0)
    {
        return -1;
    }

    client_->on_request_start(stream_id);

    return 0;
}

int Http2Session::on_read(const uint8_t* data, size_t len)
{
    auto rv = nghttp2_session_mem_recv(session_, data, len);
    if (rv < 0)
    {
        return -1;
    }

    assert(static_cast<size_t>(rv) == len);

    if (nghttp2_session_want_read(session_) == 0 &&
        nghttp2_session_want_write(session_) == 0 && !client_->any_pending_data_to_write())
    {
        return -1;
    }

    client_->signal_write();

    return 0;
}

int Http2Session::on_write()
{
    auto rv = nghttp2_session_send(session_);
    if (rv != 0)
    {
        return -1;
    }

    if (nghttp2_session_want_read(session_) == 0 &&
        nghttp2_session_want_write(session_) == 0 && !client_->any_pending_data_to_write())
    {
        return -1;
    }

    return 0;
}

void Http2Session::terminate()
{
    nghttp2_session_terminate_session(session_, NGHTTP2_NO_ERROR);
    client_->signal_write();
}

size_t Http2Session::max_concurrent_streams()
{
    return (size_t)config->max_concurrent_streams;
}

void Http2Session::submit_rst_stream(int32_t stream_id)
{
    nghttp2_submit_rst_stream(session_, NGHTTP2_FLAG_END_STREAM, stream_id, NGHTTP2_STREAM_CLOSED);
}

int Http2Session::_submit_request()
{
    if (nghttp2_session_check_request_allowed(session_) == 0)
    {
        return -1;
    }

    // tear down the client which is approaching max stream, and reconnect again
    if ((config->nclients > 1 && config->rps > 0 &&
         INT32_MAX - curr_stream_id < 64 * config->nclients * config->rps &&
         rand() % config->nclients == 0) ||
        (curr_stream_id >= INT32_MAX - 1))
    {
        return MAX_STREAM_TO_BE_EXHAUSTED;
    }

    nghttp2_data_provider prd {{0}, buffer_read_callback};

    std::vector<nghttp2_nv> http2_nvs;
    auto data = std::move(client_->get_request_to_submit());
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
        if (data.req_headers_of_individual.count(header.first))
        {
            continue;
        }
        if (header.first == path_header || header.first == scheme_header || header.first == authority_header
            || header.first == method_header)
        {
            continue;
        }
        http2_nvs.emplace_back(http2::make_nv(header.first, header.second, false));
    }

    for (auto& header : data.req_headers_of_individual)
    {
        if (header.first == path_header || header.first == scheme_header || header.first == authority_header
            || header.first == method_header)
        {
            continue;
        }
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

    int32_t stream_id =
        nghttp2_submit_request(session_, nullptr, http2_nvs.data(), http2_nvs.size(),
                               data.req_payload->empty() ? nullptr : &prd, nullptr);
    if (stream_id < 0)
    {
        return -1;
    }

    curr_stream_id = stream_id;
    request_map.insert(std::make_pair(stream_id, std::move(data)));
    client_->on_request_start(stream_id);
    return 0;
}

void Http2Session::submit_ping()
{
    nghttp2_submit_ping(session_, NGHTTP2_FLAG_NONE, NULL);
}

} // namespace h2load
