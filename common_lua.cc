#include <algorithm>
#include <numeric>
#include <cctype>
#include <mutex>
#include <iterator>
#include <future>
#include <map>
#include <iomanip>
#include <iostream>
#include <fstream>
#include <string>
#include <random>
#include <sstream>

extern "C" {
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
}
#include "common_lua.h"
#include "url-parser/url_parser.h"
#include "util.h"



// global data shared across worker threads
static std::map<std::string, std::string> global_datas[0x100][0x100];
static std::mutex global_data_mutexes[0x100][0x100];

int time_since_epoch(lua_State* L)
{
    auto curr_time_point = std::chrono::steady_clock::now();
    auto ms_since_epoch = std::chrono::duration_cast<std::chrono::milliseconds>(curr_time_point.time_since_epoch()).count();
    lua_pushinteger(L, ms_since_epoch);
    return 1;
}

namespace
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

}

int store_value(lua_State* L)
{
    std::string key;
    std::string value;
    if ((lua_gettop(L) == 2))
    {
        size_t len;
        const char* str = lua_tolstring(L, -1, &len);
        lua_pop(L, 1);
        value.assign(str, len);
        str = lua_tolstring(L, -1, &len);
        key.assign(str, len);
        lua_pop(L, 1);
    }
    else
    {
        std::cerr << __FUNCTION__ << " invalid arguments" << std::endl;
        lua_settop(L, 0);
    }
    uint16_t sum = get_u16_sum(key);
    uint8_t row = sum >> 16;
    uint8_t col = sum & 0xFF;
    std::lock_guard<std::mutex> guard(global_data_mutexes[row][col]);
    auto& global_data = global_datas[row][col];
    global_data[key] = value;
    return 0;
}

int get_value(lua_State* L)
{
    std::string key;
    std::string value;
    if ((lua_gettop(L) == 1))
    {
        size_t len;
        const char* str = lua_tolstring(L, -1, &len);
        key.assign(str, len);
        lua_pop(L, 1);
    }
    else
    {
        std::cerr << __FUNCTION__ << " invalid arguments" << std::endl;
        lua_settop(L, 0);
    }
    uint16_t sum = get_u16_sum(key);
    uint8_t row = sum >> 16;
    uint8_t col = sum & 0xFF;
    std::lock_guard<std::mutex> guard(global_data_mutexes[row][col]);
    auto& global_data = global_datas[row][col];
    auto it = global_data.find(key);
    if (it != global_data.end())
    {
        lua_pushlstring(L, it->second.c_str(), it->second.size());
        return 1;
    }
    else
    {
        return 0;
    }
}

int delete_value(lua_State* L)
{
    std::string key;
    std::string value;
    if ((lua_gettop(L) == 1))
    {
        size_t len;
        const char* str = lua_tolstring(L, -1, &len);
        key.assign(str, len);
        lua_pop(L, 1);
    }
    else
    {
        std::cerr << __FUNCTION__ << " invalid arguments" << std::endl;
        lua_settop(L, 0);
    }

    uint16_t sum = get_u16_sum(key);
    uint8_t row = sum >> 16;
    uint8_t col = sum & 0xFF;
    std::lock_guard<std::mutex> guard(global_data_mutexes[row][col]);
    auto& global_data = global_datas[row][col];
    auto it = global_data.find(key);
    if (it != global_data.end())
    {
        lua_pushlstring(L, it->second.c_str(), it->second.size());
        global_data.erase(it);
        return 1;
    }
    else
    {
        return 0;
    }
}

int generate_uuid(lua_State* L)
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
    lua_pushlstring(L, ss.str().c_str(), ss.str().size());
    return 1;
}

void register_3rd_party_lib_func_to_lua(lua_State* L)
{
    const std::string pb = "pb";
    luaopen_pb(L);
    lua_setglobal(L, pb.c_str());

    const std::string pbio = "pb.io";
    luaopen_pb_io(L);
    lua_setglobal(L, pbio.c_str());

    const std::string pbconv = "pb.conv";
    luaopen_pb_conv(L);
    lua_setglobal(L, pbconv.c_str());

    const std::string pbslice = "pb.slice";
    luaopen_pb_slice(L);
    lua_setglobal(L, pbslice.c_str());

    const std::string pbbuffer = "pb.buffer";
    luaopen_pb_buffer(L);
    lua_setglobal(L, pbbuffer.c_str());

    const std::string lua_rapidJson = "rapidjson";
    luaopen_rapidjson(L);
    lua_setglobal(L, lua_rapidJson.c_str());

    lua_settop(L, 0);
}

int parse_uri(lua_State* L)
{
    const std::string scheme_header = ":scheme";
    const std::string path_header = ":path";
    const std::string authority_header = ":authority";

    std::string uri;
    if ((lua_gettop(L) == 1))
    {
        size_t len;
        const char* str = lua_tolstring(L, -1, &len);
        uri.assign(str, len);
        lua_pop(L, 1);
    }
    else
    {
        std::cerr << __FUNCTION__ << " invalid arguments" << std::endl;
        lua_settop(L, 0);
    }
    if (uri.size())
    {
        http_parser_url u {};
        if (http_parser_parse_url(uri.c_str(), uri.size(), 0, &u) == 0)
        {
            std::string path;
            if (nghttp2::util::has_uri_field(u, UF_PATH))
            {
                path = nghttp2::util::get_uri_field(uri.c_str(), u, UF_PATH).str();
            }
            else
            {
                path = "/";
            }

            if (nghttp2::util::has_uri_field(u, UF_QUERY))
            {
                path += '?';
                path += nghttp2::util::get_uri_field(uri.c_str(), u, UF_QUERY);
            }

            std::string schema;
            std::string host;
            if (nghttp2::util::has_uri_field(u, UF_SCHEMA) && nghttp2::util::has_uri_field(u, UF_HOST))
            {
                schema = nghttp2::util::get_uri_field(uri.c_str(), u, UF_SCHEMA).str();
                host = nghttp2::util::get_uri_field(uri.c_str(), u, UF_HOST).str();
                if (nghttp2::util::has_uri_field(u, UF_PORT))
                {
                    host.append(":").append(nghttp2::util::utos(u.port));
                }
            }
            lua_createtable(L, 0, schema.empty() ? 1 : 3);
            lua_pushlstring(L, path_header.c_str(), path_header.size());
            lua_pushlstring(L, path.c_str(), path.size());
            lua_rawset(L, -3);
            if (schema.size())
            {
                lua_pushlstring(L, scheme_header.c_str(), scheme_header.size());
                lua_pushlstring(L, schema.c_str(), schema.size());
                lua_rawset(L, -3);
                lua_pushlstring(L, authority_header.c_str(), authority_header.size());
                lua_pushlstring(L, host.c_str(), host.size());
                lua_rawset(L, -3);
            }
            return 1;
        }
    }
    return 0;
}


void init_new_lua_state_with_common_apis(lua_State* L)
{
    lua_register(L, "time_since_epoch", time_since_epoch);
    lua_register(L, "store_value", store_value);
    lua_register(L, "get_value", get_value);
    lua_register(L, "delete_value", delete_value);
    lua_register(L, "generate_uuid_v4", generate_uuid);
    lua_register(L, "parse_uri", parse_uri);
    register_3rd_party_lib_func_to_lua(L);
}


