#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include "udsf_data_store.h"
#include "sba_util.h"
#include "asio_util.h"
#include "util.h"
#include "H2Server_Response.h"

extern bool debug_mode;
extern h2load::Config g_egress_config;

bool schema_loose_check = true;
size_t min_concurrent_clients = 10;

extern thread_local size_t g_current_thread_id;
extern size_t number_of_worker_thread;
extern std::vector<boost::asio::io_service* > g_io_services;
extern std::vector<boost::asio::io_service::strand> g_strands;

template <class Request, class Result>
class Distributed_Request
{
public:
    Request request;
    std::vector<Result> results;
};

class Search_Result
{
public:
    std::set<std::string> matched_records;
    size_t matched_record_count = 0;
    bool search_done = false;
};

class Search_Request
{
public:
    std::string method;
    bool count_indicator = false;
    size_t number_limit = 0;
    size_t max_payload_size = 0;
    bool meta_only = false;
    rapidjson::Document search_exp;
};

thread_local std::map<std::pair<size_t, int32_t>, Distributed_Request<Search_Request, Search_Result>>
                                                                                                   filter_based_search;

std::map<std::string, std::string> get_queries(const nghttp2::asio_http2::server::asio_server_request& req)
{
    auto& raw_query = req.uri().raw_query;
    std::map<std::string, std::string> queries;
    if (raw_query.size())
    {
        auto query = util::percent_decode(raw_query.begin(), raw_query.end());
        auto query_tokens = tokenize_string(query, QUERY_DELIMETER);
        for (auto& q : query_tokens)
        {
            auto p = tokenize_string(q, EQUAL);
            if (p.size() != 2)
            {
                std::cerr << "illformed query: " << q << std::endl;
                continue;
            }
            queries[p[0]] = p[1];
        }
    }
    return queries;
}

void handle_incoming_http2_message(const nghttp2::asio_http2::server::asio_server_request& req,
                                   nghttp2::asio_http2::server::asio_server_response& res,
                                   uint64_t handler_id, int32_t stream_id)
{
    send_error_response(res);
}

void udsf_entry(const H2Server_Config_Schema& config_schema)
{
    try
    {
        std::size_t num_threads = config_schema.threads;

        nghttp2::asio_http2::server::asio_httpx_server server(config_schema);

        server.num_threads(num_threads);

        server.handle("/", handle_incoming_http2_message);

        std::string addr = config_schema.address;
        std::string port = std::to_string(config_schema.port);
        std::cerr << "addr: " << addr << ", port: " << port << std::endl;

        boost::system::error_code ec;
        if (config_schema.cert_file.size() && config_schema.private_key_file.size())
        {
            if (config_schema.verbose)
            {
                std::cout << "cert file: " << config_schema.cert_file << std::endl;
                std::cout << "private key file: " << config_schema.private_key_file << std::endl;
            }
            boost::asio::ssl::context tls(boost::asio::ssl::context::sslv23);
            tls.use_private_key_file(config_schema.private_key_file, boost::asio::ssl::context::pem);
            tls.use_certificate_chain_file(config_schema.cert_file);
            bool enable_mTLS = config_schema.enable_mTLS;
            if (enable_mTLS)
            {
                if (config_schema.verbose)
                {
                    std::cout << "ca cert file: " << config_schema.ca_cert_file << std::endl;
                }
                if (config_schema.ca_cert_file.size())
                {
                    tls.load_verify_file(config_schema.ca_cert_file);
                }
                else
                {
                    std::cerr << "mTLS enabled, but no CA cert file given, mTLS is thus disabled" << std::endl;
                    enable_mTLS = false;
                }
            }

            nghttp2::asio_http2::server::configure_tls_context_easy(ec, tls, enable_mTLS, config_schema.ciphers);

            if (server.listen_and_serve(ec, tls, addr, port))
            {
                std::cerr << "error: " << ec.message() << std::endl;
            }
        }
        else
        {
            if (server.listen_and_serve(ec, addr, port))
            {
                std::cerr << "error: " << ec.message() << std::endl;
            }
        }
    }
    catch (std::exception& e)
    {
        std::cerr << "exception: " << e.what() << "\n";
    }
}

int main(int argc, char** argv)
{
    H2Server_Config_Schema config_schema;

    if (argc < 2)
    {
        config_schema.address = "0.0.0.0";
        config_schema.port = 8081;
        config_schema.threads = std::thread::hardware_concurrency();
    }
    else
    {
        std::string config_file_name = argv[1];
        std::ifstream buffer(config_file_name);
        std::string jsonStr((std::istreambuf_iterator<char>(buffer)), std::istreambuf_iterator<char>());

        staticjson::ParseStatus result;
        if (!staticjson::from_json_string(jsonStr.c_str(), &config_schema, &result))
        {
            std::cout << "error reading config file:" << result.description() << std::endl;
            exit(1);
        }

        rapidjson::Document udsf_config;
        udsf_config.Parse(jsonStr.c_str());
        if (udsf_config.HasParseError())
        {
            std::cout << "error reading config file "<< std::endl;
            exit(1);
        }

        min_concurrent_clients = get_uint64_from_json_ptr(udsf_config, "/minimum-egress-concurrent-connections");
        g_egress_config.json_config_schema.cert_verification_mode = get_uint64_from_json_ptr(udsf_config, "/certVerificationMode");
        g_egress_config.json_config_schema.interval_to_send_ping = get_uint64_from_json_ptr(udsf_config, "/interval-between-ping-frames");
        g_egress_config.ciphers = config_schema.ciphers;
        g_egress_config.connection_window_bits = config_schema.connection_window_bits;
        g_egress_config.encoder_header_table_size = config_schema.encoder_header_table_size;
        g_egress_config.json_config_schema.ca_cert = config_schema.ca_cert_file;
        g_egress_config.json_config_schema.client_cert = config_schema.cert_file;
        g_egress_config.json_config_schema.private_key = config_schema.private_key_file;
        g_egress_config.json_config_schema.skt_recv_buffer_size = config_schema.skt_recv_buffer_size;
        g_egress_config.json_config_schema.skt_send_buffer_size = config_schema.skt_send_buffer_size;
        g_egress_config.max_concurrent_streams = config_schema.max_concurrent_streams;
        init_config_for_egress();

        if (config_schema.verbose)
        {
            std::cerr << "Configuration dump:" << std::endl << staticjson::to_pretty_json_string(config_schema)
                      << std::endl;
            debug_mode = true;
        }
    }

    udsf_entry(config_schema);

    return 0;
}

