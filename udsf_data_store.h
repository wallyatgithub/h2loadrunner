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


const std::string CONTENT_ID = "Content-Id";
const std::string CONTENT_TYPE = "Content-Type";
const std::string COLON  = ":";
const std::string CRLF = "\r\n";
const std::string VERY_SPECIAL_BOUNARY_WITH_LEADING_TWO_DASHES = "-----wallyweiwallzzllawiewyllaw---";
const std::string ENDING_TWO_DASH = "--";
const std::string JSON_CONTENT = "application/json";
const std::string META_CONTENT_ID = "meta";
const std::string MULTIPART_CONTENT_TYPE = "multipart/mixed; boundary=---wallyweiwallzzllawiewyllaw---";

const int8_t PLUS = 1;
const int8_t MINUS = -1;

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

std::string get_string_value_from_Json_object(rapidjson::Value& object, const std::string& name)
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

std::set<std::string> run_and_operator(const std::vector<std::set<std::string>>& operands)
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

std::set<std::string> run_or_operator(const std::vector<std::set<std::string>>& operands)
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

std::set<std::string> run_not_operator(const std::set<std::string>& source, const std::set<std::string>& operands)
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
    bool insert_or_update_block(std::string& id, std::string& type, std::string& blockData,
                                std::map<std::string, std::string, Case_Independent_Less>& headers)
    {
        std::unique_lock<std::shared_timed_mutex> write_guard(*blocks_mutex);
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
        std::unique_lock<std::shared_timed_mutex> guard(*blocks_mutex);
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
        std::shared_lock<std::shared_timed_mutex> guard(*blocks_mutex);
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
        std::shared_lock<std::shared_timed_mutex> blocks_guard(*blocks_mutex);
        std::shared_lock<std::shared_timed_mutex> meta_guard(*meta_mutex);
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

    std::string get_meta()
    {
        std::shared_lock<std::shared_timed_mutex> guard(*meta_mutex);
        auto metaString = std::move(staticjson::to_json_string(meta));
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

class Storage
{
public:
    std::unordered_map<std::string, Record> records[0x100][0x100];
    std::unordered_map<std::string, MetaSchema> schemas[0x100][0x100];
    std::shared_timed_mutex records_mutexes[0x100][0x100];
    std::shared_timed_mutex schemas_mutexes[0x100][0x100];
    Tags_Db tags_db;
    std::shared_timed_mutex tags_db_main_mutex;

    void install_tags_from_schema(const MetaSchema& schema)
    {
        Tags_Name_Db_Of_One_Schema tags_name_db;
        for (auto& tag : schema.metaTags)
        {
            auto& tag_value_db = tags_name_db.name_to_value_db_map[tag.tagName];
        }
        std::unique_lock<std::shared_timed_mutex> tags_db_main_write_lock(tags_db_main_mutex);
        std::string schema_id = schema.schemaId;
        tags_db.emplace(std::move(schema_id), std::move(tags_name_db));
    }

    void delete_tags_from_schema(const std::string& schema_id)
    {
        std::unique_lock<std::shared_timed_mutex> tags_db_main_write_guard(tags_db_main_mutex);
        tags_db.erase(schema_id);
    }

    void insert_tag_value(const std::string& schema_id, const std::string& tag_name, const std::string& tag_value,
                          const std::string& record_id)
    {
        std::unique_lock<std::shared_timed_mutex> tags_db_main_read_guard(tags_db_main_mutex);
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
        std::unique_lock<std::shared_timed_mutex> tags_db_main_read_lock(tags_db_main_mutex);
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
        std::unique_lock<std::shared_timed_mutex> tags_db_main_read_lock(tags_db_main_mutex);
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
        for (auto tags_db_iter: tag_db_iterators_to_go_through)
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
        std::unique_lock<std::shared_timed_mutex> tags_db_main_read_lock(tags_db_main_mutex);
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
        for (auto tags_db_iter: tag_db_iterators_to_go_through)
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
            uint16_t sum = get_u16_sum(record_id);
            uint8_t row = sum >> 8;
            uint8_t col = sum & 0xFF;
            std::unique_lock<std::shared_timed_mutex> record_map_write_guard(records_mutexes[row][col]);
            auto record_id_copy = record_id;
            records[row][col].emplace(std::move(record_id_copy), std::move(record));
            for (auto& tag : record_meta_copy.tags)
            {
                auto& tag_name = tag.first;
                auto& tag_values = tag.second;
                for (auto& tag_value : tag_values)
                {
                    insert_tag_value(record_meta_copy.schemaId, tag_name, tag_value, record_id);
                }
            }

            return 0;
        }
        return -3;
    }
    std::string delete_record(const std::string& record_id, bool get_previous = false)
    {
        std::string ret;
        if (!get_previous)
        {
            delete_record_directly(record_id);
        }
        else
        {
            ret = get_record(record_id);
            delete_record_directly(record_id);
        }
        return ret;
    }

    void delete_record_directly(const std::string& record_id)
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
            write_lock.unlock();

            for (auto& tag : record.meta.tags)
            {
                auto& tag_name = tag.first;
                auto& tag_values = tag.second;
                for (auto& tag_value : tag_values)
                {
                    remove_tag_value(record.meta.schemaId, tag_name, tag_value, record_id);
                }
            }
        }
    }

    std::string get_record(const std::string& record_id)
    {
        uint16_t sum = get_u16_sum(record_id);
        uint8_t row = sum >> 8;
        uint8_t col = sum & 0xFF;
        std::shared_lock<std::shared_timed_mutex> guard(records_mutexes[row][col]);
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
        uint8_t row = sum >> 8;
        uint8_t col = sum & 0xFF;
        std::shared_lock<std::shared_timed_mutex> guard(schemas_mutexes[row][col]);
        auto iter = schemas[row][col].find(schema_id);
        if (iter != schemas[row][col].end())
        {
            ret = std::move(staticjson::to_json_string(iter->second));
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

    bool add_or_update_schema(const std::string& schema_id, const std::string& schema)
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
            std::unique_lock<std::shared_timed_mutex> guard(schemas_mutexes[row][col]);
            auto id = schema_id;
            schemas[row][col].emplace(std::move(id), std::move(metaSchema));
            return true;
        }
        return false;
    }

    std::string delete_schema(const std::string& schema_id, bool get_previous = false)
    {
        std::string ret;
        if (!get_previous)
        {
            delete_schema_directly(schema_id);
        }
        else
        {
            ret = get_schema(schema_id);
            delete_schema_directly(schema_id);
        }
        return ret;

    }

    std::string delete_schema_directly(const std::string& schema_id)
    {
        std::string ret;
        uint16_t sum = get_u16_sum(schema_id);
        uint8_t row = sum >> 8;
        uint8_t col = sum & 0xFF;
        std::unique_lock<std::shared_timed_mutex> guard(schemas_mutexes[row][col]);
        schemas[row][col].erase(schema_id);
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

    bool insert_or_update_block(const std::string& record_id, std::string& id, std::string& type, std::string& blockData,
                                std::map<std::string, std::string, Case_Independent_Less>& headers)
    {
        uint16_t sum = get_u16_sum(record_id);
        uint8_t row = sum >> 8;
        uint8_t col = sum & 0xFF;
        std::shared_lock<std::shared_timed_mutex> guard(records_mutexes[row][col]);
        auto iter = records[row][col].find(record_id);
        if (iter != records[row][col].end())
        {
            return iter->second.insert_or_update_block(id, type, blockData, headers);
        }
        return false;
    }

    std::string delete_block(const std::string& record_id, const std::string& blockId, bool get_previous = false)
    {
        uint16_t sum = get_u16_sum(record_id);
        uint8_t row = sum >> 8;
        uint8_t col = sum & 0xFF;
        std::shared_lock<std::shared_timed_mutex> guard(records_mutexes[row][col]);
        auto iter = records[row][col].find(record_id);
        if (iter != records[row][col].end())
        {
            return iter->second.delete_block(blockId, get_previous);
        }
        return "";
    }

    std::string get_block(const std::string& record_id, const std::string& blockId)
    {
        uint16_t sum = get_u16_sum(record_id);
        uint8_t row = sum >> 8;
        uint8_t col = sum & 0xFF;
        std::shared_lock<std::shared_timed_mutex> guard(records_mutexes[row][col]);
        auto iter = records[row][col].find(record_id);
        if (iter != records[row][col].end())
        {
            return iter->second.get_block(blockId);
        }
        return "";
    }

    std::set<std::string> run_search_expression(const std::string& schema_id, rapidjson::Value& value)
    {
        const std::string RECORD_ID_LIST = "recordIdList";
        const std::string CONDITION = "cond";
        const std::string OPERATION = "op";
        const std::string UNITS = "units";
        const std::string CONDITION_OP_AND = "AND";
        const std::string CONDITION_OP_OR = "OR";
        const std::string CONDITION_OP_NOT = "NOT";
        std::set<std::string> ret;
        auto actual_schema_id = schema_id;
        int8_t ret_op = PLUS;

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
                        for (size_t i = 0; i < units.Size(); i++)
                        {
                            auto& array_value = units[i];
                            if (array_value.IsString())
                            {
                                operands.emplace_back(run_search_expression(actual_schema_id, array_value));
                            }
                        }
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
                    // operands.size() should be 1, but do and on it anyway and set to MINUS at last
                    ret = run_and_operator(operands);
                    ret_op = MINUS;
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

                if (sub_value.IsArray())
                {
                    for (size_t i = 0; i < sub_value.Size(); i++)
                    {
                        auto& array_value = sub_value[i];
                        if (array_value.IsString())
                        {
                            ret.emplace(array_value.GetString(), array_value.GetStringLength());
                        }
                    }
                }
            }
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
        uint8_t row = sum >> 8;
        uint8_t col = sum & 0xFF;
        std::lock_guard<std::mutex> guard(storages_mutexes[row][col]);
        return storages[row][col][storage_id];
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

