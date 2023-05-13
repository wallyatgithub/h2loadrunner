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

const std::string TAG_ACCESS_TYPE = "accessType";
const std::string TAG_NF_ID = "nfId";
const std::string TAG_NF_TYPE = "nfType";

size_t MAX_UEs = 2000;
size_t min_concurrent_clients = 10;

class Ues_Update_Result
{
public:
    std::string supi;
    Nssai snssai;
    bool update_done = false;
    bool update_success = false;
    explicit Ues_Update_Result(const std::string& su, const Nssai& ns)
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

class Ues_Ac_Control_Block
{
public:
    Ingress_Request_Identify ingress_identity;
    size_t acu_index = 0;
    std::string supi;
    std::string nf_id;
    std::string nf_type;
    std::string access_type;
    std::string additional_access_type;
    AcuOperationItem acu_item;
    explicit Ues_Ac_Control_Block(boost::asio::io_service* ios, uint64_t h_id, int32_t s_id,
                                  size_t index,
                                  std::string uid,
                                  std::string nfId,
                                  std::string nfType,
                                  std::string accessType,
                                  std::string additionalAnType,
                                  AcuOperationItem item):
                                  ingress_identity(ios, h_id, s_id),
                                  acu_index(index),
                                  supi(std::move(uid)),
                                  nf_id(std::move(nfId)),
                                  nf_type(std::move(nfType)),
                                  access_type(std::move(accessType)),
                                  additional_access_type(std::move(additionalAnType)),
                                  acu_item(std::move(item))
    {
    }
    
};

thread_local std::map<std::pair<size_t, int32_t>, std::vector<Ues_Update_Result>> UeAcuStatus;


int64_t find_index_of_target_value(const udsf::RecordMeta& meta, const std::string& tag_name, const std::string& tag_value)
{
    int64_t ret = -1;
    auto iter = meta.tags.find(tag_name);
    if (iter == meta.tags.end())
    {
        return ret;
    }
    for (size_t i = 0; i < iter->second.size(); i++)
    {
        if (iter->second[i] == tag_value)
        {
            ret = i;
            return ret;
        }
    }
    return ret;
}

std::string remove_tag_value_at_index(udsf::RecordMeta& meta, const std::string& tag_name, size_t index)
{
    std::string ret;
    auto iter = meta.tags.find(tag_name);
    if (iter == meta.tags.end())
    {
        return ret;
    }
    if (iter->second.size() > index)
    {
        ret = std::move(iter->second[index]);
        iter->second.erase(iter->second.begin() + index);
    }
    return ret;
}

bool update_tag_value_at_index(udsf::RecordMeta& meta, const std::string& tag_name, size_t index, std::string tag_value)
{
    bool ret = true;
    auto& tag_values = meta.tags[tag_name];
    if (index + 1 > tag_values.size())
    {
        size_t gap = (index + 1 - tag_values.size());
        for (size_t count = 0; count < gap; count++)
        {
            tag_values.emplace_back("");
        }
        ret = false;
    }
    tag_values[index] = std::move(tag_value);

    return ret;
}

std::string get_tag_value_at_index(udsf::RecordMeta& meta, const std::string& tag_name, size_t index)
{
    std::string ret;
    auto iter = meta.tags.find(tag_name);
    if (iter == meta.tags.end())
    {
        return ret;
    }
    if (iter->second.size() > index)
    {
        ret = iter->second[index];
    }
    return ret;
}


void add_tag_value(udsf::RecordMeta& meta, const std::string& tag_name, std::string tag_value)
{
    meta.tags[tag_name].emplace_back(std::move(tag_value));
}

void merge_result(Ingress_Request_Identify& source_req_identity, size_t acu_index, bool success_or_failure)
{
    auto handler_id = source_req_identity.handler_id;
    auto stream_id = source_req_identity.stream_id;
    auto iter = UeAcuStatus.find(std::pair<size_t, int32_t>(handler_id, stream_id));
    if (iter != UeAcuStatus.end())
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
        if (!update_all_done)
        {
            return;
        }
        auto all_result = std::move(iter->second);
        UeAcuStatus.erase(iter);

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
                if (result.update_success)
                {
                    continue;
                }
                AcuFailureItem item;
                item.snssai = result.snssai;
                response.acuFailureList[result.supi].push_back(std::move(item));
            }
            auto body = staticjson::to_json_string(response);
            res.write_head(200, {{CONTENT_TYPE, {JSON_CONTENT}}, {CONTENT_LENGTH, {std::to_string(body.size())}}});
            res.end(std::move(body));
        }
    }
}

void process_udsf_ues_update_response(Ues_Ac_Control_Block& cb,
                                  std::vector<std::map<std::string, std::string, ci_less>>& resp_headers,
                                  std::string& resp_payload)
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
    merge_result(cb.ingress_identity, cb.acu_index, update_success);
}


void add_or_update_ue_snssai_record(Ues_Ac_Control_Block& cb, udsf::Record& record)
{
    std::string uri;
    uri.append(udsf_address).append(PATH_DELIMETER).append(REALM_NAME).append(PATH_DELIMETER).append(STORAGE_PREFIX).append(cb.acu_item.snssai.sst + "-" + cb.acu_item.snssai.sd).append(PATH_DELIMETER).append(RESOUCE_RECORDS);
    uri.append(PATH_DELIMETER).append(UE_RECORD_PREFIX).append(cb.supi);
    std::map<std::string, std::string, ci_less> additionalHeaders;
    auto body = record.produce_multipart_body(true, "");
    additionalHeaders.insert(std::make_pair(CONTENT_TYPE, MULTIPART_CONTENT_TYPE));
    additionalHeaders.insert(std::make_pair(CONTENT_LENGTH, std::to_string(body.size())));
    auto process_response = [cb = std::move(cb)](std::vector<std::map<std::string, std::string, ci_less>>& resp_headers, std::string& resp_payload) mutable
    {
        process_udsf_ues_update_response(cb, resp_headers, resp_payload);
    };
    send_http2_request(METHOD_PUT, uri, std::move(process_response), additionalHeaders, body);
}

void delete_ue_snssai_record(Ues_Ac_Control_Block& cb)
{
    std::string uri;
    uri.append(udsf_address).append(PATH_DELIMETER).append(REALM_NAME).append(PATH_DELIMETER).append(STORAGE_PREFIX).append(cb.acu_item.snssai.sst + "-" + cb.acu_item.snssai.sd).append(PATH_DELIMETER).append(RESOUCE_RECORDS);
    uri.append(PATH_DELIMETER).append(UE_RECORD_PREFIX).append(cb.supi);
    auto process_response = [cb = std::move(cb)](std::vector<std::map<std::string, std::string, ci_less>>& resp_headers, std::string& resp_payload) mutable
    {
        process_udsf_ues_update_response(cb, resp_headers, resp_payload);
    };
    send_http2_request(METHOD_DELETE, uri, std::move(process_response));
}

void process_read_number_of_ues_response(Ues_Ac_Control_Block& cb, udsf::Record& record, std::vector<std::map<std::string, std::string, ci_less>>& resp_headers,
                                  std::string& resp_payload)
{
    if (resp_headers.empty())
    {
        return merge_result(cb.ingress_identity, cb.acu_index, false);
    }

    auto code_iter = resp_headers[0].find(STATUS);
    if ((code_iter == resp_headers[0].end()) || (code_iter->second != "200") || resp_payload.empty())
    {
        return merge_result(cb.ingress_identity, cb.acu_index, false);
    }

    rapidjson::Document d;
    d.Parse(resp_payload.c_str());
    if (d.HasParseError())
    {
        return merge_result(cb.ingress_identity, cb.acu_index, false);
    }

    rapidjson::Pointer ptr("/count");
    auto count = ptr.Get(d);

    if (!count || !count->IsUint64() || count->GetUint64() >= MAX_UEs)
    {
        return merge_result(cb.ingress_identity, cb.acu_index, false);
    }

    auto& meta = record.meta;
    meta.tags.clear();

    meta.tags[TAG_NF_ID].emplace_back(std::move(cb.nf_id));
    meta.tags[TAG_NF_TYPE].emplace_back(std::move(cb.nf_type));
    meta.tags[TAG_ACCESS_TYPE].emplace_back(std::move(cb.access_type));
    if (cb.additional_access_type.size())
    {
        meta.tags[TAG_NF_ID].emplace_back(meta.tags[TAG_NF_ID].back());
        meta.tags[TAG_NF_TYPE].emplace_back(meta.tags[TAG_NF_TYPE].back());
        meta.tags[TAG_ACCESS_TYPE].emplace_back(std::move(cb.additional_access_type));
    }

    return add_or_update_ue_snssai_record(cb, record);
}

void read_number_of_ues(Ues_Ac_Control_Block& cb, udsf::Record& record)
{
    std::string uri;
    const std::string filter = "?count-indicator=true&filter=%7B%22op%22%3A%22GTE%22%2C%22tag%22%3A%22nfId%22%2C%22value%22%3A%22%22%7D";
    uri.append(udsf_address).append(PATH_DELIMETER).append(REALM_NAME).append(PATH_DELIMETER).append(STORAGE_PREFIX).append(cb.acu_item.snssai.sst + "-" + cb.acu_item.snssai.sd).append(PATH_DELIMETER).append(RESOUCE_RECORDS);
    uri.append(filter);
    auto process_response = [cb = std::move(cb), record = std::move(record)](std::vector<std::map<std::string, std::string, ci_less>>& resp_headers, std::string& resp_payload) mutable
    {
        process_read_number_of_ues_response(cb, record, resp_headers, resp_payload);
    };
    send_http2_request(METHOD_GET, uri, std::move(process_response));
}

void update_ues_record(udsf::Record& record, Ues_Ac_Control_Block& cb)
{
    auto target_index = find_index_of_target_value(record.meta, TAG_ACCESS_TYPE, cb.access_type);
    if (target_index < 0)
    {
        add_tag_value(record.meta, TAG_ACCESS_TYPE, std::move(cb.access_type));
        target_index = (record.meta.tags[TAG_ACCESS_TYPE].size() - 1);
    }
    update_tag_value_at_index(record.meta, TAG_NF_ID, target_index, std::move(cb.nf_id));
    update_tag_value_at_index(record.meta, TAG_NF_TYPE, target_index, std::move(cb.nf_type));
    
    if (cb.additional_access_type.size())
    {
        auto addl_target_index = find_index_of_target_value(record.meta, TAG_ACCESS_TYPE, cb.additional_access_type);
        if (addl_target_index < 0)
        {
            add_tag_value(record.meta, TAG_ACCESS_TYPE, std::move(cb.additional_access_type));
            addl_target_index = (record.meta.tags[TAG_ACCESS_TYPE].size() - 1);
        }
        update_tag_value_at_index(record.meta, TAG_NF_ID, addl_target_index,
                                  get_tag_value_at_index(record.meta, TAG_NF_ID, target_index));
        update_tag_value_at_index(record.meta, TAG_NF_TYPE, addl_target_index,
                                  get_tag_value_at_index(record.meta, TAG_NF_TYPE, target_index));
    }
}

void process_udsf_ues_read_response(Ues_Ac_Control_Block& cb,
                                  std::vector<std::map<std::string, std::string, ci_less>>& resp_headers,
                                  std::string& resp_payload)
{
    bool record_present = false;
    udsf::Record record(UE_RECORD_PREFIX + cb.supi);
    if (resp_headers.size())
    {
        auto code_iter = resp_headers[0].find(STATUS);
        if (code_iter == resp_headers[0].end())
        {
            return merge_result(cb.ingress_identity, cb.acu_index, false);
        }
        if (code_iter->second == "200")
        {
            auto iter = resp_headers[0].find(CONTENT_TYPE);
            if (iter == resp_headers[0].end())
            {
                return merge_result(cb.ingress_identity, cb.acu_index, false);
            }
            
            auto boundary = get_boundary(iter->second);
            if (boundary.empty())
            {
                return merge_result(cb.ingress_identity, cb.acu_index, false);
            }

            udsf::MultipartParser parser(boundary);
            auto parts = std::move(parser.get_parts(resp_payload));
            if (parts.empty())
            {
                return merge_result(cb.ingress_identity, cb.acu_index, false);
            }

            staticjson::ParseStatus result;
            if (!staticjson::from_json_string(parts[0].second.c_str(), &record.meta, &result))
            {
                return merge_result(cb.ingress_identity, cb.acu_index, false);
            }
            record_present = true;
        }
    }

    if (cb.acu_item.updateFlag == ACU_DECREASE)
    {
        if (!record_present)
        {
            return merge_result(cb.ingress_identity, cb.acu_index, true);
        }
        auto target_index = find_index_of_target_value(record.meta, TAG_ACCESS_TYPE, cb.access_type);
        if (target_index >= 0)
        {
            remove_tag_value_at_index(record.meta, TAG_ACCESS_TYPE, target_index);
            auto nf_id = remove_tag_value_at_index(record.meta, TAG_NF_ID, target_index);
            // TODO: check nf_id matching with cb.nf_id?
            auto nf_type = remove_tag_value_at_index(record.meta, TAG_NF_TYPE, target_index);
            // TODO: check nf_type matching with cb.nf_type?
        }

        if (record.meta.tags[TAG_ACCESS_TYPE].empty() || cb.additional_access_type.size())
        {
            return delete_ue_snssai_record(cb);
        }
        else if (target_index >= 0)
        {
            return add_or_update_ue_snssai_record(cb, record);
        }
        else
        {
            return merge_result(cb.ingress_identity, cb.acu_index, true);
        }
    }
    else if (cb.acu_item.updateFlag == ACU_INCREASE || cb.acu_item.updateFlag == ACU_UPDATE)
    {
        if (record_present)
        {
            update_ues_record(record, cb);
            return add_or_update_ue_snssai_record(cb, record);
        }
        else
        {
            return read_number_of_ues(cb, record);
        }
    }
    else
    {
        return merge_result(cb.ingress_identity, cb.acu_index, false);
    }
}

void read_ue_snssai_record(Ues_Ac_Control_Block& cb)
{
    std::string uri;
    uri.append(udsf_address).append(PATH_DELIMETER).append(REALM_NAME).append(PATH_DELIMETER).append(STORAGE_PREFIX).append(cb.acu_item.snssai.sst + "-" + cb.acu_item.snssai.sd).append(PATH_DELIMETER).append(RESOUCE_RECORDS);
    uri.append(PATH_DELIMETER).append(UE_RECORD_PREFIX).append(cb.supi);
    auto process_response = [cb = std::move(cb)](std::vector<std::map<std::string, std::string, ci_less>>& resp_headers, std::string& resp_payload) mutable
    {
        process_udsf_ues_read_response(cb, resp_headers, resp_payload);
    };
    send_http2_request(METHOD_GET, uri, std::move(process_response));
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
        std::vector<Ues_Update_Result> update_result;
        size_t result_size = 0;
        for (UeACRequestInfo& reqInfo: reqData.ueACRequestInfo)
        {
            result_size += reqInfo.acuOperationList.size();
        }
        update_result.reserve(result_size);
        size_t acu_index = 0;
        for (UeACRequestInfo& reqInfo: reqData.ueACRequestInfo)
        {
            for (AcuOperationItem& ac: reqInfo.acuOperationList)
            {
                update_result.emplace_back(reqInfo.supi, ac.snssai);
                Ues_Ac_Control_Block cb(source_ios, handler_id, stream_id, acu_index,
                                        reqInfo.supi,
                                        reqData.nfId,
                                        reqData.nfType,
                                        reqInfo.anType,
                                        reqInfo.additionalAnType,
                                        ac);
                read_ue_snssai_record(cb);
                acu_index++;
            }
        }
        UeAcuStatus[std::make_pair(handler_id, stream_id)] = std::move(update_result);
    }
    else
    {
        uint32_t status_code = 400;
        std::string resp_payload = result.description();
        res.write_head(status_code);
        res.end(std::move(resp_payload));
    }
    return true;
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


