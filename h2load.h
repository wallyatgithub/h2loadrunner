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
#ifndef H2LOAD_H
#define H2LOAD_H
#include <string>
#include <map>
#include <set>
#include <iostream>
#include <vector>
#include "h2load_Cookie.h"
#include "http2.h"
#ifdef USE_LIBEV
#include "memchunk.h"
#endif
#include "template.h"
#include "common_types.h"
#include "config_schema.h"

#ifdef ENABLE_HTTP3
#  include <ngtcp2/ngtcp2.h>
#  include <ngtcp2/ngtcp2_crypto.h>
#endif // ENABLE_HTTP3

#ifdef ENABLE_HTTP3
#  include "quic.h"
#endif // ENABLE_HTTP3


using namespace nghttp2;

namespace h2load
{

const std::string scheme_header = ":scheme";
const std::string path_header = ":path";
const std::string authority_header = ":authority";
const std::string method_header = ":method";
const std::string host_header = "host";

static std::set<std::string> reserved_headers =
{
    scheme_header,
    path_header,
    authority_header,
    method_header,
    host_header
};

const std::string x_envoy_original_dst_host_header = "x-envoy-original-dst-host";

const std::string x_proto_to_use = "x-proto";

const int SSL_EXT_DATA_INDEX_KEYLOG_FILE = 128;

static std::string emptyString;

class base_client;

struct Scenario_Data_Per_User
{
    std::vector<std::string> variable_index_to_value;
    std::map<std::string, Cookie, std::greater<std::string>> saved_cookies;
    std::vector<uint64_t> user_ids;

    Scenario_Data_Per_User(const std::vector<uint64_t>& u_ids): variable_index_to_value(u_ids.size(), ""), user_ids(u_ids)
    {
    }
    friend std::ostream& operator<<(std::ostream& o, const Scenario_Data_Per_User& data)
    {
        for (size_t id = 0; id < data.user_ids.size(); id++)
        {
            o << "variable id: " << id
              << ", variable cursor:" << data.user_ids[id]
              << ", variable string value:" << data.variable_index_to_value[id]
              << std::endl;
        }
        for (auto& c : data.saved_cookies)
        {
            o << "cookie key: " << c.first
              << "cookie value: " << c.second
              << std::endl;
        }
        return o;
    }

};

struct Request_Response_Data
{
    std::string* schema;
    std::string* authority;
    std::string* req_payload;
    std::string* path;
    std::string* method;
    PROTO_TYPE proto_type;
    size_t req_payload_cursor;
    std::map<std::string, std::string, ci_less>* req_headers_from_config;
    std::map<std::string, std::string, ci_less> req_headers_of_individual;
    std::string resp_payload;
    std::vector<std::map<std::string, std::string, ci_less>> resp_headers;
    bool resp_trailer_present = false;
    uint16_t status_code;
    uint16_t expected_status_code;
    uint32_t delay_before_executing_next;
    size_t curr_request_idx;
    size_t scenario_index;
    std::vector<std::string> string_collection;
    std::function<void(int64_t, h2load::base_client*)> request_sent_callback;
    uint32_t stream_timeout_in_ms;
    std::shared_ptr<Scenario_Data_Per_User> scenario_data_per_user;

    void init()
    {
        schema = &emptyString;
        authority = &emptyString;
        req_payload = &emptyString;
        path = &emptyString;
        method = &emptyString;
        stream_timeout_in_ms = 0;
        status_code = 0;
        expected_status_code = 0;
        delay_before_executing_next = 0;
        curr_request_idx = 0;
        scenario_index = 0;
        req_payload_cursor = 0;
        proto_type = PROTO_UNSPECIFIED;
        string_collection.reserve(12); // (path, authority, method, schema, payload, xx) * 2
    }
    explicit Request_Response_Data(const std::vector<uint64_t>& u_ids)
    {
        init();
        scenario_data_per_user = std::make_shared<Scenario_Data_Per_User>(u_ids);
    }

    explicit Request_Response_Data(std::shared_ptr<Scenario_Data_Per_User>& scenario_data_from_sibling)
    {
        init();
        scenario_data_per_user = scenario_data_from_sibling;
    }

    explicit Request_Response_Data(std::shared_ptr<Scenario_Data_Per_User>&& scenario_data_from_sibling)
    {
        init();
        scenario_data_per_user = std::move(scenario_data_from_sibling);
    }

    ~Request_Response_Data()
    {
        if (request_sent_callback)
        {
            request_sent_callback(-1, nullptr);
            auto dummy = std::move(request_sent_callback);
        }
    }

    friend std::ostream& operator<<(std::ostream& o, const Request_Response_Data& request_data)
    {
        o << "Request_Response_Data:"
          <<"{ " << std::endl
          << "  scenario index: " << request_data.scenario_index << std::endl
          << "  request index: " << request_data.curr_request_idx << std::endl
          << "  schema:" << *request_data.schema << std::endl
          << "  authority:" << *request_data.authority << std::endl
          << "  req_payload:" << *request_data.req_payload << std::endl
          << "  path:" << *request_data.path << std::endl
          << "  method:" << *request_data.method << std::endl
          << "  proto_type: " << request_data.proto_type << std::endl
          << "  scenario data:" << std::endl
          << "  " << *request_data.scenario_data_per_user
          << std::endl
          << "  expected_status_code:" << request_data.expected_status_code << std::endl
          << "  delay_before_executing_next:" << request_data.delay_before_executing_next << std::endl;

        for (auto& it : * (request_data.req_headers_from_config))
        {
            o << "  request header name from template: " << it.first << ", header value: " << it.second << std::endl;
        }
        for (auto& it : request_data.req_headers_of_individual)
        {
            o << "  updated request header name: " << it.first << ", header value: " << it.second << std::endl;
        }

        o << "  response status code:" << request_data.status_code << std::endl;
        o << "  resp_payload:" << request_data.resp_payload << std::endl;
        for (auto& vit : request_data.resp_headers)
        {
            for (auto& it : vit)
            {
                o << "  response header name: " << it.first << ", header value: " << it.second << std::endl;
            }
        }
        for (auto& it : request_data.scenario_data_per_user->saved_cookies)
        {
            o << "  cookie name: " << it.first << ", cookie content: " << it.second << std::endl;
        }
        o << "}" << std::endl;
        return o;
    };

    bool is_empty()
    {
        if (schema == &emptyString && authority == &emptyString && method == &emptyString)
        {
            return true;
        }
        return false;
    };

};

static std::unique_ptr<Request_Response_Data> dummy_rr_data(nullptr);


struct Variable_Data_Per_Client
{
    uint64_t req_variable_value_start = 0;
    uint64_t req_variable_value_end = 0;
    uint64_t curr_req_variable_value = 0;
    std::vector<uint64_t> req_variable_values_start;
    std::vector<uint64_t> req_variable_values_end;
    std::vector<uint64_t> curr_req_variable_values;

    explicit Variable_Data_Per_Client(const std::vector<uint64_t>& range_start, const std::vector<uint64_t>& range_end,
                                      const std::vector<uint64_t>& current_var):
        req_variable_values_start(range_start),
        req_variable_values_end(range_end),
        curr_req_variable_values(current_var)
    {
    };
    void inc_var()
    {
        for (size_t i = 0; i < req_variable_values_start.size(); i++)
        {
            if (req_variable_values_end[i] <= req_variable_values_start[i])
            {
                continue;
            }
            curr_req_variable_values[i]++;
            if (curr_req_variable_values[i] >= req_variable_values_end[i])
            {
                curr_req_variable_values[i] = req_variable_values_start[i];
            }
        }
    }
    const std::vector<uint64_t>& get_curr_vars() const
    {
        return curr_req_variable_values;
    }
};

constexpr auto BACKOFF_WRITE_BUFFER_THRES = 16_k;
constexpr int MAX_STREAM_TO_BE_EXHAUSTED = -2;
constexpr int MAX_CONCURRENT_STREAM_REACHED = -3;
constexpr int CONNECTION_ESTABLISH_IN_PROGRESSS = -4;
constexpr int REQUEST_SENDING_FAILURE = -1;




enum ClientState { CLIENT_IDLE, CLIENT_CONNECTING, CLIENT_CONNECTED };

// This type tells whether the client is in warmup phase or not or is over
enum class Phase
{
    INITIAL_IDLE = 0,  // Initial idle state before warm-up phase
    WARM_UP,       // Warm up phase when no measurements are done
    MAIN_DURATION, // Main measurement phase; if timing-based
    MAIN_DURATION_GRACEFUL_SHUTDOWN,
    // test is not run, this is the default phase
    DURATION_OVER  // This phase occurs after the measurements are over
};


// We use reservoir sampling method
struct Sampling
{
    // maximum number of samples
    size_t max_samples;
    // number of samples seen, including discarded samples.
    size_t n;
};

struct RequestStat
{
    // time point when request was sent
    std::chrono::steady_clock::time_point request_time;
    // same, but in wall clock reference frame
    std::chrono::system_clock::time_point request_wall_time;
    // time point when stream was closed
    std::chrono::steady_clock::time_point stream_close_time;
    // upload data length sent so far
    int64_t data_offset;
    // HTTP status code
    int status;
    // true if stream was successfully closed.  This means stream was
    // not reset, but it does not mean HTTP level error (e.g., 404).
    bool completed;
    size_t scenario_index;
    size_t request_index;

    explicit RequestStat(size_t scenario_id, size_t request_id):
        scenario_index(scenario_id),
        request_index(request_id),
        status(0x1FFF)
    {};
};

struct ClientStat
{
    // time client started (i.e., first connect starts)
    std::chrono::steady_clock::time_point client_start_time = std::chrono::steady_clock::time_point();
    // time client end (i.e., client somehow processed all requests it
    // is responsible for, and disconnected)
    std::chrono::steady_clock::time_point client_end_time = std::chrono::steady_clock::time_point();
    // The number of requests completed successful, but not necessarily
    // means successful HTTP status code.
    size_t req_success;

    // The following 3 numbers are overwritten each time when connection
    // is made.

    // time connect starts
    std::chrono::steady_clock::time_point connect_start_time = std::chrono::steady_clock::time_point();
    // time to connect
    std::chrono::steady_clock::time_point connect_time = std::chrono::steady_clock::time_point();
    // time to first byte (TTFB)
    std::chrono::steady_clock::time_point ttfb = std::chrono::steady_clock::time_point();

    ClientStat()
    {
        req_success = 0;
    }

    friend std::ostream& operator<<(std::ostream& o, const ClientStat& stat)
    {
        auto now = std::chrono::steady_clock::now();
        o << "ClientStats: "<<std::endl
          <<"{ " << std::endl
          << "  client_start_time: " << std::chrono::duration_cast<std::chrono::milliseconds>(stat.client_start_time - now).count() << std::endl
          << "  client_end_time: " << std::chrono::duration_cast<std::chrono::milliseconds>(stat.client_end_time - now).count() << std::endl
          << "  connect_start_time: " << std::chrono::duration_cast<std::chrono::milliseconds>(stat.connect_start_time - now).count() << std::endl
          << "  connect_time: " << std::chrono::duration_cast<std::chrono::milliseconds>(stat.connect_time - now).count() << std::endl
          << "  time to first byte: " << std::chrono::duration_cast<std::chrono::milliseconds>(stat.ttfb - now).count() << std::endl
          << "  req_success: " << stat.req_success << std::endl
          << "}" << std::endl;
        return o;
    }

};

struct SDStat
{
    // min, max, mean and sd (standard deviation)
    double min, max, mean, sd;
    // percentage of samples inside mean -/+ sd
    double within_sd;
};

struct SDStats
{
    // time for request
    SDStat request;
    // time for connect
    SDStat connect;
    // time to first byte (TTFB)
    SDStat ttfb;
    // request per second for each client
    SDStat rps;
};

struct Stats
{
    Stats(size_t req_todo, size_t nclients)
        : req_todo(req_todo),
          req_started(0),
          req_done(0),
          req_success(0),
          req_status_success(0),
          req_failed(0),
          req_error(0),
          req_timedout(0),
          bytes_total(0),
          bytes_head(0),
          bytes_head_decomp(0),
          bytes_body(0),
          status()
    {}
    // The total number of requests
    size_t req_todo = 0;
    // The number of requests issued so far
    size_t req_started = 0;
    // The number of requests finished
    size_t req_done = 0;
    // The number of requests completed successful, but not necessarily
    // means successful HTTP status code.
    size_t req_success = 0;
    // The number of requests marked as success.  HTTP status code is
    // also considered as success. This is subset of req_done.
    size_t req_status_success = 0;
    // The number of requests failed. This is subset of req_done.
    size_t req_failed = 0;
    // The number of requests failed due to network errors. This is
    // subset of req_failed.
    size_t req_error = 0;
    // The number of requests that failed due to timeout.
    size_t req_timedout = 0;
    // The number of bytes received on the "wire". If SSL/TLS is used,
    // this is the number of decrypted bytes the application received.
    int64_t bytes_total = 0;
    // The number of bytes received for header fields.  This is
    // compressed version.
    int64_t bytes_head = 0;
    // The number of bytes received for header fields after they are
    // decompressed.
    int64_t bytes_head_decomp = 0;
    // The number of bytes received in DATA frame.
    int64_t bytes_body = 0;
    // The number of each HTTP status category, status[i] is status code
    // in the range [i*100, (i+1)*100).
    std::array<size_t, 6> status;
    // The statistics per request
    std::vector<RequestStat> req_stats;
    // The statistics per client
    std::vector<ClientStat> client_stats;
    // The number of UDP datagrams received.
    size_t udp_dgram_recv = 0;
    // The number of UDP datagrams sent.
    size_t udp_dgram_sent = 0;

    friend std::ostream& operator<<(std::ostream& o, const Stats& stat)
    {
        o << "Stats: "<<std::endl
          <<"{ " << std::endl
          << "  req_todo: " << stat.req_todo << std::endl
          << "  req_started: " << stat.req_started << std::endl
          << "  req_done: " << stat.req_done << std::endl
          << "  req_success: " << stat.req_success << std::endl
          << "  req_status_success: " << stat.req_status_success << std::endl
          << "  req_failed: " << stat.req_failed << std::endl
          << "  req_error: " << stat.req_error << std::endl
          << "  req_timedout: " << stat.req_timedout << std::endl
          << "  bytes_total: " << stat.bytes_total << std::endl
          << "  bytes_head: " << stat.bytes_head << std::endl
          << "  bytes_head_decomp: " << stat.bytes_head_decomp << std::endl
          << "  bytes_body: " << stat.bytes_body << std::endl
          << "  udp_dgram_recv: " << stat.udp_dgram_recv << std::endl
          << "  udp_dgram_sent: " << stat.udp_dgram_sent << std::endl;
        for (size_t i = 1; i < stat.status.size(); i++)
        {
            o << "  " << i <<"xx status code: " << stat.status[i] <<std::endl;
        }
        for (size_t i = 0; i < stat.client_stats.size(); i++)
        {
            o << "  client: " << i <<", req_success: " << stat.client_stats[i].req_success <<std::endl;
        }
        o <<"}" << std::endl;
        return o;
    }
};


struct Stream
{
    RequestStat req_stat;
    int status_success;
    bool statistics_eligible;
    std::unique_ptr<Request_Response_Data> request_response;
    Stream(bool stats_eligible, std::unique_ptr<Request_Response_Data>& rr_data)
    : req_stat(rr_data ? rr_data->scenario_index : 0, rr_data ? rr_data->curr_request_idx : 0),
      status_success(-1),
      statistics_eligible(stats_eligible)
    {
    if (rr_data)
    {
        request_response = std::move(rr_data);
    }
    }

};

} // namespace h2load

#endif // H2LOAD_H
