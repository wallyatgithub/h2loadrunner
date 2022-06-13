#ifndef H2SERVER_MESSAGE_H
#define H2SERVER_MESSAGE_H

#include <rapidjson/pointer.h>
#include <rapidjson/document.h>
#include <vector>
#include <list>
#include <map>
#include <nghttp2/asio_http2_server.h>
#include "asio_server_response.h"
#include "asio_server_request.h"

#include "H2Server_Config_Schema.h"


class H2Server_Request_Message
{
public:
    std::multimap<std::string, std::string> headers;
    rapidjson::Document  json_payload;
    const std::string* json_payload_string;
    std::map<size_t, bool> match_result;
    H2Server_Request_Message(const nghttp2::asio_http2::server::asio_server_request& req)
    {
        json_payload_string = &(req.unmutable_payload());
        std::string path_header_name = ":path";
        std::string header_val = req.uri().path;
        if (req.uri().raw_query.size())
        {
            header_val.append("?").append(req.uri().raw_query);
        }
        headers.insert(std::make_pair(path_header_name, header_val));
        std::string method_header_name = ":method";
        headers.insert(std::make_pair(method_header_name, req.method()));
        std::string scheme_header_name = ":scheme";
        headers.insert(std::make_pair(scheme_header_name, req.uri().scheme));
        std::string authority_header_name = ":authority";
        headers.insert(std::make_pair(authority_header_name, req.uri().host));
        for (auto& hdr : req.header())
        {
            headers.insert(std::make_pair(hdr.first, hdr.second.value));
        }
        if (debug_mode)
        {
            for (auto& header: headers)
            {
                std::cout<<"header: "<<header.first<<", value: "<<header.second<<std::endl;
            }
        }
    }
    void decode_json_if_not_yet()
    {
        if (json_payload_string)
        {
            json_payload.Parse(json_payload_string->c_str());
            json_payload_string == nullptr;
        }
    }
};

#endif

