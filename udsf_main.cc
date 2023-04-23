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

extern bool debug_mode;
bool schema_loose_check = true;

extern thread_local size_t current_thread_id;
extern size_t number_of_worker_thread;

std::atomic<size_t> thread_id_counter;

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

std::string get_boundary(const std::string& content_type)
{
    std::string boundary;
    const std::string BOUNDARY = "boundary=";
    auto boundary_start = content_type.find(BOUNDARY);
    if (boundary_start != std::string::npos)
    {
        boundary = content_type.substr(boundary_start + BOUNDARY.size(), std::string::npos);
        boundary = boundary.substr(0, boundary.find(";"));
        std::string tmp;
        tmp.reserve(TWO_LEADING_DASH.size() + boundary.size());
        tmp.append(TWO_LEADING_DASH).append(boundary);
        boundary = std::move(tmp);
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

    std::string body;
    auto queries = get_queries(req);
    bool get_previous = false;
    if (queries[GET_PREVIOUS] == TRUE)
    {
        body = storage.get_record_multipart_body(record_id);
        get_previous = true;
    }

    bool update = false;
    auto ret = storage.create_or_update_record(record_id, boundary, msg_body, update);
    switch (ret)
    {
        case 0:
        {
            if (update)
            {
                if (get_previous && body.size())
                {
                    res.write_head(200, {{CONTENT_TYPE, {MULTIPART_CONTENT_TYPE}}, {CONTENT_LENGTH, {std::to_string(body.size())}}});
                    res.end(std::move(body));
                }
                else
                {
                    res.write_head(204);
                    res.end();
                }
            }
            else
            {
                std::string location;
                location.reserve(req.uri().scheme.size() + 3 + req.uri().host.size() + req.uri().path.size());
                location.append(req.uri().scheme).append("://").append(req.uri().host).append(req.uri().path);
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
    auto body = storage.get_record_multipart_body(record_id);
    if (body.size())
    {
        res.write_head(200, {{CONTENT_TYPE, {MULTIPART_CONTENT_TYPE}}, {CONTENT_LENGTH, {std::to_string(body.size())}}});
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

void populate_block_header_and_body(const nghttp2::asio_http2::server::asio_server_request& req,
                                    nghttp2::asio_http2::server::asio_server_response& res,
                                    uint64_t handler_id, int32_t stream_id,
                                    const std::string& record_id, const std::string& block_id,
                                    std::string& previous_content,
                                    nghttp2::asio_http2::header_map& headers,
                                    udsf::Storage& storage)
{
    auto block = storage.get_block(record_id, block_id);
    if (block.content.size() && block.content_id.size())
    {
        nghttp2::asio_http2::header_value hdr_val;
        hdr_val.sensitive = false;

        hdr_val.value = block.content_id;
        headers.insert(std::make_pair(CONTENT_ID, hdr_val));

        if (block.content_type.size())
        {
            hdr_val.value = block.content_type;
            headers.insert(std::make_pair(CONTENT_TYPE, hdr_val));
        }

        for (auto& hdr : block.headers)
        {
            hdr_val.value = hdr.second;
            headers.insert(std::make_pair(hdr.first, hdr_val));
        }
        if (block.content.size() && headers.count(CONTENT_LENGTH) == 0)
        {
            hdr_val.value = std::to_string(block.content.size());
            headers.insert(std::make_pair(CONTENT_LENGTH, hdr_val));
        }
        previous_content = std::move(block.content);
    }
}

bool process_delete_record(const nghttp2::asio_http2::server::asio_server_request& req,
                           nghttp2::asio_http2::server::asio_server_response& res,
                           uint64_t handler_id, int32_t stream_id,
                           const std::string& record_id, const std::string& msg_body,
                           udsf::Storage& storage)
{
    auto queries = get_queries(req);
    bool get_previous = (queries[GET_PREVIOUS] == TRUE);
    bool record_delete_success;
    auto body = storage.delete_record(record_id, record_delete_success, get_previous);
    if (record_delete_success)
    {
        if (get_previous && body.size())
        {
            res.write_head(200, {{CONTENT_TYPE, {MULTIPART_CONTENT_TYPE}}, {CONTENT_LENGTH, {std::to_string(body.size())}}});
            res.end(std::move(body));
        }
        else
        {
            res.write_head(204);
            res.end();
        }
        return true;
    }
    const std::string msg = "record not found";
    res.write_head(404);
    res.end(msg);
    return true;
}

bool process_delete_block(const nghttp2::asio_http2::server::asio_server_request& req,
                          nghttp2::asio_http2::server::asio_server_response& res,
                          uint64_t handler_id, int32_t stream_id,
                          const std::string& record_id, const std::string& block_id, const std::string& msg_body,
                          udsf::Storage& storage)
{
    auto queries = get_queries(req);
    bool get_previous = (queries[GET_PREVIOUS] == TRUE);
    std::string previous_content;
    nghttp2::asio_http2::header_map headers;
    if (get_previous)
    {
        populate_block_header_and_body(req, res, handler_id, stream_id, record_id, block_id, previous_content, headers,
                                       storage);
    }
    auto delete_success = storage.delete_block(record_id, block_id);
    if (delete_success)
    {
        if (get_previous && previous_content.size())
        {
            res.write_head(200, std::move(headers));
            res.end(std::move(previous_content));
        }
        else
        {
            res.write_head(204);
            res.end();
        }
    }
    else
    {
        const std::string msg = "not found";
        res.write_head(404);
        res.end(msg);
    }
    return true;
}

bool process_get_block(const nghttp2::asio_http2::server::asio_server_request& req,
                       nghttp2::asio_http2::server::asio_server_response& res,
                       uint64_t handler_id, int32_t stream_id,
                       const std::string& record_id, const std::string& block_id, const std::string& msg_body,
                       udsf::Storage& storage)
{
    std::string content;
    nghttp2::asio_http2::header_map headers;
    populate_block_header_and_body(req, res, handler_id, stream_id, record_id, block_id, content, headers, storage);
    if (headers.size())
    {
        res.write_head(content.size() ? 200 : 204, std::move(headers));
        res.end(std::move(content));
    }
    else
    {
        const std::string msg = "not found";
        res.write_head(404);
        res.end(msg);
    }
    return true;
}

bool process_insert_or_update_block(const nghttp2::asio_http2::server::asio_server_request& req,
                                    nghttp2::asio_http2::server::asio_server_response& res,
                                    uint64_t handler_id, int32_t stream_id,
                                    const std::string& record_id, const std::string& block_id,
                                    const std::string& msg_body,
                                    udsf::Storage& storage)
{
    auto& req_headers =  req.header();
    std::map<std::string, std::string, ci_less> headers;
    //std::string content_id;
    std::string content_type;
    auto content_id_iter = req_headers.find(CONTENT_ID);
    auto content_type_iter = req_headers.find(CONTENT_TYPE);

    if (content_type_iter != req_headers.end())
    {
        content_type = content_type_iter->second.value;
    }
    auto iter = req_headers.begin();
    while (iter != req_headers.end())
    {
        if (iter != content_id_iter && iter != content_type_iter)
        {
            headers.insert(std::make_pair(iter->first, iter->second.value));
        }
        iter++;
    }

    udsf::Block previous_block;
    auto queries = get_queries(req);
    bool get_previous = false;
    std::string previous_content;
    nghttp2::asio_http2::header_map previous_headers;
    if (queries[GET_PREVIOUS] == TRUE)
    {
        previous_block = storage.get_block(record_id, block_id);
        get_previous = true;
        populate_block_header_and_body(req, res, handler_id, stream_id, record_id, block_id, previous_content, previous_headers,
                                       storage);
    }

    bool update = false;
    auto ret = storage.create_or_update_block(record_id, block_id, content_type, msg_body, headers, update);
    if (ret)
    {
        if (update)
        {
            res.write_head(previous_content.size() ? 200 : 204, std::move(previous_headers));
            res.end(std::move(previous_content));
        }
        else
        {
            std::string location;
            location.reserve(req.uri().scheme.size() + 3 + req.uri().host.size() + req.uri().path.size());
            location.append(req.uri().scheme).append("://").append(req.uri().host).append(req.uri().path);
            res.write_head(201, {{LOCATION, {location}}});
            res.end();
        }
    }
    else
    {
        res.write_head(404);
        const std::string msg = "not found";
        res.end(msg);
    }
    return true;

}

bool proces_get_blocks(const nghttp2::asio_http2::server::asio_server_request& req,
                       nghttp2::asio_http2::server::asio_server_response& res,
                       uint64_t handler_id, int32_t stream_id,
                       const std::string& record_id,
                       udsf::Storage& storage)
{
    auto body = storage.get_record_multipart_body(record_id, false);
    if (body.size())
    {
        res.write_head(200, {{CONTENT_TYPE, {MULTIPART_CONTENT_TYPE}}, {CONTENT_LENGTH, {std::to_string(body.size())}}});
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

bool proces_get_record_meta(const nghttp2::asio_http2::server::asio_server_request& req,
                            nghttp2::asio_http2::server::asio_server_response& res,
                            uint64_t handler_id, int32_t stream_id,
                            const std::string& record_id,
                            udsf::Storage& storage)
{
    auto body = storage.get_record_meta(record_id);
    if (body.size())
    {
        res.write_head(200, {{CONTENT_TYPE, {JSON_CONTENT}}, {CONTENT_LENGTH, {std::to_string(body.size())}}});
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

bool proces_update_record_meta(const nghttp2::asio_http2::server::asio_server_request& req,
                               nghttp2::asio_http2::server::asio_server_response& res,
                               uint64_t handler_id, int32_t stream_id,
                               const std::string& record_id,
                               const std::string& msg_body,
                               udsf::Storage& storage)
{
    bool record_found = false;
    auto ret = storage.update_record_meta(record_id, msg_body, record_found);
    if (!record_found)
    {
        const std::string record_not_found = "record not found";
        res.write_head(404);
        res.end(record_not_found);
    }
    else
    {
        if (ret)
        {
            res.write_head(204);
            res.end();
        }
        else
        {
            const std::string record_not_found = "meta patch failed";
            res.write_head(400);
            res.end(record_not_found);
        }
    }
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

    static auto send_method_not_allowed = [](nghttp2::asio_http2::server::asio_server_response & res)
    {
        const std::string response = "method not allowed";
        res.write_head(405);
        res.end(response);
        return true;
    };

    if (path_tokens.size() == BLOCK_ID_INDEX + 1)
    {
        const std::string& block_id = path_tokens[BLOCK_ID_INDEX];
        if (method == METHOD_PUT)
        {
            return process_insert_or_update_block(req, res, handler_id, stream_id, record_id, block_id, msg_body, storage);
        }
        else if (method == METHOD_GET)
        {
            return process_get_block(req, res, handler_id, stream_id, record_id, block_id, msg_body, storage);
        }
        else if (method == METHOD_DELETE)
        {
            return process_delete_block(req, res, handler_id, stream_id, record_id, block_id, msg_body, storage);
        }
        else
        {
            return send_method_not_allowed(res);
        }
    }
    else if (path_tokens.size() == BLOCKS_INDEX + 1)
    {
        auto& resource = path_tokens[path_tokens.size() - 1];
        if (resource == RESOURCE_BLOCKS)
        {
            if (method == METHOD_GET)
            {
                return proces_get_blocks(req, res, handler_id, stream_id, record_id, storage);
            }
            else
            {
                return send_method_not_allowed(res);
            }
        }
        else if (resource == RESOURCE_META)
        {
            if (method == METHOD_GET)
            {
                return proces_get_record_meta(req, res, handler_id, stream_id, record_id, storage);
            }
            else if (method == METHOD_PATCH)
            {
                return proces_update_record_meta(req, res, handler_id, stream_id, record_id, msg_body, storage);
            }
            else
            {
                return send_method_not_allowed(res);
            }
        }
    }
    else if (path_tokens.size() == RECORD_ID_INDEX + 1)
    {
        if (method == METHOD_PUT)
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
            return send_method_not_allowed(res);
        }
    }
    return false;
}

bool process_records(const nghttp2::asio_http2::server::asio_server_request& req,
                     nghttp2::asio_http2::server::asio_server_response& res,
                     uint64_t handler_id, int32_t stream_id,
                     const std::string& method,
                     const std::string& realm_id, const std::string& storage_id,
                     const std::vector<std::string>& path_tokens,
                     const std::string& msg_body)
{
    const static std::string ONLY_META = "ONLY_META";
    const static std::string META_AND_BLOCKS = "META_AND_BLOCKS";

    auto& realm = udsf::get_realm(realm_id);
    auto& storage = realm.get_storage(storage_id);
    auto queries = get_queries(req);
    auto filter = queries[FILTER];
    auto number_limit_string = queries[LIMIT_RANGE];
    auto max_payload_size_string = queries[MAX_PAYLOAD_SIZE];
    auto retrieve_records_string = queries[RETRIEVE_RECORDS];

    bool meta_only = (retrieve_records_string == ONLY_META);
    bool count_indicator = (queries[COUNT_INDICATOR] == "true");
    size_t number_limit = 0;
    if (number_limit_string.size())
    {
        number_limit = std::atoi(number_limit_string.c_str());
    }

    size_t max_payload_size = 0;
    if (max_payload_size_string.size())
    {
        max_payload_size = std::atoi(max_payload_size_string.c_str());
    }

    auto return_count = [&res](size_t count)
    {
        rapidjson::Document d;
        rapidjson::Pointer("/count").Set(d, count);
        rapidjson::StringBuffer buffer;
        rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
        d.Accept(writer);
        auto response_body = std::string(buffer.GetString());
        res.write_head(200, {{CONTENT_TYPE, {JSON_CONTENT}}, {CONTENT_LENGTH, {std::to_string(response_body.size())}}});
        res.end(std::move(response_body));
        return true;
    };

    if (count_indicator && method == METHOD_GET && filter.empty())
    {
        auto count = storage.get_all_record_count();
        return return_count(count);
    }

    rapidjson::Document search_exp;
    search_exp.Parse(filter.c_str());
    if (!search_exp.HasParseError())
    {
        if (search_exp.HasMember(OPERATION.c_str()) && count_indicator && method == METHOD_GET)
        {
            std::string op = udsf::get_string_value_from_Json_object(search_exp, OPERATION);
            std::string tag = udsf::get_string_value_from_Json_object(search_exp, "tag");
            std::string val = udsf::get_string_value_from_Json_object(search_exp, "value");
            auto schema_id = udsf::get_string_value_from_Json_object(search_exp, SCHEMA_ID);
            size_t count;
            auto ret = storage.run_search_comparison(schema_id, op, tag, val, storage.record_tags_db_main_mutex, storage.record_tags_db, count, true);
            return return_count(count);
        }

        auto records = storage.run_search_expression_non_recursive_opt(search_exp);
        if (method == METHOD_GET)
        {
            rapidjson::Document d;
            rapidjson::Pointer("/count").Set(d, records.size());
            if (!count_indicator)
            {
                auto count = 0;
                auto payload_size = 0;
                for (auto& r : records)
                {
                    std::string location;
                    location.reserve(req.uri().scheme.size() + 3 + req.uri().host.size() + req.uri().path.size() + PATH_DELIMETER.size() +
                                     r.size());
                    location.append(req.uri().scheme).append("://").append(req.uri().host).append(req.uri().path);
                    location.append(PATH_DELIMETER).append(r);
                    std::string jptr = "/references/";
                    jptr.append(std::to_string(count));
                    rapidjson::Pointer(jptr.c_str()).Set(d, location.c_str());

                    if (max_payload_size_string.empty() || payload_size < max_payload_size)
                    {
                        std::string body = storage.get_record_json_body(r, meta_only);
                        if (payload_size + body.size() < max_payload_size)
                        {
                            rapidjson::Document record_doc;
                            record_doc.Parse(body.c_str());
                            if (!record_doc.HasParseError())
                            {
                                std::string jptr = "/matchingRecords/";
                                jptr.append(r);
                                rapidjson::Value value(record_doc, d.GetAllocator());
                                rapidjson::Pointer(jptr.c_str()).Set(d, value);
                                payload_size += body.size();
                            }
                        }
                    }

                    count++;
                    if (number_limit && count > number_limit)
                    {
                        break;
                    }
                }
            }
            rapidjson::StringBuffer buffer;
            rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
            d.Accept(writer);
            auto response_body = std::string(buffer.GetString());
            res.write_head(200, {{CONTENT_TYPE, {JSON_CONTENT}}, {CONTENT_LENGTH, {std::to_string(response_body.size())}}});
            res.end(std::move(response_body));
        }
        else if (method == METHOD_DELETE)
        {
            udsf::RecordIdList rlist;
            rlist.recordIdList.reserve(records.size());
            for (auto& r : records)
            {
                rlist.recordIdList.push_back(r);
                bool record_delete_success;
                storage.delete_record(r, record_delete_success, false);
            }
            auto body = staticjson::to_json_string(rlist);
            res.write_head(200, {{CONTENT_TYPE, {JSON_CONTENT}}, {CONTENT_LENGTH, {std::to_string(body.size())}}});
            res.end(std::move(body));
        }
        else
        {
            const std::string response = "method not allowed";
            res.write_head(405);
            res.end(response);
        }
        return true;
    }
    else
    {
        res.write_head(400);
        const std::string msg = "bad request: SearchExpression filter decode failure";

        if (debug_mode)
        {
            std::cerr<<"bad search filter: "<<filter<<std::endl<<std::flush;
        }
        res.end(msg);
        return true;
    }
}

void send_error_response(nghttp2::asio_http2::server::asio_server_response& res)
{
    uint32_t status_code = 501;
    std::string resp_payload = "Operation not implemented";
    res.write_head(status_code);
    res.end(std::move(resp_payload));
}


bool process_get_subscriptions(const nghttp2::asio_http2::server::asio_server_request& req,
                               nghttp2::asio_http2::server::asio_server_response& res,
                               uint64_t handler_id, int32_t stream_id,
                               const std::string& method,
                               const std::string& realm_id, const std::string& storage_id,
                               const std::vector<std::string>& path_tokens,
                               const std::string& msg_body)
{
    if (method == METHOD_GET)
    {
        auto queries = get_queries(req);
        auto number_limit_string = queries[LIMIT_RANGE];
        size_t number_limit = 0;
        if (number_limit_string.size())
        {
            number_limit = std::atoi(number_limit_string.c_str());
        }
        auto& realm = udsf::get_realm(realm_id);
        auto& storage = realm.get_storage(storage_id);
        auto subs = storage.get_subscriptions(number_limit);
        auto body = staticjson::to_json_string(subs);
        res.write_head(200, {{CONTENT_TYPE, {JSON_CONTENT}}, {CONTENT_LENGTH, {std::to_string(body.size())}}});
        res.end(std::move(body));
        return true;
    }
    else
    {
        const std::string response = "method not allowed";
        res.write_head(405);
        res.end(response);
        return true;
    }
}


bool process_individual_subscription(const nghttp2::asio_http2::server::asio_server_request& req,
                                     nghttp2::asio_http2::server::asio_server_response& res,
                                     uint64_t handler_id, int32_t stream_id,
                                     const std::string& method,
                                     const std::string& realm_id, const std::string& storage_id,
                                     const std::string& subscription_id,
                                     const std::string& msg_body)

{
    if (subscription_id.empty())
    {
        return false;
    }
    auto& realm = udsf::get_realm(realm_id);
    auto& storage = realm.get_storage(storage_id);
    static auto send_200_or_404 = [](nghttp2::asio_http2::server::asio_server_response & res, std::string & body)
    {
        if (body.size())
        {
            res.write_head(200, {{CONTENT_TYPE, {JSON_CONTENT}}, {CONTENT_LENGTH, {std::to_string(body.size())}}});
            res.end(std::move(body));
        }
        else
        {
            const std::string response = "subscription not found";
            res.write_head(404);
            res.end(response);
        }
    };
    if (method == METHOD_PUT)
    {
        bool is_update = false;
        auto ret = storage.update_subscription(subscription_id, msg_body, is_update);
        if (ret == OPERATION_SUCCESSFUL)
        {
            if (is_update)
            {
                auto new_body = storage.get_subscription(subscription_id);
                send_200_or_404(res, new_body);
            }
            else
            {
                res.write_head(201);
                res.end();
            }
        }
        else
        {
            const std::string response = "invalid subscription";
            res.write_head(403);
            res.end(response);
        }
        return true;
    }
    else if (method == METHOD_GET)
    {
        auto body = storage.get_subscription(subscription_id);
        send_200_or_404(res, body);
        return true;
    }
    else if (method == METHOD_DELETE)
    {
        std::string previous_subscription_body;
        auto queries = get_queries(req);
        std::string& client_id = queries[CLIENT_ID];
        bool get_previous = (queries[GET_PREVIOUS] == TRUE);
        udsf::ClientId clientId;
        staticjson::ParseStatus result;
        if (client_id.empty() || !staticjson::from_json_string(client_id.c_str(), &clientId, &result))
        {
            const std::string response = "client-id not present or decode failure";
            res.write_head(403);
            res.end(response);
            return true;
        }

        bool found = false;
        bool delete_success = false;
        auto body = storage.delete_subscription(subscription_id, clientId, get_previous, found, delete_success);
        if (delete_success)
        {
            if (get_previous)
            {
                send_200_or_404(res, body);
            }
            else
            {
                res.write_head(204);
                res.end();
            }
        }
        else if (found)
        {
            res.write_head(403);
            const std::string msg = "client id does not match";
            res.end(msg);
        }
        else
        {
            const std::string response = "subscription not found";
            res.write_head(404);
            res.end(response);
        }
        return true;
    }
    else if (method == METHOD_PATCH)
    {
        auto ret = storage.patch_subscription(subscription_id, msg_body);
        if (ret == OPERATION_SUCCESSFUL)
        {
            res.write_head(204);
            res.end();
        }
        else
        {
            const std::string response = "subscription patch decode failure";
            res.write_head(403);
            res.end(response);
        }
        return true;
    }
    else
    {
        const std::string response = "method not allowed";
        res.write_head(405);
        res.end(response);
        return true;
    }
}

bool process_meta_schema(const nghttp2::asio_http2::server::asio_server_request& req,
                         nghttp2::asio_http2::server::asio_server_response& res,
                         uint64_t handler_id, int32_t stream_id,
                         const std::string& method,
                         const std::string& realm_id, const std::string& storage_id,
                         const std::string& schema_id, const std::vector<std::string>& path_tokens,
                         const std::string& msg_body)

{
    if (schema_id.empty())
    {
        return false;
    }
    auto& realm = udsf::get_realm(realm_id);
    auto& storage = realm.get_storage(storage_id);
    static auto send_200_or_204 = [](nghttp2::asio_http2::server::asio_server_response & res, std::string & body)
    {
        if (body.size())
        {
            res.write_head(200, {{CONTENT_TYPE, {JSON_CONTENT}}, {CONTENT_LENGTH, {std::to_string(body.size())}}});
            res.end(std::move(body));
        }
        else
        {
            res.write_head(204);
            res.end();
        }
    };
    if (method == METHOD_PUT)
    {
        std::string previous;
        bool found;
        auto queries = get_queries(req);
        if (queries[GET_PREVIOUS] == TRUE)
        {
            previous = storage.get_schema(schema_id, found);
        }
        bool update = false;
        auto ret = storage.create_or_update_schema(schema_id, msg_body, update);
        if (ret)
        {
            if (update)
            {
                send_200_or_204(res, previous);
            }
            else
            {
                std::string location;
                location.reserve(req.uri().scheme.size() + 3 + req.uri().host.size() + req.uri().path.size());
                location.append(req.uri().scheme).append("://").append(req.uri().host).append(req.uri().path);
                res.write_head(201, {{LOCATION, {location}}});
                res.end();
            }
        }
        else
        {
            res.write_head(400);
            const std::string msg = "bad request";
            res.end(msg);
        }
        return true;
    }
    else if (method == METHOD_GET)
    {
        bool found = false;
        auto body = storage.get_schema(schema_id, found);
        if (found)
        {
            send_200_or_204(res, body);
        }
        else
        {
            res.write_head(404);
            const std::string msg = "not found";
            res.end(msg);
        }
        return true;
    }
    else if (method == METHOD_DELETE)
    {
        std::string previous;
        bool found = true;
        auto queries = get_queries(req);
        if (queries[GET_PREVIOUS] == TRUE)
        {
            previous = storage.get_schema(schema_id, found);
        }
        auto deleted = storage.delete_schema(schema_id);
        if (deleted)
        {
            send_200_or_204(res, previous);
        }
        else
        {
            res.write_head(404);
            const std::string msg = "not found";
            res.end(msg);
        }
        return true;
    }
    else
    {
        const std::string response = "method not allowed";
        res.write_head(405);
        res.end(response);
        return true;
    }
}

bool start_tick_timer(boost::asio::deadline_timer& timer,
                      std::multimap<std::chrono::steady_clock::time_point, std::pair<uint64_t, int32_t>>& streams)
{
    timer.expires_from_now(boost::posix_time::millisec(100));

    timer.async_wait
    (
        [&timer, &streams](const boost::system::error_code & ec)
    {
        std::chrono::steady_clock::time_point curr_time_point = std::chrono::steady_clock::now();
        auto barrier = streams.upper_bound(curr_time_point);
        auto it = streams.begin();
        while (it != barrier)
        {
            auto handler_id = it->second.first;
            auto stream_id = it->second.second;
            it = streams.erase(it);
            auto handler = nghttp2::asio_http2::server::base_handler::find_handler(handler_id);
            if (!handler)
            {
                continue;
            }
            auto stream = handler->find_stream(stream_id);
            if (!stream)
            {
                continue;
            }
            auto& res = stream->response();
            static auto msg = "request timeout";
            res.write_head(500);
            res.end(msg);
        }
        start_tick_timer(timer, streams);
    });
    return true;
}

bool process_individual_timer(const nghttp2::asio_http2::server::asio_server_request& req,
                                     nghttp2::asio_http2::server::asio_server_response& res,
                                     uint64_t handler_id, int32_t stream_id,
                                     const std::string& method,
                                     const std::string& realm_id, const std::string& storage_id,
                                     const std::string& timer_id,
                                     const std::string& msg_body)

{
    if (timer_id.empty())
    {
        return false;
    }
    auto& realm = udsf::get_realm(realm_id);
    auto& storage = realm.get_storage(storage_id);
    if (method == METHOD_PUT)
    {
        bool is_update = false;
        auto ret = storage.create_or_update_timer(timer_id, msg_body, is_update);
        if (ret == OPERATION_SUCCESSFUL)
        {
            if (is_update)
            {
                res.write_head(204);
                res.end();
            }
            else
            {
                std::string location;
                location.reserve(req.uri().scheme.size() + 3 + req.uri().host.size() + req.uri().path.size());
                location.append(req.uri().scheme).append("://").append(req.uri().host).append(req.uri().path);
                res.write_head(201, {{LOCATION, {location}}});
                res.end();
            }

        }
        else
        {
            const std::string response = "invalid timer";
            res.write_head(403);
            res.end(response);
        }
        return true;
    }
    else if (method == METHOD_GET)
    {
        auto timer = storage.get_timer_object(timer_id);
        if (timer.expires.size())
        {
            auto body = staticjson::to_json_string(timer);
            res.write_head(200);
            res.end(std::move(body));
        }
        else
        {
            const std::string response = "timer not found";
            res.write_head(404);
            res.end(response);
        }
        return true;
    }
    else if (method == METHOD_DELETE)
    {
        auto timer = storage.delete_timer(timer_id);
        if (timer.expires.size())
        {
            res.write_head(204);
            res.end();
        }
        else
        {
            const std::string response = "timer not found";
            res.write_head(404);
            res.end(response);
        }
        return true;
    }
    else if (method == METHOD_PATCH)
    {
        auto ret = storage.patch_timer(timer_id, msg_body);
        if (ret == OPERATION_SUCCESSFUL)
        {
            res.write_head(204);
            res.end();
        }
        else if (ret == RESOURCE_DOES_NOT_EXIST)
        {
            const std::string response = "timer not found";
            res.write_head(404);
            res.end(response);
        }
        else
        {
            const std::string response = "bad patch request";
            res.write_head(400);
            res.end(response);
        }
        return true;
    }
    else
    {
        const std::string response = "method not allowed";
        res.write_head(405);
        res.end(response);
        return true;
    }
}

bool process_timers(const nghttp2::asio_http2::server::asio_server_request& req,
                     nghttp2::asio_http2::server::asio_server_response& res,
                     uint64_t handler_id, int32_t stream_id,
                     const std::string& method,
                     const std::string& realm_id, const std::string& storage_id,
                     const std::vector<std::string>& path_tokens,
                     const std::string& msg_body)
{
    auto& realm = udsf::get_realm(realm_id);
    auto& storage = realm.get_storage(storage_id);
    auto queries = get_queries(req);
    auto filter = queries[FILTER];
    bool expired_filter = (queries.find("expired-filter") != queries.end());
    std::set<std::string> timers;
    if (!expired_filter)
    {
        rapidjson::Document search_exp;
        search_exp.Parse(filter.c_str());
        std::string response_body;
        if (!search_exp.HasParseError())
        {
            timers = storage.run_search_expression_non_recursive_opt(search_exp, true);
        }
    }
    else
    {
        timers = storage.get_expired_timers();
    }
    if (timers.size())
    {
        if (method == METHOD_DELETE)
        {
            for (auto& t: timers)
            {
                storage.delete_timer(t);
            }
            res.write_head(204);
            res.end();
        }
        else if (method == METHOD_GET)
        {
            udsf::TimerIdList timerIds;
            timerIds.timerIds = std::move(std::vector<std::string>(timers.begin(), timers.end()));
            auto response_body = staticjson::to_json_string(timerIds);
            res.write_head(200, {{CONTENT_TYPE, {JSON_CONTENT}}, {CONTENT_LENGTH, {std::to_string(response_body.size())}}});
            res.end(std::move(response_body));
        }
        else
        {
            const std::string response = "method not allowed";
            res.write_head(405);
            res.end(response);
        }
    }
    else
    {
        res.write_head(404);
        const std::string msg = "not found";
        res.end(msg);
    }
    return true;

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
    static thread_local boost::asio::deadline_timer tick_timer(*io_service);
    static thread_local std::multimap<std::chrono::steady_clock::time_point, std::pair<uint64_t, int32_t>> active_requests;
    static thread_local auto dummy = start_tick_timer(tick_timer, active_requests);
    const std::chrono::milliseconds REQUEST_TTL(5000);
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

    if (path_tokens.size() > RESOURCE_TYPE_INDEX)
    {
        std::string& realm_id = path_tokens[REALM_ID_INDEX];
        std::string& storage_id = path_tokens[STORAGE_ID_INDEX];
        std::string& resource = path_tokens[RESOURCE_TYPE_INDEX];
        if (resource == RESOUCE_RECORDS)
        {
            if (path_tokens.size() > RECORD_ID_INDEX)
            {
                std::string& record_id = path_tokens[RECORD_ID_INDEX];
                ret = process_record(req, res, handler_id, stream_id, method, realm_id, storage_id, record_id, path_tokens, payload);
            }
            else
            {
                ret = process_records(req, res, handler_id, stream_id, method, realm_id, storage_id, path_tokens, payload);
            }
        }
        else if (resource == RESOURCE_META_SCHEMAS)
        {
            if (path_tokens.size() == RECORD_ID_INDEX + 1)
            {
                std::string& meta_schema_id = path_tokens[RECORD_ID_INDEX];
                ret = process_meta_schema(req, res, handler_id, stream_id, method, realm_id, storage_id, meta_schema_id, path_tokens,
                                          payload);
            }
        }
        else if (resource == RESOUCE_SUBS_TO_NOTIFY)
        {
            if (path_tokens.size() == RECORD_ID_INDEX + 1)
            {
                std::string& subscription_id = path_tokens[RECORD_ID_INDEX];
                ret = process_individual_subscription(req, res, handler_id, stream_id, method, realm_id, storage_id, subscription_id,
                                                      payload);
            }
            else if (path_tokens.size() == RESOURCE_TYPE_INDEX + 1)
            {
                ret = process_get_subscriptions(req, res, handler_id, stream_id, method, realm_id, storage_id, path_tokens, payload);
            }
        }
        else if (resource == RESOURCE_TIMERS)
        {
            if (path_tokens.size() == RECORD_ID_INDEX + 1)
            {
                std::string& timer_id = path_tokens[RECORD_ID_INDEX];
                ret = process_individual_timer(req, res, handler_id, stream_id, method, realm_id, storage_id, timer_id,
                                               payload);
            }
            else if (path_tokens.size() == RESOURCE_TYPE_INDEX + 1)
            {
                ret = process_timers(req, res, handler_id, stream_id, method, realm_id, storage_id, path_tokens, payload);
            }
        }
    }

    if (!ret)
    {
        send_error_response(res);
    }
}

void udsf_entry(const H2Server_Config_Schema& config_schema)
{
    auto init_thread_id = []()
    {
        current_thread_id = thread_id_counter++;
        return current_thread_id;
    };
    static thread_local auto dummy = init_thread_id();

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
    thread_id_counter = 0;
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

    number_of_worker_thread = config_schema.threads;
    udsf_entry(config_schema);

    return 0;
}

