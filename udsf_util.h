#ifndef UDSF_UTIL_H
#define UDSF_UTIL_H

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

namespace udsf
{
h2load::asio_worker* get_worker();

bool send_http2_request(const std::string& method, const std::string& uri,
                               const std::map<std::string, std::string, ci_less>& headers,
                               const std::string& message_body);
}
#endif
