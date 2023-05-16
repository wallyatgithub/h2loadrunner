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

const std::string REALM_NAME = "nsacf-realm";

const std::string STORAGE_NAME = "nsacf-storage";

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
const std::string TAG_SNSSAI = "snssai";


size_t MAX_UEs = 2000;
size_t min_concurrent_clients = 10;

class Ingress_Request_Identify
{
public:
    uint64_t handler_id = 0;
    int32_t stream_id = 0;
    explicit Ingress_Request_Identify(uint64_t h_id, int32_t s_id):
        handler_id(h_id), stream_id(s_id)
    {
    }
};

class Ues_Snssai_Result
{
public:
    Nssai snssai;
    bool req_done = false;
    bool req_success = false;
    explicit Ues_Snssai_Result(const Nssai& ns)
        : snssai(ns), req_done(false), req_success(false)
    {
    };
};

class Ues_Supi_Control_Block
{
public:
    Ingress_Request_Identify ingress_identity;
    size_t supi_index = 0;
    std::string supi;
    std::string nf_id;
    std::string nf_type;
    std::string access_type;
    std::string additional_access_type;
    std::vector<AcuOperationItem> acu_items;
    std::vector<Ues_Snssai_Result> ues_snssai_result;
    std::unique_ptr<udsf::Record> record;
    explicit Ues_Supi_Control_Block(uint64_t h_id, int32_t s_id,
                                    size_t index,
                                    std::string& uid,
                                    std::string& nfId,
                                    std::string& nfType,
                                    std::string& accessType,
                                    std::string& additionalAnType,
                                    std::vector<AcuOperationItem>& item):
        ingress_identity(h_id, s_id),
        supi_index(index),
        supi(std::move(uid)),
        nf_id(std::move(nfId)),
        nf_type(std::move(nfType)),
        access_type(std::move(accessType)),
        additional_access_type(std::move(additionalAnType)),
        acu_items(std::move(item))
    {
        for (auto& ac : acu_items)
        {
            ues_snssai_result.emplace_back(ac.snssai);
        }
    }
};

class Ues_Supi_Update_Result
{
public:
    bool update_done = false;
    bool update_success = false;
    std::unique_ptr<Ues_Supi_Control_Block> control_block;
    explicit Ues_Supi_Update_Result(std::unique_ptr<Ues_Supi_Control_Block>& cb)
        : control_block(std::move(cb))
    {
    }
};

thread_local std::map<std::pair<uint64_t, int32_t>, std::vector<Ues_Supi_Update_Result>> UesAcStatus;

Ues_Supi_Control_Block* get_per_supi_control_block(Ingress_Request_Identify ingress_id, size_t supi_index)
{
    auto iter = UesAcStatus.find(std::make_pair(ingress_id.handler_id, ingress_id.stream_id));
    if (iter == UesAcStatus.end())
    {
        return nullptr;
    }
    if (iter->second.size() < supi_index + 1)
    {
        return nullptr;
    }
    return iter->second[supi_index].control_block.get();
}



int64_t find_index_of_target_value(const udsf::RecordMeta& meta, const std::string& tag_name,
                                   const std::string& tag_value)
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

void merge_ues_record_update_result(Ues_Supi_Control_Block& cb, bool success_or_failure)
{
    uint64_t handler_id = cb.ingress_identity.handler_id;
    int32_t stream_id = cb.ingress_identity.stream_id;
    auto iter = UesAcStatus.find(std::pair<uint64_t, int32_t>(handler_id, stream_id));
    if (iter == UesAcStatus.end())
    {
        std::cerr << "handler_id: " << handler_id << ", stream_id: " << stream_id << " does not have an entry" << std::endl <<
                  std::flush;
        return;
    }
    if (iter->second.size() < (cb.supi_index + 1))
    {
        std::cerr << "handler_id: " << handler_id << ", stream_id: " << stream_id << std::endl << std::flush;
        std::cerr << "supi_index: " << cb.supi_index << " out of range" << std::endl << std::flush;
        return;
    }

    iter->second[cb.supi_index].update_done = true;
    iter->second[cb.supi_index].update_success = success_or_failure;
    if (!iter->second[cb.supi_index].update_success)
    {
        for (auto& acu_result : cb.ues_snssai_result)
        {
            acu_result.req_success = false;
        }
    }

    bool all_done = true;
    bool all_success = true;
    for (auto& supi_update_result : iter->second)
    {
        if (!supi_update_result.update_done)
        {
            all_done = false;
            break;
        }
        if (!supi_update_result.update_success)
        {
            all_success = false;
        }
    }

    if (!all_done)
    {
        return;
    }


    auto all_result = std::move(iter->second);
    UesAcStatus.erase(iter);

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
            for (auto& acu : result.control_block->ues_snssai_result)
                for (size_t i = 0; i < result.control_block->ues_snssai_result.size(); i++)
                {
                    if (result.control_block->ues_snssai_result[i].req_success)
                    {
                        continue;
                    }
                    AcuFailureItem item;
                    item.snssai = result.control_block->acu_items[i].snssai;
                    response.acuFailureList[result.control_block->supi].push_back(std::move(item));
                }
        }
        auto body = staticjson::to_json_string(response);
        res.write_head(200, {{CONTENT_TYPE, {JSON_CONTENT}}, {CONTENT_LENGTH, {std::to_string(body.size())}}});
        res.end(std::move(body));
    }
}



void process_udsf_ues_update_response(Ues_Supi_Control_Block& cb,
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
    merge_ues_record_update_result(cb, update_success);
}


void add_or_update_ue_snssai_record(Ues_Supi_Control_Block& cb, udsf::Record& record)
{
    std::string uri;
    uri.append(udsf_address).append(PATH_DELIMETER).append(REALM_NAME).append(PATH_DELIMETER).append(STORAGE_NAME).append(
        PATH_DELIMETER).append(RESOUCE_RECORDS);
    uri.append(PATH_DELIMETER).append(UE_RECORD_PREFIX).append(cb.supi);
    std::map<std::string, std::string, ci_less> additionalHeaders;
    auto body = record.produce_multipart_body(true, "");
    additionalHeaders.insert(std::make_pair(CONTENT_TYPE, MULTIPART_CONTENT_TYPE));
    additionalHeaders.insert(std::make_pair(CONTENT_LENGTH, std::to_string(body.size())));
    auto ingress_id = cb.ingress_identity;
    auto supi_index = cb.supi_index;
    auto process_response = [ingress_id = std::move(ingress_id),
                                        supi_index](std::vector<std::map<std::string, std::string, ci_less>>& resp_headers, std::string & resp_payload) mutable
    {
        auto cb = get_per_supi_control_block(ingress_id, supi_index);
        if (!cb)
        {
            return;
        }
        process_udsf_ues_update_response(*cb, resp_headers, resp_payload);
    };
    send_http2_request(METHOD_PUT, uri, std::move(process_response), additionalHeaders, body);
}

void delete_ues_supi_record(Ues_Supi_Control_Block& cb)
{
    std::string uri;
    uri.append(udsf_address).append(PATH_DELIMETER).append(REALM_NAME).append(PATH_DELIMETER).append(STORAGE_NAME).append(
        PATH_DELIMETER).append(RESOUCE_RECORDS);
    uri.append(PATH_DELIMETER).append(UE_RECORD_PREFIX).append(cb.supi);
    auto ingress_id = cb.ingress_identity;
    auto supi_index = cb.supi_index;
    auto process_response = [ingress_id = std::move(ingress_id),
                                        supi_index](std::vector<std::map<std::string, std::string, ci_less>>& resp_headers, std::string & resp_payload) mutable
    {
        auto cb = get_per_supi_control_block(ingress_id, supi_index);
        if (!cb)
        {
            return;
        }
        process_udsf_ues_update_response(*cb, resp_headers, resp_payload);
    };
    send_http2_request(METHOD_DELETE, uri, std::move(process_response));
}

void update_ues_record_block(udsf::Record& record, Ues_Supi_Control_Block& cb, size_t acu_index)
{
    AcuOperationItem& ac_item = cb.acu_items[acu_index];
    auto snssai = staticjson::to_json_string(ac_item.snssai);
    auto& update_flag = ac_item.updateFlag;

    size_t target_block_index = 0;
    for (size_t i = 0; i < record.blocks.size(); i++)
    {
        if (record.blocks[i].content_id == snssai)
        {
            target_block_index = i;
            break;
        }
    }
    udsf::Block& target_block = record.blocks[target_block_index];

    udsf::RecordMeta block_content;

    if (update_flag != ACU_UPDATE && target_block.content.size())
    {
        staticjson::ParseStatus result;
        if (!staticjson::from_json_string(target_block.content.c_str(), &block_content, &result))
        {
            block_content.tags.clear();
        }
    }

    if (update_flag == ACU_DECREASE)
    {
        auto target_index = find_index_of_target_value(block_content, TAG_ACCESS_TYPE, cb.access_type);
        if (target_index >= 0)
        {
            remove_tag_value_at_index(record.meta, TAG_ACCESS_TYPE, target_index);
            auto nf_id = remove_tag_value_at_index(record.meta, TAG_NF_ID, target_index);
            auto nf_type = remove_tag_value_at_index(record.meta, TAG_NF_TYPE, target_index);
        }

        if (record.meta.tags[TAG_ACCESS_TYPE].empty() || cb.additional_access_type.size())
        {
            auto iter = record.blocks.begin();
            std::advance(iter, target_block_index);
            record.blocks.erase(iter);
            return;
        }
    }
    else
    {
        auto target_index = find_index_of_target_value(block_content, TAG_ACCESS_TYPE, cb.access_type);
        if (target_index < 0)
        {
            add_tag_value(block_content, TAG_ACCESS_TYPE, std::move(cb.access_type));
            target_index = (block_content.tags[TAG_ACCESS_TYPE].size() - 1);
        }
        update_tag_value_at_index(block_content, TAG_NF_ID, target_index, std::move(cb.nf_id));
        update_tag_value_at_index(block_content, TAG_NF_TYPE, target_index, std::move(cb.nf_type));
        if (cb.additional_access_type.size())
        {
            auto addl_target_index = find_index_of_target_value(block_content, TAG_ACCESS_TYPE, cb.additional_access_type);
            if (addl_target_index < 0)
            {
                add_tag_value(block_content, TAG_ACCESS_TYPE, std::move(cb.additional_access_type));
                addl_target_index = (block_content.tags[TAG_ACCESS_TYPE].size() - 1);
            }
            update_tag_value_at_index(block_content, TAG_NF_ID, addl_target_index,
                                      get_tag_value_at_index(block_content, TAG_NF_ID, target_index));
            update_tag_value_at_index(block_content, TAG_NF_TYPE, addl_target_index,
                                      get_tag_value_at_index(block_content, TAG_NF_TYPE, target_index));
        }
    }

    target_block.content = staticjson::to_json_string(block_content);
    target_block.content_type = JSON_CONTENT;
    target_block.content_id = snssai;

    auto& meta_nf_id = record.meta.tags[TAG_NF_ID];
    for (auto& s : block_content.tags[TAG_NF_ID])
    {
        if (std::find(meta_nf_id.begin(), meta_nf_id.end(), s) == meta_nf_id.end())
        {
            meta_nf_id.push_back(s);
        }
    }

    auto& meta_nf_type = record.meta.tags[TAG_NF_TYPE];
    for (auto& s : block_content.tags[TAG_NF_TYPE])
    {
        if (std::find(meta_nf_type.begin(), meta_nf_type.end(), s) == meta_nf_type.end())
        {
            meta_nf_type.push_back(s);
        }
    }

    auto& meta_an_type = record.meta.tags[TAG_ACCESS_TYPE];
    for (auto s : block_content.tags[TAG_ACCESS_TYPE])
    {
        if (std::find(meta_an_type.begin(), meta_an_type.end(), s) == meta_an_type.end())
        {
            meta_an_type.push_back(s);
        }
    }

    auto& meta_snssai = record.meta.tags[TAG_SNSSAI];
    if (std::find(meta_snssai.begin(), meta_snssai.end(), snssai) == meta_snssai.end())
    {
        meta_snssai.push_back(snssai);
    }

}

void update_ues_record(Ues_Supi_Control_Block& cb, size_t supi_index)
{

    auto& acu_items = cb.acu_items;
    auto& record = cb.record;
    auto& snssai_count_read_results = cb.ues_snssai_result;

    record->meta.tags.clear();

    for (size_t acu_index = 0; acu_index < acu_items.size(); acu_index++)
    {
        if (acu_items[acu_index].updateFlag == ACU_INCREASE && !snssai_count_read_results[acu_index].req_success)
        {
            continue;
        }
        update_ues_record_block(*record, cb, acu_index);
    }
    if (record->blocks.size())
    {
        add_or_update_ue_snssai_record(cb, *record);
    }
    else
    {
        delete_ues_supi_record(cb);
    }
}


void merge_snssai_count_read_result(Ues_Supi_Control_Block& cb, size_t acu_index, bool success_or_failure)
{
    cb.ues_snssai_result[acu_index].req_done = true;
    cb.ues_snssai_result[acu_index].req_success = success_or_failure;

    bool all_done = true;
    bool all_success = true;
    for (auto& result : cb.ues_snssai_result)
    {
        if (!result.req_done)
        {
            all_done = false;
            break;
        }
        if (!result.req_success)
        {
            all_success = false;
        }
    }
    if (!all_done)
    {
        return;
    }

    update_ues_record(cb, cb.supi_index);
}

void process_read_number_of_ues_response(Ues_Supi_Control_Block& cb, size_t acu_index,
                                         std::vector<std::map<std::string, std::string, ci_less>>& resp_headers,
                                         std::string& resp_payload)
{
    if (resp_headers.empty())
    {
        return merge_snssai_count_read_result(cb, acu_index, false);
    }

    auto code_iter = resp_headers[0].find(STATUS);
    if ((code_iter == resp_headers[0].end()) || (code_iter->second != "200") || resp_payload.empty())
    {
        merge_snssai_count_read_result(cb, acu_index, false);
        return;
    }

    rapidjson::Document d;
    d.Parse(resp_payload.c_str());
    if (d.HasParseError())
    {
        merge_snssai_count_read_result(cb, acu_index, false);
        return;
    }

    rapidjson::Pointer ptr("/count");
    auto count = ptr.Get(d);

    if (!count || !count->IsUint64() || count->GetUint64() >= MAX_UEs)
    {
        merge_snssai_count_read_result(cb, acu_index, false);
        return;
    }

    merge_snssai_count_read_result(cb, acu_index, true);
    return;
}




void read_number_of_ues(Ues_Supi_Control_Block& cb, size_t acu_index)
{
    std::string uri;
    const std::string filter =
        "?count-indicator=true&filter=%7B%22op%22%3A%22GTE%22%2C%22tag%22%3A%22nssai%22%2C%22value%22%3A%22";
    uri.append(udsf_address).append(PATH_DELIMETER).append(REALM_NAME).append(PATH_DELIMETER).append(STORAGE_NAME).append(
        PATH_DELIMETER).append(RESOUCE_RECORDS);
    uri.append(filter);
    uri.append(staticjson::to_json_string(cb.acu_items[acu_index].snssai)).append("%22%7D");
    auto ingress_id = cb.ingress_identity;
    auto supi_index = cb.supi_index;
    auto process_response = [ingress_id = std::move(ingress_id), supi_index,
                                        acu_index](std::vector<std::map<std::string, std::string, ci_less>>& resp_headers, std::string & resp_payload) mutable
    {
        auto cb = get_per_supi_control_block(ingress_id, supi_index);
        if (!cb)
        {
            return;
        }
        process_read_number_of_ues_response(*cb, acu_index, resp_headers, resp_payload);
    };
    send_http2_request(METHOD_GET, uri, std::move(process_response));
}

void process_ues_read_response(Ues_Supi_Control_Block& cb,
                               std::vector<std::map<std::string, std::string, ci_less>>& resp_headers,
                               std::string& resp_payload)
{
    bool record_present = false;
    if (resp_headers.empty())
    {
        return merge_ues_record_update_result(cb, false);
    }

    cb.record = std::make_unique<udsf::Record>(UE_RECORD_PREFIX + cb.supi);
    auto& record = cb.record;

    auto code_iter = resp_headers[0].find(STATUS);
    if (code_iter == resp_headers[0].end())
    {
        return merge_ues_record_update_result(cb, false);
    }

    if (code_iter->second == "200")
    {
        auto iter = resp_headers[0].find(CONTENT_TYPE);
        if (iter == resp_headers[0].end())
        {
            return merge_ues_record_update_result(cb, false);
        }

        auto boundary = get_boundary(iter->second);
        if (boundary.empty())
        {
            return merge_ues_record_update_result(cb, false);
        }

        udsf::MultipartParser parser(boundary);
        auto parts = std::move(parser.get_parts(resp_payload));
        if (parts.empty())
        {
            return merge_ues_record_update_result(cb, false);
        }

        staticjson::ParseStatus result;
        if (!staticjson::from_json_string(parts[0].second.c_str(), &record->meta, &result))
        {
            return merge_ues_record_update_result(cb, false);
        }
        record_present = true;

        for (size_t i = 1; i < parts.size(); i++)
        {
            std::string content_id;
            std::string content_type;
            auto iter = parts[i].first.find(CONTENT_ID);
            if (iter == parts[i].first.end())
            {
                continue;
            }
            content_id = std::move(iter->second);
            parts[i].first.erase(iter);

            iter = parts[i].first.find(CONTENT_TYPE);
            if (iter != parts[i].first.end())
            {
                content_type = std::move(iter->second);
                parts[i].first.erase(iter);
            }
            bool dummy;
            record->create_or_update_block(content_id, content_type, parts[i].second, parts[i].first, dummy);
        }

    }

    std::set<std::string> snssais_inc_in_req;
    for (auto& acu : cb.acu_items)
    {
        auto& snssai = acu.snssai;
        if (acu.updateFlag != ACU_INCREASE)
        {
            continue;
        }
        snssais_inc_in_req.insert(staticjson::to_json_string(snssai));
    }

    std::set<std::string> snssais_need_count;
    if (record_present)
    {
        auto& tag_snssai_values = record->meta.tags[TAG_SNSSAI];
        for (auto& s : snssais_inc_in_req)
        {
            if (std::find(tag_snssai_values.begin(), tag_snssai_values.end(), s) == tag_snssai_values.end())
            {
                snssais_need_count.insert(s);
            }
        }
    }

    bool read_count_sent = false;
    for (size_t acu_index = 0; acu_index < cb.acu_items.size(); acu_index++)
    {
        if (snssais_need_count.count(staticjson::to_json_string(cb.acu_items[acu_index].snssai)) == 0)
        {
            cb.ues_snssai_result[acu_index].req_done = true;
            cb.ues_snssai_result[acu_index].req_success = true;
        }
        else
        {
            read_number_of_ues(cb, acu_index);
            read_count_sent = true;
        }
    }

    if (read_count_sent)
    {
        return;
    }

    update_ues_record(cb, cb.supi_index);

}

void read_ues_record(Ues_Supi_Control_Block& cb)
{
    std::string uri;
    uri.append(udsf_address).append(PATH_DELIMETER).append(REALM_NAME).append(PATH_DELIMETER).append(STORAGE_NAME).append(
        PATH_DELIMETER).append(RESOUCE_RECORDS);
    uri.append(PATH_DELIMETER).append(UE_RECORD_PREFIX).append(cb.supi);

    auto ingress_id = cb.ingress_identity;
    auto supi_id = cb.supi_index;
    auto process_response = [ingress_id = std::move(ingress_id),
                                        supi_id](std::vector<std::map<std::string, std::string, ci_less>>& resp_headers, std::string & resp_payload) mutable
    {
        auto cb = get_per_supi_control_block(ingress_id, supi_id);
        if (!cb)
        {
            return;
        }
        process_ues_read_response(*cb, resp_headers, resp_payload);
    };
    send_http2_request(METHOD_GET, uri, std::move(process_response));
}

bool process_ues(const nghttp2::asio_http2::server::asio_server_request& req,
                 nghttp2::asio_http2::server::asio_server_response& res,
                 uint64_t handler_id, int32_t stream_id,
                 const std::string& msg_body)
{
    UeACRequestData reqData;
    staticjson::ParseStatus result;
    if (!staticjson::from_json_string(msg_body.c_str(), &reqData, &result))
    {
        uint32_t status_code = 400;
        std::string resp_payload = result.description();
        res.write_head(status_code);
        res.end(std::move(resp_payload));
        return true;
    }

    size_t supi_index = 0;
    for (UeACRequestInfo& reqInfo : reqData.ueACRequestInfo)
    {
        auto cb = std::make_unique<Ues_Supi_Control_Block>(handler_id, stream_id, supi_index,
                                                           reqInfo.supi,
                                                           reqData.nfId,
                                                           reqData.nfType,
                                                           reqInfo.anType,
                                                           reqInfo.additionalAnType,
                                                           reqInfo.acuOperationList);
        auto cb_ptr = cb.get();
        UesAcStatus[std::make_pair(handler_id, stream_id)].emplace_back(cb);
        supi_index++;
        read_ues_record(*cb_ptr);
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
            ret = process_ues(req, res, handler_id, stream_id, payload);
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
            std::cout << "error reading config file " << std::endl;
            exit(1);
        }

        udsf_address = get_string_from_json_ptr(nsacf_config, "/udsf-url");
        MAX_UEs = get_uint64_from_json_ptr(nsacf_config, "/max-number-of-ues");
        min_concurrent_clients = get_uint64_from_json_ptr(nsacf_config, "/minimum-egress-concurrent-connections");
        g_egress_config.json_config_schema.cert_verification_mode = get_uint64_from_json_ptr(nsacf_config,
                                                                                             "/certVerificationMode");
        g_egress_config.json_config_schema.interval_to_send_ping = get_uint64_from_json_ptr(nsacf_config,
                                                                                            "/interval-between-ping-frames");
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


