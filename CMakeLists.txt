cmake_minimum_required(VERSION 3.0)
project(h2loadrunner VERSION 0.1)

set(CMAKE_VERBOSE_MAKEFILE ON)

set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake" ${CMAKE_MODULE_PATH})

set(Boost_USE_STATIC_LIBS ON)

#set (USE_LIBEV ON)

add_subdirectory(third-party)

find_package(OpenSSL 1.0.1)
find_package(Boost 1.53.0 REQUIRED system thread)
find_package(LuaJIT)

find_path(GETOPT_INCLUDE_DIR getopt.h)

find_path(NGHTTP2_INCLUDE_DIR nghttp2/nghttp2.h)

find_library(NGHTTP2_LIBRARY NAMES nghttp2)

if (DEFINED ALPINE_STATIC)
     add_link_options(-static -lmimalloc)
     find_library(EXECINFO_LIBRARY execinfo)
endif()

include(GNUInstallDirs)
# Checks for header files.
# Checks for header files.
include(CheckIncludeFile)
check_include_file("arpa/inet.h"    HAVE_ARPA_INET_H)
check_include_file("fcntl.h"        HAVE_FCNTL_H)
check_include_file("inttypes.h"     HAVE_INTTYPES_H)
check_include_file("limits.h"       HAVE_LIMITS_H)
check_include_file("netdb.h"        HAVE_NETDB_H)
check_include_file("netinet/in.h"   HAVE_NETINET_IN_H)
check_include_file("pwd.h"          HAVE_PWD_H)
check_include_file("sys/socket.h"   HAVE_SYS_SOCKET_H)
check_include_file("sys/time.h"     HAVE_SYS_TIME_H)
check_include_file("syslog.h"       HAVE_SYSLOG_H)
check_include_file("time.h"         HAVE_TIME_H)
check_include_file("unistd.h"       HAVE_UNISTD_H)

include(CheckTypeSize)
# Checks for typedefs, structures, and compiler characteristics.
# AC_TYPE_SIZE_T
check_type_size("ssize_t" SIZEOF_SSIZE_T)
if(SIZEOF_SSIZE_T STREQUAL "")
  # ssize_t is a signed type in POSIX storing at least -1.
  # Set it to "int" to match the behavior of AC_TYPE_SSIZE_T (autotools).
  set(ssize_t int)
endif()
# AC_TYPE_UINT8_T
# AC_TYPE_UINT16_T
# AC_TYPE_UINT32_T
# AC_TYPE_UINT64_T
# AC_TYPE_INT8_T
# AC_TYPE_INT16_T
# AC_TYPE_INT32_T
# AC_TYPE_INT64_T
# AC_TYPE_OFF_T
# AC_TYPE_PID_T
# AC_TYPE_UID_T
# XXX To support inline for crappy compilers, see https://cmake.org/Wiki/CMakeTestInline
# AC_C_INLINE
# XXX is AC_SYS_LARGEFILE still needed for modern systems?
# add_definitions(-D_FILE_OFFSET_BITS=64)

include(CheckStructHasMember)
check_struct_has_member("struct tm" tm_gmtoff time.h HAVE_STRUCT_TM_TM_GMTOFF)

# Check size of pointer to decide we need 8 bytes alignment adjustment.
check_type_size("int *"   SIZEOF_INT_P)
check_type_size("time_t"  SIZEOF_TIME_T)

# Checks for library functions.
include(CheckFunctionExists)
check_function_exists(_Exit     HAVE__EXIT)
check_function_exists(accept4   HAVE_ACCEPT4)
check_function_exists(mkostemp  HAVE_MKOSTEMP)

include(CheckSymbolExists)
# XXX does this correctly detect initgroups (un)availability on cygwin?
check_symbol_exists(initgroups grp.h HAVE_DECL_INITGROUPS)
if(NOT HAVE_DECL_INITGROUPS AND HAVE_UNISTD_H)
  # FreeBSD declares initgroups() in unistd.h
  check_symbol_exists(initgroups unistd.h HAVE_DECL_INITGROUPS2)
  if(HAVE_DECL_INITGROUPS2)
    set(HAVE_DECL_INITGROUPS 1)
  endif()
endif()

if(DEFINED ADDRESS_SANITIZE)
    add_compile_options(-fsanitize=address)
    add_link_options(-fsanitize=address)
endif()

set(C_ARES_INCLUDE
"."
)

if(DEFINED USE_LIBEV)
    find_package(Libev 4.11)

    set(C_ARES_INCLUDE
      "${CMAKE_CURRENT_SOURCE_DIR}/third-party/c-ares/include"
      "${CMAKE_CURRENT_SOURCE_DIR}/third-party/c-ares"
    )

    set(H2LOAD_SOURCE_USING_LIBEV
      libev_client.cc
      libev_worker.cc
    )

    add_definitions(-DUSE_LIBEV=1)

    link_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/third-party/c-ares/lib
    ${CMAKE_CURRENT_SOURCE_DIR}/third-party/c-ares/lib64
    )
endif()

if (NOT DEFINED LIBEV_INCLUDE_DIR)
    set(LIBEV_INCLUDE_DIR
    "."
    )
endif()

IF(WIN32)
    add_definitions(-DNOMINMAX=1)
    #add_definitions(-D_WIN32_WINNT=${ver})
    add_definitions(-DWIN32_LEAN_AND_MEAN)
    #add_definitions(-D_ITERATOR_DEBUG_LEVEL=0)
    find_library(GETOPT_LIBRARY NAMES getopt)
endif()


if(VCPKG_TARGET_TRIPLET MATCHES "${__ARCH}-windows-static")
    if(MSVC)
        add_compile_options(
            $<$<CONFIG:>:/MT>
            $<$<CONFIG:Debug>:/MTd>
            $<$<CONFIG:Release>:/MT>
        )
    endif()
endif()

include_directories(
  "${CMAKE_CURRENT_SOURCE_DIR}/"
  "${CMAKE_CURRENT_SOURCE_DIR}/includes"
  "${CMAKE_CURRENT_SOURCE_DIR}/third-party"
  "${CMAKE_CURRENT_SOURCE_DIR}/third-party/llhttp/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/third-party/StaticJSON/include"
  "${LUA_INCLUDE_DIR}"
  "${LIBEV_INCLUDE_DIR}"
  "${C_ARES_INCLUDE}"
  "${Boost_INCLUDE_DIRS}"
  "${GETOPT_INCLUDE_DIR}"
  "${NGHTTP2_INCLUDE_DIR}"
)

link_libraries(
  ${NGHTTP2_LIBRARY}
  ${LIBEV_LIBRARIES}
  ${OPENSSL_LIBRARIES}
  ${LUA_LIBRARIES}
  ${Boost_LIBRARIES}
  ${GETOPT_LIBRARY}
  ${EXECINFO_LIBRARY}
)

set (CMAKE_INCLUDE_CURRENT_DIR ON)

add_definitions(-DHAVE_CONFIG_H)
configure_file(cmakeconfig.h.in config.h)

set(ASIO_SV_SOURCES
  asio_common.cc
  asio_io_service_pool.cc
  asio_httpx_server.cc
  asio_httpx_server_impl.cc
  asio_server.cc
  asio_server_http2_handler.cc
  asio_server_request.cc
  asio_server_response.cc
  asio_server_stream.cc
  asio_server_serve_mux.cc
  asio_server_request_handler.cc
  asio_server_tls_context.cc
  asio_server_base_handler.cc
  asio_server_http1_handler.cc
  asio_util.cc
)

# h2load
set(H2LOAD_SOURCES
  util.cc
  http2.cc
  h2load.cc
  h2load_utils.cc
  h2load_Config.cc
  h2load_Cookie.cc
  timegm.c
  tls.cc
  h2load_http2_session.cc
  h2load_http1_session.cc
  base_client.cc
  base_worker.cc
  asio_client_connection.cc
  asio_worker.cc
  pb.c
  h2load_lua.cc
  ${H2LOAD_SOURCE_USING_LIBEV}
  ${ASIO_SV_SOURCES}
)

add_executable(h2loadrunner ${H2LOAD_SOURCES} $<TARGET_OBJECTS:llhttp>
  $<TARGET_OBJECTS:url-parser> $<TARGET_OBJECTS:staticjson>)

if(DEFINED USE_LIBEV)
    target_link_libraries(h2loadrunner cares_static)
    target_include_directories(h2loadrunner PUBLIC 
    "${CMAKE_CURRENT_SOURCE_DIR}/third-party/c-ares/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/third-party/c-ares"
    )
endif()

install(TARGETS h2loadrunner
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")


