#ifndef HAR_SCHEMA_H
#define HAR_SCHEMA_H

#include <vector>
#include "staticjson/document.hpp"
#include "staticjson/staticjson.hpp"
#include "rapidjson/schema.h"
#include "rapidjson/prettywriter.h"

class HAR_Header
{
public:
    std::string name;
    std::string value;
    std::string comment;

    void staticjson_init(staticjson::ObjectHandler* h)
    {
        h->add_property("name", &this->name);
        h->add_property("value", &this->value);
        h->add_property("comment", &this->comment, staticjson::Flags::Optional);
    }
};

class HAR_Cookie
{
public:
    std::string name;
    std::string value;
    std::string path;
    std::string domain;
    std::string expires;
    bool httpOnly;
    bool secure;
    std::string comment;

    void staticjson_init(staticjson::ObjectHandler* h)
    {
        h->add_property("name", &this->name);
        h->add_property("value", &this->value);
        h->add_property("path", &this->path, staticjson::Flags::Optional);
        h->add_property("domain", &this->domain, staticjson::Flags::Optional);
        //h->add_property("expires", &this->expires, staticjson::Flags::Optional); // TODO: handle null
        h->add_property("httpOnly", &this->httpOnly, staticjson::Flags::Optional);
        h->add_property("secure", &this->secure, staticjson::Flags::Optional);
        h->add_property("comment", &this->comment, staticjson::Flags::Optional);
    }
};

class HAR_Query_String
{
public:
    std::string name;
    std::string value;
    std::string comment;

    void staticjson_init(staticjson::ObjectHandler* h)
    {
        h->add_property("name", &this->name);
        h->add_property("value", &this->value);
        h->add_property("comment", &this->comment, staticjson::Flags::Optional);
    }
};

class HAR_PostData_Param
{
public:
    std::string name;
    std::string value;
    std::string fileName;
    std::string contentType;
    std::string comment;

    void staticjson_init(staticjson::ObjectHandler* h)
    {
        h->add_property("name", &this->name);
        h->add_property("value", &this->value, staticjson::Flags::Optional);
        h->add_property("fileName", &this->fileName, staticjson::Flags::Optional);
        h->add_property("contentType", &this->contentType, staticjson::Flags::Optional);
        h->add_property("comment", &this->comment, staticjson::Flags::Optional);
    }
};

class HAR_PostData
{
public:
    std::string mimeType;
    std::vector<HAR_PostData_Param> params;
    std::string text;
    std::string comment;
    void staticjson_init(staticjson::ObjectHandler* h)
    {
        h->add_property("mimeType", &this->mimeType);
        h->add_property("params", &this->params, staticjson::Flags::Optional);
        h->add_property("text", &this->text, staticjson::Flags::Optional);
        h->add_property("comment", &this->comment, staticjson::Flags::Optional);
    }
};

class HAR_Request
{
public:
    std::string method;
    std::string url;
    std::string httpVersion;
    std::vector<HAR_Cookie> cookies;
    std::vector<HAR_Header> headers;
    std::vector<HAR_Query_String> queryString;
    HAR_PostData postData;
    int64_t headersSize;
    int64_t bodySize;
    std::string comment;
    void staticjson_init(staticjson::ObjectHandler* h)
    {
        h->add_property("method", &this->method);
        h->add_property("url", &this->url);
        h->add_property("httpVersion", &this->httpVersion);
        h->add_property("cookies", &this->cookies);
        h->add_property("headers", &this->headers);
        h->add_property("queryString", &this->queryString);
        h->add_property("postData", &this->postData, staticjson::Flags::Optional);
        h->add_property("headersSize", &this->headersSize);
        h->add_property("bodySize", &this->bodySize);
        h->add_property("comment", &this->comment, staticjson::Flags::Optional);
    }
};

class HAR_Response_Content
{
public:
    int64_t size;
    int64_t compression;
    std::string mimeType;
    std::string text;
    std::string encoding;
    std::string comment;
    void staticjson_init(staticjson::ObjectHandler* h)
    {
        h->add_property("size", &this->size);
        h->add_property("compression", &this->compression, staticjson::Flags::Optional);
        h->add_property("mimeType", &this->mimeType);
        h->add_property("text", &this->text, staticjson::Flags::Optional);
        h->add_property("encoding", &this->encoding, staticjson::Flags::Optional);
        h->add_property("comment", &this->comment, staticjson::Flags::Optional);
    }
};

class HAR_Response
{
public:
    uint32_t status;
    std::string statusText;
    std::string httpVersion;
    std::vector<HAR_Cookie> cookies;
    std::vector<HAR_Header> headers;
    HAR_Response_Content content;
    std::string redirectURL;
    int64_t headersSize;
    int64_t bodySize;
    std::string comment;
    void staticjson_init(staticjson::ObjectHandler* h)
    {
        h->add_property("status", &this->status);
        h->add_property("statusText", &this->statusText);
        h->add_property("httpVersion", &this->httpVersion);
        h->add_property("cookies", &this->cookies);
        h->add_property("headers", &this->headers);
        h->add_property("content", &this->content);
        h->add_property("redirectURL", &this->redirectURL);
        h->add_property("headersSize", &this->headersSize);
        h->add_property("bodySize", &this->bodySize);
        h->add_property("comment", &this->comment, staticjson::Flags::Optional);
    }
};

class HAR_Cache_BeforeAfter
{
public:
    std::string expires;
    std::string lastAccess;
    std::string eTag;
    int64_t hitCount;
    std::string comment;
    void staticjson_init(staticjson::ObjectHandler* h)
    {
        h->add_property("expires", &this->expires, staticjson::Flags::Optional);
        h->add_property("lastAccess", &this->lastAccess);
        h->add_property("eTag", &this->eTag);
        h->add_property("hitCount", &this->hitCount);
        h->add_property("comment", &this->comment, staticjson::Flags::Optional);
    }
};

class HAR_Cache
{
public:
    HAR_Cache_BeforeAfter beforeRequest;
    HAR_Cache_BeforeAfter afterRequest;
    std::string comment;
    void staticjson_init(staticjson::ObjectHandler* h)
    {
        h->add_property("beforeRequest", &this->beforeRequest, staticjson::Flags::Optional);
        h->add_property("afterRequest", &this->afterRequest, staticjson::Flags::Optional);
        h->add_property("comment", &this->comment, staticjson::Flags::Optional);
    }
};

class HAR_timings
{
public:
    double blocked;
    double dns;
    double connect;
    double send;
    double wait;
    double receive;
    double ssl;
    std::string comment;
    void staticjson_init(staticjson::ObjectHandler* h)
    {
        h->add_property("blocked", &this->blocked, staticjson::Flags::Optional);
        h->add_property("dns", &this->dns, staticjson::Flags::Optional);
        h->add_property("connect", &this->connect, staticjson::Flags::Optional);
        h->add_property("send", &this->send);
        h->add_property("wait", &this->wait);
        h->add_property("receive", &this->receive);
        h->add_property("ssl", &this->ssl, staticjson::Flags::Optional);
        h->add_property("comment", &this->comment, staticjson::Flags::Optional);
    }
};

class HAR_Entry
{
public:
    std::string pageref;
    std::string startedDateTime;
    double time;
    HAR_Request request;
    HAR_Response response;
    HAR_Cache cache;
    HAR_timings timings;
    std::string serverIPAddress;
    std::string connection;
    std::string comment;
    void staticjson_init(staticjson::ObjectHandler* h)
    {
        h->add_property("pageref", &this->pageref, staticjson::Flags::Optional);
        h->add_property("startedDateTime", &this->startedDateTime);
        h->add_property("time", &this->time);
        h->add_property("request", &this->request);
        h->add_property("response", &this->response);
        h->add_property("cache", &this->cache);
        h->add_property("timings", &this->timings);
        h->add_property("serverIPAddress", &this->serverIPAddress, staticjson::Flags::Optional);
        h->add_property("connection", &this->connection, staticjson::Flags::Optional);
        h->add_property("comment", &this->comment, staticjson::Flags::Optional);
    }
};


class HAR_pageTimings
{
public:
    double onContentLoad;
    double onLoad;
    std::string comment;
    void staticjson_init(staticjson::ObjectHandler* h)
    {
        h->add_property("onContentLoad", &this->onContentLoad, staticjson::Flags::Optional);
        // h->add_property("onLoad", &this->onLoad, staticjson::Flags::Optional); // TODO: handle null
        h->add_property("comment", &this->comment, staticjson::Flags::Optional);
    }
};

class HAR_Page
{
public:
    std::string startedDateTime;
    std::string id;
    std::string title;
    HAR_pageTimings pageTimings;
    std::string comment;
    void staticjson_init(staticjson::ObjectHandler* h)
    {
        h->add_property("startedDateTime", &this->startedDateTime);
        h->add_property("id", &this->id);
        h->add_property("title", &this->title);
        h->add_property("pageTimings", &this->pageTimings);
        h->add_property("comment", &this->comment, staticjson::Flags::Optional);
    }
};

class HAR_Creator
{
public:
    std::string name;
    std::string version;
    std::string comment;

    void staticjson_init(staticjson::ObjectHandler* h)
    {
        h->add_property("name", &this->name);
        h->add_property("version", &this->version);
        h->add_property("comment", &this->comment, staticjson::Flags::Optional);
    }
};

class HAR_Browser
{
public:
    std::string name;
    std::string version;
    std::string comment;

    void staticjson_init(staticjson::ObjectHandler* h)
    {
        h->add_property("name", &this->name);
        h->add_property("version", &this->version);
        h->add_property("comment", &this->comment, staticjson::Flags::Optional);
    }
};

class HAR_Log
{
public:
    std::string version;
    HAR_Creator creator;
    HAR_Browser browser;
    std::vector<HAR_Page> pages;
    std::vector<HAR_Entry> entries;
    std::string comment;
    void staticjson_init(staticjson::ObjectHandler* h)
    {
        h->add_property("version", &this->version);
        h->add_property("creator", &this->creator);
        h->add_property("browser", &this->browser, staticjson::Flags::Optional);
        h->add_property("pages", &this->pages, staticjson::Flags::Optional);
        h->add_property("entries", &this->entries);
        h->add_property("comment", &this->comment, staticjson::Flags::Optional);
    }
};

class HAR_File
{
public:
    HAR_Log log;
    void staticjson_init(staticjson::ObjectHandler* h)
    {
        h->add_property("log", &this->log);
    }
};

#endif
