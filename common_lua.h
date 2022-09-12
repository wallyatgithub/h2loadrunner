#ifndef COMMON_LUA_H
#define COMMON_LUA_H

extern "C" {
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
}


extern "C"
{
    int time_since_epoch(lua_State* L);

    int store_value(lua_State* L);

    int get_value(lua_State* L);

    int delete_value(lua_State* L);

    int generate_uuid(lua_State* L);


    // from pb.so
    LUALIB_API int luaopen_pb_io(lua_State* L);
    LUALIB_API int luaopen_pb_conv(lua_State* L);
    LUALIB_API int luaopen_pb_buffer(lua_State* L);
    LUALIB_API int luaopen_pb_slice(lua_State* L);
    LUALIB_API int luaopen_pb(lua_State* L);
    LUALIB_API int luaopen_pb_unsafe(lua_State* L);
    //from lua-rapidJson
    LUALIB_API int luaopen_rapidjson(lua_State* L);

}
void register_3rd_party_lib_func_to_lua(lua_State* L);

void init_new_lua_state_with_common_apis(lua_State* L);

#endif
