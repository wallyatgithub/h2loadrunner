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
#include <iostream>
#include <vector>
#include "h2load_Cookie.h"
#include "http2.h"
#ifdef USE_LIBEV
#include "memchunk.h"
#endif
#include "template.h"
#include "H2Server_Request.h"
#include "config_schema.h"


using namespace nghttp2;

namespace h2load
{

const std::string scheme_header = ":scheme";
const std::string path_header = ":path";
const std::string authority_header = ":authority";
const std::string method_header = ":method";

const std::string x_envoy_original_dst_host_header = "x-envoy-original-dst-host";

const std::string x_proto_to_use = "x-protocol-to-use";

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

struct Request_Data
{
    std::string* schema;
    std::string* authority;
    std::string* req_payload;
    std::string* path;
    std::string* method;
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
    std::function<void(int32_t, h2load::base_client*)> request_sent_callback;
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
        string_collection.reserve(12); // (path, authority, method, schema, payload, xx) * 2
    }
    explicit Request_Data(const std::vector<uint64_t>& u_ids)
    {
        init();
        scenario_data_per_user = std::make_shared<Scenario_Data_Per_User>(u_ids);
    }

    explicit Request_Data(std::shared_ptr<Scenario_Data_Per_User>& scenario_data_from_sibling)
    {
        init();
        scenario_data_per_user = scenario_data_from_sibling;
    }

    explicit Request_Data(std::shared_ptr<Scenario_Data_Per_User>&& scenario_data_from_sibling)
    {
        init();
        scenario_data_per_user = std::move(scenario_data_from_sibling);
    }

    friend std::ostream& operator<<(std::ostream& o, const Request_Data& request_data)
    {
        o << "Request_Data: { " << std::endl
          << "scenario index: " << request_data.scenario_index << std::endl
          << "request index: " << request_data.curr_request_idx << std::endl
          << "schema:" << *request_data.schema << std::endl
          << "authority:" << *request_data.authority << std::endl
          << "req_payload:" << *request_data.req_payload << std::endl
          << "path:" << *request_data.path << std::endl
          << "method:" << *request_data.method << std::endl
          << "scenario data:" << std::endl
          << *request_data.scenario_data_per_user
          << std::endl
          << "expected_status_code:" << request_data.expected_status_code << std::endl
          << "delay_before_executing_next:" << request_data.delay_before_executing_next << std::endl;

        for (auto& it : * (request_data.req_headers_from_config))
        {
            o << "request header name from template: " << it.first << ", header value: " << it.second << std::endl;
        }
        for (auto& it : request_data.req_headers_of_individual)
        {
            o << "updated request header name: " << it.first << ", header value: " << it.second << std::endl;
        }

        o << "response status code:" << request_data.status_code << std::endl;
        o << "resp_payload:" << request_data.resp_payload << std::endl;
        for (auto& vit : request_data.resp_headers)
        {
            for (auto& it : vit)
            {
                o << "response header name: " << it.first << ", header value: " << it.second << std::endl;
            }
        }
        for (auto& it : request_data.scenario_data_per_user->saved_cookies)
        {
            o << "cookie name: " << it.first << ", cookie content: " << it.second << std::endl;
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

struct Scenario_Data_Per_Client
{
    uint64_t req_variable_value_start = 0;
    uint64_t req_variable_value_end = 0;
    uint64_t curr_req_variable_value = 0;
    std::vector<uint64_t> req_variable_values_start;
    std::vector<uint64_t> req_variable_values_end;
    std::vector<uint64_t> curr_req_variable_values;

    explicit Scenario_Data_Per_Client(const std::vector<uint64_t>& range_start, const std::vector<uint64_t>& range_end,
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

} // namespace h2load

#endif // H2LOAD_H
