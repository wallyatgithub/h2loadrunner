#ifndef UDSF_STORAGE_H
#define UDSF_STORAGE_H
#include <set>
#include <map>
#include <vector>
#include <iostream>
#include <fstream>
#include <cstring>
#include <numeric>
#include <shared_mutex>
#include "staticjson/document.hpp"
#include "staticjson/staticjson.hpp"
#include "rapidjson/schema.h"
#include "rapidjson/prettywriter.h"
#include "multipart_parser.h"
#include "nlohmann/json.hpp"
#include "rapidjson/writer.h"
#include "rapidjson/stringbuffer.h"
#include "common_types.h"
#include "h2load_utils.h"
#include "udsf_util.h"


const std::string CONTENT_ID = "Content-Id";
const std::string CONTENT_TYPE = "Content-Type";
const std::string CONTENT_LENGTH = "content-length";
const std::string CONTENT_TRANSFER_ENCODDING = "Content-Transfer-Encoding";
const std::string COLON  = ":";
const std::string CRLF = "\r\n";
const std::string VERY_SPECIAL_BOUNARY_WITH_LEADING_TWO_DASHES = "-----wallyweiwallzzllawiewyllaw---";
const std::string TWO_LEADING_DASH = "--";
const std::string ENDING_TWO_DASH = "--";
const std::string JSON_CONTENT = "application/json";
const std::string META_CONTENT_ID = "meta";
const std::string NOTIFICATION_DESCRIPTION_ID = "notification-description";
const std::string MULTIPART_CONTENT_TYPE = "multipart/mixed; boundary=---wallyweiwallzzllawiewyllaw---";
const std::string RECORD_ID_LIST = "recordIdList";
const std::string CONDITION = "cond";
const std::string OPERATION = "op";
const std::string UNITS = "units";
const std::string CONDITION_OP_AND = "AND";
const std::string CONDITION_OP_OR = "OR";
const std::string CONDITION_OP_NOT = "NOT";

const std::string RECORDS = "records";

const std::string RECORD_OPERATION_CREATED = "CREATED";
const std::string RECORD_OPERATION_UPDATED = "UPDATED";
const std::string RECORD_OPERATION_DELETED = "DELETED";

const int OPERATION_SUCCESSFUL = 0;
const int DECODE_MULTIPART_FAILURE = -1;
const int CONTENT_ID_NOT_PRESDENT = -2;
const int RECORD_META_DECODE_FAILURE = -3;
const int RECORD_DOES_NOT_EXIST = -4;
const int TTL_EXPIRED = -5;
const int DECODE_SUBCRIPTION_FAILURE = -6;
const int SUBSCRIPTION_EXPIRY_INVALID = -7;

const std::string PATH_DELIMETER = "/";
const std::string QUERY_DELIMETER = "&";

const std::string udsf_base_uri = "/nudsf-dr/v1";
const size_t number_of_tokens_in_api_prefix = 0;
const std::string api_prefix = "";

const size_t REALM_ID_INDEX = number_of_tokens_in_api_prefix + 2;
const size_t STORAGE_ID_INDEX = number_of_tokens_in_api_prefix + 3;
const size_t RESOURCE_TYPE_INDEX = number_of_tokens_in_api_prefix + 4;
const size_t RECORD_ID_INDEX = number_of_tokens_in_api_prefix + 5;
const size_t BLOCKS_INDEX = number_of_tokens_in_api_prefix + 6;
const size_t BLOCK_ID_INDEX = number_of_tokens_in_api_prefix + 7;

const std::string RESOUCE_RECORDS = "records";

const std::string RESOUCE_SUBS_TO_NOTIFY = "subs-to-notify";

const std::string RESOURCE_META_SCHEMAS = "meta-schemas";

const std::string RESOUCE_BLOCKS = "records";

const std::string& METHOD_PUT = "put";
const std::string& METHOD_POST = "post";
const std::string& METHOD_PATCH = "patch";
const std::string& METHOD_DELETE = "delete";
const std::string& METHOD_GET = "get";

const std::string& LOCATION = "location";

const std::string EQUAL = "=";
const std::string GET_PREVIOUS = "get-previous";
const std::string TRUE = "true";
const std::string FALSE = "FALSE";

constexpr uint16_t ONE_DIMENSION_SIZE = 0x100;

namespace udsf
{

std::string get_path(const std::string& uri)
{
    http_parser_url u {};
    if (http_parser_parse_url(uri.c_str(), uri.size(), 0, &u) != 0)
    {
        std::cerr << "invalid uri:" << uri << std::endl;
        return "";
    }
    return get_reqline(uri.c_str(), u);
}

std::string get_relative_path_starting_from_records(const std::string& path)
{
    std::string s = "";

    auto tokens = tokenize_string(path, PATH_DELIMETER);
    size_t size = 0;
    for (size_t i = RESOURCE_TYPE_INDEX; i < tokens.size(); i++)
    {
        if (i != RESOURCE_TYPE_INDEX)
        {
            size += PATH_DELIMETER.size();
        }
        size += tokens[i].size();
    }
    for (size_t i = RESOURCE_TYPE_INDEX; i < tokens.size(); i++)
    {
        if (i != RESOURCE_TYPE_INDEX)
        {
            s += PATH_DELIMETER;
        }
        s += tokens[i];
    }
    return s;
}

inline uint16_t get_u16_sum(const std::string& key)
{
    return (std::accumulate(key.begin(),
                            key.end(),
                            0,
                            [](uint16_t sum, const char& c)
    {
        return sum + uint8_t(c);
    }));
}

inline uint8_t get_u8_sum(const std::string& key)
{
    return (std::accumulate(key.begin(),
                            key.end(),
                            0,
                            [](uint8_t sum, const char& c)
    {
        return sum + uint8_t(c);
    }));
}

inline std::string get_string_value_from_Json_object(rapidjson::Value& object, const std::string& name)
{
    if (object.HasMember(name.c_str()))
    {
        auto& member_value = object[name.c_str()];
        if (member_value.IsString())
        {
            return std::string(member_value.GetString(), member_value.GetStringLength());
        }
    }
    return "";
}

inline std::set<std::string> run_and_operator(const std::vector<std::set<std::string>>& operands)
{
    std::set<std::string> ret;
    size_t shortest_vector_index = 0;
    for (size_t i = 0; i < operands.size(); i ++)
    {
        if (operands[i].size() < operands[shortest_vector_index].size())
        {
            shortest_vector_index = i;
        }
    }
    auto& smallest_vector = operands[shortest_vector_index];
    for (auto& s : smallest_vector)
    {
        size_t i = 0;
        for (i = 0; i < operands.size(); i++)
        {
            if (i == shortest_vector_index)
            {
                continue;
            }
            if (operands[i].count(s) == 0)
            {
                break;
            }
        }
        if (i == operands.size())
        {
            ret.insert(s);
        }
    }
    return ret;
}

inline std::set<std::string> run_or_operator(const std::vector<std::set<std::string>>& operands)
{
    std::set<std::string> ret;
    for (auto& v : operands)
    {
        for (auto& s : v)
        {
            ret.insert(s);
        }
    }
    return ret;
}

inline std::set<std::string> run_not_operator(const std::set<std::string>& source,
                                              const std::set<std::string>& operands)
{
    std::set<std::string> ret;
    for (auto& s : source)
    {
        if (operands.count(s) == 0)
        {
            ret.insert(s);
        }
    }
    return ret;
}

inline uint64_t iso8601_timestamp_to_seconds_since_epoch(const std::string& iso8601_timestamp)
{
    const std::string sample_tz_offset = "+08:00";

    if (iso8601_timestamp.empty())
    {
        return 0;
    }
    std::time_t local_t = std::time(nullptr);
    std::tm tm = {};
#ifdef _WINDOWS
    tm = *localtime(&local_t);
#else
    localtime_r(&local_t, &tm);
#endif
    std::stringstream buffer;
    buffer << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S%z");
    auto local_time_stamp = buffer.str();
    std::string local_tz = local_time_stamp.substr(local_time_stamp.size() - 5);
    int local_tz_hours, local_tz_minutes;
    local_tz_hours = std::atoi(local_tz.substr(1, 2).c_str());
    local_tz_minutes = std::atoi(local_tz.substr(3, 2).c_str());
    auto local_tz_offset = (std::chrono::hours(local_tz_hours) + std::chrono::minutes(local_tz_minutes));
    if (local_tz[0] == '-')
    {
        local_tz_offset = -local_tz_offset;
    }

    std::tm timeinfo = {};
    timeinfo.tm_isdst = tm.tm_isdst;
    std::istringstream ss(iso8601_timestamp);
    ss >> std::get_time(&timeinfo, "%Y-%m-%dT%H:%M:%S");

    std::time_t t = std::mktime(&timeinfo);
    if (t == -1)
    {
        std::cerr << "Error: mktime() failed" << std::endl;
        return 0;
    }

    std::chrono::system_clock::time_point tp =
        std::chrono::system_clock::from_time_t(t);
    auto duration = tp.time_since_epoch();

    std::string incoming_tz;
    if (iso8601_timestamp[iso8601_timestamp.size() - 1] == 'z' || iso8601_timestamp[iso8601_timestamp.size() - 1] == 'Z')
    {
        incoming_tz = "+00:00";
    }
    else
    {
        incoming_tz = iso8601_timestamp.substr(iso8601_timestamp.size() - sample_tz_offset.size());
    }
    int incoming_tz_hours, incoming_tz_minutes;
    std::sscanf(incoming_tz.substr(1).c_str(), "%d:%d", &incoming_tz_hours, &incoming_tz_minutes);
    auto incoming_tz_offset = (std::chrono::hours(incoming_tz_hours) + std::chrono::minutes(incoming_tz_minutes));
    if (incoming_tz[0] == '-')
    {
        incoming_tz_offset = -incoming_tz_offset;
    }

    auto tz_difference = local_tz_offset - incoming_tz_offset;
    duration = duration + tz_difference;
    return std::chrono::duration_cast<std::chrono::seconds>(duration).count();
}

class MultipartParser
{
public:
    using MutiParts = std::vector<std::pair<std::map<std::string, std::string, ci_less>, std::string>>;
    MultipartParser(const std::string& boundary)
    {
        memset(&m_callbacks, 0, sizeof(multipart_parser_settings));
        m_callbacks.on_part_data_begin = onPartStart;
        m_callbacks.on_header_field = onHeaderName;
        m_callbacks.on_header_value = onHeaderValue;
        m_callbacks.on_part_data = onPartData;
        m_parser = multipart_parser_init(boundary.c_str(), &m_callbacks);
        multipart_parser_set_data(m_parser, this);
    }

    ~MultipartParser()
    {
        multipart_parser_free(m_parser);
    }

    MutiParts& get_parts(const std::string& content)
    {
        auto size = multipart_parser_execute(m_parser, content.c_str(), content.size());
        return parts;
    }


private:
    static int onHeaderName(multipart_parser* p, const char* at, size_t length)
    {
        MultipartParser* me = (MultipartParser*)multipart_parser_get_data(p);
        me->currHeaderName = std::string(at, length);
        return 0;
    }
    static int onHeaderValue(multipart_parser* p, const char* at, size_t length)
    {
        MultipartParser* me = (MultipartParser*)multipart_parser_get_data(p);
        auto& part = me->parts.back();
        part.first[me->currHeaderName] = std::string(at, length);
        return 0;
    }
    static int onPartData(multipart_parser* p, const char* at, size_t length)
    {
        MultipartParser* me = (MultipartParser*)multipart_parser_get_data(p);
        auto& part = me->parts.back();
        part.second = std::string(at, length);
        return 0;
    }
    static int onPartStart(multipart_parser* p)
    {
        MultipartParser* me = (MultipartParser*)multipart_parser_get_data(p);
        me->parts.emplace_back();
        return 0;
    }

    multipart_parser* m_parser;
    multipart_parser_settings m_callbacks;
    MutiParts parts;
    std::string currHeaderName;
};


class TagType
{
public:
    std::string tagName;
    std::string keyType;
    bool sort;
    bool presence;
    void staticjson_init(staticjson::ObjectHandler* h)
    {
        h->add_property("tagName", &this->tagName);
        h->add_property("keyType", &this->keyType);
        h->add_property("sort", &this->sort, staticjson::Flags::Optional);
        h->add_property("presence", &this->presence, staticjson::Flags::Optional);
    }
};

class MetaSchema
{
public:
    std::string schemaId;
    std::vector<TagType> metaTags;
    std::map<std::string, size_t> tagNameIndex;
    void staticjson_init(staticjson::ObjectHandler* h)
    {
        h->add_property("schemaId", &this->schemaId);
        h->add_property("metaTags", &this->metaTags);
    }
    void build_index()
    {
        for (size_t i = 0; i < metaTags.size(); i++)
        {
            tagNameIndex[metaTags[i].tagName] = i;
        }
    }
};

class RecordMeta
{
public:
    std::string ttl;
    std::string callbackReference;
    std::map<std::string, std::vector<std::string>> tags;
    std::string schemaId;
    void staticjson_init(staticjson::ObjectHandler* h)
    {
        h->add_property("ttl", &this->ttl, staticjson::Flags::Optional);
        h->add_property("callbackReference", &this->callbackReference, staticjson::Flags::Optional);
        h->add_property("tags", &this->tags, staticjson::Flags::Optional);
        h->add_property("schemaId", &this->schemaId, staticjson::Flags::Optional);
    }
};

class Block
{
public:
    std::string content_id;
    std::string content_type;
    std::string content;
    std::map<std::string, std::string, ci_less> headers;
    void staticjson_init(staticjson::ObjectHandler* h)
    {
        h->add_property("Content-Id", &this->content_id);
        h->add_property("Content-Type", &this->content_type);
        h->add_property("content", &this->content);
        //h->add_property("additional-headers", &this->headers);
    }

    std::string produce_body_part()
    {
        std::string ret;
        auto final_size = 0;
        bool insert_content_transfer_encoding = (!headers.count(CONTENT_TRANSFER_ENCODDING));
        const static std::string binary = "binary";
        //final_size += CRLF.size();
        final_size += CONTENT_ID.size() + COLON.size() + content_id.size() + CRLF.size();
        final_size += CONTENT_TYPE.size() + COLON.size() + content_type.size() + CRLF.size();
        if (insert_content_transfer_encoding)
        {
            final_size += CONTENT_TRANSFER_ENCODDING.size() + COLON.size() + binary.size() + CRLF.size();
        }
        for (auto& h : headers)
        {
            final_size += (h.first.size() + COLON.size() + h.second.size() + CRLF.size());
        }
        final_size += CRLF.size();
        final_size += content.size() + CRLF.size();

        ret.reserve(final_size);

        //ret.append(CRLF);
        ret.append(CONTENT_ID).append(COLON).append(content_id).append(CRLF);
        ret.append(CONTENT_TYPE).append(COLON).append(content_type).append(CRLF);
        if (insert_content_transfer_encoding)
        {
            ret.append(CONTENT_TRANSFER_ENCODDING).append(COLON).append(binary).append(CRLF);
        }
        for (auto& h : headers)
        {
            ret.append(h.first).append(COLON).append(h.second).append(CRLF);
        }
        ret.append(CRLF);
        ret.append(content).append(CRLF);
        return ret;
    }
};

class NotificationDescription
{
public:
    std::string recordRef;
    std::string operationType;
    std::string subscriptionId;
    void staticjson_init(staticjson::ObjectHandler* h)
    {
        h->add_property("recordRef", &this->recordRef);
        h->add_property("operationType", &this->operationType);
        h->add_property("subscriptionId", &this->subscriptionId, staticjson::Flags::Optional);
    }
};

class Record
{
public:
    std::unordered_map<std::string, size_t> blockId_to_index;
    std::vector<Block> blocks;
    RecordMeta meta;
    std::unique_ptr<std::shared_timed_mutex> blocks_mutex;
    std::unique_ptr<std::shared_timed_mutex> meta_mutex;

    explicit Record():
        blocks_mutex(std::make_unique<std::shared_timed_mutex>()),
        meta_mutex(std::make_unique<std::shared_timed_mutex>())
    {
    }

    /*
    void staticjson_init(staticjson::ObjectHandler* h)
    {
        h->add_property("blocks", &this->blocks, staticjson::Flags::Optional);
        h->add_property("meta", &this->meta);
    }
    */
    bool insert_or_update_block(const std::string& id, const std::string& type, const std::string& blockData,
                                const std::map<std::string, std::string, ci_less>& headers,
                                bool& update)
    {
        std::unique_lock<std::shared_timed_mutex> write_guard(*blocks_mutex);
        auto iter = blockId_to_index.find(id);
        if (iter != blockId_to_index.end())
        {
            blocks[iter->second].content = blockData;
            blocks[iter->second].content_id = id;
            blocks[iter->second].content_type = type;
            blocks[iter->second].headers = headers;
            update = true;
            return true;
        }
        else
        {
            blocks.emplace_back();
            blocks.back().content = blockData;
            blocks.back().content_id = id;
            blocks.back().content_type = type;
            blocks.back().headers = headers;
            blockId_to_index[blocks.back().content_id] = (blocks.size() - 1);
            update = false;
            return true;
        }
        return false;
    }

    bool delete_block(const std::string& blockId)
    {
        bool delete_successful = true;
        std::unique_lock<std::shared_timed_mutex> guard(*blocks_mutex);
        auto iter = blockId_to_index.find(blockId);
        if (iter == blockId_to_index.end())
        {
            delete_successful = false;
            return delete_successful;
        }
        delete_successful = true;
        auto index = iter->second;
        blockId_to_index.erase(iter);
        blocks.erase(blocks.begin() + index);
        for (auto& m : blockId_to_index)
        {
            if (m.second > index)
            {
                m.second--;
            }
        }
        return delete_successful;
    }

    Block get_block_object(const std::string& blockId)
    {
        std::shared_lock<std::shared_timed_mutex> guard(*blocks_mutex);
        std::string ret;
        auto iter = blockId_to_index.find(blockId);
        if (iter == blockId_to_index.end())
        {

            return Block();
        }
        auto index = iter->second;
        return blocks[index];
    }

    std::string produce_multipart_body(const std::string& notificationDescription = "")
    {
        std::string ret;
        size_t final_size = 0;
        std::shared_lock<std::shared_timed_mutex> blocks_guard(*blocks_mutex);
        std::shared_lock<std::shared_timed_mutex> meta_guard(*meta_mutex);
        auto metaString = staticjson::to_json_string(meta);
        std::vector<std::string> block_body_parts;
        for (auto& b : blocks)
        {
            block_body_parts.emplace_back(b.produce_body_part());
        }

        final_size += CRLF.size();

        if (notificationDescription.size())
        {
            final_size += VERY_SPECIAL_BOUNARY_WITH_LEADING_TWO_DASHES.size() + CRLF.size();
            final_size += CONTENT_ID.size() + COLON.size() + NOTIFICATION_DESCRIPTION_ID.size() + CRLF.size();
            final_size += CONTENT_TYPE.size() + COLON.size() + JSON_CONTENT.size() + CRLF.size();
            final_size += CRLF.size();
            final_size += notificationDescription.size() + CRLF.size();
        }

        final_size += VERY_SPECIAL_BOUNARY_WITH_LEADING_TWO_DASHES.size() + CRLF.size();
        final_size += CONTENT_ID.size() + COLON.size() + META_CONTENT_ID.size() + CRLF.size();
        final_size += CONTENT_TYPE.size() + COLON.size() + JSON_CONTENT.size() + CRLF.size();
        final_size += CRLF.size();
        final_size += metaString.size() + CRLF.size();

        for (auto& bb : block_body_parts)
        {
            final_size += VERY_SPECIAL_BOUNARY_WITH_LEADING_TWO_DASHES.size() + CRLF.size();
            final_size += bb.size();
        }

        final_size += VERY_SPECIAL_BOUNARY_WITH_LEADING_TWO_DASHES.size() + ENDING_TWO_DASH.size() + CRLF.size();

        ret.append(CRLF);

        if (notificationDescription.size())
        {
            ret.append(VERY_SPECIAL_BOUNARY_WITH_LEADING_TWO_DASHES).append(CRLF);
            ret.append(CONTENT_ID).append(COLON).append(NOTIFICATION_DESCRIPTION_ID).append(CRLF);
            ret.append(CONTENT_TYPE).append(COLON).append(JSON_CONTENT).append(CRLF);
            ret.append(CRLF);
            ret.append(notificationDescription).append(CRLF);
        }

        ret.append(VERY_SPECIAL_BOUNARY_WITH_LEADING_TWO_DASHES).append(CRLF);
        ret.append(CONTENT_ID).append(COLON).append(META_CONTENT_ID).append(CRLF);
        ret.append(CONTENT_TYPE).append(COLON).append(JSON_CONTENT).append(CRLF);
        ret.append(CRLF);
        ret.append(metaString).append(CRLF);

        for (auto& bb : block_body_parts)
        {
            ret.append(VERY_SPECIAL_BOUNARY_WITH_LEADING_TWO_DASHES).append(CRLF);
            ret.append(bb);
        }

        ret.append(VERY_SPECIAL_BOUNARY_WITH_LEADING_TWO_DASHES).append(ENDING_TWO_DASH).append(CRLF);

        return ret;
    }

    std::string get_meta()
    {
        std::shared_lock<std::shared_timed_mutex> guard(*meta_mutex);
        auto metaString = staticjson::to_json_string(meta);
        return metaString;
    }

    bool set_meta(const std::string& metaString)
    {
        std::unique_lock<std::shared_timed_mutex> guard(*meta_mutex);
        staticjson::ParseStatus result;
        return staticjson::from_json_string(metaString.c_str(), &meta, &result);
    }

    RecordMeta& get_meta_object()
    {
        std::shared_lock<std::shared_timed_mutex> guard(*meta_mutex);
        return meta;
    }

    void set_meta_object(RecordMeta& new_meta)
    {
        std::unique_lock<std::shared_timed_mutex> guard(*meta_mutex);
        meta = std::move(new_meta);
    }

    bool validate_meta_with_schema(const RecordMeta& new_meta, const MetaSchema& schema_object)
    {
        auto ret = true;
        for (auto& tag : schema_object.metaTags)
        {
            if (tag.presence && new_meta.tags.count(tag.tagName) == 0)
            {
                ret = false;
                break;
            }
        }
        return ret;
    }

    bool update_meta(const std::string& meta_patch, const MetaSchema& metaSchema)
    {
        try
        {
            std::shared_lock<std::shared_timed_mutex> read_lock(*meta_mutex);
            auto metaString = staticjson::to_json_string(meta);
            read_lock.unlock();
            nlohmann::json original_meta = nlohmann::json::parse(metaString);
            nlohmann::json patch = nlohmann::json::parse(meta_patch);
            nlohmann::json patched_meta = original_meta.patch(patch);
            staticjson::ParseStatus result;
            RecordMeta new_meta;
            if (staticjson::from_json_string(patched_meta.dump().c_str(), &new_meta, &result)
                && validate_meta_with_schema(new_meta, metaSchema))
            {
                set_meta_object(new_meta);
                return true;
            }
        }
        catch (...)
        {
        }
        return false;
    }
};

class Tags_Value_Db_Of_One_Tag_Name
{
public:
    std::unique_ptr<std::shared_timed_mutex> value_to_record_id_map_mutex;
    std::multimap<std::string, std::string> value_to_record_id_map;
    explicit Tags_Value_Db_Of_One_Tag_Name():
        value_to_record_id_map_mutex(std::make_unique<std::shared_timed_mutex>())
    {
    }
};

class Tags_Name_Db_Of_One_Schema
{
public:
    std::unique_ptr<std::shared_timed_mutex> name_to_value_db_map_mutex;
    std::map<std::string, Tags_Value_Db_Of_One_Tag_Name> name_to_value_db_map;
    explicit Tags_Name_Db_Of_One_Schema():
        name_to_value_db_map_mutex(std::make_unique<std::shared_timed_mutex>())
    {
    }
};

using Tags_Db = std::map<std::string, Tags_Name_Db_Of_One_Schema>;

class SubscriptionFilter
{
public:
    std::vector<std::string> monitoredResourceUris;
    std::vector<std::string> operations;
    void staticjson_init(staticjson::ObjectHandler* h)
    {
        h->add_property("monitoredResourceUris", &this->monitoredResourceUris);
        h->add_property("operations", &this->operations);
    }
};

class ClientId
{
public:
    std::string nfId;
    std::string nfSetId;
    void staticjson_init(staticjson::ObjectHandler* h)
    {
        h->add_property("nfId", &this->nfId, staticjson::Flags::Optional);
        h->add_property("nfSetId", &this->nfSetId, staticjson::Flags::Optional);
    }
};

class NotificationSubscription
{
public:
    ClientId clientId;
    std::string callbackReference;
    std::string expiryCallbackReference;
    std::string expiry;
    uint64_t expiryNotification = 0;
    SubscriptionFilter subFilter;
    std::string supportedFeatures;
    void staticjson_init(staticjson::ObjectHandler* h)
    {
        h->add_property("clientId", &this->clientId);
        h->add_property("callbackReference", &this->callbackReference);
        h->add_property("expiryCallbackReference", &this->expiryCallbackReference, staticjson::Flags::Optional);
        h->add_property("expiry", &this->expiry, staticjson::Flags::Optional);
        h->add_property("expiryNotification", &this->expiryNotification, staticjson::Flags::Optional);
        h->add_property("subFilter", &this->subFilter, staticjson::Flags::Optional);
        h->add_property("supportedFeatures", &this->supportedFeatures, staticjson::Flags::Optional);
    }
};

class Storage
{
public:
    std::string realm_id;
    std::string storage_id;
    std::unordered_map<std::string, Record> records[ONE_DIMENSION_SIZE][ONE_DIMENSION_SIZE];
    std::unordered_map<std::string, MetaSchema> schemas[ONE_DIMENSION_SIZE][ONE_DIMENSION_SIZE];
    std::map<std::string, std::pair<std::unique_ptr<std::shared_timed_mutex>, std::set<std::string>>>
    schema_id_to_record_ids[ONE_DIMENSION_SIZE];
    std::shared_timed_mutex schema_id_to_record_ids_mutex[ONE_DIMENSION_SIZE];
    std::shared_timed_mutex records_mutexes[ONE_DIMENSION_SIZE][ONE_DIMENSION_SIZE];
    std::shared_timed_mutex schemas_mutexes[ONE_DIMENSION_SIZE][ONE_DIMENSION_SIZE];

    Tags_Db tags_db;
    std::shared_timed_mutex tags_db_main_mutex;

    std::map<uint64_t, std::set<std::string>> records_ttl;
    std::shared_timed_mutex record_ttl_mutex;

    std::unordered_map<std::string, NotificationSubscription>
    subscription_id_to_subscription[ONE_DIMENSION_SIZE][ONE_DIMENSION_SIZE];
    std::shared_timed_mutex subscription_id_to_subscription_mutex[ONE_DIMENSION_SIZE][ONE_DIMENSION_SIZE];
    std::unordered_map<std::string, std::set<std::string>>
                                                        monitored_resource_uri_to_subscription_id[ONE_DIMENSION_SIZE][ONE_DIMENSION_SIZE];
    std::shared_timed_mutex monitored_resource_uri_to_subscription_id_mutex[ONE_DIMENSION_SIZE][ONE_DIMENSION_SIZE];
    std::map<uint64_t, std::set<std::string>> subscription_expiry_to_subscription_id;
    std::shared_timed_mutex subscription_expiry_to_subscription_id_mutex;
    std::map<uint64_t, std::set<std::string>> subscription_expiryNotification_to_subscription_id;
    std::shared_timed_mutex subscription_expiryNotification_to_subscription_id_mutex;


    void init_default_schema_with_empty_schema_id()
    {
        std::unique_lock<std::shared_timed_mutex> schemas_write_lock(schemas_mutexes[0][0]);
        auto& s = schemas[0][0][""];
        schemas_write_lock.unlock();

        std::unique_lock<std::shared_timed_mutex> record_ids_write_lock(schema_id_to_record_ids_mutex[0]);
        auto& r = schema_id_to_record_ids[0][""];
        r.first = std::make_unique<std::shared_timed_mutex>();
    }
    void install_tags_from_schema(const MetaSchema& schema)
    {
        Tags_Name_Db_Of_One_Schema tags_name_db;
        for (auto& tag : schema.metaTags)
        {
            auto& tag_value_db = tags_name_db.name_to_value_db_map[tag.tagName];
        }
        std::string schema_id = schema.schemaId;
        std::unique_lock<std::shared_timed_mutex> tags_db_main_write_lock(tags_db_main_mutex);
        tags_db.emplace(std::move(schema_id), std::move(tags_name_db));
    }

    std::set<std::string> get_all_record_ids(const std::string& schema_id)
    {
        std::set<std::string> ret;
        auto u8 = get_u8_sum(schema_id);
        std::vector<decltype(schema_id_to_record_ids[0].begin())> iters_to_go_through;
        if (schema_id.size())
        {
            std::shared_lock<std::shared_timed_mutex> record_ids_read_lock(schema_id_to_record_ids_mutex[u8]);
            auto iter = schema_id_to_record_ids[u8].find(schema_id);
            if (iter != schema_id_to_record_ids[u8].end())
            {
                iters_to_go_through.push_back(iter);
            }
        }
        else
        {
            for (size_t i = 0; i < ONE_DIMENSION_SIZE; i++)
            {
                std::shared_lock<std::shared_timed_mutex> record_ids_read_lock(schema_id_to_record_ids_mutex[i]);
                auto iter = schema_id_to_record_ids[i].begin();
                while (iter != schema_id_to_record_ids[i].end())
                {
                    iters_to_go_through.push_back(iter);
                    iter++;
                }
            }
        }
        for (auto iter : iters_to_go_through)
        {
            std::shared_lock<std::shared_timed_mutex> record_ids_read_lock(*iter->second.first);
            ret.insert(iter->second.second.begin(), iter->second.second.end());
        }
        return ret;
    }

    void delete_tags_from_schema(const std::string& schema_id)
    {
        std::unique_lock<std::shared_timed_mutex> tags_db_main_write_guard(tags_db_main_mutex);
        tags_db.erase(schema_id);
    }

    void insert_tag_value(const std::string& schema_id, const std::string& tag_name, const std::string& tag_value,
                          const std::string& record_id)
    {
        std::shared_lock<std::shared_timed_mutex> tags_db_main_read_guard(tags_db_main_mutex);
        auto tags_db_iter = tags_db.find(schema_id);
        if (tags_db_iter != tags_db.end())
        {
            auto& tags_name_db = tags_db_iter->second;
            std::shared_lock<std::shared_timed_mutex> tags_name_db_read_lock(*tags_name_db.name_to_value_db_map_mutex);
            auto tag_name_db_iter = tags_name_db.name_to_value_db_map.find(tag_name);
            if (tag_name_db_iter != tags_name_db.name_to_value_db_map.end())
            {
                auto& tags_value_db = tag_name_db_iter->second;
                std::unique_lock<std::shared_timed_mutex> tags_value_db_write_lock(*(tags_value_db.value_to_record_id_map_mutex));
                tags_value_db.value_to_record_id_map.emplace(tag_value, record_id);
            }
        }
    }

    void remove_tag_value(const std::string& schema_id, const std::string& tag_name, const std::string& tag_value,
                          const std::string& record_id)
    {
        std::shared_lock<std::shared_timed_mutex> tags_db_main_read_lock(tags_db_main_mutex);
        auto tags_db_iter = tags_db.find(schema_id);
        if (tags_db_iter != tags_db.end())
        {
            auto& tags_name_db = tags_db_iter->second;
            std::shared_lock<std::shared_timed_mutex> tags_name_db_read_lock(*tags_name_db.name_to_value_db_map_mutex);
            auto tag_name_db_iter = tags_name_db.name_to_value_db_map.find(tag_name);
            if (tag_name_db_iter != tags_name_db.name_to_value_db_map.end())
            {
                auto& tags_value_db = tag_name_db_iter->second;
                std::unique_lock<std::shared_timed_mutex> tags_value_db_write_lock(*(tags_value_db.value_to_record_id_map_mutex));
                auto range = tags_value_db.value_to_record_id_map.equal_range(tag_value);
                std::vector<decltype(range.first)> iters_to_erase;
                for (auto i = range.first; i != range.second; ++i)
                {
                    if (i->second == record_id)
                    {
                        iters_to_erase.emplace_back(i);
                    }
                }
                for (auto i : iters_to_erase)
                {
                    tags_value_db.value_to_record_id_map.erase(i);
                }
            }
        }
    }

    std::set<std::string> get_record_ids_with_tag_eq_to_value(const std::string& schema_id, const std::string& tag_name,
                                                              const std::string& tag_value)
    {
        return run_search_comparison(schema_id, "EQ", tag_name, tag_value);
    }

    std::set<std::string> run_search_comparison(const std::string& schema_id, const std::string& op,
                                                const std::string& tag_name, const std::string& tag_value)
    {
        std::set<std::string> s;
        std::shared_lock<std::shared_timed_mutex> tags_db_main_read_lock(tags_db_main_mutex);
        std::vector<Tags_Db::iterator> tag_db_iterators_to_go_through;
        if (schema_id.size())
        {
            auto db_iter = tags_db.find(schema_id);
            if (db_iter != tags_db.end())
            {
                tag_db_iterators_to_go_through.push_back(db_iter);
            }
        }
        else
        {
            auto db_iter = tags_db.begin();
            while (db_iter != tags_db.end())
            {
                tag_db_iterators_to_go_through.push_back(db_iter);
                db_iter++;
            }
        }
        for (auto tags_db_iter : tag_db_iterators_to_go_through)
        {
            auto& tags_name_db = tags_db_iter->second;
            std::shared_lock<std::shared_timed_mutex> tags_name_db_read_lock(*tags_name_db.name_to_value_db_map_mutex);
            auto tag_name_db_iter = tags_name_db.name_to_value_db_map.find(tag_name);
            if (tag_name_db_iter != tags_name_db.name_to_value_db_map.end())
            {
                auto& tags_value_db = tag_name_db_iter->second;
                std::shared_lock<std::shared_timed_mutex> tags_value_db_read_lock(*(tags_value_db.value_to_record_id_map_mutex));

                if (op == "EQ")
                {
                    auto range = tags_value_db.value_to_record_id_map.equal_range(tag_value);
                    for (auto i = range.first; i != range.second; ++i)
                    {
                        s.insert(i->second);
                    }
                }
                else if (op == "GT")
                {
                    auto iter = tags_value_db.value_to_record_id_map.upper_bound(tag_value);
                    while (iter != tags_value_db.value_to_record_id_map.end())
                    {
                        s.insert(iter->second);
                        iter++;
                    }
                }
                else if (op == "GTE")
                {
                    auto iter = tags_value_db.value_to_record_id_map.lower_bound(tag_value);
                    while (iter != tags_value_db.value_to_record_id_map.end())
                    {
                        s.insert(iter->second);
                        iter++;
                    }
                }
                else if (op == "LT")
                {
                    auto lower_bound = tags_value_db.value_to_record_id_map.lower_bound(tag_value);
                    auto iter = tags_value_db.value_to_record_id_map.begin();
                    while (iter != lower_bound)
                    {
                        s.insert(iter->second);
                        iter++;
                    }
                }
                else if (op == "LTE")
                {
                    auto upper_bound = tags_value_db.value_to_record_id_map.upper_bound(tag_value);
                    auto iter = tags_value_db.value_to_record_id_map.begin();
                    while (iter != upper_bound)
                    {
                        s.insert(iter->second);
                        iter++;
                    }
                }
                else if (op == "NEQ")
                {
                    auto lower_bound = tags_value_db.value_to_record_id_map.lower_bound(tag_value);
                    auto upper_bound = tags_value_db.value_to_record_id_map.upper_bound(tag_value);
                    for (auto iter = tags_value_db.value_to_record_id_map.begin(); iter != lower_bound; ++iter)
                    {
                        s.insert(iter->second);
                    }
                    for (auto iter = upper_bound; iter != tags_value_db.value_to_record_id_map.end(); ++iter)
                    {
                        s.insert(iter->second);
                    }
                }
            }
        }
        return s;
    };

    size_t count_records_of_tag_name(const std::string& schema_id, const std::string& tag_name)
    {
        size_t ret = 0;
        std::shared_lock<std::shared_timed_mutex> tags_db_main_read_lock(tags_db_main_mutex);
        std::vector<Tags_Db::iterator> tag_db_iterators_to_go_through;
        if (schema_id.size())
        {
            auto db_iter = tags_db.find(schema_id);
            if (db_iter != tags_db.end())
            {
                tag_db_iterators_to_go_through.push_back(db_iter);
            }
        }
        else
        {
            auto db_iter = tags_db.begin();
            while (db_iter != tags_db.end())
            {
                tag_db_iterators_to_go_through.push_back(db_iter);
                db_iter++;
            }
        }
        for (auto tags_db_iter : tag_db_iterators_to_go_through)
        {
            auto& tags_name_db = tags_db_iter->second;

            std::shared_lock<std::shared_timed_mutex> tags_name_db_read_lock(*tags_name_db.name_to_value_db_map_mutex);
            auto tag_name_db_iter = tags_name_db.name_to_value_db_map.find(tag_name);
            if (tag_name_db_iter != tags_name_db.name_to_value_db_map.end())
            {
                auto& tags_value_db = tag_name_db_iter->second;
                std::shared_lock<std::shared_timed_mutex> tags_value_db_read_lock(*(tags_value_db.value_to_record_id_map_mutex));
                ret = +tags_value_db.value_to_record_id_map.size();
            }
        }
        return ret;
    }

    bool validate_record_meta(const RecordMeta& meta)
    {
        bool ret = true;
        if (meta.schemaId.empty())
        {
            return ret;
        }
        bool success;
        auto schema_object = get_schema_object(meta.schemaId, success);
        if (success)
        {
            for (auto& tag : schema_object.metaTags)
            {
                if (tag.presence && meta.tags.count(tag.tagName) == 0)
                {
                    ret = false;
                    break;
                }
            }
        }
        return ret;
    }

    void track_record_id(const std::string& schema_id, const std::string& record_id, bool insert = true)
    {
        auto u8 = get_u8_sum(schema_id);
        std::shared_lock<std::shared_timed_mutex> record_ids_read_lock(schema_id_to_record_ids_mutex[u8]);
        auto iter = schema_id_to_record_ids[u8].find(schema_id);
        if (iter != schema_id_to_record_ids[u8].end())
        {
            std::unique_lock<std::shared_timed_mutex> per_schema_record_ids_write_lock(*iter->second.first);
            if (insert)
            {
                iter->second.second.insert(record_id);
            }
            else
            {
                iter->second.second.erase(record_id);
            }
        }
    }

    void track_record_ttl(uint64_t ttl, const std::string& record_id, bool insert = true)
    {
        std::unique_lock<std::shared_timed_mutex> record_ttl_write_lock(record_ttl_mutex);
        if (insert)
        {
            auto& s = records_ttl[ttl];
            s.insert(record_id);
        }
        else
        {
            auto iter = records_ttl.find(ttl);
            if (iter != records_ttl.end())
            {
                iter->second.erase(record_id);
            }
        }
    }

    std::set<std::string> get_ttl_expired_records()
    {
        std::set<std::string> s;
        auto curr_time_point = std::chrono::system_clock::now();
        auto seconds_since_epoch = std::chrono::duration_cast<std::chrono::seconds>(curr_time_point.time_since_epoch()).count();
        std::unique_lock<std::shared_timed_mutex> record_ttl_read_lock(record_ttl_mutex);
        auto upper_bound = records_ttl.upper_bound(seconds_since_epoch);
        auto iter = records_ttl.begin();
        while (iter != upper_bound)
        {
            s.insert(iter->second.begin(), iter->second.end());
            iter++;
        }
        return s;
    }

    /**
    * @brief insert or update record
    *
    * @returns int
    * return 0: successful
    * return -1: decode multipart failure
    * return -2: block content-id absent
    * return -3: meta decode or validation failure
    * return -5: record ttl already expired
    * @thows None
    * */
    int insert_or_update_record(const std::string& record_id, const std::string& boundary,
                                const std::string& multipart_content, bool& update)
    {
        MultipartParser parser(boundary);
        auto parts = std::move(parser.get_parts(multipart_content));
        if (parts.empty())
        {
            return DECODE_MULTIPART_FAILURE;
        }
        RecordMeta record_meta;
        staticjson::ParseStatus result;
        if (staticjson::from_json_string(parts[0].second.c_str(), &record_meta, &result) &&
            validate_record_meta(record_meta))
        {
            uint64_t ttl = 0;
            if (record_meta.ttl.size())
            {
                ttl = iso8601_timestamp_to_seconds_since_epoch(record_meta.ttl);
                auto seconds_since_epoch = std::chrono::duration_cast<std::chrono::seconds>
                                           (std::chrono::system_clock::now().time_since_epoch()).count();
                if (ttl <= seconds_since_epoch)
                {
                    return TTL_EXPIRED;
                }
            }
            Record record;
            auto record_meta_copy = record_meta;
            record.set_meta_object(record_meta);
            for (size_t i = 1; i < parts.size(); i++)
            {
                std::string content_id;
                std::string content_type;
                auto iter = parts[i].first.find(CONTENT_ID);
                if (iter == parts[i].first.end())
                {
                    return CONTENT_ID_NOT_PRESDENT;
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
                record.insert_or_update_block(content_id, content_type, parts[i].second, parts[i].first, dummy);
            }
            uint16_t sum = get_u16_sum(record_id);
            uint8_t row = sum >> 8;
            uint8_t col = sum & 0xFF;
            std::unique_lock<std::shared_timed_mutex> record_map_write_guard(records_mutexes[row][col]);
            Record* target = nullptr;
            auto iter = records[row][col].find(record_id);
            if (iter == records[row][col].end())
            {
                target = &records[row][col][record_id];
                update = false;
            }
            else
            {
                target = &iter->second;
                update = true;
            }
            auto old_ttl = iso8601_timestamp_to_seconds_since_epoch(target->meta.ttl);
            *target = std::move(record);
            // this is commented out to prevent record from being deleted from other thread before tags value insertion is done here
            // record_map_write_guard.unlock();

            for (auto& tag : record_meta_copy.tags)
            {
                auto& tag_name = tag.first;
                auto& tag_values = tag.second;
                for (auto& tag_value : tag_values)
                {
                    insert_tag_value(record_meta_copy.schemaId, tag_name, tag_value, record_id);
                }
            }

            track_record_id(record_meta_copy.schemaId, record_id);
            if (update && old_ttl)
            {
                track_record_ttl(old_ttl, record_id, false);
            }
            if (ttl)
            {
                track_record_ttl(ttl, record_id, true);
            }
            record_map_write_guard.unlock();
            send_notify(record_id, "", update ? RECORD_OPERATION_UPDATED : RECORD_OPERATION_CREATED);
            return 0;
        }
        return RECORD_META_DECODE_FAILURE;
    }
    std::string delete_record(const std::string& record_id, bool& record_exist_before, bool get_previous = false)
    {
        send_notify(record_id, "", RECORD_OPERATION_DELETED);
        std::string ret;
        if (!get_previous)
        {
            record_exist_before = delete_record_directly(record_id);
        }
        else
        {
            ret = get_record(record_id);
            record_exist_before = delete_record_directly(record_id);
        }
        return ret;
    }

    bool delete_record_directly(const std::string& record_id)
    {
        uint16_t sum = get_u16_sum(record_id);
        uint8_t row = sum >> 8;
        uint8_t col = sum & 0xFF;
        std::unique_lock<std::shared_timed_mutex> write_lock(records_mutexes[row][col]);
        auto iter = records[row][col].find(record_id);
        if (iter != records[row][col].end())
        {
            auto record = std::move(iter->second);
            records[row][col].erase(iter);

            for (auto& tag : record.meta.tags)
            {
                auto& tag_name = tag.first;
                auto& tag_values = tag.second;
                for (auto& tag_value : tag_values)
                {
                    remove_tag_value(record.meta.schemaId, tag_name, tag_value, record_id);
                }
            }
            track_record_id(record.meta.schemaId, record_id, false);
            if (record.meta.ttl.size())
            {
                track_record_ttl(iso8601_timestamp_to_seconds_since_epoch(record.meta.ttl), record_id, false);
            }
            return true;
        }
        return false;
    }

    std::string get_record(const std::string& record_id, const std::string& notificationDescription = "")
    {
        uint16_t sum = get_u16_sum(record_id);
        uint8_t row = sum >> 8;
        uint8_t col = sum & 0xFF;
        std::shared_lock<std::shared_timed_mutex> guard(records_mutexes[row][col]);
        std::string ret;
        auto iter = records[row][col].find(record_id);
        if (iter != records[row][col].end())
        {
            ret = iter->second.produce_multipart_body(notificationDescription);
        }
        return ret;
    }

    std::string get_schema(const std::string& schema_id, bool& found)
    {
        std::string ret;
        uint16_t sum = get_u16_sum(schema_id);
        uint8_t row = sum >> 8;
        uint8_t col = sum & 0xFF;
        std::shared_lock<std::shared_timed_mutex> guard(schemas_mutexes[row][col]);
        auto iter = schemas[row][col].find(schema_id);
        if (iter != schemas[row][col].end())
        {
            ret = staticjson::to_json_string(iter->second);
            found = true;
        }
        else
        {
            found = false;
        }
        return ret;
    }

    MetaSchema get_schema_object(const std::string& schema_id, bool& success)
    {
        static MetaSchema dummyMetaSchama;
        success = false;
        uint16_t sum = get_u16_sum(schema_id);
        uint8_t row = sum >> 8;
        uint8_t col = sum & 0xFF;
        std::shared_lock<std::shared_timed_mutex> guard(schemas_mutexes[row][col]);
        auto iter = schemas[row][col].find(schema_id);
        if (iter != schemas[row][col].end())
        {
            success = true;
            return iter->second;
        }
        return dummyMetaSchama;
    }

    bool create_or_update_schema(const std::string& schema_id, const std::string& schema, bool& update)
    {
        uint16_t sum = get_u16_sum(schema_id);
        uint8_t row = sum >> 8;
        uint8_t col = sum & 0xFF;
        staticjson::ParseStatus result;
        MetaSchema metaSchema;
        if (staticjson::from_json_string(schema.c_str(), &metaSchema, &result) &&
            metaSchema.schemaId == schema_id)
        {
            install_tags_from_schema(metaSchema);
            std::unique_lock<std::shared_timed_mutex> schemas_mutexes_write_lock(schemas_mutexes[row][col]);
            auto& target = schemas[row][col][schema_id];
            if (target.schemaId.empty())
            {
                update = false;
            }
            else
            {
                update = true;
            }
            schemas[row][col][schema_id] = std::move(metaSchema);
            schemas_mutexes_write_lock.unlock();

            auto u8 = get_u8_sum(schema_id);
            std::shared_lock<std::shared_timed_mutex> record_ids_read_lock(schema_id_to_record_ids_mutex[u8]);
            if (schema_id_to_record_ids[u8].find(schema_id) == schema_id_to_record_ids[u8].end())
            {
                record_ids_read_lock.unlock();
                std::unique_lock<std::shared_timed_mutex> record_ids_write_lock(schema_id_to_record_ids_mutex[u8]);
                auto& r = schema_id_to_record_ids[u8][schema_id];
                r.first = std::make_unique<std::shared_timed_mutex>();
            }
            return true;
        }
        return false;
    }

    bool delete_schema(const std::string& schema_id)
    {
        bool ret = false;
        uint16_t sum = get_u16_sum(schema_id);
        uint8_t row = sum >> 8;
        uint8_t col = sum & 0xFF;
        std::unique_lock<std::shared_timed_mutex> guard(schemas_mutexes[row][col]);
        auto iter = schemas[row][col].find(schema_id);
        if (iter != schemas[row][col].end())
        {
            ret = true;
            schemas[row][col].erase(iter);
        }
        delete_tags_from_schema(schema_id);
        return ret;
    }

    bool update_record_meta(const std::string& record_id, const std::string& meta_patch)
    {
        uint16_t sum = get_u16_sum(record_id);
        uint8_t row = sum >> 8;
        uint8_t col = sum & 0xFF;
        std::shared_lock<std::shared_timed_mutex> guard(records_mutexes[row][col]);
        auto iter = records[row][col].find(record_id);
        if (iter != records[row][col].end())
        {
            auto schema_id = iter->second.meta.schemaId;
            bool success = false;
            auto schema_object = get_schema_object(schema_id, success);
            if (success)
            {
                return iter->second.update_meta(meta_patch, schema_object);
            }
        }
        return false;
    }

    bool insert_or_update_block(const std::string& record_id, const std::string& id, const std::string& type,
                                const std::string& blockData,
                                const std::map<std::string, std::string, ci_less>& headers, bool& update)
    {
        uint16_t sum = get_u16_sum(record_id);
        uint8_t row = sum >> 8;
        uint8_t col = sum & 0xFF;
        std::shared_lock<std::shared_timed_mutex> guard(records_mutexes[row][col]);
        auto iter = records[row][col].find(record_id);
        if (iter != records[row][col].end())
        {
            auto ret = iter->second.insert_or_update_block(id, type, blockData, headers, update);
            guard.unlock();
            send_notify(record_id, id, RECORD_OPERATION_CREATED);
            return ret;
        }
        return false;
    }

    bool delete_block(const std::string& record_id, const std::string& blockId)
    {
        bool delete_successful = true;
        uint16_t sum = get_u16_sum(record_id);
        uint8_t row = sum >> 8;
        uint8_t col = sum & 0xFF;
        std::shared_lock<std::shared_timed_mutex> guard(records_mutexes[row][col]);
        auto iter = records[row][col].find(record_id);
        if (iter != records[row][col].end())
        {
            delete_successful = iter->second.delete_block(blockId);
            guard.unlock();
            if (delete_successful)
            {
                send_notify(record_id, blockId, RECORD_OPERATION_DELETED);
            }
        }
        return delete_successful;
    }

    Block get_block(const std::string& record_id, const std::string& blockId)
    {
        uint16_t sum = get_u16_sum(record_id);
        uint8_t row = sum >> 8;
        uint8_t col = sum & 0xFF;
        std::shared_lock<std::shared_timed_mutex> guard(records_mutexes[row][col]);
        auto iter = records[row][col].find(record_id);
        if (iter != records[row][col].end())
        {
            return iter->second.get_block_object(blockId);
        }
        return Block();
    }

    std::set<std::string> run_search_expression(const std::set<std::string>& curr_set, const std::string& schema_id,
                                                rapidjson::Value& value)
    {
        std::set<std::string> ret;
        auto actual_schema_id = schema_id;

        auto schema_violation_handling = [](rapidjson::Value & value)
        {
            rapidjson::StringBuffer buffer;
            rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
            value.Accept(writer);
            std::cerr << "schema violation: " << buffer.GetString() << std::endl;
        };

        auto get_record_id_list_from_array = [&schema_violation_handling](rapidjson::Value & value)
        {
            std::set<std::string> s;
            if (value.IsArray())
            {
                for (size_t i = 0; i < value.Size(); i++)
                {
                    if (value[i].IsString())
                    {
                        s.emplace(value[i].GetString());
                    }
                }
            }
            else
            {
                schema_violation_handling(value);
            }
            return s;
        };

        if (value.IsObject())
        {
            auto first_name = std::string(value.MemberBegin()->name.GetString(), value.MemberBegin()->name.GetStringLength());
            if (first_name == CONDITION)
            {
                if (actual_schema_id.empty())
                {
                    actual_schema_id = get_string_value_from_Json_object(value, "schemaId");
                }
                auto condition_operator = get_string_value_from_Json_object(value, CONDITION);
                std::vector<std::set<std::string>> operands;
                if (value.HasMember(UNITS.c_str()))
                {
                    auto& units = value[UNITS.c_str()];
                    if (units.IsArray())
                    {
                        std::vector<rapidjson::Value*> simple_values;
                        std::vector<rapidjson::Value*> nested_values;
                        for (size_t i = 0; i < units.Size(); i++)
                        {
                            auto& array_value = units[i];
                            if (array_value.IsObject())
                            {
                                auto first_name = std::string(array_value.MemberBegin()->name.GetString(),
                                                              array_value.MemberBegin()->name.GetStringLength());
                                if (first_name == CONDITION)
                                {
                                    nested_values.push_back(&array_value);
                                }
                                else
                                {
                                    simple_values.push_back(&array_value);
                                }
                            }
                        }
                        for (auto v : simple_values)
                        {
                            operands.emplace_back(run_search_expression(curr_set, actual_schema_id, *v));

                        }
                        if (condition_operator == CONDITION_OP_AND)
                        {
                            auto s = run_and_operator(operands);
                            operands.clear();
                            operands.emplace_back(std::move(s));
                        }
                        for (auto v : nested_values)
                        {
                            operands.emplace_back(run_search_expression(*operands.rbegin(), actual_schema_id, *v));
                        }
                    }
                    else
                    {
                        schema_violation_handling(value);
                    }
                }
                if (condition_operator == CONDITION_OP_AND)
                {
                    ret = run_and_operator(operands);
                }
                else if (condition_operator == CONDITION_OP_OR)
                {
                    ret = run_or_operator(operands);
                }
                else if (condition_operator == CONDITION_OP_NOT)
                {
                    if (curr_set.empty())
                    {
                        ret = run_not_operator(get_all_record_ids(schema_id), run_or_operator(operands));
                    }
                    else
                    {
                        ret = run_not_operator(curr_set, run_or_operator(operands));
                    }
                    // TODO:
                }
            }
            else if (first_name == OPERATION)
            {
                std::string op = get_string_value_from_Json_object(value, OPERATION);
                std::string tag = get_string_value_from_Json_object(value, "tag");
                std::string val = get_string_value_from_Json_object(value, "value");
                ret = run_search_comparison(schema_id, op, tag, val);
            }
            else if (first_name == RECORD_ID_LIST)
            {
                auto& sub_value = value.MemberBegin()->value;

                ret = get_record_id_list_from_array(sub_value);
            }
        }
        else
        {
            rapidjson::StringBuffer buffer;
            rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
            value.Accept(writer);
            std::cerr << "Invalid SearchExpression: " << buffer.GetString() << std::endl;
        }
        return ret;
    }

    int create_subscription(const std::string& subscription_id, const std::string& subscription_body)
    {
        NotificationSubscription subscription;
        staticjson::ParseStatus result;
        if (staticjson::from_json_string(subscription_body.c_str(), &subscription, &result))
        {
            for (size_t i = 0; i < subscription.subFilter.monitoredResourceUris.size(); i++)
            {
                subscription.subFilter.monitoredResourceUris[i] = get_path(subscription.subFilter.monitoredResourceUris[i]);
                // TODO: If the monitoredResourceUris is present, only "UPDATED" and "DELETED" are allowed values
            }
            auto expiry = iso8601_timestamp_to_seconds_since_epoch(subscription.expiry);
            auto seconds_since_epoch = std::chrono::duration_cast<std::chrono::seconds>
                                       (std::chrono::system_clock::now().time_since_epoch()).count();
            if (expiry <= seconds_since_epoch)
            {
                return SUBSCRIPTION_EXPIRY_INVALID;
            }
            if (expiry - subscription.expiryNotification <= seconds_since_epoch)
            {
                subscription.expiryNotification = 0;
            }
            uint16_t sum = get_u16_sum(subscription_id);
            auto row = sum >> 8;
            auto col = sum & 0xFF;
            std::unique_lock<std::shared_timed_mutex> subscription_id_to_subscriptions_write_lock(
                subscription_id_to_subscription_mutex[row][col]);
            subscription_id_to_subscription[row][col][subscription_id] = subscription;

            std::unique_lock<std::shared_timed_mutex> subscription_expiry_to_subscription_id_write_lock(
                subscription_expiry_to_subscription_id_mutex);
            subscription_expiry_to_subscription_id[expiry].insert(subscription_id);

            if (subscription.expiryNotification)
            {
                std::unique_lock<std::shared_timed_mutex> subscription_expiryNotification_to_subscription_id_write_lock(
                    subscription_expiryNotification_to_subscription_id_mutex);
                subscription_expiryNotification_to_subscription_id[seconds_since_epoch - subscription.expiryNotification].insert(
                    subscription_id);
            }

            for (auto& monUri : subscription.subFilter.monitoredResourceUris)
            {
                uint16_t sum = get_u16_sum(monUri);
                auto row = sum >> 8;
                auto col = sum & 0xFF;
                std::unique_lock<std::shared_timed_mutex> monitored_resource_uri_to_subscription_id_write_lock(
                    monitored_resource_uri_to_subscription_id_mutex[row][col]);
                monitored_resource_uri_to_subscription_id[row][col][monUri].insert(subscription_id);
            }
            return OPERATION_SUCCESSFUL;
        }
        return DECODE_SUBCRIPTION_FAILURE;
    }

    int update_subscription(const std::string& subscription_id, const std::string& subscription_body)
    {
        delete_subscription(subscription_id);
        return create_subscription(subscription_id, subscription_body);
    }

    template<typename M, typename K, typename V>
    void remove_from_map(M& m, const K& k, const V& value)
    {
        auto i = m.find(k);
        if (i != m.end())
        {
            i->second.erase(value);
            if (i->second.empty())
            {
                m.erase(i);
            }
        }
    }

    bool delete_subscription(const std::string& subscription_id)
    {
        uint16_t sum = get_u16_sum(subscription_id);
        auto row = sum >> 8;
        auto col = sum & 0xFF;
        std::unique_lock<std::shared_timed_mutex> subscription_id_to_subscription_write_lock(
            subscription_id_to_subscription_mutex[row][col]);
        auto& id_to_s = subscription_id_to_subscription[row][col];
        auto iter = id_to_s.find(subscription_id);
        if (iter != id_to_s.end())
        {
            auto& subscription = iter->second;
            auto expiry = iso8601_timestamp_to_seconds_since_epoch(subscription.expiry);
            auto expiryNotification = expiry - subscription.expiryNotification;

            std::unique_lock<std::shared_timed_mutex> subscription_expiry_to_subscription_id_write_lock(
                subscription_expiry_to_subscription_id_mutex);
            remove_from_map(subscription_expiry_to_subscription_id, expiry, subscription_id);

            if (subscription.expiryNotification)
            {
                std::unique_lock<std::shared_timed_mutex> subscription_expiryNotification_to_subscription_id_write_lock(
                    subscription_expiryNotification_to_subscription_id_mutex);
                remove_from_map(subscription_expiryNotification_to_subscription_id, expiry - subscription.expiryNotification,
                                subscription_id);
            }

            for (auto& monUri : subscription.subFilter.monitoredResourceUris)
            {
                uint16_t sum = get_u16_sum(monUri);
                auto row = sum >> 8;
                auto col = sum & 0xFF;
                std::unique_lock<std::shared_timed_mutex> monitored_resource_uri_to_subscription_id_write_lock(
                    monitored_resource_uri_to_subscription_id_mutex[row][col]);
                remove_from_map(monitored_resource_uri_to_subscription_id[row][col], monUri, subscription_id);
            }

            id_to_s.erase(iter);
            return true;
        }
        return false;
    }

    std::string get_subscription(const std::string& subscription_id)
    {
        uint16_t sum = get_u16_sum(subscription_id);
        auto row = sum >> 8;
        auto col = sum & 0xFF;
        std::shared_lock<std::shared_timed_mutex> subscription_id_to_subscription_read_lock(
            subscription_id_to_subscription_mutex[row][col]);
        auto& id_to_s = subscription_id_to_subscription[row][col];
        auto iter = id_to_s.find(subscription_id);
        if (iter != id_to_s.end())
        {
            return staticjson::to_json_string(iter->second);
        }
        return "";
    }

    auto get_decoded_subscription(const std::string& subscription_id)
    {
        uint16_t sum = get_u16_sum(subscription_id);
        auto row = sum >> 8;
        auto col = sum & 0xFF;
        std::shared_lock<std::shared_timed_mutex> subscription_id_to_subscription_read_lock(
            subscription_id_to_subscription_mutex[row][col]);
        auto& id_to_s = subscription_id_to_subscription[row][col];
        auto iter = id_to_s.find(subscription_id);
        if (iter != id_to_s.end())
        {
            return (iter->second);
        }
        NotificationSubscription subscription;
        return subscription;
    }

    std::set<std::string> get_expired_subscription_ids()
    {
        std::shared_lock<std::shared_timed_mutex> read_lock(subscription_expiry_to_subscription_id_mutex);
        std::set<std::string> s;
        auto seconds_since_epoch = std::chrono::duration_cast<std::chrono::seconds>
                                   (std::chrono::system_clock::now().time_since_epoch()).count();
        auto end = subscription_expiry_to_subscription_id.upper_bound(seconds_since_epoch);
        for (auto iter = subscription_expiry_to_subscription_id.begin(); iter != end; iter++)
        {
            s.insert(iter->second.begin(), iter->second.end());
        }
        return s;
    }

    std::set<std::string> get_subscription_ids_about_to_expire()
    {
        std::shared_lock<std::shared_timed_mutex> read_lock(subscription_expiryNotification_to_subscription_id_mutex);
        std::set<std::string> s;
        auto seconds_since_epoch = std::chrono::duration_cast<std::chrono::seconds>
                                   (std::chrono::system_clock::now().time_since_epoch()).count();
        auto end = subscription_expiryNotification_to_subscription_id.upper_bound(seconds_since_epoch);
        for (auto iter = subscription_expiryNotification_to_subscription_id.begin(); iter != end; iter++)
        {
            s.insert(iter->second.begin(), iter->second.end());
        }
        return s;
    }

    std::set<std::string> get_subscription_ids_to_notify(const std::string& record_id, const std::string& block_id,
                                                         const std::string& operation)
    {
        auto uri = RECORDS;
        if (record_id.size())
        {
            uri.append(PATH_DELIMETER).append(record_id);
            if (block_id.size())
            {
                uri.append(PATH_DELIMETER).append(block_id);
            }
        }

        auto subscription_ids_to_notify = [this](const std::string & path)
        {
            std::set<std::string> s;
            uint16_t sum = get_u16_sum(path);
            auto row = sum >> 8;
            auto col = sum & 0xFF;
            std::shared_lock<std::shared_timed_mutex> read_lock(monitored_resource_uri_to_subscription_id_mutex[row][col]);
            auto iter = monitored_resource_uri_to_subscription_id[row][col].find(path);
            if (iter != monitored_resource_uri_to_subscription_id[row][col].end())
            {
                return iter->second;
            }
            return s;
        };

        std::set<std::string> ret;
        auto v = subscription_ids_to_notify(RECORDS);
        ret.insert(v.begin(), v.end());

        if (uri.size() > RECORDS.size())
        {
            auto v = subscription_ids_to_notify(RECORDS);
            for (auto& s : v)
            {
                ret.insert(s);
            }
        }
        return ret;
    }

    void send_notify(const std::string& record_id, const std::string& block_id, const std::string& operation)
    {
        std::string recordNotificationBody;
        const std::map<std::string, std::string, ci_less> additionalHeaders;

        auto v = get_subscription_ids_to_notify(record_id, block_id, operation);
        for (auto& s : v)
        {
            auto subscription = get_decoded_subscription(s);
            if (std::find(subscription.subFilter.operations.begin(), subscription.subFilter.operations.end(),
                          operation) != subscription.subFilter.operations.end())
            {
                if (recordNotificationBody.empty())
                {
                    NotificationDescription notificationDescription;
                    notificationDescription.operationType = operation;
                    notificationDescription.recordRef.reserve(udsf_base_uri.size() +
                                                              PATH_DELIMETER.size() +
                                                              realm_id.size() +
                                                              PATH_DELIMETER.size() +
                                                              storage_id.size() +
                                                              PATH_DELIMETER.size() +
                                                              record_id.size());
                    notificationDescription.recordRef += udsf_base_uri;
                    notificationDescription.recordRef += PATH_DELIMETER;
                    notificationDescription.recordRef += realm_id;
                    notificationDescription.recordRef += PATH_DELIMETER;
                    notificationDescription.recordRef += storage_id;
                    notificationDescription.recordRef += PATH_DELIMETER;
                    notificationDescription.recordRef += record_id;
                    auto description = staticjson::to_json_string(notificationDescription);
                    recordNotificationBody = get_record(record_id, description);
                }
                if (recordNotificationBody.size())
                {
                    send_http2_request(METHOD_POST, subscription.callbackReference, additionalHeaders, recordNotificationBody);
                }
            }
        }
    }

};

class Realm
{
public:
    std::unordered_map<std::string, Storage> storages;
    std::shared_timed_mutex storages_mutexes;
    std::string realm_id;
    Storage& get_storage(const std::string& storage_id)
    {
        std::shared_lock<std::shared_timed_mutex> read_lock(storages_mutexes);
        auto iter = storages.find(storage_id);
        if (iter == storages.end())
        {
            read_lock.unlock();
            std::unique_lock<std::shared_timed_mutex> write_lock(storages_mutexes);
            auto& ret = storages[storage_id];
            ret.storage_id = storage_id;
            ret.realm_id = realm_id;
            ret.init_default_schema_with_empty_schema_id();
            return ret;
        }
        else
        {
            return iter->second;
        }
    }
};

static std::unordered_map<std::string, Realm> realms;
static std::shared_timed_mutex realm_mutex;

inline Realm& get_realm(const std::string& realm_id)
{
    std::shared_lock<std::shared_timed_mutex> read_lock(realm_mutex);
    auto iter = realms.find(realm_id);
    if (iter == realms.end())
    {
        read_lock.unlock();
        std::unique_lock<std::shared_timed_mutex> write_lock(realm_mutex);
        auto& ret = realms[realm_id];
        ret.realm_id = realm_id;
        return ret;
    }
    return iter->second;
}

}


#endif

