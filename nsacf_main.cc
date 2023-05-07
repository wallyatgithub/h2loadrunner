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
#include "udsf_data_store.h"

extern bool debug_mode;

const std::string nsacf_base_uri = "/nnsacf-nsac/v1";
const size_t nsacf_number_of_tokens_in_api_prefix = 0;
const std::string nsacf_api_prefix = "";

const size_t SLICES_INDEX = nsacf_number_of_tokens_in_api_prefix + 2;

const std::string SLICES = "slices";

const std::string UES = "ues";

const std::string PDUS = "pdus";

const std::string udsf_address = "http://127.0.0.1:8081/nudsf-dr/v1";

const std::string REALM_NAME = "realm";

const std::string STORAGE_PREFIX = "snssai-";

const std::string UE_RECORD_PREFIX = "ue-";

const std::string NFTYPE = "nfType";

const std::string NFID = "NfId";

const std::string PLMNID = "plmnId";

const std::string STATUS = ":status";

class Ues_Update_Result
{
public:
    std::string supi;
    std::string snssai;
    bool update_done = false;
    bool update_success = false;
    explicit Ues_Update_Result(const std::string& su, const std::string& ns)
    :supi(su), snssai(ns), update_done(false), update_success(false)
    {
    };
};

class Ingress_Request_Identify
{
public:
    boost::asio::io_service* source_ios = nullptr;
    uint64_t handler_id = 0;
    int32_t stream_id = 0;
    explicit Ingress_Request_Identify(boost::asio::io_service* ios, uint64_t h_id, int32_t s_id):
    source_ios(ios), handler_id(h_id), stream_id(s_id)
    {
    }
};

thread_local std::map<std::pair<size_t, int32_t>, std::vector<Ues_Update_Result>> UeAcrStatus;

void merge_result(Ingress_Request_Identify source_req_identity, size_t acu_index, bool success_or_failure)
{
    auto handler_id = source_req_identity.handler_id;
    auto stream_id = source_req_identity.stream_id;
    auto run_in_worker = [handler_id, stream_id, acu_index, success_or_failure]()
    {
        auto iter = UeAcrStatus.find(std::pair<size_t, int32_t>(handler_id, stream_id));
        if (iter != UeAcrStatus.end())
        {
            iter->second[acu_index].update_done = true;
            iter->second[acu_index].update_success = success_or_failure;
            bool update_all_done = true;
            bool all_success = true;
            for (auto& result : iter->second)
            {
                if (!result.update_done)
                {
                    update_all_done = false;
                    break;
                }
                if (!result.update_success)
                {
                    all_success = false;
                    break;
                }
            }
            if (update_all_done)
            {
                auto all_result = std::move(iter->second);
                UeAcrStatus.erase(iter);

                auto handler = nghttp2::asio_http2::server::base_handler::find_handler(handler_id);
                if (!handler)
                {
                    return;
                }
                auto orig_stream = handler->find_stream(stream_id);
                if (!orig_stream)
                {
                    return;
                }
                auto& res = orig_stream->response();
                auto& req = orig_stream->request();

                if (all_success)
                {
                    res.write_head(204);
                    res.end();
                }
                else
                {
                    UeACResponseData response;
                    for (auto& result : all_result)
                    {
                        if (!result.update_success)
                        {
                            AcuFailureItem item;
                            item.snssai = result.snssai;
                            response.acuFailureList[result.supi].push_back(std::move(item));
                        }
                    }
                    auto body = staticjson::to_json_string(response);
                    res.write_head(200, {{CONTENT_TYPE, {JSON_CONTENT}}, {CONTENT_LENGTH, {std::to_string(body.size())}}});
                    res.end(std::move(body));
                }
            }
        }
    };
    if (g_io_services[g_current_thread_id] != source_req_identity.source_ios)
    {
        source_req_identity.source_ios->post(run_in_worker);
    }
    else
    {
        run_in_worker();
    }
}

void process_udsf_ues_update_response(const Ingress_Request_Identify& source_req_identity,
                                  size_t acu_index,
                                  const std::vector<std::map<std::string, std::string, ci_less>>& resp_headers,
                                  const std::string& resp_payload)
{
    bool update_success = false;
    if (resp_headers.size())
    {
        auto code_iter = resp_headers[0].find(STATUS);
        if ((code_iter != resp_headers[0].end()) &&
            (code_iter->second == "201" || code_iter->second == "204" || code_iter->second == "200"))
        {
            update_success = true;
        }
    }
    merge_result(source_req_identity, acu_index, update_success);
}

void add_ue_snssai_record(const Ingress_Request_Identify& source_req_identity,
                                  size_t acu_index,
                                  const std::string& snsaai, const std::string& supi,
                                  const std::string& nfType, const std::string& nfId,
                                  const std::string& plmnid,
                                  udsf::Record record)
{
    std::string uri;
    uri.append(udsf_base_uri).append(PATH_DELIMETER).append(REALM_NAME).append(PATH_DELIMETER).append(STORAGE_PREFIX).append(snsaai).append(PATH_DELIMETER).append(RESOUCE_RECORDS);
    uri.append(PATH_DELIMETER).append(UE_RECORD_PREFIX).append(supi);
    auto r = std::move(record);
    r.meta.tags[NFTYPE].emplace_back(nfType);
    r.meta.tags[NFID].emplace_back(nfId);
    r.meta.tags[PLMNID].emplace_back(plmnid);
    std::map<std::string, std::string, ci_less> additionalHeaders;
    auto body = r.produce_multipart_body(true, "");
    additionalHeaders.insert(std::make_pair(CONTENT_TYPE, MULTIPART_CONTENT_TYPE));
    auto process_response = [source_req_identity, acu_index](const std::vector<std::map<std::string, std::string, ci_less>>& resp_headers, const std::string& resp_payload)
    {
        process_udsf_ues_update_response(source_req_identity, acu_index, resp_headers, resp_payload);
    };
    send_http2_request(METHOD_PUT, uri, process_response, additionalHeaders, body);
}

void delete_ue_snssai_record(const Ingress_Request_Identify& source_req_identity,
                                  size_t acu_index,
                                  const std::string& snsaai, const std::string& supi)
{
    std::string uri;
    uri.append(udsf_base_uri).append(PATH_DELIMETER).append(REALM_NAME).append(PATH_DELIMETER).append(STORAGE_PREFIX).append(snsaai).append(PATH_DELIMETER).append(RESOUCE_RECORDS);
    uri.append(PATH_DELIMETER).append(UE_RECORD_PREFIX).append(supi);
    auto process_response = [source_req_identity, acu_index](const std::vector<std::map<std::string, std::string, ci_less>>& resp_headers, const std::string& resp_payload)
    {
        process_udsf_ues_update_response(source_req_identity, acu_index, resp_headers, resp_payload);
    };
    send_http2_request(METHOD_DELETE, uri, process_response);
}

void process_udsf_ues_read_response(const Ingress_Request_Identify& source_req_identity,
                                  size_t acu_index,
                                  const std::string& supi, const AcuOperationItem& acu_item,
                                  const std::vector<std::map<std::string, std::string, ci_less>>& resp_headers,
                                  const std::string& resp_payload)
{
    udsf::Record record(UE_RECORD_PREFIX + supi);
    if (resp_headers.size())
    {
        auto code_iter = resp_headers[0].find(STATUS);
        if (code_iter != resp_headers[0].end() && code_iter->second == "200")
        {
        }
    }
}

void read_ue_snssai_record(const Ingress_Request_Identify& source_req_identity,
                                   size_t acu_index,
                                   const std::string& supi, const AcuOperationItem& acu_item)
{
    std::string uri;
    uri.append(udsf_base_uri).append(PATH_DELIMETER).append(REALM_NAME).append(PATH_DELIMETER).append(STORAGE_PREFIX).append(acu_item.snssai).append(PATH_DELIMETER).append(RESOUCE_RECORDS);
    uri.append(PATH_DELIMETER).append(UE_RECORD_PREFIX).append(supi);
    auto process_response = [source_req_identity, supi, acu_index, acu_item](const std::vector<std::map<std::string, std::string, ci_less>>& resp_headers, const std::string& resp_payload)
    {
        process_udsf_ues_read_response(source_req_identity, acu_index, supi, acu_item, resp_headers, resp_payload);
    };
    send_http2_request(METHOD_GET, uri, process_response);
}


void process_one_ue_nssai(const Ingress_Request_Identify& source_req_identity,
                                  size_t acu_index, const std::string& supi, const AcuOperationItem& acu_item)
{
    
}

bool process_ues(boost::asio::io_service* source_ios, const nghttp2::asio_http2::server::asio_server_request& req,
                               nghttp2::asio_http2::server::asio_server_response& res,
                               uint64_t handler_id, int32_t stream_id,
                               const std::string& msg_body)
{
    UeACRequestData reqData;
    staticjson::ParseStatus result;
    if (staticjson::from_json_string(msg_body.c_str(), &reqData, &result))
    {
        Ingress_Request_Identify in_req_id(source_ios, handler_id, stream_id);
        
        std::vector<Ues_Update_Result> update_result;
        update_result.reserve(reqData.ueACRequestInfo.size());
        size_t acu_index = 0;
        for (UeACRequestInfo& reqInfo: reqData.ueACRequestInfo)
        {
            for (AcuOperationItem& ac: reqInfo.acuOperationList)
            {
                update_result.emplace_back(reqInfo.supi, ac.snssai);
                process_one_ue_nssai(in_req_id, acu_index, reqInfo.supi, ac);
                acu_index++;
            }
        }
    }
    return false;
}

void handle_incoming_http2_message(const nghttp2::asio_http2::server::asio_server_request& req,
                                   nghttp2::asio_http2::server::asio_server_response& res,
                                   uint64_t handler_id, int32_t stream_id)
{
    bool ret = false;
    static thread_local auto io_service = g_io_services[g_current_thread_id];
    static thread_local boost::asio::deadline_timer tick_timer(*io_service);
    static thread_local std::multimap<std::chrono::steady_clock::time_point, std::pair<uint64_t, int32_t>> active_requests;
    static thread_local auto dummy = start_tick_timer(tick_timer, active_requests);
    const std::chrono::milliseconds REQUEST_TTL(15000);
    active_requests.insert(std::make_pair(std::chrono::steady_clock::now() + REQUEST_TTL, std::make_pair(handler_id,
                                                                                                         stream_id)));
    auto method = req.method();
    util::inp_strlower(method);
    auto& payload = req.unmutable_payload();
    auto& path = req.uri().path;
    auto& raw_query = req.uri().raw_query;
    auto query = util::percent_decode(raw_query.begin(), raw_query.end());

    auto path_tokens = tokenize_string(path, PATH_DELIMETER);
    if (path_tokens.size() && path_tokens[0].empty())
    {
        path_tokens.erase(path_tokens.begin());
    }

    if (path_tokens.size() > SLICES_INDEX && method == METHOD_POST)
    {
        auto& slices = path_tokens[SLICES_INDEX];
        auto& type = path_tokens[SLICES_INDEX + 1];
        if (SLICES == slices && type == UES)
        {
            ret = process_ues(io_service, req, res, handler_id, stream_id, payload);
        }
        else if (SLICES == slices && type == PDUS)
        {
            ///ret = process_pdus(req, res, handler_id, stream_id, payload);
        }
    }

    if (!ret)
    {
        send_error_response(res);
    }
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

        if (config_schema.verbose)
        {
            std::cerr << "Configuration dump:" << std::endl << staticjson::to_pretty_json_string(config_schema)
                      << std::endl;
            debug_mode = true;
        }
    }

    nsacf_entry(config_schema);

    return 0;
}


