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
#include <stack>
#include "staticjson/document.hpp"
#include "staticjson/staticjson.hpp"
#include "rapidjson/pointer.h"
#include "rapidjson/schema.h"
#include "rapidjson/prettywriter.h"
#include "multipart_parser.h"
#include "nlohmann/json.hpp"
#include "rapidjson/writer.h"
#include "rapidjson/stringbuffer.h"
#include "common_types.h"
#include "h2load_utils.h"
#include "udsf_util.h"

extern bool debug_mode;
extern bool schema_loose_check;
extern std::vector<boost::asio::io_service* > g_io_services;
extern std::vector<boost::asio::io_service::strand> g_strands;
extern thread_local size_t g_current_thread_id;
extern size_t number_of_worker_thread;

const std::string CONTENT_ID = "Content-Id";
const std::string CONTENT_TYPE = "Content-Type";
const std::string CONTENT_LENGTH = "content-length";
const std::string CONTENT_LOCATION = "content-location";
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

const std::set<std::string> ALLOWED_OPERATIONS = {RECORD_OPERATION_CREATED, RECORD_OPERATION_UPDATED, RECORD_OPERATION_DELETED};

const std::string LIMIT_RANGE = "limit-range";
const std::string MAX_PAYLOAD_SIZE = "max-payload-size";
const std::string RETRIEVE_RECORDS = "retrieve-records";
const std::string COUNT_INDICATOR = "count-indicator";
const std::string CLIENT_ID = "client-id";
const std::string SCHEMA_ID = "schemaId";
const std::string FILTER = "filter";


const int OPERATION_SUCCESSFUL = 0;
const int DECODE_MULTIPART_FAILURE = -1;
const int CONTENT_ID_NOT_PRESDENT = -2;
const int RECORD_META_DECODE_FAILURE = -3;
const int RESOURCE_DOES_NOT_EXIST = -4;
const int TTL_EXPIRED = -5;
const int DECODE_FAILURE = -6;
const int SUBSCRIPTION_EXPIRY_INVALID = -7;
const int INVALID_OPERATIONS_IN_SUBSCRIPTION = -8;


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

const std::string RESOURCE_BLOCKS = "blocks";

const std::string RESOURCE_META = "meta";

const std::string RESOURCE_TIMERS = "timers";

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
    if (debug_mode)
    {
        std::cerr << __func__ << ":" << s << std::endl << std::flush;
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
    const std::string sample_timestamp_with_tz = "2046-03-31T14:10:00+08:00";
    const std::string sample_timestamp_without_tz = "2046-03-31T14:10:00";
    const std::string sample_timestamp_z = "2046-03-31T14:10:00Z";

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

    if (iso8601_timestamp.size() > sample_timestamp_without_tz.size())
    {
        std::string incoming_tz;
        if (iso8601_timestamp.size() == sample_timestamp_z.size() &&
            (iso8601_timestamp[iso8601_timestamp.size() - 1] == 'z' || iso8601_timestamp[iso8601_timestamp.size() - 1] == 'Z'))
        {
            incoming_tz = "+00:00";
        }
        else if (iso8601_timestamp.size() == sample_timestamp_with_tz.size())
        {
            incoming_tz = iso8601_timestamp.substr(iso8601_timestamp.size() - sample_tz_offset.size());
        }

        if (incoming_tz.size() == sample_tz_offset.size())
        {
            int incoming_tz_hours, incoming_tz_minutes;
            std::sscanf(incoming_tz.substr(1).c_str(), "%d:%d", &incoming_tz_hours, &incoming_tz_minutes);
            auto incoming_tz_offset = (std::chrono::hours(incoming_tz_hours) + std::chrono::minutes(incoming_tz_minutes));
            if (incoming_tz[0] == '-')
            {
                incoming_tz_offset = -incoming_tz_offset;
            }
            auto tz_difference = local_tz_offset - incoming_tz_offset;
            duration = duration + tz_difference;
        }
    }

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
        h->add_property(CONTENT_ID, &this->content_id);
        h->add_property(CONTENT_TYPE, &this->content_type);
        h->add_property("content", &this->content);
        //h->add_property("additional-headers", &this->headers);
    }

    std::string produce_multipart_body()
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
    std::string record_id;
    std::unique_ptr<std::shared_timed_mutex> blocks_mutex;
    std::unique_ptr<std::shared_timed_mutex> meta_mutex;

    explicit Record(const std::string& id):
        record_id(id),
        blocks_mutex(std::make_unique<std::shared_timed_mutex>()),
        meta_mutex(std::make_unique<std::shared_timed_mutex>())
    {
    }

    void staticjson_init(staticjson::ObjectHandler* h)
    {
        h->add_property("blocks", &this->blocks, staticjson::Flags::Optional);
        h->add_property("meta", &this->meta);
    }

    bool create_or_update_block(const std::string& id, const std::string& type, const std::string& blockData,
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

    std::string produce_json_body(bool meta_only)
    {
        if (meta_only)
        {
            std::shared_lock<std::shared_timed_mutex> meta_guard(*meta_mutex);
            return staticjson::to_json_string(meta);
        }

        else
        {
            std::shared_lock<std::shared_timed_mutex> blocks_guard(*blocks_mutex);
            std::shared_lock<std::shared_timed_mutex> meta_guard(*meta_mutex);
            return staticjson::to_json_string(*this);
        }
    }

    std::string produce_multipart_body(bool include_meta, const std::string& notificationDescription)
    {
        std::string ret;
        size_t final_size = 0;
        std::shared_lock<std::shared_timed_mutex> blocks_guard(*blocks_mutex);
        std::shared_lock<std::shared_timed_mutex> meta_guard(*meta_mutex);
        auto metaString = staticjson::to_json_string(meta);
        std::vector<std::string> block_body_parts;
        for (auto& b : blocks)
        {
            block_body_parts.emplace_back(b.produce_multipart_body());
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
        if (include_meta)
        {
            final_size += VERY_SPECIAL_BOUNARY_WITH_LEADING_TWO_DASHES.size() + CRLF.size();
            final_size += CONTENT_ID.size() + COLON.size() + META_CONTENT_ID.size() + CRLF.size();
            final_size += CONTENT_TYPE.size() + COLON.size() + JSON_CONTENT.size() + CRLF.size();
            final_size += CRLF.size();
            final_size += metaString.size() + CRLF.size();
        }

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

        if (include_meta)
        {
            ret.append(VERY_SPECIAL_BOUNARY_WITH_LEADING_TWO_DASHES).append(CRLF);
            ret.append(CONTENT_ID).append(COLON).append(META_CONTENT_ID).append(CRLF);
            ret.append(CONTENT_TYPE).append(COLON).append(JSON_CONTENT).append(CRLF);
            ret.append(CRLF);
            ret.append(metaString).append(CRLF);
        }

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

    RecordMeta get_meta_object()
    {
        std::shared_lock<std::shared_timed_mutex> guard(*meta_mutex);
        return meta;
    }

    void set_meta_object(RecordMeta& new_meta)
    {
        std::unique_lock<std::shared_timed_mutex> guard(*meta_mutex);
        meta = std::move(new_meta);
    }

    static bool validate_meta_with_schema(const RecordMeta& new_meta, const MetaSchema& schema_object)
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
    std::unique_ptr<std::shared_timed_mutex> value_to_resource_id_map_mutex;
    std::map<std::string, std::set<std::string>> value_to_resource_id_map;
    size_t count = 0;
    explicit Tags_Value_Db_Of_One_Tag_Name():
        value_to_resource_id_map_mutex(std::make_unique<std::shared_timed_mutex>())
    {
    }
};

class Tags_Values_Db_With_Tags_Name_As_Key
{
public:
    std::unique_ptr<std::shared_timed_mutex> name_to_value_db_map_mutex;
    std::map<std::string, Tags_Value_Db_Of_One_Tag_Name> name_to_value_db_map;
    explicit Tags_Values_Db_With_Tags_Name_As_Key():
        name_to_value_db_map_mutex(std::make_unique<std::shared_timed_mutex>())
    {
    }
};

using Record_Tags_Db = std::map<std::string, Tags_Values_Db_With_Tags_Name_As_Key>;

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
    bool operator== (const ClientId& rh)
    {
        return (nfId == rh.nfId && nfSetId == rh.nfSetId);
    }
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
    int64_t expiryNotification = -1;
    SubscriptionFilter subFilter;
    std::string supportedFeatures;
    unsigned expiryNotificationFlag = staticjson::Flags::Optional | staticjson::Flags::IgnoreWrite;
    void staticjson_init(staticjson::ObjectHandler* h)
    {
        h->add_property("clientId", &this->clientId);
        h->add_property("callbackReference", &this->callbackReference);
        h->add_property("expiryCallbackReference", &this->expiryCallbackReference, staticjson::Flags::Optional);
        h->add_property("expiry", &this->expiry, staticjson::Flags::Optional);
        h->add_property("expiryNotification", &this->expiryNotification, expiryNotificationFlag);
        h->add_property("subFilter", &this->subFilter, staticjson::Flags::Optional);
        h->add_property("supportedFeatures", &this->supportedFeatures, staticjson::Flags::Optional);
    }
};

class NotificationInfo
{
public:
    std::vector<std::string> expiredSubscriptions;
    void staticjson_init(staticjson::ObjectHandler* h)
    {
        h->add_property("expiredSubscriptions", &this->expiredSubscriptions);
    }
};

class Timer
{
public:
    std::string timerId;
    std::string expires;
    std::map<std::string, std::vector<std::string>> metaTags;
    std::string callbackReference;
    unsigned callbackReferenceFlag = staticjson::Flags::Optional | staticjson::Flags::IgnoreWrite;
    uint64_t deleteAfter = 0;
    void staticjson_init(staticjson::ObjectHandler* h)
    {
        h->add_property("timerId", &this->timerId, staticjson::Flags::Optional);
        h->add_property("expires", &this->expires);
        h->add_property("metaTags", &this->metaTags, staticjson::Flags::Optional);
        h->add_property("callbackReference", &this->callbackReference, callbackReferenceFlag);
        h->add_property("deleteAfter", &this->deleteAfter, staticjson::Flags::Optional);
    }
};

class TimerIdList
{
public:
    std::vector<std::string> timerIds;
    void staticjson_init(staticjson::ObjectHandler* h)
    {
        h->add_property("timerIds", &this->timerIds);
    }
};
class RecordIdList
{
public:
    std::vector<std::string> recordIdList;
    void staticjson_init(staticjson::ObjectHandler* h)
    {
        h->add_property("recordIdList", &this->recordIdList);
    }
};

class Record_Ids_Of_One_Schema
{
public:
    std::unique_ptr<std::shared_timed_mutex> record_ids_mutex;
    std::set<std::string> record_ids;
    explicit Record_Ids_Of_One_Schema():
        record_ids_mutex(std::make_unique<std::shared_timed_mutex>())
    {
    }
};
class Storage
{
public:
    std::string realm_id;
    std::string storage_id;
    std::unordered_map<std::string, Record> records[ONE_DIMENSION_SIZE * ONE_DIMENSION_SIZE];
    std::shared_timed_mutex records_mutexes[ONE_DIMENSION_SIZE * ONE_DIMENSION_SIZE];

    std::unordered_map<std::string, MetaSchema> schemas[ONE_DIMENSION_SIZE * ONE_DIMENSION_SIZE];
    std::shared_timed_mutex schemas_mutexes[ONE_DIMENSION_SIZE * ONE_DIMENSION_SIZE];

    std::map<uint64_t, std::set<std::string>> records_ttl;
    std::shared_timed_mutex record_ttl_mutex;

    std::unordered_map<std::string, NotificationSubscription>
    subscription_id_to_subscription[ONE_DIMENSION_SIZE * ONE_DIMENSION_SIZE];
    std::shared_timed_mutex subscription_id_to_subscription_mutex[ONE_DIMENSION_SIZE * ONE_DIMENSION_SIZE];
    std::unordered_map<std::string, std::set<std::string>>
                                                        monitored_resource_uri_to_subscription_id[ONE_DIMENSION_SIZE * ONE_DIMENSION_SIZE];
    std::shared_timed_mutex monitored_resource_uri_to_subscription_id_mutex[ONE_DIMENSION_SIZE * ONE_DIMENSION_SIZE];
    std::map<uint64_t, std::set<std::string>> subscription_expiry_to_subscription_id;
    std::shared_timed_mutex subscription_expiry_to_subscription_id_mutex;
    std::map<uint64_t, std::set<std::string>> subscription_expiryNotification_to_subscription_id;
    std::shared_timed_mutex subscription_expiryNotification_to_subscription_id_mutex;

    std::unordered_map<std::string, Timer> timers[ONE_DIMENSION_SIZE * ONE_DIMENSION_SIZE];
    std::shared_timed_mutex timers_mutexes[ONE_DIMENSION_SIZE * ONE_DIMENSION_SIZE];

    std::map<uint64_t, std::set<std::string>> timer_expires_to_timer_id;
    std::shared_timed_mutex timer_expires_to_timer_id_mutex;
    std::map<uint64_t, std::set<std::string>> timer_deleteAfter_to_timer_id;
    std::shared_timed_mutex timer_deleteAfter_to_timer_id_mutex;

    std::vector<std::map<std::string, Record_Ids_Of_One_Schema>> schema_id_to_record_ids;
    std::vector<std::shared_timed_mutex> schema_id_to_record_ids_mutex;
    std::vector<Record_Tags_Db> record_tags_db;
    std::vector<std::shared_timed_mutex> record_tags_db_main_mutex;

    std::vector<std::set<std::string>> all_timer_ids;
    std::vector<std::shared_timed_mutex> all_timer_ids_mutex;
    std::vector<Record_Tags_Db> timer_tags_db;
    std::vector<std::shared_timed_mutex> timer_tags_db_main_mutex;

    explicit Storage()
        : schema_id_to_record_ids(number_of_worker_thread),
          schema_id_to_record_ids_mutex(number_of_worker_thread),
          record_tags_db(number_of_worker_thread),
          record_tags_db_main_mutex(number_of_worker_thread),
          all_timer_ids(number_of_worker_thread),
          all_timer_ids_mutex(number_of_worker_thread),
          timer_tags_db(number_of_worker_thread),
          timer_tags_db_main_mutex(number_of_worker_thread)

    {
    }

    void init_default_schema_with_empty_schema_id()
    {
        for (auto thread_id = 0; thread_id < schema_id_to_record_ids.size(); thread_id++)
        {
            auto run_in_worker = [thread_id, this]()
            {
                auto& r = schema_id_to_record_ids[thread_id][""];

                auto& r_tagsDb = record_tags_db[thread_id][""];
            };
            if (thread_id == g_current_thread_id)
            {
                run_in_worker();
            }
            else
            {
                g_strands[thread_id].post(run_in_worker);
            }
        }
    }

    void install_tags_from_schema(const MetaSchema& schema)
    {
        for (size_t thread_id = 0; thread_id < number_of_worker_thread; thread_id++)
        {
            auto run_in_worker = [schema, thread_id, this]()
            {
                Tags_Values_Db_With_Tags_Name_As_Key tags_name_db;
                for (auto& tag : schema.metaTags)
                {
                    auto& tag_value_db = tags_name_db.name_to_value_db_map[tag.tagName];
                }
                std::string schema_id = schema.schemaId;
                record_tags_db[thread_id].emplace(std::move(schema_id), std::move(tags_name_db));
            };
            if (thread_id != g_current_thread_id)
            {
                g_strands[thread_id].post(run_in_worker);
            }
            else
            {
                run_in_worker();
            }
        }
    }

    void print_record_tags_db(Record_Tags_Db& db)
    {
        std::cerr << "realm id: " << realm_id << std::endl << std::flush;
        std::cerr << "storage id: " << storage_id << std::endl << std::flush;
        for (auto& i : db)
        {
            std::cerr << "--schema name: " << i.first << std::endl << std::flush;
            for (auto& tag_name_to_values : i.second.name_to_value_db_map)
            {
                std::cerr << "----tag name: " << tag_name_to_values.first << std::endl << std::flush;
                for (auto& v : tag_name_to_values.second.value_to_resource_id_map)
                {
                    std::cerr << "------tag value: " << v.first << std::endl << std::flush;
                    for (auto& r : v.second)
                    {
                        std::cerr << "--------record id: " << r << std::endl << std::flush;
                    }
                }
            }
        }
    }

    std::set<std::string> get_all_record_ids(size_t thread_id, const std::string& schema_id)
    {
        std::set<std::string> ret;
        if (schema_id.size())
        {
            auto iter = schema_id_to_record_ids[thread_id].find(schema_id);
            if (iter != schema_id_to_record_ids[thread_id].end())
            {
                ret.insert(iter->second.record_ids.begin(), iter->second.record_ids.end());
            }
        }
        else
        {
            auto iter = schema_id_to_record_ids[thread_id].begin();
            while (iter != schema_id_to_record_ids[thread_id].end())
            {
                ret.insert(iter->second.record_ids.begin(), iter->second.record_ids.end());
                iter++;
            }
        }
        return ret;
    }

    void delete_tags_from_schema(const std::string& schema_id)
    {
        for (size_t thread_id = 0; thread_id < number_of_worker_thread; thread_id++)
        {
            auto run_in_worker = [schema_id, thread_id, this]()
            {
                record_tags_db[thread_id].erase(schema_id);
            };
            if (thread_id != g_current_thread_id)
            {
                g_strands[thread_id].post(run_in_worker);
            }
            else
            {
                run_in_worker();
            }
        }
    }

    void insert_value_to_tags_db(const std::string& tag_name, const std::string& tag_value,
                                 const std::string& resource_id,
                                 Tags_Values_Db_With_Tags_Name_As_Key& tags_name_db,
                                 bool create_new_value_db_if_not_exist = false)
    {
        auto tag_name_db_iter = tags_name_db.name_to_value_db_map.find(tag_name);
        if (tag_name_db_iter != tags_name_db.name_to_value_db_map.end())
        {
            auto& tags_value_db = tag_name_db_iter->second;
            auto count_before = tags_value_db.value_to_resource_id_map[tag_value].size();
            tags_value_db.value_to_resource_id_map[tag_value].insert(resource_id);
            auto count_after = tags_value_db.value_to_resource_id_map[tag_value].size();
            tags_value_db.count += (count_after - count_before);
        }
        else if (create_new_value_db_if_not_exist)
        {
            auto& tags_value_db = tags_name_db.name_to_value_db_map[tag_name];
            tags_value_db.value_to_resource_id_map[tag_value].insert(resource_id);
            tags_value_db.count++;
        }
    }

    void insert_record_tag_value(const std::string& schema_id, const std::string& tag_name, const std::string& tag_value,
                                  const std::string& record_id, size_t thread_id)
    {
        if (debug_mode)
        {
            std::cerr << __func__ << std::endl <<::std::flush;
            std::cerr << "schema_id: " << schema_id << std::endl <<::std::flush;
            std::cerr << "tag_name: " << tag_name << std::endl <<::std::flush;
            std::cerr << "tag_value: " << tag_value << std::endl <<::std::flush;
            std::cerr << "record_id: " << record_id << std::endl <<::std::flush;
        }
        auto tags_db_iter = record_tags_db[thread_id].find(schema_id);
        if (tags_db_iter != record_tags_db[thread_id].end())
        {
            auto& tags_name_db = tags_db_iter->second;
            insert_value_to_tags_db(tag_name, tag_value, record_id, tags_name_db, schema_loose_check);
        }
        else
        {
            if (schema_loose_check)
            {
                auto& tags_name_db = record_tags_db[thread_id][schema_id];
                insert_value_to_tags_db(tag_name, tag_value, record_id, tags_name_db, true);
            }
        }
    }

    void remove_value_from_tags_db(const std::string& tag_name, const std::string& tag_value,
                                   const std::string& resource_id,
                                   Tags_Values_Db_With_Tags_Name_As_Key& tags_name_db
                                  )
    {
        auto tag_name_db_iter = tags_name_db.name_to_value_db_map.find(tag_name);
        if (tag_name_db_iter != tags_name_db.name_to_value_db_map.end())
        {
            auto& tags_value_db = tag_name_db_iter->second;
            auto iter = tags_value_db.value_to_resource_id_map.find(tag_value);
            if (iter != tags_value_db.value_to_resource_id_map.end())
            {
                auto count_before = iter->second.size();
                iter->second.erase(resource_id);
                auto count_after = iter->second.size();
                if (count_before > count_after)
                {
                    tags_value_db.count -= (count_before - count_after);
                }
                if (iter->second.empty())
                {
                    tags_value_db.value_to_resource_id_map.erase(iter);
                }
            }
        }
    }

    void remove_record_tag_value(const std::string& schema_id, const std::string& tag_name, const std::string& tag_value,
                                  const std::string& record_id, size_t thread_id)
    {
        auto tags_db_iter = record_tags_db[thread_id].find(schema_id);
        if (tags_db_iter != record_tags_db[thread_id].end())
        {
            auto& tags_name_db = tags_db_iter->second;
            remove_value_from_tags_db(tag_name, tag_value, record_id, tags_name_db);
        }
    }

    std::set<std::string> run_search_comparison(const std::string& schema_id, const std::string& op,
                                                const std::string& tag_name, const std::string& tag_value,
                                                std::shared_timed_mutex& mutex, Record_Tags_Db& tags_db, size_t& count, bool count_indicator = false)
    {
        if (debug_mode)
        {
            std::cerr << __func__ << ":" << std::endl << std::flush;
            std::cerr << "schema_id: " << schema_id << std::endl << std::flush;
            std::cerr << "op: " << op << std::endl << std::flush;
            std::cerr << "tag_name: " << tag_name << std::endl << std::flush;
            std::cerr << "tag_value: " << tag_value << std::endl << std::flush;
            for (size_t i = 0; i < record_tags_db.size(); i++)
            {
                print_record_tags_db(record_tags_db[i]);
            }
        }
        std::set<std::string> s;
        count = 0;
        if (op == "NEQ")
        {
            size_t gt_count = 0;
            s = run_search_comparison(schema_id, "GT", tag_name, tag_value, mutex, tags_db, gt_count, count_indicator);
            size_t lt_count = 0;
            auto s_lt = run_search_comparison(schema_id, "LT", tag_name, tag_value, mutex, tags_db, lt_count, count_indicator);
            s.insert(s_lt.begin(), s_lt.end());
            count = gt_count + lt_count;
            return s;
        }
        std::vector<Record_Tags_Db::iterator> tag_db_iterators_to_go_through;
        if (debug_mode)
        {
            for (auto& tags_db_iter : tags_db)
            {
                std::cerr << "schema_id in db: " << tags_db_iter.first << std::endl << std::flush;
            }
        }

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

            if (debug_mode)
            {
                for (auto& name_iter : tags_name_db.name_to_value_db_map)
                {
                    std::cerr << "tag name in db: " << name_iter.first << std::endl << std::flush;
                }
            }

            auto tag_name_db_iter = tags_name_db.name_to_value_db_map.find(tag_name);
            if (tag_name_db_iter != tags_name_db.name_to_value_db_map.end())
            {
                auto& tags_value_db = tag_name_db_iter->second;
                if (debug_mode)
                {
                    std::cerr << "value db content: " << schema_id << std::endl << std::flush;
                    auto iter = tags_value_db.value_to_resource_id_map.begin();
                    while (iter != tags_value_db.value_to_resource_id_map.end())
                    {
                        std::cerr << "tag_value: " << iter->first << std::endl << std::flush;

                        for (auto& s : iter->second)
                        {
                            std::cerr << "record/timer Id: " << s << std::endl << std::flush;
                        }
                        iter++;
                    }
                }
                if (count_indicator && tag_value.empty() && ((op == "GT") || (op == "GTE")))
                {
                    count += tag_name_db_iter->second.count;
                    continue;
                }

                auto iter_start = tags_value_db.value_to_resource_id_map.begin();
                auto iter_end = iter_start;
                if (op == "EQ")
                {
                    iter_start = tags_value_db.value_to_resource_id_map.find(tag_value);
                    iter_end = iter_start;
                    if (iter_start != tags_value_db.value_to_resource_id_map.end())
                    {
                        iter_end++;
                    }
                }
                else if (op == "GT")
                {
                    iter_start = tags_value_db.value_to_resource_id_map.upper_bound(tag_value);
                    iter_end = tags_value_db.value_to_resource_id_map.end();
                }
                else if (op == "GTE")
                {
                    iter_start = tags_value_db.value_to_resource_id_map.lower_bound(tag_value);
                    iter_end = tags_value_db.value_to_resource_id_map.end();
                }
                else if (op == "LT")
                {
                    iter_start = tags_value_db.value_to_resource_id_map.begin();
                    iter_end = tags_value_db.value_to_resource_id_map.lower_bound(tag_value);
                }
                else if (op == "LTE")
                {
                    iter_start = tags_value_db.value_to_resource_id_map.begin();
                    iter_end = tags_value_db.value_to_resource_id_map.upper_bound(tag_value);
                }
                else
                {
                    std::cerr << "invalid operation: " << op << std::endl << std::flush;
                }

                auto iter = iter_start;
                while (iter != iter_end)
                {
                    if (debug_mode)
                    {
                        std::cerr << "db value: " << iter->first << std::endl << std::flush;
                        for (auto& s : iter->second)
                        {
                            std::cerr << "record/timer Id with the above db value: " << s << std::endl << std::flush;
                        }
                    }
                    if (count_indicator)
                    {
                        count += iter->second.size();
                    }
                    else
                    {
                        s.insert(iter->second.begin(), iter->second.end());
                    }
                    iter++;
                }
            }
        }
        return s;
    };

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
            ret = Record::validate_meta_with_schema(meta, schema_object);
        }
        else
        {
            if (!schema_loose_check)
            {
                // reject the record if not loose check (means strict check)
                ret = false;
            }
        }
        return ret;
    }

    size_t get_all_record_count(size_t thread_id)
    {
        size_t count = 0;
        for (auto& s : schema_id_to_record_ids[thread_id])
        {
            count += s.second.record_ids.size();
        }
        return count;
    }

    void track_record_id(const std::string& schema_id, const std::string& record_id, size_t thread_id, bool insert = true)
    {
        auto iter = schema_id_to_record_ids[thread_id].find(schema_id);
        auto track_it = [insert, &record_id](decltype(schema_id_to_record_ids[thread_id].begin()->second)&
                                             schema_id_to_record_id)
        {
            if (insert)
            {
                schema_id_to_record_id.record_ids.insert(record_id);
            }
            else
            {
                schema_id_to_record_id.record_ids.erase(record_id);
            }
        };
        if (iter != schema_id_to_record_ids[thread_id].end())
        {
            track_it(iter->second);
        }
        else
        {
            auto& schema_id_to_record_id = schema_id_to_record_ids[thread_id][schema_id];
            track_it(schema_id_to_record_id);
        }
    }

    void track_ttl(std::shared_timed_mutex& mutex, std::map<uint64_t, std::set<std::string>>& ttl_map,
                   uint64_t ttl, const std::string& id, bool insert = true)
    {
        std::unique_lock<std::shared_timed_mutex> write_lock(mutex);
        if (insert)
        {
            auto& s = ttl_map[ttl];
            s.insert(id);
        }
        else
        {
            auto iter = ttl_map.find(ttl);
            if (iter != ttl_map.end())
            {
                iter->second.erase(id);
            }
        }

    }

    std::set<std::string> get_expired_items(std::shared_timed_mutex& mutex,
                                            std::map<uint64_t, std::set<std::string>>& tracking_map)
    {
        std::shared_lock<std::shared_timed_mutex> read_lock(mutex);
        std::set<std::string> s;
        auto seconds_since_epoch = std::chrono::duration_cast<std::chrono::seconds>
                                   (std::chrono::system_clock::now().time_since_epoch()).count();
        auto end = tracking_map.upper_bound(seconds_since_epoch);
        for (auto iter = tracking_map.begin(); iter != end; iter++)
        {
            s.insert(iter->second.begin(), iter->second.end());
        }
        return s;
    }

    void track_record_ttl(uint64_t ttl, const std::string& record_id, bool insert = true)
    {
        track_ttl(record_ttl_mutex, records_ttl, ttl, record_id, insert);
    }

    std::set<std::string> get_ttl_expired_records()
    {
        return get_expired_items(record_ttl_mutex, records_ttl);
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
    int create_or_update_record(const std::string& record_id, const std::string& boundary,
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
            Record record(record_id);
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
                record.create_or_update_block(content_id, content_type, parts[i].second, parts[i].first, dummy);
            }
            uint16_t sum = get_u16_sum(record_id);
            std::unique_lock<std::shared_timed_mutex> record_map_write_guard(records_mutexes[sum]);
            Record* target = nullptr;
            auto iter = records[sum].find(record_id);
            if (iter == records[sum].end())
            {
                records[sum].emplace(std::make_pair(record_id, record_id));
                target = &records[sum].find(record_id)->second;
                update = false;
            }
            else
            {
                target = &iter->second;
                checkout_record(record_id, iter->second.meta);
                update = true;
            }
            checkin_record(record_id, record.meta);
            *target = std::move(record);
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
            ret = get_record_multipart_body(record_id);
            record_exist_before = delete_record_directly(record_id);
        }
        return ret;
    }

    void checkout_record(const std::string& record_id, const RecordMeta& old_meta)
    {
        auto u8 = get_u8_sum(record_id);
        auto thread_id = (u8 % number_of_worker_thread);
        if (debug_mode)
        {
            std::cerr << __func__<<": index of: " << record_id << " is managed by thread id: " << thread_id << std::endl;
        }

        auto run_in_dest_worker = [this, thread_id, record_id, old_meta]()
        {
            if (debug_mode)
            {
                std::cerr << __func__<<": record tags db before delete, thread id: "<<thread_id<< std::endl;
                print_record_tags_db(record_tags_db[thread_id]);
            }
            for (auto& tag : old_meta.tags)
            {
                auto& tag_name = tag.first;
                auto& tag_values = tag.second;
                for (auto& tag_value : tag_values)
                {
                    remove_record_tag_value(old_meta.schemaId, tag_name, tag_value, record_id, thread_id);
                }
            }
            if (debug_mode)
            {
                std::cerr << __func__<<": record tags db before delete, thread id: "<<thread_id<< std::endl;
                print_record_tags_db(record_tags_db[thread_id]);
            }
            track_record_id(old_meta.schemaId, record_id, thread_id, false);
            if (old_meta.ttl.size())
            {
                track_record_ttl(iso8601_timestamp_to_seconds_since_epoch(old_meta.ttl), record_id, false);
            }
        };
        if (thread_id != g_current_thread_id)
        {
            g_strands[thread_id].post(run_in_dest_worker);
        }
        else
        {
            run_in_dest_worker();
        }
    }

    void checkin_record(const std::string& record_id, const RecordMeta& new_meta)
    {
        auto u8 = get_u8_sum(record_id);
        auto thread_id = (u8 % number_of_worker_thread);
        if (debug_mode)
        {
            std::cerr << __func__<<": index of: " << record_id << " is managed by thread id: " << thread_id << std::endl;
        }
        auto run_in_dest_worker = [this, thread_id, record_id, new_meta]()
        {
            if (debug_mode)
            {
                std::cerr << __func__<<": record tags db before insert, thread id: "<<thread_id<< std::endl;
                print_record_tags_db(record_tags_db[thread_id]);
            }
            for (auto& tag : new_meta.tags)
            {
                auto& tag_name = tag.first;
                auto& tag_values = tag.second;
                for (auto& tag_value : tag_values)
                {
                    insert_record_tag_value(new_meta.schemaId, tag_name, tag_value, record_id, thread_id);
                }
            }
            if (debug_mode)
            {
                std::cerr << __func__<<": record tags db after insert, thread id: "<<thread_id<< std::endl;
                print_record_tags_db(record_tags_db[thread_id]);
            }
            track_record_id(new_meta.schemaId, record_id, thread_id);
            if (new_meta.ttl.size())
            {
                track_record_ttl(iso8601_timestamp_to_seconds_since_epoch(new_meta.ttl), record_id);
            }
        };
        if (thread_id != g_current_thread_id)
        {
            g_strands[thread_id].post(run_in_dest_worker);
        }
        else
        {
            run_in_dest_worker();
        }
    }

    bool delete_record_directly(const std::string& record_id)
    {
        uint16_t sum = get_u16_sum(record_id);
        std::unique_lock<std::shared_timed_mutex> write_lock(records_mutexes[sum]);
        auto iter = records[sum].find(record_id);
        if (iter != records[sum].end())
        {
            auto record = std::move(iter->second);
            records[sum].erase(iter);
            checkout_record(record_id, record.meta);
            // checkout_record(record_id, iter->second.meta);
            return true;
        }
        return false;
    }

    std::string get_record_multipart_body(const std::string& record_id, bool include_meta = true,
                                          const std::string& notificationDescription = "")
    {
        uint16_t sum = get_u16_sum(record_id);
        std::shared_lock<std::shared_timed_mutex> guard(records_mutexes[sum]);
        std::string ret;
        auto iter = records[sum].find(record_id);
        if (iter != records[sum].end())
        {
            ret = iter->second.produce_multipart_body(include_meta, notificationDescription);
        }
        return ret;
    }

    std::string get_record_json_body(const std::string& record_id, bool meta_only)
    {
        uint16_t sum = get_u16_sum(record_id);
        std::shared_lock<std::shared_timed_mutex> guard(records_mutexes[sum]);
        std::string ret;
        auto iter = records[sum].find(record_id);
        if (iter != records[sum].end())
        {
            ret = iter->second.produce_json_body(meta_only);
        }
        return ret;
    }

    std::string get_record_meta(const std::string& record_id)
    {
        uint16_t sum = get_u16_sum(record_id);
        std::shared_lock<std::shared_timed_mutex> guard(records_mutexes[sum]);
        std::string ret;
        auto iter = records[sum].find(record_id);
        if (iter != records[sum].end())
        {
            ret = iter->second.get_meta();
        }
        return ret;
    }

    RecordMeta get_record_meta_object(const std::string& record_id)
    {
        uint16_t sum = get_u16_sum(record_id);
        std::shared_lock<std::shared_timed_mutex> guard(records_mutexes[sum]);
        auto iter = records[sum].find(record_id);
        if (iter != records[sum].end())
        {
            return iter->second.get_meta_object();
        }
        return RecordMeta();
    }

    std::string get_schema(const std::string& schema_id, bool& found)
    {
        std::string ret;
        uint16_t sum = get_u16_sum(schema_id);
        std::shared_lock<std::shared_timed_mutex> guard(schemas_mutexes[sum]);
        auto iter = schemas[sum].find(schema_id);
        if (iter != schemas[sum].end())
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
        std::shared_lock<std::shared_timed_mutex> guard(schemas_mutexes[sum]);
        auto iter = schemas[sum].find(schema_id);
        if (iter != schemas[sum].end())
        {
            success = true;
            return iter->second;
        }
        return dummyMetaSchama;
    }

    bool create_or_update_schema(const std::string& schema_id, const std::string& schema, bool& update)
    {
        uint16_t sum = get_u16_sum(schema_id);
        staticjson::ParseStatus result;
        MetaSchema metaSchema;
        if (staticjson::from_json_string(schema.c_str(), &metaSchema, &result))
        {
            install_tags_from_schema(metaSchema);
            std::unique_lock<std::shared_timed_mutex> schemas_mutexes_write_lock(schemas_mutexes[sum]);
            auto& target = schemas[sum][schema_id];
            if (target.schemaId.empty())
            {
                update = false;
            }
            else
            {
                update = true;
            }
            metaSchema.schemaId == schema_id;
            schemas[sum][schema_id] = std::move(metaSchema);
            schemas_mutexes_write_lock.unlock();

            for (size_t thread_id = 0; thread_id < number_of_worker_thread; thread_id++)
            {
                auto run_in_worker = [schema_id, thread_id, this]()
                {
                    if (schema_id_to_record_ids[thread_id].find(schema_id) == schema_id_to_record_ids[thread_id].end())
                    {
                        auto& r = schema_id_to_record_ids[thread_id][schema_id];
                    }
                };
                if (thread_id != g_current_thread_id)
                {
                    g_strands[thread_id].post(run_in_worker);
                }
                else
                {
                    run_in_worker();
                }
            }
            return true;
        }
        return false;
    }

    bool delete_schema(const std::string& schema_id)
    {
        bool ret = false;
        uint16_t sum = get_u16_sum(schema_id);
        std::unique_lock<std::shared_timed_mutex> guard(schemas_mutexes[sum]);
        auto iter = schemas[sum].find(schema_id);
        if (iter != schemas[sum].end())
        {
            ret = true;
            schemas[sum].erase(iter);
        }
        delete_tags_from_schema(schema_id);
        return ret;
    }

    bool update_record_meta(const std::string& record_id, const std::string& meta_patch, bool& record_found)
    {
        uint16_t sum = get_u16_sum(record_id);
        record_found = false;
        std::shared_lock<std::shared_timed_mutex> guard(records_mutexes[sum]);
        auto iter = records[sum].find(record_id);
        if (iter != records[sum].end())
        {
            record_found = true;
            auto schema_id = iter->second.meta.schemaId;
            bool schema_found;
            auto schema_object = get_schema_object(schema_id, schema_found);
            auto old_meta_object = iter->second.get_meta_object();
            auto ret = iter->second.update_meta(meta_patch, schema_object);
            if (ret)
            {
                checkout_record(record_id, old_meta_object);
                auto new_meta_object = iter->second.get_meta_object();
                checkin_record(record_id, new_meta_object);
                send_notify(record_id, "", RECORD_OPERATION_UPDATED);
            }
            return ret;
        }
        return false;
    }

    bool create_or_update_block(const std::string& record_id, const std::string& id, const std::string& type,
                                const std::string& blockData,
                                const std::map<std::string, std::string, ci_less>& headers, bool& update)
    {
        uint16_t sum = get_u16_sum(record_id);
        std::shared_lock<std::shared_timed_mutex> guard(records_mutexes[sum]);
        auto iter = records[sum].find(record_id);
        if (iter != records[sum].end())
        {
            auto ret = iter->second.create_or_update_block(id, type, blockData, headers, update);
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
        std::shared_lock<std::shared_timed_mutex> guard(records_mutexes[sum]);
        auto iter = records[sum].find(record_id);
        if (iter != records[sum].end())
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
        std::shared_lock<std::shared_timed_mutex> guard(records_mutexes[sum]);
        auto iter = records[sum].find(record_id);
        if (iter != records[sum].end())
        {
            return iter->second.get_block_object(blockId);
        }
        return Block();
    }

    void schema_violation_handling(rapidjson::Value& value)
    {
        rapidjson::StringBuffer buffer;
        rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
        value.Accept(writer);
        std::cerr << "schema violation: " << buffer.GetString() << std::endl;
    }

    std::set<std::string> get_record_id_list_from_array(rapidjson::Value& value)
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
    }

    std::set<std::string> run_search_comparison_or_record_id_list(const std::string& schema_id,
                                                                   rapidjson::Value& value, size_t thread_id, bool timer_operation = false)
    {
        std::set<std::string> ret;
        size_t count;
        if (value.HasMember(OPERATION.c_str()))
        {
            std::string op = get_string_value_from_Json_object(value, OPERATION);
            std::string tag = get_string_value_from_Json_object(value, "tag");
            std::string val = get_string_value_from_Json_object(value, "value");
            if (!timer_operation)
            {
                ret = run_search_comparison(schema_id, op, tag, val, record_tags_db_main_mutex[thread_id],
                                            record_tags_db[thread_id], count, false);
            }
            else
            {
                ret = run_search_comparison(schema_id, op, tag, val, timer_tags_db_main_mutex[thread_id], timer_tags_db[thread_id], count, false);
            }
        }
        else if (value.HasMember(RECORD_ID_LIST.c_str()))
        {
            auto& sub_value = value[RECORD_ID_LIST.c_str()];

            ret = get_record_id_list_from_array(sub_value);
        }
        return ret;
    }

    std::set<std::string> run_search_expression_non_recursive_opt(rapidjson::Document& doc,
                                                                   size_t thread_id, bool timer_operation = false)
    {
        if (debug_mode)
        {
            rapidjson::StringBuffer buffer;
            rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
            doc.Accept(writer);
            std::cerr << "search expression: " << buffer.GetString() << std::endl << std::flush;
        }
        struct Search_Expression_In_Stack
        {
            Search_Expression_In_Stack* parent;
            rapidjson::Value* value;
            std::vector<std::set<std::string>> operands;
            Search_Expression_In_Stack(Search_Expression_In_Stack* p, rapidjson::Value* s)
                : parent(p), value(s)
            {
            }
        };
        std::set<std::string> result;
        std::stack<Search_Expression_In_Stack> search_conditions;
        std::stack<Search_Expression_In_Stack> values;
        values.push(Search_Expression_In_Stack(nullptr, &doc));
        while (values.size())
        {
            auto v = values.top();
            values.pop();
            if (v.value->HasMember(CONDITION.c_str()) && v.value->HasMember(UNITS.c_str()))
            {
                auto& units = (*v.value)[UNITS.c_str()];
                if (!units.IsArray())
                {
                    schema_violation_handling(*v.value);
                    continue;
                }
                search_conditions.push(v);
                for (size_t i = 0; i < units.Size(); i++)
                {
                    auto schema_id = get_string_value_from_Json_object(units[i], SCHEMA_ID);
                    if (units[i].HasMember(CONDITION.c_str()))
                    {
                        values.push(Search_Expression_In_Stack(&(search_conditions.top()), &units[i]));
                    }
                    else
                    {
                        search_conditions.top().operands.emplace_back(run_search_comparison_or_record_id_list(schema_id, units[i],
                                                                                                               thread_id, timer_operation));
                    }
                }

            }
        }

        while (search_conditions.size())
        {
            auto search_cond = search_conditions.top();
            search_conditions.pop();
            auto& value = *(search_cond.value);
            auto& operands = search_cond.operands;
            if (debug_mode)
            {
                std::cerr << "search condition begin: " << std::endl << std::flush;
                std::cerr << "--- search condition: " << get_string_value_from_Json_object(value, CONDITION) << std::endl << std::flush;
                for (auto& operand : operands)
                {
                    std::cerr << "--- unit begin:" << std::endl << std::flush;
                    for (auto& s : operand)
                    {
                        std::cerr << "------record:" << s << std::endl << std::flush;
                    }
                    std::cerr << "--- unit end" << std::endl << std::flush;
                }
                std::cerr << "search condition end" << std::endl << std::flush;
            }
            auto& units = value[UNITS.c_str()];
            if (operands.size() < units.Size())
            {
                std::cerr << " nested search condtion is not fully resolved" << std::endl << std::flush;
                schema_violation_handling(value);
                return {};
            }
            auto schema_id = get_string_value_from_Json_object(value, SCHEMA_ID);
            auto condition_operator = get_string_value_from_Json_object(value, CONDITION);
            if (condition_operator == CONDITION_OP_AND)
            {
                result = run_and_operator(operands);
            }
            else if (condition_operator == CONDITION_OP_OR)
            {
                result = run_or_operator(operands);
            }
            else if (condition_operator == CONDITION_OP_NOT)
            {
                auto p = &search_cond;
                auto cond_op = condition_operator;
                std::set<std::string> not_op_source;
                while (cond_op != CONDITION_OP_AND && p->parent)
                {
                    p = p->parent;
                    cond_op = get_string_value_from_Json_object(*p->value, CONDITION);
                }
                if (cond_op == CONDITION_OP_AND && p && p->operands.size())
                {
                    not_op_source = p->operands[0];
                }
                result = run_not_operator(not_op_source.size() ? not_op_source : (timer_operation ? get_all_timer_ids(thread_id) :
                                                                                  get_all_record_ids(thread_id, schema_id)),
                                          run_or_operator(operands));
            }
            if (search_cond.parent)
            {
                search_cond.parent->operands.emplace_back(std::move(result));
            }
        }
        return result;
    }

    int create_subscription(const std::string& subscription_id, NotificationSubscription subscription)
    {
        bool clear_monitoredResourceUris = false;
        for (size_t i = 0; i < subscription.subFilter.monitoredResourceUris.size(); i++)
        {
            subscription.subFilter.monitoredResourceUris[i] = get_path(subscription.subFilter.monitoredResourceUris[i]);
        }

        for (size_t i = 0; i < subscription.subFilter.operations.size(); i++)
        {
            if (ALLOWED_OPERATIONS.count(subscription.subFilter.operations[i]) == 0)
            {
                return INVALID_OPERATIONS_IN_SUBSCRIPTION;
            }
        }
        if (subscription.subFilter.monitoredResourceUris.size())
        {
            auto iter = std::find(subscription.subFilter.operations.begin(), subscription.subFilter.operations.end(),
                                  RECORD_OPERATION_CREATED);
            if (iter != subscription.subFilter.operations.end())
            {
                subscription.subFilter.operations.erase(iter);
            }
        }

        if (subscription.subFilter.operations.empty())
        {
            subscription.subFilter.operations.emplace_back(RECORD_OPERATION_UPDATED);
            subscription.subFilter.operations.emplace_back(RECORD_OPERATION_DELETED);
            if (subscription.subFilter.monitoredResourceUris.empty())
            {
                subscription.subFilter.operations.emplace_back(RECORD_OPERATION_CREATED);
            }
        }

        if (subscription.subFilter.monitoredResourceUris.empty())
        {
            subscription.subFilter.monitoredResourceUris.emplace_back(""); // subscribe change of the whole storage
            clear_monitoredResourceUris = true;
        }

        auto expiry = iso8601_timestamp_to_seconds_since_epoch(subscription.expiry);
        auto seconds_since_epoch = std::chrono::duration_cast<std::chrono::seconds>
                                   (std::chrono::system_clock::now().time_since_epoch()).count();
        if (expiry <= seconds_since_epoch)
        {
            return SUBSCRIPTION_EXPIRY_INVALID;
        }
        if (subscription.expiryNotification >= 0)
        {
            subscription.expiryNotificationFlag &= ~staticjson::Flags::IgnoreWrite;
        }
        if ((subscription.expiryNotification >= 0) &&
            (expiry - subscription.expiryNotification <= seconds_since_epoch))
        {
            subscription.expiryNotification = 0;
        }
        uint16_t sum = get_u16_sum(subscription_id);
        std::unique_lock<std::shared_timed_mutex> subscription_id_to_subscriptions_write_lock(
            subscription_id_to_subscription_mutex[sum]);

        std::unique_lock<std::shared_timed_mutex> subscription_expiry_to_subscription_id_write_lock(
            subscription_expiry_to_subscription_id_mutex);
        subscription_expiry_to_subscription_id[expiry].insert(subscription_id);

        if (subscription.expiryNotification >= 0)
        {
            std::unique_lock<std::shared_timed_mutex> subscription_expiryNotification_to_subscription_id_write_lock(
                subscription_expiryNotification_to_subscription_id_mutex);
            subscription_expiryNotification_to_subscription_id[expiry - subscription.expiryNotification].insert(
                subscription_id);
        }

        for (auto& monUri : subscription.subFilter.monitoredResourceUris)
        {
            uint16_t sum = get_u16_sum(monUri);
            std::unique_lock<std::shared_timed_mutex> monitored_resource_uri_to_subscription_id_write_lock(
                monitored_resource_uri_to_subscription_id_mutex[sum]);
            monitored_resource_uri_to_subscription_id[sum][monUri].insert(subscription_id);
        }
        if (clear_monitoredResourceUris)
        {
            subscription.subFilter.monitoredResourceUris.clear();
        }
        subscription_id_to_subscription[sum][subscription_id] = std::move(subscription);
        return OPERATION_SUCCESSFUL;
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

    std::string delete_subscription(const std::string& subscription_id, const ClientId& client_id, bool get_previous,
                                    bool& found, bool& delete_success)
    {
        found = false;
        delete_success = false;
        std::string ret;
        uint16_t sum = get_u16_sum(subscription_id);
        std::unique_lock<std::shared_timed_mutex> subscription_id_to_subscription_write_lock(
            subscription_id_to_subscription_mutex[sum]);
        auto& id_to_s = subscription_id_to_subscription[sum];
        auto iter = id_to_s.find(subscription_id);
        if (iter != id_to_s.end())
        {
            found = true;
            if ((client_id.nfId.size() || client_id.nfSetId.size()) && !(iter->second.clientId == client_id))
            {
                return ret;
            }
            auto& subscription = iter->second;
            auto expiry = iso8601_timestamp_to_seconds_since_epoch(subscription.expiry);
            auto expiryNotification = expiry - subscription.expiryNotification;

            std::unique_lock<std::shared_timed_mutex> subscription_expiry_to_subscription_id_write_lock(
                subscription_expiry_to_subscription_id_mutex);
            remove_from_map(subscription_expiry_to_subscription_id, expiry, subscription_id);

            if (subscription.expiryNotification >= 0)
            {
                std::unique_lock<std::shared_timed_mutex> subscription_expiryNotification_to_subscription_id_write_lock(
                    subscription_expiryNotification_to_subscription_id_mutex);
                remove_from_map(subscription_expiryNotification_to_subscription_id, expiry - subscription.expiryNotification,
                                subscription_id);
            }

            for (auto& monUri : subscription.subFilter.monitoredResourceUris)
            {
                uint16_t sum = get_u16_sum(monUri);
                std::unique_lock<std::shared_timed_mutex> monitored_resource_uri_to_subscription_id_write_lock(
                    monitored_resource_uri_to_subscription_id_mutex[sum]);
                remove_from_map(monitored_resource_uri_to_subscription_id[sum], monUri, subscription_id);
            }
            if (get_previous)
            {
                ret = staticjson::to_json_string(iter->second);
            }
            id_to_s.erase(iter);
            delete_success = true;
        }
        return ret;
    }

    std::string get_subscription(const std::string& subscription_id)
    {
        uint16_t sum = get_u16_sum(subscription_id);
        std::shared_lock<std::shared_timed_mutex> subscription_id_to_subscription_read_lock(
            subscription_id_to_subscription_mutex[sum]);
        auto& id_to_s = subscription_id_to_subscription[sum];
        auto iter = id_to_s.find(subscription_id);
        if (iter != id_to_s.end())
        {
            if (iter->second.expiryNotification >= 0)
            {
                iter->second.expiryNotificationFlag &= ~staticjson::Flags::IgnoreWrite;
            }
            return staticjson::to_json_string(iter->second);
        }
        return "";
    }

    std::vector<NotificationSubscription> get_subscriptions(size_t count)
    {
        std::vector<NotificationSubscription> ret;
        for (size_t sum = 0; sum < ONE_DIMENSION_SIZE * ONE_DIMENSION_SIZE; sum++)
        {
            std::shared_lock<std::shared_timed_mutex> subscription_id_to_subscription_read_lock(
                subscription_id_to_subscription_mutex[sum]);
            for (auto& s : subscription_id_to_subscription[sum])
            {
                if ((!count) || (ret.size() < count))
                {
                    ret.push_back(s.second);
                    if (ret.back().expiryNotification >= 0)
                    {
                        ret.back().expiryNotificationFlag &= ~staticjson::Flags::IgnoreWrite;
                    }
                }
            }
        }
        return ret;
    }

    auto get_decoded_subscription(const std::string& subscription_id)
    {
        uint16_t sum = get_u16_sum(subscription_id);
        std::shared_lock<std::shared_timed_mutex> subscription_id_to_subscription_read_lock(
            subscription_id_to_subscription_mutex[sum]);
        auto& id_to_s = subscription_id_to_subscription[sum];
        auto iter = id_to_s.find(subscription_id);
        if (iter != id_to_s.end())
        {
            return (iter->second);
        }
        NotificationSubscription subscription;
        return subscription;
    }


    int update_subscription(const std::string& subscription_id, const std::string& subscription_body, bool& existing_found)
    {
        NotificationSubscription subscription;
        staticjson::ParseStatus result;
        if (staticjson::from_json_string(subscription_body.c_str(), &subscription, &result))
        {
            bool found;
            bool delete_success;
            ClientId emptyClientId;
            delete_subscription(subscription_id, emptyClientId, false, found, delete_success);
            if (found && delete_success)
            {
                existing_found = true;
            }
            return create_subscription(subscription_id, std::move(subscription));
        }
        else
        {
            return DECODE_FAILURE;
        }
    }

    int patch_subscription(const std::string& subscription_id, const std::string& patch_data)
    {
        auto s = get_decoded_subscription(subscription_id);
        if (s.clientId.nfId.empty() && s.clientId.nfSetId.empty())
        {
            return RESOURCE_DOES_NOT_EXIST;
        }
        try
        {
            nlohmann::json original_json = nlohmann::json::parse(staticjson::to_json_string(s));
            nlohmann::json patch_json = nlohmann::json::parse(patch_data);
            nlohmann::json patched_obj = original_json.patch(patch_json);
            bool is_update = false;
            update_subscription(subscription_id, patched_obj.dump(), is_update);
            return OPERATION_SUCCESSFUL;
        }
        catch (...)
        {
        }
        return DECODE_FAILURE;
    }

    std::set<std::string> get_expired_subscription_ids()
    {
        return get_expired_items(subscription_expiry_to_subscription_id_mutex, subscription_expiry_to_subscription_id);
    }

    std::set<std::string> get_subscription_ids_about_to_expire()
    {
        return get_expired_items(subscription_expiryNotification_to_subscription_id_mutex,
                                 subscription_expiryNotification_to_subscription_id);
    }

    std::set<std::string> get_subscription_ids_to_notify(const std::string& record_id, const std::string& block_id)
    {
        auto subscription_ids_to_notify = [this](const std::string & path)
        {
            std::set<std::string> s;
            uint16_t sum = get_u16_sum(path);
            std::shared_lock<std::shared_timed_mutex> read_lock(monitored_resource_uri_to_subscription_id_mutex[sum]);
            auto iter = monitored_resource_uri_to_subscription_id[sum].find(path);
            if (iter != monitored_resource_uri_to_subscription_id[sum].end())
            {
                return iter->second;
            }
            return s;
        };

        std::set<std::string> ret;
        auto v = subscription_ids_to_notify(""); // subscribed change of the whole storage
        ret.insert(v.begin(), v.end());

        auto uri = udsf_base_uri;
        auto size = udsf_base_uri.size() +
                    PATH_DELIMETER.size() +
                    realm_id.size() +
                    PATH_DELIMETER.size() +
                    storage_id.size() +
                    PATH_DELIMETER.size() +
                    RECORDS.size() +
                    PATH_DELIMETER.size() +
                    record_id.size();
        uri.reserve(size);
        uri.append(PATH_DELIMETER).append(realm_id).append(PATH_DELIMETER).append(storage_id).append(PATH_DELIMETER).append(
            RECORDS);
        uri.append(PATH_DELIMETER).append(record_id);
        v = subscription_ids_to_notify(uri);
        for (auto& s : v)
        {
            ret.insert(s);
        }

        return ret;
    }

    void send_notify(const std::string& record_id, const std::string& block_id, const std::string& operation)
    {
        NotificationDescription notificationDescription;
        std::map<std::string, std::string, ci_less> additionalHeaders;

        auto v = get_subscription_ids_to_notify(record_id, block_id);
        for (auto& s : v)
        {
            auto subscription = get_decoded_subscription(s);
            if (std::find(subscription.subFilter.operations.begin(), subscription.subFilter.operations.end(),
                          operation) != subscription.subFilter.operations.end())
            {
                std::string recordNotificationBody;
                if (notificationDescription.recordRef.empty())
                {
                    notificationDescription.operationType = operation;
                    notificationDescription.subscriptionId = s;
                    notificationDescription.recordRef.reserve(udsf_base_uri.size() +
                                                              PATH_DELIMETER.size() +
                                                              realm_id.size() +
                                                              PATH_DELIMETER.size() +
                                                              storage_id.size() +
                                                              PATH_DELIMETER.size() +
                                                              RECORDS.size() +
                                                              PATH_DELIMETER.size() +
                                                              record_id.size());
                    notificationDescription.recordRef += udsf_base_uri;
                    notificationDescription.recordRef += PATH_DELIMETER;
                    notificationDescription.recordRef += realm_id;
                    notificationDescription.recordRef += PATH_DELIMETER;
                    notificationDescription.recordRef += storage_id;
                    notificationDescription.recordRef += PATH_DELIMETER;
                    notificationDescription.recordRef += RECORDS;
                    notificationDescription.recordRef += PATH_DELIMETER;
                    notificationDescription.recordRef += record_id;
                }
                auto description = staticjson::to_json_string(notificationDescription);
                recordNotificationBody = get_record_multipart_body(record_id, true, description);
                if (recordNotificationBody.size())
                {
                    if (additionalHeaders.empty())
                    {
                        additionalHeaders.insert(std::make_pair(CONTENT_TYPE, MULTIPART_CONTENT_TYPE));
                    }
                    send_http2_request(METHOD_POST, subscription.callbackReference, additionalHeaders, recordNotificationBody);
                }
            }
        }
    }

    void disable_expiryNotification_of_subscription(const std::string& subscription_id)
    {
        auto subscription = get_decoded_subscription(subscription_id);
        if (subscription.expiryNotification >= 0)
        {
            std::unique_lock<std::shared_timed_mutex> subscription_expiryNotification_to_subscription_id_write_lock(
                subscription_expiryNotification_to_subscription_id_mutex);
            remove_from_map(subscription_expiryNotification_to_subscription_id,
                            iso8601_timestamp_to_seconds_since_epoch(subscription.expiry) - subscription.expiryNotification,
                            subscription_id);
        }
    }

    std::set<std::string> get_all_timer_ids(size_t thread_id)
    {
        //std::shared_lock<std::shared_timed_mutex> timer_id_set_read_lock(all_timer_ids_mutex);
        return all_timer_ids[thread_id];
    }

    void track_timer_ttl(uint64_t ttl, const std::string& timer_id, bool insert = true)
    {
        track_ttl(timer_expires_to_timer_id_mutex, timer_expires_to_timer_id, ttl, timer_id, insert);
    }

    void track_timer_deleteAfter(uint64_t ttl, const std::string& timer_id, bool insert = true)
    {
        track_ttl(timer_deleteAfter_to_timer_id_mutex, timer_deleteAfter_to_timer_id, ttl, timer_id, insert);
    }

    void checkin_timer(const std::string& timer_id, const Timer& timer)
    {
        auto u8 = get_u8_sum(timer_id);
        auto thread_id = (u8 % number_of_worker_thread);
        auto run_in_worker = [timer_id, timer, thread_id, this]()
        {
            for (auto& t : timer.metaTags)
            {
                auto& tag_name = t.first;
                auto& tag_values = t.second;
                if (timer_tags_db[thread_id].empty())
                {
                    auto& dummy = timer_tags_db[thread_id][""];
                }
                auto& tags_name_db = timer_tags_db[thread_id].begin()->second;
                for (auto& tag_value : tag_values)
                {
                    insert_value_to_tags_db(tag_name, tag_value, timer_id, tags_name_db, true);
                }
            }
            auto ttl = iso8601_timestamp_to_seconds_since_epoch(timer.expires);
            track_timer_ttl(ttl, timer_id);
            all_timer_ids[thread_id].insert(timer_id);
        };
        g_strands[thread_id].post(run_in_worker);
    }

    void checkout_timer(const std::string& timer_id, const Timer& timer)
    {
        auto u8 = get_u8_sum(timer_id);
        auto thread_id = (u8 % number_of_worker_thread);
        auto run_in_worker = [timer_id, timer, thread_id, this]()
        {
            for (auto& t : timer.metaTags)
            {
                auto& tag_name = t.first;
                auto& tag_values = t.second;
                auto& tags_name_db = timer_tags_db[thread_id].begin()->second;
                for (auto& tag_value : tag_values)
                {
                    remove_value_from_tags_db(tag_name, tag_value, timer_id, tags_name_db);
                }
            }
            track_timer_ttl(iso8601_timestamp_to_seconds_since_epoch(timer.expires), timer_id, false);
            all_timer_ids[thread_id].erase(timer_id);
        };
        g_strands[thread_id].post(run_in_worker);
    }

    Timer delete_timer(const std::string& timer_id)
    {
        uint16_t sum = get_u16_sum(timer_id);
        std::shared_lock<std::shared_timed_mutex> timers_mutexes_read_lock(
            timers_mutexes[sum]);
        auto& id_to_timers = timers[sum];
        auto iter = id_to_timers.find(timer_id);
        if (iter == id_to_timers.end())
        {
            return Timer();
        }
        auto timer_object = std::move(iter->second);
        checkout_timer(timer_id, timer_object);
        id_to_timers.erase(iter);
        return timer_object;
    }

    int create_or_update_timer(const std::string& timer_id, const std::string& timer_body, bool& update)
    {
        Timer timer;
        staticjson::ParseStatus result;
        if (staticjson::from_json_string(timer_body.c_str(), &timer, &result))
        {
            auto ttl = iso8601_timestamp_to_seconds_since_epoch(timer.expires);
            auto seconds_since_epoch = std::chrono::duration_cast<std::chrono::seconds>
                                       (std::chrono::system_clock::now().time_since_epoch()).count();
            if (ttl <= seconds_since_epoch)
            {
                return TTL_EXPIRED;
            }

            uint16_t sum = get_u16_sum(timer_id);
            std::unique_lock<std::shared_timed_mutex> timers_mutexes_write_lock(
                timers_mutexes[sum]);
            auto& id_to_timers = timers[sum];
            Timer& timer_object = id_to_timers[timer_id];
            update = false;
            if (timer_object.expires.size())
            {
                update = true;
            }

            if (update)
            {
                checkout_timer(timer_id, timer_object);
            }

            checkin_timer(timer_id, timer);

            timer_object = timer;
            timer_object.timerId = timer_id;

        }
        else
        {
            return DECODE_FAILURE;
        }
        return OPERATION_SUCCESSFUL;
    }

    int patch_timer(const std::string& timer_id, const std::string& patch_data)
    {
        auto t = get_timer_object(timer_id);
        if (t.expires.empty())
        {
            return RESOURCE_DOES_NOT_EXIST;
        }
        try
        {
            nlohmann::json original_json = nlohmann::json::parse(staticjson::to_json_string(t));
            nlohmann::json patch_json = nlohmann::json::parse(patch_data);
            nlohmann::json patched_obj = original_json.patch(patch_json);
            bool is_update = true;
            return create_or_update_timer(timer_id, patched_obj.dump(), is_update);
        }
        catch (...)
        {
        }
        return DECODE_FAILURE;
    }

    Timer get_timer_object(const std::string& timer_id)
    {
        uint16_t sum = get_u16_sum(timer_id);
        std::shared_lock<std::shared_timed_mutex> timers_mutexes_read_lock(
            timers_mutexes[sum]);
        auto& id_to_timers = timers[sum];
        auto iter = id_to_timers.find(timer_id);
        if (iter != id_to_timers.end())
        {
            return iter->second;
        }
        return Timer();
    }

    std::set<std::string> get_expired_timers()
    {
        return get_expired_items(timer_expires_to_timer_id_mutex, timer_expires_to_timer_id);
    }

    void delete_deleteAfter_expired_timers()
    {
        std::unique_lock<std::shared_timed_mutex> write_lock(timer_deleteAfter_to_timer_id_mutex);
        auto seconds_since_epoch = std::chrono::duration_cast<std::chrono::seconds>
                                   (std::chrono::system_clock::now().time_since_epoch()).count();
        auto end = timer_deleteAfter_to_timer_id.upper_bound(seconds_since_epoch);
        auto iter = timer_deleteAfter_to_timer_id.begin();
        while (iter != end)
        {
            for (auto& t : iter->second)
            {
                delete_timer(t);
            }
            iter = timer_deleteAfter_to_timer_id.erase(iter);
        }
    }

    static void ttl_routine(Storage& storage)
    {
        while (true)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            auto subs_to_expire = storage.get_subscription_ids_about_to_expire();
            for (auto& s : subs_to_expire)
            {
                storage.disable_expiryNotification_of_subscription(s);
                auto subs = storage.get_decoded_subscription(s);
                if (subs.expiryCallbackReference.size())
                {
                    NotificationInfo notifInfo;
                    notifInfo.expiredSubscriptions.emplace_back(s);
                    std::map<std::string, std::string, ci_less> additionalHeaders;
                    additionalHeaders.insert(std::make_pair(CONTENT_TYPE, JSON_CONTENT));
                    auto body = staticjson::to_json_string(notifInfo);
                    additionalHeaders.insert(std::make_pair(CONTENT_LENGTH, std::to_string(body.size())));
                    send_http2_request(METHOD_POST, subs.callbackReference, additionalHeaders, body);
                }
            }

            auto expired_subs = storage.get_expired_subscription_ids();
            for (auto& s : expired_subs)
            {
                bool found;
                bool delete_success;
                ClientId emptyClientId;
                storage.delete_subscription(s, emptyClientId, false, found, delete_success);
            }

            auto expired_records = storage.get_ttl_expired_records();
            for (auto& r : expired_records)
            {
                auto meta = storage.get_record_meta_object(r);
                if (meta.callbackReference.size())
                {
                    std::string content_location;
                    content_location.reserve(udsf_base_uri.size() +
                                             PATH_DELIMETER.size() +
                                             storage.realm_id.size() +
                                             PATH_DELIMETER.size() +
                                             storage.storage_id.size() +
                                             PATH_DELIMETER.size() +
                                             RECORDS.size() +
                                             PATH_DELIMETER.size() +
                                             r.size());
                    content_location += udsf_base_uri;
                    content_location += PATH_DELIMETER;
                    content_location += storage.realm_id;
                    content_location += PATH_DELIMETER;
                    content_location += storage.storage_id;
                    content_location += PATH_DELIMETER;
                    content_location += RECORDS;
                    content_location += PATH_DELIMETER;
                    content_location += r;
                    auto body = storage.get_record_multipart_body(r, true);
                    std::map<std::string, std::string, ci_less> additionalHeaders;
                    additionalHeaders.insert(std::make_pair(CONTENT_TYPE, MULTIPART_CONTENT_TYPE));
                    additionalHeaders.insert(std::make_pair(CONTENT_LOCATION, content_location));
                    additionalHeaders.insert(std::make_pair(CONTENT_LENGTH, std::to_string(body.size())));
                    send_http2_request(METHOD_POST, meta.callbackReference, additionalHeaders, body);

                    storage.delete_record_directly(r);
                }
            }

            auto timers = storage.get_expired_timers();
            for (auto& t : timers)
            {
                auto timer_object = storage.get_timer_object(t);
                if (timer_object.expires.size() && timer_object.callbackReference.size())
                {
                    std::map<std::string, std::string, ci_less> additionalHeaders;
                    additionalHeaders.insert(std::make_pair(CONTENT_TYPE, JSON_CONTENT));
                    auto body = staticjson::to_json_string(timer_object);
                    additionalHeaders.insert(std::make_pair(CONTENT_LENGTH, std::to_string(body.size())));
                    send_http2_request(METHOD_POST, timer_object.callbackReference, additionalHeaders,
                                       staticjson::to_json_string(timer_object));
                }
                if (timer_object.deleteAfter)
                {
                    storage.track_timer_ttl(iso8601_timestamp_to_seconds_since_epoch(timer_object.expires), t, false);
                    auto seconds_since_epoch = std::chrono::duration_cast<std::chrono::seconds>
                                               (std::chrono::system_clock::now().time_since_epoch()).count();
                    storage.track_timer_deleteAfter(timer_object.deleteAfter + seconds_since_epoch, t, true);
                }
                else
                {
                    storage.delete_timer(t);
                }
            }

            storage.delete_deleteAfter_expired_timers();
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
            std::thread ttl_thead(Storage::ttl_routine, std::ref(ret));
            ttl_thead.detach();
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

