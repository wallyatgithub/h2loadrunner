#ifndef ASIO_UTIL_H
#define ASIO_UTIL_H
#include <algorithm>
#include <numeric>
#include <iostream>
#include <string>
#include <fstream>
#include <thread>
#include <future>
#include <memory>
#include <tuple>
#ifdef _WINDOWS
#include <sdkddkver.h>
#endif
#include <boost/asio/io_service.hpp>
#include <boost/thread/thread.hpp>

#include <nghttp2/asio_http2_server.h>
#include "asio_server_http2_handler.h"
#include "asio_server_stream.h"

#include "H2Server_Config_Schema.h"
#include "H2Server_Request.h"
#include "H2Server.h"

struct ResponseStatistics
{
    uint64_t response_sent = 0;
    uint64_t response_throttled = 0;
};

void start_statistic_thread(std::vector<uint64_t>& totalReqsReceived,
                            std::vector<std::vector<std::vector<ResponseStatistics>>>& respStats,
                            std::vector<uint64_t>& totalUnMatchedResponses,
                            H2Server_Config_Schema& config_schema);

void close_stream(uint64_t& handler_id, int32_t stream_id);

size_t get_req_name_max_size(const H2Server_Config_Schema& config_schema);

size_t get_resp_name_max_size(const H2Server_Config_Schema& config_schema);

void send_response(uint32_t status_code,
                   const std::map<std::string, std::string>& resp_headers,
                   const std::string& resp_payload,
                   uint64_t handler_id,
                   int32_t stream_id,
                   uint64_t& matchedResponsesSent
                  );

void send_response_from_another_thread(boost::asio::io_service* target_io_service,
                                       uint64_t handler_id,
                                       int32_t stream_id,
                                       std::map<std::string, std::string>& resp_headers,
                                       std::string& resp_payload
                                      );

void update_response_with_lua(const H2Server_Response* matched_response,
                              std::multimap<std::string, std::string>& req_headers,
                              std::string& req_payload,
                              std::map<std::string, std::string>& resp_headers,
                              std::string& resp_payload,
                              boost::asio::io_service* ios,
                              uint64_t handler_id,
                              int32_t stream_id,
                              uint64_t& matchedResponsesSent);

void asio_svr_entry(const H2Server_Config_Schema& config_schema,
                         std::vector<uint64_t>& totalReqsReceived,
                         std::vector<uint64_t>& totalUnMatchedResponses,
                         std::vector<std::vector<std::vector<ResponseStatistics>>>& respStats);

std::vector<H2Server>& get_H2Server_match_Instances(std::thread::id thread_id);

std::map<std::string, nghttp2::asio_http2::server::http2*>::iterator get_h2_server_instance(const std::string& thread_id);

void init_H2Server_match_Instances(std::size_t number_of_instances, const std::string& config_schema);

void install_request_callback(std::thread::id thread_id, const std::string& name, Request_Processor request_processor);

/* this will block */
void start_server(const std::string& config_file_name, bool start_stats_thread);

void stop_server(const std::string& thread_id);

#endif
