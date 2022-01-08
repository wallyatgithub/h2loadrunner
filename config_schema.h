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

static const std::string extended_json_pointer_indicator = "/~#";
static const std::string extended_json_pointer_name_indicator = "#name";
static const std::string extended_json_pointer_value_indicator = "#value";
static const char* validate_response = "validate_response";
static const char* make_request = "make_request";

struct ci_less
{
  // case-independent (ci) compare_less binary function
  struct nocase_compare
  {
    bool operator() (const unsigned char& c1, const unsigned char& c2) const {
        return tolower (c1) < tolower (c2);
    }
  };
  bool operator() (const std::string & s1, const std::string & s2) const {
    return std::lexicographical_compare
      (s1.begin (), s1.end (),   // source range
      s2.begin (), s2.end (),   // dest range
      nocase_compare ());  // comparison
  }
};

class Uri {
public:
    std::string typeOfAction;
    std::string input;
    void staticjson_init(staticjson::ObjectHandler* h)
    {
        h->add_property("typeOfAction", &this->typeOfAction);
        h->add_property("input", &this->input, staticjson::Flags::Optional);
    }
};

class Schema_Header_Match
{
public:
    std::string matchType;
    std::string header;
    std::string input;
    void staticjson_init(staticjson::ObjectHandler* h)
    {
        h->add_property("matchType", &this->matchType);
        h->add_property("header-name", &this->header);
        h->add_property("input", &this->input);
    }
};

class Schema_Payload_Match
{
public:
    std::string matchType;
    std::string jsonPointer;
    std::string input;
    void staticjson_init(staticjson::ObjectHandler* h)
    {
        h->add_property("matchType", &this->matchType);
        h->add_property("JsonPointer", &this->jsonPointer);
        h->add_property("input", &this->input);
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

template<typename RapidJsonType>
inline const rapidjson::Value* getNthValue(const RapidJsonType& d, int64_t n)
{
    if (d.MemberCount() && n > 0)
    {
        int64_t index = 1;
        for (rapidjson::Value::ConstMemberIterator itr = d.MemberBegin();
            itr != d.MemberEnd();)
        {
            if (index == n)
            {
                return &(itr->value);
            }
            ++index;
            ++itr;
        }
    }
    else if (d.MemberCount() && n < 0)
    {
        if ((0 - n) == d.MemberCount())
        {
            return &(d.MemberBegin()->value);
        }
        else
        {
            int64_t index = -1;
            rapidjson::Value::ConstMemberIterator itr = d.MemberEnd();
            itr--;
            while (itr != d.MemberBegin())
            {
                if (index == n)
                {
                    return &(itr->value);
                }
                itr--;
                index--;
            }
        }

    }
    return nullptr;
}

template<typename RapidJsonType>
inline const rapidjson::Value* getNthName(const RapidJsonType& d, int64_t n)
{
    if (d.MemberCount() && n > 0)
    {
        int64_t index = 1;
        for (rapidjson::Value::ConstMemberIterator itr = d.MemberBegin();
            itr != d.MemberEnd();)
        {
            if (index == n)
            {
                return &(itr->name);
            }
            ++index;
            ++itr;
        }
    }
    else if (d.MemberCount() && n < 0)
    {
        if ((0 - n) == d.MemberCount())
        {
            return &(d.MemberBegin()->name);
        }
        else
        {
            int64_t index = -1;
            rapidjson::Value::ConstMemberIterator itr = d.MemberEnd();
            itr--;
            while (itr != d.MemberBegin())
            {
                if (index == n)
                {
                    return &(itr->name);
                }
                itr--;
                index--;
            }
        }

    }
    return nullptr;
}

inline std::vector<std::string> splitComplexJsonPointer(const std::string& json_pointer)
{
    std::vector<std::string> JsonPointers;
    std::string extended_json_pointer_indicator = "/~#";
    size_t start_pos = 0;
    while (start_pos != std::string::npos)
    {
        size_t end_pos = json_pointer.find(extended_json_pointer_indicator, start_pos);
        if (end_pos - start_pos)
        {
            JsonPointers.emplace_back(json_pointer.substr(start_pos, (end_pos - start_pos)));
        }
        if (end_pos != std::string::npos)
        {
            size_t nextJsonPtrPos = json_pointer.find("/", end_pos+1);
            JsonPointers.emplace_back(json_pointer.substr(end_pos, (nextJsonPtrPos - end_pos)));
            start_pos = nextJsonPtrPos;
        }
        else
        {
            break;
        }
    }
    return JsonPointers;
}

template<typename RapidJsonType>
inline std::string convertRapidVJsonValueToStr(RapidJsonType* value)
{
    if (value)
    {
        if (value->IsString())
        {
            return value->GetString();
        }
        else if (value->IsBool())
        {
            return value->GetBool() ? "true" : "false";
        }
        else if (value->IsUint64())
        {
            return std::to_string(value->GetUint64());
        }
        else if (value->IsDouble())
        {
            return std::to_string(value->GetDouble());
        }
        else if (value->IsObject())
        {
            rapidjson::StringBuffer buffer;
            rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
            value->Accept(writer);
            return std::string(buffer.GetString());
        }
    }
    return "";
}


template<typename RapidJsonType>
inline std::string getValueFromJsonPtr(const RapidJsonType& d, const std::string& json_pointer)
{
    std::vector<std::string> pointers = splitComplexJsonPointer(json_pointer);
    const rapidjson::Value* value = &d;
    for (size_t vectorIndex = 0; vectorIndex < pointers.size(); vectorIndex++)
    {
        if (pointers[vectorIndex].find(extended_json_pointer_indicator) != std::string::npos)
        {
            try
            {
                int64_t index = std::stoi(pointers[vectorIndex].substr(extended_json_pointer_indicator.size()));
                std::string str = extended_json_pointer_indicator;
                str.append(std::to_string(index));
                std::string name_or_value = pointers[vectorIndex].substr(str.size());
                if (name_or_value.compare(extended_json_pointer_name_indicator) == 0)
                {
                    value = getNthName(*value, index);
                    break; // a name is always a string, no more pointer into this string any more
                }
                else if (name_or_value.compare(extended_json_pointer_value_indicator) == 0)
                {
                    value = getNthValue(*value, index);
                }
                else
                {
                    std::cerr<<"invalid json pointer: "<<pointers[vectorIndex]<<std::endl;
                }
            }
            catch (std::invalid_argument& e)
            {
                std::cerr<<"invalid_argument: "<<pointers[vectorIndex]<<std::endl;
                exit(1);
            }
            catch (std::out_of_range& e)
            {
                std::cerr<<"out_of_range: "<<pointers[vectorIndex]<<std::endl;
                exit(1);
            }
        }
        else
        {
            rapidjson::Pointer ptr(pointers[vectorIndex].c_str());
            value = ptr.Get(*value);
        }
        if (!value)
        {
            break;
        }
    }
    return convertRapidVJsonValueToStr(value);
}

class Match_Rule
{
public:
    enum Match_Type
    {
        EQUALS_TO = 0,
        START_WITH,
        END_WITH,
        CONTAINS,
        REGEX_MATCH
    };

    Match_Type match_type;
    std::string header_name;
    std::string json_pointer;
    std::string object;
    mutable uint64_t unique_id;
    std::regex reg_exp;
    Match_Rule(const Schema_Header_Match& header_match)
    {
        object = header_match.input;
        header_name = header_match.header;
        json_pointer = "";
        match_type = string_to_match_type[header_match.matchType];
        if (REGEX_MATCH == match_type)
        {
            reg_exp.assign(object, std::regex_constants::grep|std::regex_constants::optimize);
        }
    }
    Match_Rule(const Schema_Payload_Match& payload_match)
    {
        object = payload_match.input;
        header_name = "";
        json_pointer = payload_match.jsonPointer;
        match_type = string_to_match_type[payload_match.matchType];
        if (REGEX_MATCH == match_type)
        {
            reg_exp.assign(object, std::regex_constants::grep|std::regex_constants::optimize);
        }
    }

    std::map<std::string, Match_Type> string_to_match_type {{"EqualsTo", EQUALS_TO}, {"StartsWith", START_WITH}, {"EndsWith", END_WITH}, {"Contains", CONTAINS}, {"RegexMatch", REGEX_MATCH}};

    bool match(const std::string& subject, Match_Type verb, const std::string& object) const
    {
        switch (verb)
        {
            case EQUALS_TO:
            {
                return (subject == object);
            }
            case START_WITH:
            {
                return (subject.find(object) == 0);
            }
            case END_WITH:
            {
                return (subject.size() >= object.size() && 0 == subject.compare(subject.size() - object.size(), object.size(), object));
            }
            case CONTAINS:
            {
                return (subject.find(object) != std::string::npos);
            }
            case REGEX_MATCH:
            {
                return std::regex_match(subject, reg_exp);
            }
        }
        return false;
    }

    bool match_header(const std::map<std::string, std::string, ci_less>& response_headers) const
    {
        auto it = response_headers.find(header_name);
        if (it != response_headers.end())
        {
            return match(it->second, match_type, object);
        }
        return false;
    }

    bool match_json_doc(const rapidjson::Document& d) const
    {
        return match(getValueFromJsonPtr(d, json_pointer), match_type, object);
    }

    bool match(const std::map<std::string, std::string, ci_less>& response_headers, const rapidjson::Document& d)
    {
        if (header_name.size())
        {
            return match_header(response_headers);
        }
        else
        {
            return match_json_doc(d);
        }
    }
};

class Request {
public:
    bool clear_old_cookies;
    std::string luaScript;
    bool make_request_function_present;
    bool validate_response_function_present;
    std::string schema;
    std::string authority;
    std::string path;
    Uri uri;
    std::string method;
    std::string payload;
    std::vector<std::string> additonalHeaders;
    uint32_t expected_status_code; // staticJson does not accept uint16_t
    Schema_Response_Match response_match;
    std::vector<Match_Rule> response_match_rules;
    std::map<std::string, std::string, ci_less> headers_in_map;
    std::vector<std::string> tokenized_path;
    std::vector<std::string> tokenized_payload;
    uint32_t delay_before_executing_next;
    void staticjson_init(staticjson::ObjectHandler* h)
    {
        h->add_property("luaScript", &this->luaScript, staticjson::Flags::Optional);
        h->add_property("uri", &this->uri);
        h->add_property("method", &this->method);
        h->add_property("payload", &this->payload, staticjson::Flags::Optional);
        h->add_property("additonalHeaders", &this->additonalHeaders, staticjson::Flags::Optional);
        h->add_property("clear-old-cookies", &this->clear_old_cookies, staticjson::Flags::Optional);
        h->add_property("expected-status-code", &this->expected_status_code, staticjson::Flags::Optional);
        h->add_property("delay-before-executing-next", &this->delay_before_executing_next, staticjson::Flags::Optional);
        h->add_property("response-match", &this->response_match, staticjson::Flags::Optional);
    }
    explicit Request()
    {
        clear_old_cookies = false;
        expected_status_code = 0;
        delay_before_executing_next = 0;
        make_request_function_present = false;
        validate_response_function_present = false;
    }
};

class Scenario
{
public:
    std::string name;
    uint32_t weight;
    std::string variable_name_in_path_and_data;
    std::string user_id_list_file;
    std::vector<std::vector<std::string>> user_ids;
    uint64_t variable_range_start;
    uint64_t variable_range_end;
    bool variable_range_slicing;
    std::vector<Request> requests;
    void staticjson_init(staticjson::ObjectHandler* h)
    {
        h->add_property("name", &this->name);
        h->add_property("weight", &this->weight, staticjson::Flags::Optional);
        h->add_property("user-id-variable-in-path-and-data", &this->variable_name_in_path_and_data, staticjson::Flags::Optional);
        h->add_property("user-id-list-file", &this->user_id_list_file, staticjson::Flags::Optional);
        h->add_property("user-id-range-start", &this->variable_range_start, staticjson::Flags::Optional);
        h->add_property("user-id-range-end", &this->variable_range_end, staticjson::Flags::Optional);
        h->add_property("user-id-range-slicing", &this->variable_range_slicing, staticjson::Flags::Optional);
        h->add_property("Requests", &this->requests);
    }
    explicit Scenario():
        variable_name_in_path_and_data(""),
        variable_range_start(0),
        variable_range_end(0),
        variable_range_slicing(false),
        weight(100)
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
    uint32_t rate_period;
    uint64_t duration;
    uint64_t warm_up_time;
    uint64_t connection_active_timeout;
    uint64_t connection_inactivity_timeout;
    std::string npn_list;
    uint64_t header_table_size;
    uint64_t encoder_header_table_size;
    std::string log_file;
    uint32_t statistics_interval;
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
    uint32_t interval_to_send_ping;
    std::vector<Scenario> scenarios;
    uint32_t builtin_server_port;
    std::string failed_request_log_file;

    explicit Config_Schema():
        schema("http"),
        host(""),
        port(80),
        threads(1),
        clients(1),
        max_concurrent_streams(1),
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
        statistics_interval(5),
        request_per_second(0),
        nreqs(0),
        stream_timeout_in_ms(5000),
        max_tls_version("TLSv1.3"),
        open_new_connection_based_on_authority_header(false),
        connection_retry_on_disconnect(false),
        connect_back_to_preferred_host(false),
        interval_to_send_ping(0),
        builtin_server_port(8888)
    {
    }

    void staticjson_init(staticjson::ObjectHandler* h)
    {
        h->add_property("schema", &this->schema);
        h->add_property("host", &this->host);
        h->add_property("port", &this->port, staticjson::Flags::Optional);
        h->add_property("open-new-connection-based-on-authority-header", &this->open_new_connection_based_on_authority_header, staticjson::Flags::Optional);
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
        h->add_property("switch-back-after-connection-retry", &this->connect_back_to_preferred_host, staticjson::Flags::Optional);
        h->add_property("interval-between-ping-frames", &this->interval_to_send_ping, staticjson::Flags::Optional);
        h->add_property("builtin-server-listening-port", &this->builtin_server_port, staticjson::Flags::Optional);
        h->add_property("failed-request-log-file", &this->failed_request_log_file, staticjson::Flags::Optional);
    }
};

#endif
