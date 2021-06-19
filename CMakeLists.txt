cmake_minimum_required(VERSION 3.0)
project(h2loadrunner VERSION 0.1)

set(CMAKE_VERBOSE_MAKEFILE ON)

set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake" ${CMAKE_MODULE_PATH})

add_subdirectory(third-party)
find_package(OpenSSL 1.0.1)
find_package(Libev 4.11)
find_package(LuaJIT)

include(GNUInstallDirs)
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

file(GLOB c_sources *.c)
set_source_files_properties(${c_sources} PROPERTIES
  COMPILE_FLAGS "${WARNCFLAGS}")
file(GLOB cxx_sources *.cc)
set_source_files_properties(${cxx_sources} PROPERTIES
  COMPILE_FLAGS "${WARNCXXFLAGS} ${CXX1XCXXFLAGS}")


include_directories(
  "${CMAKE_CURRENT_SOURCE_DIR}/"
  "${CMAKE_CURRENT_SOURCE_DIR}/includes"
  "${CMAKE_CURRENT_SOURCE_DIR}/third-party"
  "${CMAKE_CURRENT_SOURCE_DIR}/third-party/llhttp/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/third-party/StaticJSON/include"
  "${LUA_INCLUDE_DIR}"
)

link_libraries(
  nghttp2
  ${LIBEV_LIBRARIES}
  ${OPENSSL_LIBRARIES}
  pthread
  ${LUA_LIBRARIES}
)

set (CMAKE_INCLUDE_CURRENT_DIR ON)

add_definitions(-DHAVE_CONFIG_H)
configure_file(cmakeconfig.h.in config.h)

# h2load
set(H2LOAD_SOURCES
  util.cc
  http2.cc h2load.cc
  h2load_utils.cc
  h2load_Client.cc
  h2load_Worker.cc
  timegm.c
  tls.cc
  h2load_http2_session.cc
  h2load_http1_session.cc
)

add_executable(h2loadrunner ${H2LOAD_SOURCES} $<TARGET_OBJECTS:llhttp>
  $<TARGET_OBJECTS:url-parser> $<TARGET_OBJECTS:staticjson>)

install(TARGETS h2loadrunner
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")


