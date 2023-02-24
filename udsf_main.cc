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


std::string get_local_api_root()
{
    return ""; // TODO:
}

std::map<std::string, std::string> get_queries(const nghttp2::asio_http2::server::asio_server_request& req)
{
    auto& raw_query = req.uri().raw_query;
    auto query = util::percent_decode(raw_query.begin(), raw_query.end());
    auto query_tokens = tokenize_string(query, QUERY_DELIMETER);
    std::map<std::string, std::string> queries;
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
    return queries;
}

std::string get_boundary(const std::string& content_type)
{
    std::string boundary;
    const std::string BOUNDARY = "boundary=";
    auto boundary_start = content_type.find(BOUNDARY);
    if (boundary_start != std::string::npos)
    {
        boundary = content_type.substr(boundary_start + BOUNDARY.size(), std::string::npos);
        boundary = boundary.substr(0, boundary.find(";"));
    }
    return boundary;
}

bool process_create_or_update_record(const nghttp2::asio_http2::server::asio_server_request& req,
                                     nghttp2::asio_http2::server::asio_server_response& res,
                                     uint64_t handler_id, int32_t stream_id,
                                     const std::string& record_id, const std::string& msg_body,
                                     udsf::Storage& storage)
{
    auto error_return = [&res]()
    {
        const std::string body = "content-type header with multipart boundary not found";
        res.write_head(400);
        res.end(body);
        return true;
    };
    auto& req_headers =  req.header();

    auto iter = req_headers.find(CONTENT_TYPE);
    if (iter == req_headers.end())
    {
        return error_return();
    }

    auto boundary = get_boundary(iter->second.value);
    if (boundary.empty())
    {
        return error_return();
    }

    std::string previous_body;
    auto queries = get_queries(req);
    if (queries[GET_PREVIOUS] == TRUE)
    {
        previous_body = storage.get_record(record_id);
    }

    bool update = false;
    auto ret = storage.insert_or_update_record(record_id, boundary, msg_body, update);
    switch (ret)
    {
        case 0:
        {
            if (update)
            {
                if (previous_body.size())
                {
                    res.write_head(200);
                    res.end(std::move(previous_body));
                }
                else
                {
                    res.write_head(204);
                    res.end();
                }
            }
            else
            {
                std::string location = get_local_api_root();
                location.append(udsf_base_uri);
                location.append(RECORDS).append(PATH_DELIMETER).append(record_id);
                res.write_head(201, {{LOCATION, {location}}});
                res.end();
            }
            break;
        }
        default:
        {
            res.write_head(400);
            const std::string bad_request = "bad request";
            res.end(bad_request);
            break;
        }
    }
    return true;
}

bool process_get_record(const nghttp2::asio_http2::server::asio_server_request& req,
                        nghttp2::asio_http2::server::asio_server_response& res,
                        uint64_t handler_id, int32_t stream_id,
                        const std::string& record_id, const std::string& msg_body,
                        udsf::Storage& storage)
{
    auto body = storage.get_record(record_id);
    if (body.size())
    {
        res.write_head(200);
        res.end(std::move(body));
    }
    else
    {
        const std::string record_not_found = "record not found";
        res.write_head(404);
        res.end(record_not_found);
    }
    return true;
}

bool process_delete_record(const nghttp2::asio_http2::server::asio_server_request& req,
                           nghttp2::asio_http2::server::asio_server_response& res,
                           uint64_t handler_id, int32_t stream_id,
                           const std::string& record_id, const std::string& msg_body,
                           udsf::Storage& storage)
{
    auto queries = get_queries(req);
    if (queries[GET_PREVIOUS] == TRUE)
    {
        auto body = storage.get_record(record_id);
        auto ret = storage.delete_record_directly(record_id);
        if (ret && body.size())
        {
            res.write_head(200);
            res.end(std::move(body));
            return true;
        }
    }
    else
    {
        auto ret = storage.delete_record_directly(record_id);
        if (ret)
        {
            res.write_head(204);
            res.end();
            return true;
        }
    }
    const std::string body = "record not found";
    res.write_head(404);
    res.end(body);
    return true;
}

bool process_record(const nghttp2::asio_http2::server::asio_server_request& req,
                    nghttp2::asio_http2::server::asio_server_response& res,
                    uint64_t handler_id, int32_t stream_id,
                    const std::string& method,
                    const std::string& realm_id, const std::string& storage_id,
                    const std::string& record_id, const std::vector<std::string>& path_tokens,
                    const std::string& msg_body)
{
    auto& realm = udsf::get_realm(realm_id);

    auto& storage = realm.get_storage(storage_id);

    if (path_tokens.size() > BLOCK_ID_INDEX + 1)
    {
        const std::string& block_id = path_tokens[BLOCK_ID_INDEX];
        // TODO:  CRUD
    }
    else
    {
        if (method == METHOD_PUT || method == METHOD_PATCH)
        {
            return process_create_or_update_record(req, res, handler_id, stream_id, record_id, msg_body, storage);
        }
        else if (method == METHOD_GET)
        {
            return process_get_record(req, res, handler_id, stream_id, record_id, msg_body, storage);
        }
        else if (method == METHOD_DELETE)
        {
            return process_delete_record(req, res, handler_id, stream_id, record_id, msg_body, storage);
        }
        else
        {
            const std::string response = "method not allowed";
            res.write_head(403);
            res.end(response);
        }
    }
    return false;
}

void send_error_response(nghttp2::asio_http2::server::asio_server_response& res)
{
    uint32_t status_code = 403;
    std::string resp_payload = "request not allowed";
    res.write_head(status_code);
    res.end(std::move(resp_payload));
}

void handle_incoming_http2_message(const nghttp2::asio_http2::server::asio_server_request& req,
                                   nghttp2::asio_http2::server::asio_server_response& res,
                                   uint64_t handler_id, int32_t stream_id)
{
    bool ret = false;
    auto get_server_io_service = [handler_id]()
    {
        return nghttp2::asio_http2::server::base_handler::find_io_service(handler_id);
    };
    static thread_local auto io_service = get_server_io_service();

    auto method = req.method();
    util::inp_strlower(method);
    auto& payload = req.unmutable_payload();
    auto& path = req.uri().path;
    auto& raw_query = req.uri().raw_query;
    auto query = util::percent_decode(raw_query.begin(), raw_query.end());

    auto path_tokens = tokenize_string(path, PATH_DELIMETER);

    if (path_tokens.size() > RESOURCE_TYPE_INDEX + 1)
    {
        std::string& realm_id = path_tokens[REALM_ID_INDEX];
        std::string& storage_id = path_tokens[STORAGE_ID_INDEX];
        std::string& resource = path_tokens[RESOURCE_TYPE_INDEX];
        if (resource == RESOUCE_RECORDS)
        {
            if (path_tokens.size() > RECORD_ID_INDEX + 1)
            {
                std::string& record_id = path_tokens[RECORD_ID_INDEX];
                ret = process_record(req, res, handler_id, stream_id, method, realm_id, storage_id, record_id, path_tokens, payload);
            }
            else
            {
                //ret = process_records(req, res, handler_id, stream_id, method, realm_id, storage_id, path_tokens, payload);
            }
        }
    }

    if (!ret)
    {
        send_error_response(res);
    }
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

