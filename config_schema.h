#ifndef H2LOAD_CONFIG_SCHEMA_H
#define H2LOAD_CONFIG_SCHEMA_H

#include <iostream>
#include <map>
#include <fstream>
#include <regex>

#include "staticjson/document.hpp"
#include "staticjson/staticjson.hpp"
#include "rapidjson/schema.h"
#include "rapidjson/prettywriter.h"

#include "common_types.h"
#include "H2Server_Request.h"

static const char* validate_response = "validate_response";
static const char* make_request = "make_request";

const std::string input_uri = "input";
const std::string same_with_last_one = "sameWithLastOne";
const std::string from_response_header = "fromResponseHeader";
const std::string from_lua_script = "fromLuaScript";
const std::string from_x_path = "fromXPath";
const std::string from_json_pointer = "fromJsonPointer";
const std::string input_with_variable = "inputWithVariable";


const std::string request_header = "Request-Header";
const std::string response_header = "Response-Header";
const std::string json_ptr_of_req_payload = "Json-Pointer-In-Request-Payload";
const std::string json_ptr_of_resp_payload = "Json-Pointer-In-Response-Payload";

const std::string path_header_name = ":path";
const std::string scheme_header_name = ":scheme";
const std::string authority_header_name = ":authority";
const std::string method_header_name = ":method";
const std::string schema_http = "http";
const std::string schema_https = "https";
const std::string http1_proto = "http/1.1";
const std::string http2_proto = "h2";
const std::string http2_cleartext_proto = "h2c";
const std::string http3_proto = "h3";
const std::string HTTP1_ALPN = "\x8http/1.1";
const std::string HTTP2_ALPN = "\x2h2";
const std::string HTTP3_ALPN = "\x2h3";
const std::string http1_1_version = "http/1.1";
const std::string http2_0_version = "http/2.0";
const std::string http3_0_version = "http/3.0";
const int64_t LEAST_VALID_PAGE_ID = 1;

enum URI_ACTION
{
    INPUT_URI = 0,
    SAME_WITH_LAST_ONE,
    FROM_RESPONSE_HEADER,
    FROM_LUA_SCRIPT,
    FROM_X_PATH,
    FROM_JSON_POINTER,
    INPUT_WITH_VARIABLE
};

enum VALUE_SOURCE_TYPE
{
    SOURCE_TYPE_REQ_HEADER = 0,
    SOURCE_TYPE_REQ_POINTER,
    SOURCE_TYPE_RES_HEADER,
    SOURCE_TYPE_RES_POINTER
};

enum PROTO_TYPE
{
    PROTO_INVALID = 0,
    PROTO_HTTP1,
    PROTO_HTTP2,
    PROTO_HTTP3,
    PROTO_UNSPECIFIED
};

const std::map<std::string, PROTO_TYPE, ci_less> http_proto_map =
{
    {http1_proto,           PROTO_HTTP1},
    {http2_proto,           PROTO_HTTP2},
    {http2_cleartext_proto, PROTO_HTTP2},
    {http3_proto,           PROTO_HTTP3},
};

const static std::map<std::string, PROTO_TYPE, ci_less> http_version_to_proto_map =
{
    {http1_1_version, PROTO_HTTP1},
    {http2_0_version, PROTO_HTTP2},
    {http3_0_version, PROTO_HTTP3},
    {"default", PROTO_UNSPECIFIED}
};

enum class URI_SCHEMA
{
    SCHEMA_INVALID = 0,
    SCHEMA_HTTP,
    SCHEMA_HTTPS
};

const std::map<std::string, URI_SCHEMA, ci_less> http_schema_map =
{
    {schema_http, URI_SCHEMA::SCHEMA_HTTP},
    {schema_https, URI_SCHEMA::SCHEMA_HTTPS},
};

const std::map<std::string, VALUE_SOURCE_TYPE> value_pickup_action_map =
{
    {request_header, SOURCE_TYPE_REQ_HEADER},
    {response_header, SOURCE_TYPE_RES_HEADER},
    {json_ptr_of_req_payload, SOURCE_TYPE_REQ_POINTER},
    {json_ptr_of_resp_payload, SOURCE_TYPE_RES_POINTER}
};

const std::map<std::string, URI_ACTION> uri_action_map =
{
    {input_uri, INPUT_URI},
    {same_with_last_one, SAME_WITH_LAST_ONE},
    {from_response_header, FROM_RESPONSE_HEADER},
    {from_lua_script, FROM_LUA_SCRIPT},
    {from_x_path, FROM_X_PATH},
    {from_json_pointer, FROM_JSON_POINTER},
    {input_with_variable, INPUT_WITH_VARIABLE}
};

class Uri
{
public:
    std::string typeOfAction;
    std::string input;
    URI_ACTION uri_action;
    void staticjson_init(staticjson::ObjectHandler* h)
    {
        h->add_property("typeOfAction", &this->typeOfAction);
        h->add_property("input", &this->input, staticjson::Flags::Optional);
    }
};

class Schema_Response_Match
{
public:
    std::vector<Schema_Header_Match> header_match;
    std::vector<Schema_Payload_Match> payload_match;
    void staticjson_init(staticjson::ObjectHandler* h)
    {
        h->add_property("headers", &this->header_match);
        h->add_property("payload", &this->payload_match, staticjson::Flags::Optional);
    }
};

class Response_Value_Regex_Picker
{
public:
    std::string where_to_pickup_from = response_header;
    std::string source;
    std::string picker_regexp;
    std::string save_to_variable_name;
    void staticjson_init(staticjson::ObjectHandler* h)
    {
        h->add_property("where-to-pickup-from", &this->where_to_pickup_from);
        h->add_property("source", &this->source);
        h->add_property("regexp", &this->picker_regexp);
        h->add_property("save-to-variable", &this->save_to_variable_name);
    }
};

class Regex_Picker
{
public:
    VALUE_SOURCE_TYPE where_to_pick_up_from = SOURCE_TYPE_RES_HEADER;
    std::string source;
    std::regex picker_regexp;
    std::string variable_name;
    size_t unique_id;
    Regex_Picker(const Response_Value_Regex_Picker& picker_schema, size_t id)
    {
        auto iter = value_pickup_action_map.find(picker_schema.where_to_pickup_from);
        if (iter == value_pickup_action_map.end())
        {
            std::cerr << "invalid value: " << picker_schema.where_to_pickup_from << std::endl;
            exit(1);
        }
        where_to_pick_up_from = iter->second;
        source = picker_schema.source;
        variable_name = picker_schema.save_to_variable_name;
        unique_id = id;
        try
        {
            picker_regexp.assign(picker_schema.picker_regexp, std::regex_constants::ECMAScript | std::regex_constants::optimize);
        }
        catch (std::regex_error& e)
        {
            std::cerr << "invalid reg exp: " << picker_schema.picker_regexp << " reason: " << e.what() << std::endl;
        }
    }
};

struct String_With_Variables_In_Between
{
    std::vector<std::string> string_segments;
    std::vector<size_t> variable_ids_in_between;
};

inline std::ostream& operator<<(std::ostream& o, const String_With_Variables_In_Between& r)
{
    o << "string_segments.size(): " << r.string_segments.size() << std::endl;
    for (size_t i = 0; i < r.string_segments.size(); i++)
    {
        o << "index: " << i << ": " << r.string_segments[i] << std::endl;
    }
    o << "variable_ids_in_between.size(): " << r.variable_ids_in_between.size() << std::endl;
    for (size_t i = 0; i < r.variable_ids_in_between.size(); i++)
    {
        o << "index: " << i << ": " << r.variable_ids_in_between[i] << std::endl;
    }
    return o;
}

class Request
{
public:
    bool clear_old_cookies;
    std::string luaScript;
    bool make_request_function_present;
    bool validate_response_function_present;
    std::string schema;
    std::string authority;
    std::string http_version;
    PROTO_TYPE proto_type; // filled by post_process_json_config_schema
    //    std::string path; // filled by post_process_json_config_schema
    Uri uri;
    std::string method;
    std::string payload;
    std::vector<std::string> additonalHeaders;
    uint32_t expected_status_code; // staticJson does not accept uint16_t
    Schema_Response_Match response_match;
    std::vector<Match_Rule> response_match_rules; // filled by post_process_json_config_schema
    std::map<std::string, std::string, ci_less> headers_in_map; // filled by post_process_json_config_schema
    std::vector<std::pair<String_With_Variables_In_Between, String_With_Variables_In_Between>>
                                                                                            headers_with_variable; // filled by post_process_json_config_schema
    String_With_Variables_In_Between tokenized_path_with_vars;
    String_With_Variables_In_Between tokenized_payload_with_vars;

    uint32_t delay_before_executing_next;
    std::vector<Response_Value_Regex_Picker> response_value_regex_pickers;
    std::vector<Regex_Picker> actual_regex_value_pickers; // filled by post_process_json_config_schema
    int64_t page_id = 0;

    void staticjson_init(staticjson::ObjectHandler* h)
    {
        h->add_property("luaScript", &this->luaScript, staticjson::Flags::Optional);
        h->add_property("uri", &this->uri);
        h->add_property("page-id", &this->page_id, staticjson::Flags::Optional);
        h->add_property("method", &this->method);
        h->add_property("http-version", &this->http_version, staticjson::Flags::Optional);
        h->add_property("payload", &this->payload, staticjson::Flags::Optional);
        h->add_property("additonalHeaders", &this->additonalHeaders, staticjson::Flags::Optional);
        h->add_property("clear-old-cookies", &this->clear_old_cookies, staticjson::Flags::Optional);
        h->add_property("expected-status-code", &this->expected_status_code, staticjson::Flags::Optional);
        h->add_property("delay-before-executing-next", &this->delay_before_executing_next, staticjson::Flags::Optional);
        h->add_property("response-match", &this->response_match, staticjson::Flags::Optional);
        h->add_property("value-pickers", &this->response_value_regex_pickers, staticjson::Flags::Optional);
    }
    explicit Request()
    {
        clear_old_cookies = false;
        expected_status_code = 0;
        delay_before_executing_next = 0;
        make_request_function_present = false;
        validate_response_function_present = false;
        proto_type = PROTO_UNSPECIFIED;
    }
};

class Range_Based_Variable
{
public:
    std::string variable_name;
    uint64_t variable_range_start = 0;
    uint64_t variable_range_end = 0;
    explicit Range_Based_Variable(const std::string& var_name, uint64_t start, uint64_t end):
        variable_name(var_name),
        variable_range_start(start),
        variable_range_end(end)
    {
        if (end <= start)
        {
            std::cout << "variable: " << variable_name << "range end is not greater than range start" << std::endl;
            exit(1);
        }
    };
    explicit Range_Based_Variable():variable_range_start(0), variable_range_end(0)
    {
    }
    void staticjson_init(staticjson::ObjectHandler* h)
    {
        h->add_property("variable-name", &this->variable_name);
        h->add_property("variable-range-start", &this->variable_range_start);
        h->add_property("variable-range-end", &this->variable_range_end);
    }
};

class Two_Dimensioning_Variable
{
public:
    std::string variable_name;
    std::string id_list_file_name;
    explicit Two_Dimensioning_Variable(const std::string& var_name, const std::string& file_name):
        variable_name(var_name),
        id_list_file_name(file_name)
    {
    };
    explicit Two_Dimensioning_Variable()
    {
    };
    void staticjson_init(staticjson::ObjectHandler* h)
    {
        h->add_property("variable-name", &this->variable_name);
        h->add_property("file-name", &this->id_list_file_name);
    }
};

class Generic_Variable
{
public:
    std::string var_name;
    uint64_t range_start = 0;
    uint64_t range_end = 0;
    std::vector<std::vector<std::string>> var_value_list;
    size_t full_range_len = 0;

    explicit Generic_Variable(const std::string& name, const std::vector<std::vector<std::string>>& value_list):
        var_name(name),
        var_value_list(value_list),
        range_start(0),
        range_end(std::max_element(std::begin(value_list),
                                   std::end(value_list),
                                   [](const std::vector<std::string> & lhs,
                                      const std::vector<std::string> & rhs)
    {
        return lhs.size() < rhs.size();
    })->size())
    {
    };
    explicit Generic_Variable(const std::string& name, uint64_t start, uint64_t end):
        var_name(name),
        range_start(start),
        range_end(end),
        full_range_len(std::to_string(end).size())
    {
        if (end != 0 && end <= start)
        {
            std::cerr << "variable: " << name << " range end is not greater than range start" << std::endl;
            exit(1);
        }
    };
    std::string produce_value(size_t col, size_t user_id)
    {
        if (var_value_list.size())
        {
            size_t colume_id = col % var_value_list.size();
            if (var_value_list[colume_id].size())
            {
                size_t row_id = user_id % var_value_list[colume_id].size();
                return var_value_list[colume_id][row_id];
            }
        }
        else
        {
            if (range_end > range_start)
            {
                std::string curr_var_value_str = std::to_string(user_id);
                std::string padding(full_range_len - curr_var_value_str.size(), '0');
                curr_var_value_str.insert(0, padding);
                return curr_var_value_str;
            }
        }
        return "";
    };

    size_t estimate_value_size(size_t col, size_t user_id)
    {
        if (var_value_list.size())
        {
            size_t colume_id = col % var_value_list.size();
            if (var_value_list[colume_id].size())
            {
                size_t row_id = user_id % var_value_list[colume_id].size();
                return var_value_list[colume_id][row_id].size();
            }
        }
        else
        {
            if (range_end > range_start)
            {
                return full_range_len;
            }
        }
        return 0;
    }
};


class Variable_Manager
{
public:
    void insert(const Generic_Variable& gen_var)
    {
        if (var_name_to_id.count(gen_var.var_name) == 0)
        {
            size_t var_id = var_name_to_id.size();
            var_name_to_id[gen_var.var_name] = var_id;
            generic_variables.push_back(gen_var);
        }
        else
        {
            if (gen_var.full_range_len || gen_var.var_value_list.size())
            {
                std::cerr << std::endl << "------warning: variable " << gen_var.var_name << " is being redefined, ignore------" <<
                          std::endl;
            }
        }
    };

    bool get_gen_var_id(const std::string& gen_var_name, size_t& id) const
    {
        auto it = var_name_to_id.find(gen_var_name);
        if (it != var_name_to_id.end())
        {
            id = it->second;
            return true;
        }
        else
        {
            return false;
        }
    }
    std::string produce_value(size_t var_id, size_t req_id, size_t user_id)
    {
        return generic_variables[var_id].produce_value(req_id, user_id);
    }
    size_t estimate_value_size(size_t var_id, size_t req_id, size_t user_id)
    {
        return generic_variables[var_id].estimate_value_size(req_id, user_id);
    }
    size_t get_number_of_generic_variables()
    {
        return var_name_to_id.size();
    }
    size_t var_range_start(size_t gen_var_id)
    {
        return generic_variables[gen_var_id].range_start;
    }
    size_t var_range_end(size_t gen_var_id)
    {
        return generic_variables[gen_var_id].range_end;
    }
    std::string get_var_name(size_t gen_var_id)
    {
        return generic_variables[gen_var_id].var_name;
    }

private:
    std::vector<Generic_Variable> generic_variables;
    std::map<std::string, size_t> var_name_to_id;
};


class Scenario
{
public:
    std::string name;
    uint32_t weight = 0;
    std::string variable_name_in_path_and_data;
    std::string user_id_list_file;
    std::vector<Range_Based_Variable> range_based_variables;
    std::vector<Two_Dimensioning_Variable> two_dim_variables;
    uint32_t interval_to_wait_before_start = 0;
    uint64_t variable_range_start = 0;
    uint64_t variable_range_end = 0;
    bool variable_range_slicing = false;
    std::vector<Request> requests;
    std::string user_variables_input_file;
    Variable_Manager variable_manager; // content populated by post_process_json_config_schema
    // TODO:  refactor the next line
    size_t number_of_variables_from_value_pickers = 0;  // set by post_process_json_config_schema
    std::string har_file_name;
    void staticjson_init(staticjson::ObjectHandler* h)
    {
        h->add_property("name", &this->name);
        h->add_property("weight", &this->weight, staticjson::Flags::Optional);
        h->add_property("user-id-variable-in-path-and-data", &this->variable_name_in_path_and_data,
                        staticjson::Flags::Optional);
        h->add_property("user-id-list-file", &this->user_id_list_file, staticjson::Flags::Optional);
        h->add_property("user-id-range-start", &this->variable_range_start, staticjson::Flags::Optional);
        h->add_property("user-id-range-end", &this->variable_range_end, staticjson::Flags::Optional);
        h->add_property("user-id-range-slicing", &this->variable_range_slicing, staticjson::Flags::Optional);
        h->add_property("interval-to-wait-before-start", &this->interval_to_wait_before_start, staticjson::Flags::Optional);
        h->add_property("user-variables-input-file", &this->user_variables_input_file, staticjson::Flags::Optional);
        h->add_property("range-based-variables", &this->range_based_variables, staticjson::Flags::Optional);
        h->add_property("two-dimensional-variables", &this->two_dim_variables, staticjson::Flags::Optional);
        h->add_property("HAR-file", &this->har_file_name, staticjson::Flags::Optional);
        h->add_property("Requests", &this->requests);
    }
    explicit Scenario():
        variable_name_in_path_and_data(""),
        variable_range_start(0),
        variable_range_end(0),
        variable_range_slicing(false),
        weight(100),
        interval_to_wait_before_start(0)
    {
    }
};

class Load_Share_Host
{
public:
    std::string host;
    uint32_t port;
    void staticjson_init(staticjson::ObjectHandler* h)
    {
        h->add_property("host", &this->host);
        h->add_property("port", &this->port, staticjson::Flags::Optional);
    }
};

class Config_Schema
{
public:
    std::string schema;
    std::string host;
    uint32_t port;
    uint64_t requests;
    uint32_t threads;
    uint32_t clients;
    uint32_t max_concurrent_streams;
    uint64_t window_bits;
    uint64_t connection_window_bits;
    std::string ciphers;
    std::string no_tls_proto;
    uint32_t rate;
    double rate_period;
    double duration;
    double warm_up_time;
    double connection_active_timeout;
    double connection_inactivity_timeout;
    std::string npn_list;
    uint64_t header_table_size;
    uint64_t encoder_header_table_size;
    std::string log_file;
    uint32_t statistics_interval;
    std::string statistics_file;
    double request_per_second;
    std::string rps_file;
    uint64_t nreqs;
    uint32_t stream_timeout_in_ms;
    std::string ca_cert;
    std::string client_cert;
    std::string private_key;
    uint32_t cert_verification_mode;
    std::string max_tls_version;
    bool open_new_connection_based_on_authority_header;
    bool connection_retry_on_disconnect;
    std::vector<Load_Share_Host> load_share_hosts;
    bool connect_back_to_preferred_host;
    double interval_to_send_ping;
    std::vector<Scenario> scenarios;
    uint32_t builtin_server_port;
    std::string failed_request_log_file;
    uint64_t skt_recv_buffer_size;
    uint64_t skt_send_buffer_size;
    uint64_t config_update_sequence_number;
    uint64_t max_frame_size;
    std::string tls13_ciphers;
    std::string groups;
    bool no_udp_gso;
    uint64_t max_udp_payload_size;
    bool ktls;
    std::string qlog_file_base;
    std::string tls_keylog_file;
    std::string quic_congestion_control_algorithm;
    bool builtin_cookie_support = true;

    explicit Config_Schema():
        schema("http"),
        host(""),
        port(80),
        threads(1),
        clients(1),
        max_concurrent_streams(1024),
        window_bits(30),
        connection_window_bits(30),
        ciphers("ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384:ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305:ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256:ECDHE-ECDSA-AES256-SHA384:ECDHE-RSA-AES256-SHA384:ECDHE-ECDSA-AES128-SHA256:ECDHE-RSA-AES128-SHA256"),
        no_tls_proto("h2c"),
        rate(0),
        rate_period(1),
        duration(0),
        warm_up_time(0),
        connection_active_timeout(0),
        connection_inactivity_timeout(0),
        npn_list("h2,h2-16,h2-14,http/1.1"),
        header_table_size(4096),
        encoder_header_table_size(4096),
        log_file(""),
        statistics_interval(1),
        request_per_second(0),
        nreqs(0),
        stream_timeout_in_ms(5000),
        max_tls_version("TLSv1.3"),
        open_new_connection_based_on_authority_header(false),
        connection_retry_on_disconnect(false),
        connect_back_to_preferred_host(false),
        interval_to_send_ping(0),
        builtin_server_port(8888),
        skt_recv_buffer_size(4194304),
        skt_send_buffer_size(4194304),
        config_update_sequence_number(0),
        max_frame_size(16384),
        tls13_ciphers("TLS_AES_128_GCM_SHA256:TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256:TLS_AES_128_CCM_SHA256"),
        groups("X25519:P-256:P-384:P-521"),
        no_udp_gso(false),
        max_udp_payload_size(0),
        ktls(false)
    {
    }

    void staticjson_init(staticjson::ObjectHandler* h)
    {
        h->add_property("schema", &this->schema);
        h->add_property("host", &this->host);
        h->add_property("port", &this->port, staticjson::Flags::Optional);
        h->add_property("open-new-connection-based-on-authority-header", &this->open_new_connection_based_on_authority_header,
                        staticjson::Flags::Optional);
        h->add_property("threads", &this->threads, staticjson::Flags::Optional);
        h->add_property("clients", &this->clients, staticjson::Flags::Optional);
        h->add_property("max-concurrent-streams", &this->max_concurrent_streams, staticjson::Flags::Optional);
        h->add_property("window-bits", &this->window_bits, staticjson::Flags::Optional);
        h->add_property("connection-window-bits", &this->connection_window_bits, staticjson::Flags::Optional);
        h->add_property("no-tls-proto", &this->no_tls_proto, staticjson::Flags::Optional);
        h->add_property("ciphers", &this->ciphers, staticjson::Flags::Optional);
        h->add_property("rate", &this->rate, staticjson::Flags::Optional);
        h->add_property("rate-period", &this->rate_period, staticjson::Flags::Optional);
        h->add_property("duration", &this->duration, staticjson::Flags::Optional);
        h->add_property("total-requests", &this->nreqs, staticjson::Flags::Optional);
        h->add_property("warm-up-time", &this->warm_up_time, staticjson::Flags::Optional);
        h->add_property("connection-active-timeout", &this->connection_active_timeout, staticjson::Flags::Optional);
        h->add_property("connection-inactive-timeout", &this->connection_inactivity_timeout, staticjson::Flags::Optional);
        h->add_property("stream-timeout", &this->stream_timeout_in_ms, staticjson::Flags::Optional);
        h->add_property("npn-list", &this->npn_list, staticjson::Flags::Optional);
        h->add_property("header-table-size", &this->header_table_size, staticjson::Flags::Optional);
        h->add_property("encoder-header-table-size", &this->encoder_header_table_size, staticjson::Flags::Optional);
        h->add_property("log-file", &this->log_file, staticjson::Flags::Optional);
        h->add_property("statistics-interval", &this->statistics_interval, staticjson::Flags::Optional);
        h->add_property("request-per-second", &this->request_per_second, staticjson::Flags::Optional);
        h->add_property("request-per-second-feed-file", &this->rps_file, staticjson::Flags::Optional);
        h->add_property("Scenarios", &this->scenarios);
        h->add_property("caCert", &this->ca_cert, staticjson::Flags::Optional);
        h->add_property("cert", &this->client_cert, staticjson::Flags::Optional);
        h->add_property("privateKey", &this->private_key, staticjson::Flags::Optional);
        h->add_property("certVerificationMode", &this->cert_verification_mode, staticjson::Flags::Optional);
        h->add_property("max-tls-version", &this->max_tls_version, staticjson::Flags::Optional);
        h->add_property("connection-retry", &this->connection_retry_on_disconnect, staticjson::Flags::Optional);
        h->add_property("load-share-hosts", &this->load_share_hosts, staticjson::Flags::Optional);
        h->add_property("switch-back-after-connection-retry", &this->connect_back_to_preferred_host,
                        staticjson::Flags::Optional);
        h->add_property("interval-between-ping-frames", &this->interval_to_send_ping, staticjson::Flags::Optional);
        h->add_property("builtin-server-listening-port", &this->builtin_server_port, staticjson::Flags::Optional);
        h->add_property("failed-request-log-file", &this->failed_request_log_file, staticjson::Flags::Optional);
        h->add_property("statistics-file", &this->statistics_file, staticjson::Flags::Optional);
        h->add_property("socket-receive-buffer-size", &this->skt_recv_buffer_size, staticjson::Flags::Optional);
        h->add_property("socket-send-buffer-size", &this->skt_send_buffer_size, staticjson::Flags::Optional);
        h->add_property("max-frame-size", &this->max_frame_size, staticjson::Flags::Optional);
        h->add_property("tls13_ciphers", &this->tls13_ciphers, staticjson::Flags::Optional);
        h->add_property("groups", &this->groups, staticjson::Flags::Optional);
        h->add_property("no-udp-gso", &this->no_udp_gso, staticjson::Flags::Optional);
        h->add_property("max-udp-payload-size", &this->max_udp_payload_size, staticjson::Flags::Optional);
        h->add_property("ktls", &this->ktls, staticjson::Flags::Optional);
        h->add_property("qlog-file-base", &this->qlog_file_base, staticjson::Flags::Optional);
        h->add_property("tls-keylog-file", &this->tls_keylog_file, staticjson::Flags::Optional);
        h->add_property("quic-congestion-control-algorithm", &this->quic_congestion_control_algorithm,
                        staticjson::Flags::Optional);
        h->add_property("builtin-cookie-support", &this->builtin_cookie_support, staticjson::Flags::Optional);
    }
};

#endif
