/*
 * nghttp2 - HTTP/2 C Library
 *
 * Copyright (c) 2015 British Broadcasting Corporation
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
#include "h2load_http1_session.h"

#include <cassert>
#include <cerrno>

#include "h2load.h"
#include "h2load_Config.h"
#include "h2load_Client.h"
#include "h2load_Worker.h"

#include "util.h"
#include "template.h"

#include <iostream>
#include <fstream>

using namespace nghttp2;

namespace h2load
{

namespace
{
// HTTP response message begin
int htp_msg_begincb(llhttp_t* htp)
{
    auto session = static_cast<Http1Session*>(htp->data);

    if (session->stream_resp_counter_ > session->stream_req_counter_)
    {
        return -1;
    }

    return 0;
}
} // namespace

namespace
{
// HTTP response status code
int htp_statuscb(llhttp_t* htp, const char* at, size_t length)
{
    auto session = static_cast<Http1Session*>(htp->data);
    auto client = session->get_client();

    if (htp->status_code / 100 == 1)
    {
        return 0;
    }

    client->on_status_code(session->stream_resp_counter_, htp->status_code);

    return 0;
}
} // namespace

namespace
{
// HTTP response message complete
int htp_msg_completecb(llhttp_t* htp)
{
    auto session = static_cast<Http1Session*>(htp->data);
    auto client = session->get_client();

    if (htp->status_code / 100 == 1)
    {
        return 0;
    }

    client->final = llhttp_should_keep_alive(htp) == 0;
    auto req_stat = client->get_req_stat(session->stream_resp_counter_);

    assert(req_stat);

    auto config = client->worker->config;
    if (req_stat->data_offset >= config->data_length)
    {
        client->on_stream_close(session->stream_resp_counter_, true, client->final);
    }

    session->stream_resp_counter_ += 2;

    if (client->final)
    {
        session->stream_req_counter_ = session->stream_resp_counter_;

        // Connection is going down.  If we have still request to do,
        // create new connection and keep on doing the job.
        if (client->req_left)
        {
            client->try_new_connection();
        }

        return HPE_PAUSED;
    }

    return 0;
}
} // namespace

namespace
{
int htp_hdr_keycb(llhttp_t* htp, const char* data, size_t len)
{
    auto session = static_cast<Http1Session*>(htp->data);
    auto client = session->get_client();

    client->worker->stats.bytes_head += len;
    client->worker->stats.bytes_head_decomp += len;
    return 0;
}
} // namespace

namespace
{
int htp_hdr_valcb(llhttp_t* htp, const char* data, size_t len)
{
    auto session = static_cast<Http1Session*>(htp->data);
    auto client = session->get_client();

    client->worker->stats.bytes_head += len;
    client->worker->stats.bytes_head_decomp += len;
    return 0;
}
} // namespace

namespace
{
int htp_hdrs_completecb(llhttp_t* htp)
{
    return !http2::expect_response_body(htp->status_code);
}
} // namespace

namespace
{
int htp_body_cb(llhttp_t* htp, const char* data, size_t len)
{
    auto session = static_cast<Http1Session*>(htp->data);
    auto client = session->get_client();
    client->on_data_chunk(session->stream_resp_counter_, (const uint8_t*)data, len);

    client->record_ttfb();
    client->worker->stats.bytes_body += len;

    return 0;
}
} // namespace

namespace
{
constexpr llhttp_settings_t htp_hooks =
{
    htp_msg_begincb,     // llhttp_cb      on_message_begin;
    nullptr,             // llhttp_data_cb on_url;
    htp_statuscb,        // llhttp_data_cb on_status;
    htp_hdr_keycb,       // llhttp_data_cb on_header_field;
    htp_hdr_valcb,       // llhttp_data_cb on_header_value;
    htp_hdrs_completecb, // llhttp_cb      on_headers_complete;
    htp_body_cb,         // llhttp_data_cb on_body;
    htp_msg_completecb,  // llhttp_cb      on_message_complete;
    nullptr,             // llhttp_cb      on_chunk_header
    nullptr,             // llhttp_cb      on_chunk_complete
};
} // namespace

Http1Session::Http1Session(Client* client)
    : stream_req_counter_(1),
      stream_resp_counter_(1),
      client_(client),
      htp_(),
      complete_(false)
{
    llhttp_init(&htp_, HTTP_RESPONSE, &htp_hooks);
    htp_.data = this;
}

Http1Session::~Http1Session() {}

void Http1Session::on_connect()
{
    client_->signal_write();
}

int Http1Session::submit_request()
{
    auto config = client_->worker->config;
    if (config->json_config_schema.scenario.size())
    {
        return _submit_request();
    }

    const auto& req = config->h1reqs[client_->reqidx];
    client_->reqidx++;

    if (client_->reqidx == config->h1reqs.size())
    {
        client_->reqidx = 0;
    }

    client_->on_request_start(stream_req_counter_);

    auto req_stat = client_->get_req_stat(stream_req_counter_);

    client_->record_request_time(req_stat);
    client_->wb.append(req);

    if (config->data_fd == -1 || config->data_length == 0)
    {
        // increment for next request
        stream_req_counter_ += 2;

        return 0;
    }

    if (config->nclients > 1)
    {
        std::random_device                  rand_dev;
        std::mt19937                        generator(rand_dev());
        std::uniform_int_distribution<uint64_t>  distr(config->json_config_schema.variable_range_start,
                                                       config->json_config_schema.variable_range_end);
        client_->curr_req_variable_value = distr(generator);
    }
    else
    {
        client_->curr_req_variable_value = config->json_config_schema.variable_range_start;
    }

    return on_write();
}

int Http1Session::on_read(const uint8_t* data, size_t len)
{
    auto htperr =
        llhttp_execute(&htp_, reinterpret_cast<const char*>(data), len);
    auto nread = htperr == HPE_OK
                 ? len
                 : static_cast<size_t>(reinterpret_cast<const uint8_t*>(
                                           llhttp_get_error_pos(&htp_)) -
                                       data);

    if (client_->worker->config->verbose)
    {
        std::cout.write(reinterpret_cast<const char*>(data), nread);
    }

    if (htperr == HPE_PAUSED)
    {
        // pause is done only when connection: close is requested
        return -1;
    }

    if (htperr != HPE_OK)
    {
        std::cerr << "[ERROR] HTTP parse error: "
                  << "(" << llhttp_errno_name(htperr) << ") "
                  << llhttp_get_error_reason(&htp_) << std::endl;
        return -1;
    }

    return 0;
}

int Http1Session::on_write()
{
    if (complete_)
    {
        return -1;
    }

    auto config = client_->worker->config;
    if (config->json_config_schema.scenario.size())
    {
        return _on_write();
    }

    auto req_stat = client_->get_req_stat(stream_req_counter_);
    if (!req_stat)
    {
        return 0;
    }

    if (req_stat->data_offset < config->data_length)
    {
        auto req_stat = client_->get_req_stat(stream_req_counter_);
        auto& wb = client_->wb;

        // TODO unfortunately, wb has no interface to use with read(2)
        // family functions.
        std::array<uint8_t, 16_k> buf;

        ssize_t nread;
        while ((nread = pread(config->data_fd, buf.data(), buf.size(),
                              req_stat->data_offset)) == -1 &&
               errno == EINTR)
            ;

        if (nread == -1)
        {
            return -1;
        }

        req_stat->data_offset += nread;

        wb.append(buf.data(), nread);

        if (client_->worker->config->verbose)
        {
            std::cout << "[send " << nread << " byte(s)]" << std::endl;
        }

        if (req_stat->data_offset == config->data_length)
        {
            // increment for next request
            stream_req_counter_ += 2;

            if (stream_resp_counter_ == stream_req_counter_)
            {
                // Response has already been received
                client_->on_stream_close(stream_resp_counter_ - 2, true,
                                         client_->final);
            }
        }
    }

    return 0;
}

int Http1Session::_submit_request()
{
    h2load::Request_Data data = client_->get_request_to_submit();
    auto config = client_->worker->config;
    std::string req;
    req.append(data.method).append(" ").append(data.path).append(" HTTP/1.1\r\n");
    req.append("Host: ").append(data.authority).append("\r\n");

    for (auto& header : data.req_headers)
    {
        if (header.first == ":path" || header.first == ":scheme" || header.first == ":authority" || header.first == ":method")
        {
            continue;
        }
        req.append(header.first);
        req.append(": ");
        req.append(header.second);
        req.append("\r\n");
    }
    req += "\r\n";

    if (config->verbose)
    {
        std::cout<<"sending headers:"<<req<<std::endl;
    }

    client_->on_request_start(stream_req_counter_);

    auto req_stat = client_->get_req_stat(stream_req_counter_);

    client_->record_request_time(req_stat);
    client_->wb.append(req);

    client_->requests_awaiting_response[stream_req_counter_] = data;

    if (data.req_payload.empty())
    {
        // increment for next request
        stream_req_counter_ += 2;

        return 0;
    }

    return on_write();
}

int Http1Session::_on_write()
{
    if (complete_)
    {
        return -1;
    }

    auto config = client_->worker->config;
    auto req_stat = client_->get_req_stat(stream_req_counter_);
    if (!req_stat)
    {
        return 0;
    }
    auto request = client_->requests_awaiting_response.find(stream_req_counter_);
    assert(request != client_->requests_awaiting_response.end());
    std::string& stream_buffer = client_->requests_awaiting_response[stream_req_counter_].req_payload;

    if (!stream_buffer.empty())
    {
        auto& wb = client_->wb;
        size_t send_size = stream_buffer.size() > 16_k ? 16_k : stream_buffer.size();

        wb.append(stream_buffer.c_str(), send_size);

        if (client_->worker->config->verbose)
        {
            std::cout << "[send " << send_size << " byte(s)]" << std::endl;
        }

        if (send_size < stream_buffer.size())
        {
            stream_buffer = stream_buffer.substr(send_size, std::string::npos);
        }
        else
        {
            // increment for next request
            stream_req_counter_ += 2;

            if (stream_resp_counter_ == stream_req_counter_)
            {
                // Response has already been received
                client_->on_stream_close(stream_resp_counter_ - 2, true,
                                         client_->final);
            }
        }
    }
    return 0;
}


void Http1Session::terminate()
{
    complete_ = true;
}

Client* Http1Session::get_client()
{
    return client_;
}

size_t Http1Session::max_concurrent_streams()
{
    auto config = client_->worker->config;

    return config->data_fd == -1 ? config->max_concurrent_streams : 1;
}

} // namespace h2load
