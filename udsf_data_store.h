#ifndef UDSF_STORAGE_H
#define UDSF_STORAGE_H
#include <iostream>
#include <fstream>
#include <cstring>
#include <numeric>

#include "staticjson/document.hpp"
#include "staticjson/staticjson.hpp"
#include "rapidjson/schema.h"
#include "rapidjson/prettywriter.h"
#include "multipart_parser.h"

const std::string CONTENT_ID = "Content-Id";
const std::string CONTENT_TYPE = "Content-Type";
const std::string COLON  = ":";
const std::string CRLF = "\r\n";
const std::string VERY_SPECIAL_BOUNARY_WITH_LEADING_TWO_DASHES = "-----wallyweiwallzzllawiewyllaw---";
const std::string ENDING_TWO_DASH = "--";
const std::string JSON_CONTENT = "application/json";
const std::string META_CONTENT_ID = "meta";
const std::string MULTIPART_CONTENT_TYPE = "multipart/mixed; boundary=---wallyweiwallzzllawiewyllaw---";

namespace udsf
{
uint16_t get_u16_sum(const std::string& key)
{
    return (std::accumulate(key.begin(),
                            key.end(),
                            0,
                            [](uint16_t sum, const char& c)
    {
        return sum + uint8_t(c);
    }));
}

struct Case_Independent_Less
{
    // case-independent (ci) compare_less binary function
    struct nocase_compare
    {
        bool operator()(const unsigned char& c1, const unsigned char& c2) const
        {
            return tolower(c1) < tolower(c2);
        }
    };
    bool operator()(const std::string& s1, const std::string& s2) const
    {
        return std::lexicographical_compare
               (s1.begin(), s1.end(),     // source range
                s2.begin(), s2.end(),     // dest range
                nocase_compare());   // comparison
    }
};

class MultipartParser
{
public:
    using MutiParts = std::vector<std::pair<std::map<std::string, std::string, Case_Independent_Less>, std::string>>;
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

    MutiParts get_parts(const std::string& content)
    {
        auto size = multipart_parser_execute(m_parser, content.c_str(), content.size());
        return std::move(parts);
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
    std::string id;
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
    std::map<std::string, std::string, Case_Independent_Less> headers;
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
        //final_size += CRLF.size();
        final_size += CONTENT_ID.size() + COLON.size() + content_id.size() + CRLF.size();
        final_size += CONTENT_TYPE.size() + COLON.size() + content_type.size() + CRLF.size();
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
        for (auto& h : headers)
        {
            ret.append(h.first).append(COLON).append(h.second).append(CRLF);
        }
        ret.append(CRLF);
        ret.append(content).append(CRLF);
        return ret;
    }
};

class Record
{
public:
    std::unordered_map<std::string, size_t> blockId_to_index;
    std::vector<Block> blocks;
    RecordMeta meta;
    std::mutex blocks_mutex;
    void staticjson_init(staticjson::ObjectHandler* h)
    {
        h->add_property("blocks", &this->blocks, staticjson::Flags::Optional);
        h->add_property("meta", &this->meta);
    }
    bool insert_or_update_block(std::string& id, std::string& type, std::string& blockData,
                                std::map<std::string, std::string, Case_Independent_Less>& headers)
    {
        std::lock_guard<std::mutex> guard(blocks_mutex);
        auto iter = blockId_to_index.find(id);
        if (iter != blockId_to_index.end())
        {
            blocks[iter->second].content = std::move(blockData);
            blocks[iter->second].content_id = std::move(id);
            blocks[iter->second].content_type = std::move(type);
            blocks[iter->second].headers = std::move(headers);
        }
        else
        {
            blocks.emplace_back();
            blocks.back().content = std::move(blockData);
            blocks.back().content_id = std::move(id);
            blocks.back().content_type = std::move(type);
            blocks.back().headers = std::move(headers);
            blockId_to_index[blocks.back().content_id] = (blocks.size() - 1);
        }
        return true;
    }

    std::string delete_block(const std::string& blockId, bool get_previous = false)
    {
        std::string ret;
        std::lock_guard<std::mutex> guard(blocks_mutex);
        auto iter = blockId_to_index.find(blockId);
        if (iter == blockId_to_index.end())
        {
            return ret;
        }

        auto index = iter->second;
        blockId_to_index.erase(iter);
        if (get_previous)
        {
            ret = std::move(blocks[index].produce_body_part());
        }
        blocks.erase(blocks.begin() + index);
        for (auto& m : blockId_to_index)
        {
            if (m.second > index)
            {
                m.second--;
            }
        }
        return ret;
    }

    std::string get_block(const std::string& blockId)
    {
        std::string ret;
        auto iter = blockId_to_index.find(blockId);
        if (iter == blockId_to_index.end())
        {
            return ret;
        }

        auto index = iter->second;
        return blocks[index].produce_body_part();
    }

    std::string produce_multipart_body()
    {
        std::string ret;
        size_t final_size = 0;
        auto metaString = std::move(staticjson::to_json_string(meta));
        std::vector<std::string> block_body_parts;
        for (auto& b : blocks)
        {
            block_body_parts.emplace_back(b.produce_body_part());
        }
        final_size += CRLF.size();
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
};

class Storage
{
public:
    std::unordered_map<std::string, Record> records[0x100][0x100];
    std::unordered_map<std::string, MetaSchema> schemas[0x100][0x100];
    std::mutex records_mutexes[0x100][0x100];
    std::mutex schemas_mutexes[0x100][0x100];

    bool validate_record_meta(const RecordMeta& meta)
    {
        bool ret = true;
        if (meta.schemaId.empty())
        {
            return ret;
        }
        bool success;
        auto& schema_object = get_schema_object(meta.schemaId, success);
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

    /**
    * @brief insert or update record
    *
    * @returns int
    * return -1: decode multipart failure
    * return -2: block content-id absent
    * return -3: meta decode or validation failure
    *
    * @thows None
    * */
    int insert_or_update_record(const std::string& record_id, const std::string& boundary,
                                const std::string& multipart_content)
    {
        MultipartParser parser(boundary);
        auto parts = std::move(parser.get_parts(multipart_content));
        if (parts.empty())
        {
            return -1;
        }
        RecordMeta record_meta;
        staticjson::ParseStatus result;
        if (staticjson::from_json_string(parts[0].second.c_str(), &record_meta, &result) &&
            validate_record_meta(record_meta))
        {
            uint16_t sum = get_u16_sum(record_id);
            uint8_t row = sum >> 16;
            uint8_t col = sum & 0xFF;
            std::unique_lock<std::mutex> guard(records_mutexes[row][col]);
            auto& record = records[row][col][record_id];
            guard.release();
            record.meta = std::move(record_meta);
            for (size_t i = 1; i < parts.size(); i++)
            {
                std::string content_id;
                std::string content_type;
                auto iter = parts[i].first.find(CONTENT_ID);
                if (iter == parts[i].first.end())
                {
                    return -2;
                }
                content_id = std::move(iter->second);
                parts[i].first.erase(iter);

                iter = parts[i].first.find(CONTENT_TYPE);
                if (iter != parts[i].first.end())
                {
                    content_type = std::move(iter->second);
                    parts[i].first.erase(iter);
                }
                record.insert_or_update_block(content_id, content_type, parts[i].second, parts[i].first);
            }
            return 0;
        }
        return -3;
    }

    std::string delete_record(const std::string& record_id, bool get_previous = false)
    {
        uint16_t sum = get_u16_sum(record_id);
        uint8_t row = sum >> 16;
        uint8_t col = sum & 0xFF;
        std::lock_guard<std::mutex> guard(records_mutexes[row][col]);
        std::string ret;
        auto iter = records[row][col].find(record_id);
        if (iter != records[row][col].end())
        {
            if (get_previous)
            {
                ret = iter->second.produce_multipart_body();
            }
            records[row][col].erase(iter);
        }
        return ret;
    }

    std::string get_record(const std::string& record_id)
    {
        uint16_t sum = get_u16_sum(record_id);
        uint8_t row = sum >> 16;
        uint8_t col = sum & 0xFF;
        std::lock_guard<std::mutex> guard(records_mutexes[row][col]);
        std::string ret;
        auto iter = records[row][col].find(record_id);
        if (iter != records[row][col].end())
        {
            ret = iter->second.produce_multipart_body();
        }
        return ret;
    }

    std::string get_schema(const std::string& schema_id)
    {
        std::string ret;
        uint16_t sum = get_u16_sum(schema_id);
        uint8_t row = sum >> 16;
        uint8_t col = sum & 0xFF;
        std::lock_guard<std::mutex> guard(schemas_mutexes[row][col]);
        auto iter = schemas[row][col].find(schema_id);
        if (iter != schemas[row][col].end())
        {
            ret = std::move(staticjson::to_json_string(iter->second));
        }
        return ret;
    }

    const MetaSchema& get_schema_object(const std::string& schema_id, bool& success)
    {
        static MetaSchema dummyMetaSchama;
        success = false;
        uint16_t sum = get_u16_sum(schema_id);
        uint8_t row = sum >> 16;
        uint8_t col = sum & 0xFF;
        std::lock_guard<std::mutex> guard(schemas_mutexes[row][col]);
        auto iter = schemas[row][col].find(schema_id);
        if (iter != schemas[row][col].end())
        {
            success = true;
            return iter->second;
        }
        return dummyMetaSchama;
    }

    bool add_or_update_schema(const std::string& schema_id, const std::string& schema)
    {
        uint16_t sum = get_u16_sum(schema_id);
        uint8_t row = sum >> 16;
        uint8_t col = sum & 0xFF;
        staticjson::ParseStatus result;
        MetaSchema metaSchema;
        if (staticjson::from_json_string(schema.c_str(), &metaSchema, &result) &&
            metaSchema.schemaId == schema_id)
        {
            //metaSchema.build_index();
            std::unique_lock<std::mutex> guard(schemas_mutexes[row][col]);
            auto id = schema_id;
            schemas[row][col].emplace(std::move(id), std::move(metaSchema));
            return true;
        }
        return false;
    }

    std::string delete_schema(const std::string& schema_id, bool get_previous = false)
    {
        std::string ret;
        uint16_t sum = get_u16_sum(schema_id);
        uint8_t row = sum >> 16;
        uint8_t col = sum & 0xFF;
        std::lock_guard<std::mutex> guard(schemas_mutexes[row][col]);
        auto iter = schemas[row][col].find(schema_id);
        if (iter != schemas[row][col].end())
        {
            if (get_previous)
            {
                ret = std::move(staticjson::to_json_string(iter->second));
            }
            schemas[row][col].erase(iter);
        }
        return ret;
    }

};

class Realm
{
public:
    std::unordered_map<std::string, Storage> storages[0x100][0x100];
    std::mutex storages_mutexes[0x100][0x100];

    Storage& get_storage(const std::string& storage_id)
    {
        uint16_t sum = get_u16_sum(storage_id);
        uint8_t row = sum >> 16;
        uint8_t col = sum & 0xFF;
        std::lock_guard<std::mutex> guard(storages_mutexes[row][col]);
        return storages[row][col][storage_id];
    }

    void delete_storage(const std::string& storage_id)
    {
        uint16_t sum = get_u16_sum(storage_id);
        uint8_t row = sum >> 16;
        uint8_t col = sum & 0xFF;
        std::lock_guard<std::mutex> guard(storages_mutexes[row][col]);
        storages[row][col].erase(storage_id);
    }
};

static std::unordered_map<std::string, Realm> Realms;
static std::mutex realm_mutex;

Realm& get_realm(const std::string& realm_id)
{
    std::lock_guard<std::mutex> guard(realm_mutex);
    return Realms[realm_id];
}

}


#endif

