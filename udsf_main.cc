#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include "udsf_data_store.h"
#include "udsf_util.h"
#include "asio_util.h"
#include "util.h"
#include "H2Server_Response.h"

void handle_incoming_http2_message(const nghttp2::asio_http2::server::asio_server_request& req,
                             nghttp2::asio_http2::server::asio_server_response& res,
                             uint64_t handler_id, int32_t stream_id)
{
    auto get_server_io_service = [handler_id]()
    {
        return nghttp2::asio_http2::server::base_handler::find_io_service(handler_id);
    };

    static thread_local auto io_service = get_server_io_service();
    const std::string PATH_DELIMETER = "/";
    const std::string QUERY_DELIMETER = "&";
    const size_t REALM_ID_INDEX = 2;
    const size_t STORAGE_ID_INDEX = 3;
    const size_t RECORDS_INDEX = 4;
    const size_t RECORD_ID_INDEX = 5;
    auto& incoming_msg_method = req.method();
    auto& incoming_msg_payload = req.unmutable_payload();
    auto& incoming_msg_path = req.uri().path;
    auto& raw_query = req.uri().raw_query;
    auto incoming_msg_query = util::percent_decode(raw_query.begin(), raw_query.end());
    auto& incoming_msg_headers =  req.header();
    auto path_tokens = tokenize_string(incoming_msg_path, PATH_DELIMETER);
    auto query_tokens = tokenize_string(incoming_msg_query, QUERY_DELIMETER);

    nghttp2::asio_http2::header_map resp_headers;
    uint32_t status_code = 200;
    std::string resp_payload;
    
    res.write_head(status_code, std::move(resp_headers));
    res.end(std::move(resp_payload));
}



void asio_svr_entry(const H2Server_Config_Schema& config_schema)
{
    try
    {
        std::size_t num_threads = config_schema.threads;

        nghttp2::asio_http2::server::asio_httpx_server server(config_schema);

        server.num_threads(num_threads);

        server.handle("/", handle_incoming_http2_message);

        std::string addr = config_schema.address;
        std::string port = std::to_string(config_schema.port);
        std::cout << "addr: " << addr << ", port: " << port << std::endl;

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

            nghttp2::asio_http2::server::configure_tls_context_easy(ec, tls, enable_mTLS);

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
    return 0;
}

