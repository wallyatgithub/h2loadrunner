
set(LIBLLHTTP_SOURCES
llhttp/src/api.c
llhttp/src/http.c
llhttp/src/llhttp.c
)
add_library(llhttp OBJECT ${LIBLLHTTP_SOURCES})
target_include_directories(llhttp PRIVATE
"${CMAKE_CURRENT_SOURCE_DIR}/llhttp/include"
)
set_target_properties(llhttp PROPERTIES
POSITION_INDEPENDENT_CODE ON
)

set(LIBURL_PARSER_SOURCES
url-parser/url_parser.c
)
add_library(url-parser OBJECT ${LIBURL_PARSER_SOURCES})
set_target_properties(url-parser PROPERTIES
POSITION_INDEPENDENT_CODE ON)

include_directories(
  "${CMAKE_CURRENT_SOURCE_DIR}/"
  "${CMAKE_CURRENT_SOURCE_DIR}/StaticJSON/include"
)

set(LIBSTATICJSON_SOURCE
StaticJSON/src/staticjson.cpp
)
add_library(staticjson OBJECT ${LIBSTATICJSON_SOURCE})
set_target_properties(staticjson PROPERTIES
POSITION_INDEPENDENT_CODE ON)

