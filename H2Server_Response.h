#ifndef H2SERVER_RESPONSE_H
#define H2SERVER_RESPONSE_H

#include <rapidjson/pointer.h>
#include <rapidjson/document.h>
#include <vector>
#include <list>
#include <random>
#include <iomanip>
#include <ctime>
#include <sstream>
#include <fstream>
#include <streambuf>
#include <sstream>
#include <map>
#include <memory>
#include "H2Server_Config_Schema.h"
#include "H2Server_Request_Message.h"
#include <rapidjson/writer.h>
extern "C" {
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
}
#include "common_lua.h"

const std::string extended_json_pointer_indicator = "/~#";

const std::string extended_json_pointer_name_indicator = "#name";

const std::string extended_json_pointer_value_indicator = "#value";

const std::string status = ":status";

const std::string customize_response = "customize_response";

inline std::vector<std::string> tokenize_string(const std::string& source, const std::string& delimeter)
{
    std::vector<std::string> retVec;
    size_t start = 0;
    size_t delimeter_len = delimeter.length();
    if (!delimeter.empty())
    {
        size_t pos = source.find(delimeter, start);
        while (pos != std::string::npos)
        {
            retVec.emplace_back(source.substr(start, (pos - start)));
            start = pos + delimeter_len;
            pos = source.find(delimeter, start);
        }
        retVec.emplace_back(source.substr(start, std::string::npos));
    }
    else
    {
        retVec.emplace_back(source);
    }
    return retVec;
    if (debug_mode)
    {
        std::for_each(retVec.begin(), retVec.end(), [](const std::string& s){std::cout<<"token: "<<s<<std::endl;});
        std::cout<<"delimeter: "<<delimeter<<std::endl;
    }
}

template<typename RapidJsonType>
const rapidjson::Value* getNthValue(const RapidJsonType& d, int64_t n)
{
    if (d.MemberCount() && n > 0)
    {
        int64_t index = 1;
        for (rapidjson::Value::ConstMemberIterator itr = d.MemberBegin();
            itr != d.MemberEnd();)
        {
            if (index == n)
            {
                return &(itr->value);
            }
            ++index;
            ++itr;
        }
    }
    else if (d.MemberCount() && n < 0)
    {
        if ((0 - n) == d.MemberCount())
        {
            return &(d.MemberBegin()->value);
        }
        else
        {
            int64_t index = -1;
            rapidjson::Value::ConstMemberIterator itr = d.MemberEnd();
            itr--;
            while (itr != d.MemberBegin())
            {
                if (index == n)
                {
                    return &(itr->value);
                }
                itr--;
                index--;
            }
        }

    }
    return nullptr;
}

template<typename RapidJsonType>
const rapidjson::Value* getNthName(const RapidJsonType& d, int64_t n)
{
    if (d.MemberCount() && n > 0)
    {
        int64_t index = 1;
        for (rapidjson::Value::ConstMemberIterator itr = d.MemberBegin();
            itr != d.MemberEnd();)
        {
            if (index == n)
            {
                return &(itr->name);
            }
            ++index;
            ++itr;
        }
    }
    else if (d.MemberCount() && n < 0)
    {
        if ((0 - n) == d.MemberCount())
        {
            return &(d.MemberBegin()->name);
        }
        else
        {
            int64_t index = -1;
            rapidjson::Value::ConstMemberIterator itr = d.MemberEnd();
            itr--;
            while (itr != d.MemberBegin())
            {
                if (index == n)
                {
                    return &(itr->name);
                }
                itr--;
                index--;
            }
        }

    }
    return nullptr;
}

inline std::vector<std::string> splitComplexJsonPointer(const std::string& json_pointer)
{
    std::vector<std::string> JsonPointers;
    std::string extended_json_pointer_indicator = "/~#";
    size_t start_pos = 0;
    while (start_pos != std::string::npos)
    {
        size_t end_pos = json_pointer.find(extended_json_pointer_indicator, start_pos);
        if (end_pos - start_pos)
        {
            JsonPointers.emplace_back(json_pointer.substr(start_pos, (end_pos - start_pos)));
        }
        if (end_pos != std::string::npos)
        {
            size_t nextJsonPtrPos = json_pointer.find("/", end_pos+1);
            JsonPointers.emplace_back(json_pointer.substr(end_pos, (nextJsonPtrPos - end_pos)));
            start_pos = nextJsonPtrPos;
        }
        else
        {
            break;
        }
    }
    if (debug_mode)
    {
        std::cout<<"ComplexJsonPointer:"<<json_pointer<<std::endl;
        for (auto& ptr: JsonPointers)
        {
            std::cout<<"Simple JsonPointer:"<<ptr<<std::endl;
        }
    }
    return JsonPointers;
}

template<typename RapidJsonType>
std::string convertRapidVJsonValueToStr(RapidJsonType* value)
{
    if (value)
    {
        if (value->IsString())
        {
            return value->GetString();
        }
        else if (value->IsBool())
        {
            return value->GetBool() ? "true" : "false";
        }
        else if (value->IsUint64())
        {
            return std::to_string(value->GetUint64());
        }
        else if (value->IsDouble())
        {
            return std::to_string(value->GetDouble());
        }
        else if (value->IsObject())
        {
            rapidjson::StringBuffer buffer;
            rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
            value->Accept(writer);
            return std::string(buffer.GetString());
        }
    }
    return "";
}


template<typename RapidJsonType>
std::string getValueFromJsonPtr(const RapidJsonType& d, const std::string& json_pointer)
{
    std::vector<std::string> pointers = splitComplexJsonPointer(json_pointer);
    const rapidjson::Value* value = &d;
    for (size_t vectorIndex = 0; vectorIndex < pointers.size(); vectorIndex++)
    {
        if (pointers[vectorIndex].find(extended_json_pointer_indicator) != std::string::npos)
        {
            try
            {
                int64_t index = std::stoi(pointers[vectorIndex].substr(extended_json_pointer_indicator.size()));
                std::string str = extended_json_pointer_indicator;
                str.append(std::to_string(index));
                std::string name_or_value = pointers[vectorIndex].substr(str.size());
                if (name_or_value.compare(extended_json_pointer_name_indicator) == 0)
                {
                    value = getNthName(*value, index);
                    break; // a name is always a string, no more pointer into this string any more
                }
                else if (name_or_value.compare(extended_json_pointer_value_indicator) == 0)
                {
                    value = getNthValue(*value, index);
                }
                else
                {
                    std::cerr<<"invalid json pointer: "<<pointers[vectorIndex]<<std::endl;
                }
            }
            catch (std::invalid_argument& e)
            {
                std::cerr<<"invalid_argument: "<<pointers[vectorIndex]<<std::endl;
                exit(1);
            }
            catch (std::out_of_range& e)
            {
                std::cerr<<"out_of_range: "<<pointers[vectorIndex]<<std::endl;
                exit(1);
            }
        }
        else
        {
            rapidjson::Pointer ptr(pointers[vectorIndex].c_str());
            value = ptr.Get(*value);
        }
        if (debug_mode)
        {
            std::cout<<"json pointer:"<<pointers[vectorIndex]<<", value:"<<convertRapidVJsonValueToStr(value)<<std::endl;
        }

        if (!value)
        {
            break;
        }
    }
    return convertRapidVJsonValueToStr(value);
}

enum VALUE_TYPE
{
    VAL_INVALID = 0,
    VAL_RANDOM_HEX,
    VAL_TIMESTAMP,
    VAL_UUID,
    VAL_RAW_PAYLOAD
};

class Argument
{
public:
    std::string json_pointer;
    int64_t substring_start;
    int64_t substring_length;
    std::string header_name;
    std::regex reg_exp;
    std::string regex;
    bool regex_present = false;
    VALUE_TYPE val_type = VAL_INVALID;

    std::string generate_uuid_v4() const
    {
        static thread_local std::random_device              rd;
        static thread_local std::mt19937                    gen(rd());
        static thread_local std::uniform_int_distribution<> dis(0, 15);
        static thread_local std::uniform_int_distribution<> dis2(8, 11);
        std::stringstream ss;
        int i;
        ss << std::hex;
        for (i = 0; i < 8; i++) {
            ss << dis(gen);
        }
        ss << "-";
        for (i = 0; i < 4; i++) {
            ss << dis(gen);
        }
        ss << "-4";
        for (i = 0; i < 3; i++) {
            ss << dis(gen);
        }
        ss << "-";
        ss << dis2(gen);
        for (i = 0; i < 3; i++) {
            ss << dis(gen);
        }
        ss << "-";
        for (i = 0; i < 12; i++) {
            ss << dis(gen);
        };
        return ss.str();
    }

    Argument(const Schema_Argument& payload_argument)
    {
        if (payload_argument.type_of_value == "JsonPointer")
        {
            json_pointer = payload_argument.value_identifier;
        }
        else if (payload_argument.type_of_value == "Header")
        {
            header_name = payload_argument.value_identifier;
        }
        else if (payload_argument.type_of_value == "Pseudo-UUID")
        {
            val_type = VAL_UUID;
        }
        else if (payload_argument.type_of_value == "RandomHex")
        {
            val_type = VAL_RANDOM_HEX;
        }
        else if (payload_argument.type_of_value == "TimeStamp")
        {
            val_type = VAL_TIMESTAMP;
        }
        else if (payload_argument.type_of_value == "RawPayload")
        {
            val_type = VAL_RAW_PAYLOAD;
        }
        substring_start = payload_argument.substring_start;
        substring_length = payload_argument.substring_length;
        if (payload_argument.regex.size())
        {
            try
            {
                reg_exp.assign(payload_argument.regex, std::regex_constants::ECMAScript|std::regex_constants::optimize);
            }
            catch (std::regex_error e)
            {
                std::cerr<<"invalid reg exp: "<<payload_argument.regex<<" "<<e.what()<<std::endl;
            }
            regex = payload_argument.regex;
            regex_present = true;
        }
    }
    std::string getValue(H2Server_Request_Message& msg) const
    {
        std::string str;
        if (json_pointer.size())
        {
            msg.decode_json_if_not_yet();
            str = getValueFromJsonPtr(msg.json_payload, json_pointer);
        }
        else if (header_name.size()&&msg.headers.count(header_name))
        {
            str = msg.headers.find(header_name)->second;
        }
        else
        {
            switch (val_type)
            {
                case VAL_TIMESTAMP:
                {
                    std::time_t t = std::time(nullptr);
                    std::tm tm = *std::gmtime(&t);
                    std::stringstream buffer;
                    buffer << std::put_time(&tm, "%a, %d %b %Y %H:%M:%S %Z");
                    str = buffer.str();
                    break;
                }
                case VAL_RANDOM_HEX:
                {
                    static thread_local std::random_device              rd;
                    static thread_local std::mt19937                    gen(rd());
                    static thread_local std::uniform_int_distribution<> dis(0, 15);
                    std::stringstream stream;
                    stream << std::hex << dis(gen);
                    str = stream.str();
                    break;
                }
                case VAL_UUID:
                {
                    str = generate_uuid_v4();
                    break;
                }
                case VAL_RAW_PAYLOAD:
                {
                    str = *msg.raw_payload;
                    break;
                }
                default:
                {
                }
            }
        }

        if (debug_mode)
        {
            std::cout<<"json_pointer: "<<json_pointer<<std::endl;
            std::cout<<"header_name: "<<header_name<<std::endl;
            std::cout<<"target string: "<<str<<std::endl;
            std::cout<<"val_type: "<<val_type<<std::endl;
            std::cout<<"regex: "<<regex<<std::endl;
            std::cout<<"substring_start: "<<substring_start<<std::endl;
            std::cout<<"substring_length: "<<substring_length<<std::endl;
        }
        if (regex_present)
        {
            std::smatch match_result;
            if (std::regex_search(str, match_result, reg_exp))
            {
                str = match_result[0];
            }
            else
            {
                str.clear();
            }
        }
        if (((substring_start > 0) || (substring_length != -1)) && (substring_start < str.size()))
        {
            return str.substr(substring_start, substring_length);
        }
        return str;
    }
};

class H2Server_Response_Header
{
public:
    std::vector<std::string> tokenizedHeader;
    std::vector<Argument> header_arguments;
    H2Server_Response_Header(const Schema_Response_Header& schema_header)
    {
        size_t t = schema_header.header.find(":", 1);
        if ((t == std::string::npos) ||
            (schema_header.header[0] == ':' && 1 == t))
        {
            std::cerr << "invalid header, no name: " << schema_header.header << std::endl;
            abort();
        }
        std::string header_name = schema_header.header.substr(0, t);
        std::string header_value = schema_header.header.substr(t + 1);

        if (header_value.empty())
        {
            std::cerr << "invalid header - no value: " << schema_header.header
                      << std::endl;
            exit(1);
        }

        tokenizedHeader = tokenize_string(schema_header.header, schema_header.placeholder);
        for (auto& arg : schema_header.arguments)
        {
            header_arguments.emplace_back(Argument(arg));
        }
        if (tokenizedHeader.size() - header_arguments.size() != 1)
        {
            std::cerr << "number of placeholders does not match number of arguments:" << staticjson::to_pretty_json_string(
                          schema_header) << std::endl;
            exit(1);
        }
    }
};
class H2Server_Response
{
public:
    uint32_t status_code;
    std::vector<H2Server_Response_Header> additonalHeaders;
    std::vector<std::string> tokenizedPayload;
    std::vector<Argument> payload_arguments;
    std::shared_ptr<lua_State> luaState;
    std::string luaScript;
    bool lua_offload;
    double throttle_ratio;
    std::string name;
    uint32_t weight;
    size_t response_index;
    explicit H2Server_Response(const Schema_Response_To_Return& resp, size_t index)
    {
        status_code = resp.status_code;
        name = resp.name;
        weight = resp.weight;
        tokenizedPayload = tokenize_string(resp.payload.msg_payload, resp.payload.placeholder);
        for (auto& arg : resp.payload.arguments)
        {
            payload_arguments.emplace_back(Argument(arg));
        }
        if (tokenizedPayload.size() - payload_arguments.size() != 1)
        {
            std::cerr << "number of placeholders does not match number of arguments:" << staticjson::to_pretty_json_string(
                          resp) << std::endl;
            exit(1);
        }
        for (auto& header: resp.additonalHeaders)
        {
            additonalHeaders.emplace_back(H2Server_Response_Header(header));
        }
        luaScript = resp.luaScript;
        if (luaScript.size())
        {
            std::ifstream f(resp.luaScript);
            if (f.good())
            {
                std::string luaScriptStr((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                luaScript = luaScriptStr;
            }
        }

        if (luaScript.size())
        {
            luaState = std::shared_ptr<lua_State>(luaL_newstate(), &lua_close);
            luaL_openlibs(luaState.get());
            init_new_lua_state_with_common_apis(luaState.get());
            luaL_dostring(luaState.get(), resp.luaScript.c_str());
            lua_getglobal(luaState.get(), customize_response.c_str());
            if (!lua_isfunction(luaState.get(), -1))
            {
                lua_settop(luaState.get(), 0);
                std::cerr<<"required function not present or ill-formed: "<<customize_response<<std::endl;
                luaState.reset();
                exit(1);
            }
            lua_settop(luaState.get(), 0);
        }
        lua_offload = resp.lua_offload;
        throttle_ratio = resp.throttle_ratio;
        response_index = index;
    }

    // TODO: add trailer_response support
    bool update_response_with_lua(const std::multimap<std::string, std::string>& req_headers,
                                            const std::string& req_body,
                                            std::map<std::string, std::string>& resp_headers,
                                            std::map<std::string, std::string>& trailers,
                                            std::string& response_body) const
    {
        bool retCode = true;
        auto L = luaState.get();
        if (!L)
        {
            return retCode;
        }
        lua_getglobal(L, customize_response.c_str());
        if (lua_isfunction(L, -1))
        {
            lua_createtable(L, 0, req_headers.size());

            std::set<std::string> req_header_names;
            for (auto& header : req_headers)
            {
                req_header_names.insert(header.first);
            }
            for (auto& header_name : req_header_names)
            {
                lua_pushlstring(L, header_name.c_str(), header_name.size());
                std::string header_values;
                auto range = req_headers.equal_range(header_name);
                for (auto iter = range.first; iter != range.second; ++iter)
                {
                    if (header_values.size())
                    {
                        header_values.append(";");
                    }
                    header_values.append(iter->second);
                }

                lua_pushlstring(L, header_values.c_str(), header_values.size());
                lua_rawset(L, -3);
            }

            lua_pushlstring(L, req_body.c_str(), req_body.size());

            lua_createtable(L, 0, resp_headers.size());
            for (auto& header : resp_headers)
            {
                lua_pushlstring(L, header.first.c_str(), header.first.size());
                lua_pushlstring(L, header.second.c_str(), header.second.size());
                lua_rawset(L, -3);
            }
            
            std::string status_code_string = std::to_string(status_code);
            lua_pushlstring(L, status.c_str(), status.size());
            lua_pushlstring(L, status_code_string.c_str(), status_code_string.size());
            lua_rawset(L, -3);
            resp_headers.clear();
            trailers.clear();

            lua_pushlstring(L, response_body.c_str(), response_body.size());

            lua_pcall(L, 4, 2, 0);
            int top = lua_gettop(L);
            for (int i = 0; i < top; i++)
            {
                switch (lua_type(L, -1))
                {
                    case LUA_TSTRING:
                    {
                        size_t len;
                        const char* str = lua_tolstring(L, -1, &len);
                        response_body.assign(str, len);
                        break;
                    }
                    case LUA_TTABLE:
                    {
                        std::map<std::string, std::string>* table = &trailers;
                        if (table->size())
                        {
                            table = &resp_headers;
                        }
                        lua_pushnil(L);
                        while (lua_next(L, -2) != 0)
                        {
                            size_t len;
                            /* uses 'key' (at index -2) and 'value' (at index -1) */
                            if ((LUA_TSTRING != lua_type(L, -2)) || (LUA_TSTRING != lua_type(L, -1)))
                            {
                                std::cerr << "invalid http headers returned from lua function customize_response" << std::endl;
                            }
                            const char* k = lua_tolstring(L, -2, &len);
                            std::string key(k, len);
                            const char* v = lua_tolstring(L, -1, &len);
                            std::string value(v, len);
                            (*table)[key] = value;
                            /* removes 'value'; keeps 'key' for next iteration */
                            lua_pop(L, 1);
                        }
                        break;
                    }
                    default:
                    {
                        std::cerr << "error occured in lua function customize_response" << std::endl;
                        retCode = false;
                        break;
                    }
                }
                lua_pop(L, 1);
            }
            if (resp_headers.empty())
            {
                std::swap(trailers, resp_headers);
            }
        }
        else
        {
            lua_settop(L, 0);
            retCode = false;
        }
        return retCode;
    }

    std::string produce_payload(H2Server_Request_Message& msg) const
    {
        std::string payload;
        for (size_t index = 0; index < tokenizedPayload.size(); index++)
        {
            payload.append(tokenizedPayload[index]);
            if (index < payload_arguments.size())
            {
                payload.append(payload_arguments[index].getValue(msg));
            }
        }
        return payload;
    }

    std::pair<std::string, std::string> produce_header(const H2Server_Response_Header& header_with_var, H2Server_Request_Message& msg) const
    {
        std::string header_with_value;
        for (size_t index = 0; index < header_with_var.tokenizedHeader.size(); index++)
        {
            header_with_value.append(header_with_var.tokenizedHeader[index]);
            if (index < header_with_var.header_arguments.size())
            {
                header_with_value.append(header_with_var.header_arguments[index].getValue(msg));
            }
        }

        size_t t = header_with_value.find(":", 1);
        std::string header_name = header_with_value.substr(0, t);
        std::string header_value = header_with_value.substr(t + 1);
        /*
        header_value.erase(header_value.begin(), std::find_if(header_value.begin(), header_value.end(),
                                                              [](unsigned char ch)
        {
            return !std::isspace(ch);
        }));
        */
        return std::make_pair<std::string, std::string>(std::move(header_name), std::move(header_value));
    }

    std::map<std::string, std::string> produce_headers(H2Server_Request_Message& msg) const
    {
        std::map<std::string, std::string> headers;

        for (auto& header_with_var : additonalHeaders)
        {
            auto header_with_value = produce_header(header_with_var, msg);

            headers.insert(header_with_value);
        }
        return headers;
    }

    bool is_response_throttled() const
    {
        if (throttle_ratio > 0.)
        {
            const auto scale = 10000;
            const auto max_range = 100;
            static thread_local uint64_t scaled_throttle_ratio = static_cast<uint64_t>(throttle_ratio * scale);
            static thread_local std::random_device                  rand_dev;
            static thread_local std::mt19937                        generator(rand_dev());
            static thread_local std::uniform_int_distribution<uint64_t>  distr(0, max_range * scale - 1);

            auto number = distr(generator);
            if (number < scaled_throttle_ratio)
            {
                return true;
            }
        }
        return false;
    }
};


#endif

