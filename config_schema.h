#ifndef H2LOAD_CONFIG_SCHEMA_H
#define H2LOAD_CONFIG_SCHEMA_H

#include <iostream>

#include "staticjson/document.hpp"
#include "staticjson/staticjson.hpp"
#include "rapidjson/schema.h"
#include "rapidjson/prettywriter.h"

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

class Request {
public:
    bool clear_old_cookies;
    std::string luaScript;
    std::string schema;
    std::string authority;
    std::string path;
    Uri uri;
    std::string method;
    std::string payload;
    std::vector<std::string> additonalHeaders;
    uint32_t expected_status_code; // staticJson does not accept uint16_t
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
    }
    explicit Request()
    {
        clear_old_cookies = false;
        expected_status_code = 0;
        delay_before_executing_next = 0;
    }
};

class Scenario
{
public:
    std::string name;
    uint32_t weight;
    std::string variable_name_in_path_and_data;
    uint64_t variable_range_start;
    uint64_t variable_range_end;
    bool variable_range_slicing;
    std::vector<Request> requests;
    void staticjson_init(staticjson::ObjectHandler* h)
    {
        h->add_property("name", &this->name);
        h->add_property("weight", &this->weight, staticjson::Flags::Optional);
        h->add_property("user-id-variable-in-path-and-data", &this->variable_name_in_path_and_data, staticjson::Flags::Optional);
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
    double request_per_second;
    std::string rps_file;
//    std::string variable_name_in_path_and_data;
//    uint64_t variable_range_start;
//    uint64_t variable_range_end;
    uint64_t nreqs;
    uint32_t stream_timeout_in_ms;
//    bool variable_range_slicing;
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
        request_per_second(0),
//        variable_name_in_path_and_data(""),
//        variable_range_start(0),
//        variable_range_end(0),
//        variable_range_slicing(false),
        nreqs(0),
        stream_timeout_in_ms(5000),
        max_tls_version("TLSv1.3"),
        open_new_connection_based_on_authority_header(false),
        connection_retry_on_disconnect(false),
        connect_back_to_preferred_host(false),
        interval_to_send_ping(0)
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
        h->add_property("request-per-second", &this->request_per_second, staticjson::Flags::Optional);
        h->add_property("request-per-second-feed-file", &this->rps_file, staticjson::Flags::Optional);
//        h->add_property("variable-name-in-path-and-data", &this->variable_name_in_path_and_data, staticjson::Flags::Optional);
//        h->add_property("variable-range-start", &this->variable_range_start, staticjson::Flags::Optional);
//        h->add_property("variable-range-end", &this->variable_range_end, staticjson::Flags::Optional);
//        h->add_property("variable-range-slicing", &this->variable_range_slicing, staticjson::Flags::Optional);
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
    }

};

#endif
