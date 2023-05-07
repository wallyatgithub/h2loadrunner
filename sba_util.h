#ifndef SBA_UTIL_H
#define SBA_UTIL_H

#include <algorithm>
#include <numeric>
#include <cctype>
#include <mutex>
#include <iterator>
#include <future>

#include <iomanip>
#include <iostream>
#include <fstream>
#include <string>

#include "asio_worker.h"
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
const std::string JSON_CONTENT = "application/json";
const std::string COLON  = ":";
const std::string CRLF = "\r\n";

const std::string PATH_DELIMETER = "/";
const std::string QUERY_DELIMETER = "&";
const std::string METHOD_PUT = "put";
const std::string METHOD_POST = "post";
const std::string METHOD_PATCH = "patch";
const std::string METHOD_DELETE = "delete";
const std::string METHOD_GET = "get";

const std::string RESOUCE_RECORDS = "records";
const std::string RESOURCE_BLOCKS = "blocks";

void dummy_callback(const std::vector<std::map<std::string, std::string, ci_less>>& resp_headers, const std::string& resp_payload);

const std::map<std::string, std::string, ci_less> dummy_header;

h2load::asio_worker* get_egress_worker();

bool send_http2_request(const std::string& method, const std::string& uri,
                        h2load::Stream_Close_CallBack callback = dummy_callback,
                        const std::map<std::string, std::string, ci_less>& headers = dummy_header,
                        const std::string& message_body = "");


#endif
