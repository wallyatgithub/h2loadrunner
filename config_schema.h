#ifndef H2LOAD_CONFIG_SCHEMA_H
#define H2LOAD_CONFIG_SCHEMA_H


#include "staticjson/document.hpp"
#include "staticjson/staticjson.hpp"
#include "rapidjson/schema.h"
#include "rapidjson/prettywriter.h"

class Path {
public:
    std::string source;
    std::string input;
    std::string headerToExtract;
    void staticjson_init(staticjson::ObjectHandler* h)
    {
        h->add_property("source", &this->source);
        h->add_property("input", &this->input, staticjson::Flags::Optional);
        h->add_property("headerToExtract", &this->headerToExtract, staticjson::Flags::Optional);
    }
};

class Scenario {
public:
    std::string luaScript;
    Path path;
    std::string method;
    std::string payload;
    std::vector<std::string> additonalHeaders;
    std::map<std::string, std::string> headers_in_map;
    void staticjson_init(staticjson::ObjectHandler* h)
    {
        h->add_property("luaScript", &this->luaScript, staticjson::Flags::Optional);
        h->add_property("path", &this->path);
        h->add_property("method", &this->method);
        h->add_property("payload", &this->payload, staticjson::Flags::Optional);
        h->add_property("additonalHeaders", &this->additonalHeaders, staticjson::Flags::Optional);
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
    double request_per_second;
    std::string variable_name_in_path_and_data;
    uint64_t variable_range_start;
    uint64_t variable_range_end;
    uint64_t nreqs;
    uint32_t stream_timeout_in_ms;
    std::vector<Scenario> scenarios;

    Config_Schema():
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
        request_per_second(0),
        variable_name_in_path_and_data(""),
        variable_range_start(0),
        variable_range_end(0),
        nreqs(0),
        stream_timeout_in_ms(5000)
    {
    }

    void staticjson_init(staticjson::ObjectHandler* h)
    {
        h->add_property("schema", &this->schema);
        h->add_property("host", &this->host);
        h->add_property("port", &this->port, staticjson::Flags::Optional);
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
        h->add_property("request-per-second", &this->request_per_second, staticjson::Flags::Optional);
        h->add_property("variable-name-in-path-and-data", &this->variable_name_in_path_and_data, staticjson::Flags::Optional);
        h->add_property("variable-range-start", &this->variable_range_start, staticjson::Flags::Optional);
        h->add_property("variable-range-end", &this->variable_range_end, staticjson::Flags::Optional);
        h->add_property("scenarios", &this->scenarios);
    }

};

#endif
