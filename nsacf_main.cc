#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include "nsacf.h"
#include "sba_util.h"
#include "asio_util.h"
#include "util.h"
#include "H2Server_Response.h"
#define NO_UDSF_RECORD_LOCK 1
#include "udsf_data_store.h"

extern bool debug_mode;
extern h2load::Config g_egress_config;

const std::string nsacf_base_uri = "/nnsacf-nsac/v1";
const size_t nsacf_number_of_tokens_in_api_prefix = 0;
const std::string nsacf_api_prefix = "";

const size_t SLICES_INDEX = nsacf_number_of_tokens_in_api_prefix + 2;

const std::string SLICES = "slices";

const std::string UES = "ues";

const std::string PDUS = "pdus";

std::string udsf_address = "http://127.0.0.1:8081/nudsf-dr/v1";

const std::string REALM_NAME = "realm";

const std::string STORAGE_PREFIX = "snssai-";

const std::string UE_RECORD_PREFIX = "ue-";

const std::string NFTYPE = "nfType";

const std::string NFID = "NfId";

const std::string PLMNID = "plmnId";

const std::string STATUS = ":status";

const std::string ACU_INCREASE = "INCREASE";
const std::string ACU_DECREASE = "DECREASE";
const std::string ACU_UPDATE = "UPDATE";

const std::string TAG_ACCESS_TYPE = "anType";
const std::string TAG_NF_ID = "nfId";
const std::string TAG_NF_TYPE = "nfType";

size_t MAX_UEs = 2000;
size_t min_concurrent_clients = 10;


void handle_incoming_http2_message(const nghttp2::asio_http2::server::asio_server_request& req,
                                   nghttp2::asio_http2::server::asio_server_response& res,
                                   uint64_t handler_id, int32_t stream_id)
{
    send_error_response(res);
}


void nsacf_entry(const H2Server_Config_Schema& config_schema)
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
    H2Server_Config_Schema server_config_schema;

    if (argc < 2)
    {
        server_config_schema.address = "0.0.0.0";
        server_config_schema.port = 8082;
        server_config_schema.threads = std::thread::hardware_concurrency();
    }
    else
    {
        std::string config_file_name = argv[1];
        std::ifstream buffer(config_file_name);
        std::string jsonStr((std::istreambuf_iterator<char>(buffer)), std::istreambuf_iterator<char>());

        staticjson::ParseStatus result;
        if (!staticjson::from_json_string(jsonStr.c_str(), &server_config_schema, &result))
        {
            std::cout << "error reading config file:" << result.description() << std::endl;
            exit(1);
        }

        rapidjson::Document nsacf_config;
        nsacf_config.Parse(jsonStr.c_str());
        if (nsacf_config.HasParseError())
        {
            std::cout << "error reading config file "<< std::endl;
            exit(1);
        }
        
        udsf_address = get_string_from_json_ptr(nsacf_config, "/udsf-url");
        MAX_UEs = get_uint64_from_json_ptr(nsacf_config, "/max-number-of-ues");
        min_concurrent_clients = get_uint64_from_json_ptr(nsacf_config, "/minimum-egress-concurrent-connections");
        g_egress_config.json_config_schema.cert_verification_mode = get_uint64_from_json_ptr(nsacf_config, "/certVerificationMode");
        g_egress_config.json_config_schema.interval_to_send_ping = get_uint64_from_json_ptr(nsacf_config, "/interval-between-ping-frames");
        g_egress_config.ciphers = server_config_schema.ciphers;
        g_egress_config.connection_window_bits = server_config_schema.connection_window_bits;
        g_egress_config.encoder_header_table_size = server_config_schema.encoder_header_table_size;
        g_egress_config.json_config_schema.ca_cert = server_config_schema.ca_cert_file;
        g_egress_config.json_config_schema.client_cert = server_config_schema.cert_file;
        g_egress_config.json_config_schema.private_key = server_config_schema.private_key_file;
        g_egress_config.json_config_schema.skt_recv_buffer_size = server_config_schema.skt_recv_buffer_size;
        g_egress_config.json_config_schema.skt_send_buffer_size = server_config_schema.skt_send_buffer_size;
        g_egress_config.max_concurrent_streams = server_config_schema.max_concurrent_streams;
        init_config_for_egress();

        if (server_config_schema.verbose)
        {
            std::cerr << "Configuration dump:" << std::endl << staticjson::to_pretty_json_string(server_config_schema)
                      << std::endl;
            debug_mode = true;
            g_egress_config.verbose = true;
        }

    }

    nsacf_entry(server_config_schema);

    return 0;
}


