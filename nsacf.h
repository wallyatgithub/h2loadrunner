#ifndef NSACF_H
#define NSACF_H
#include <set>
#include <map>
#include <vector>
#include <iostream>
#include <fstream>
#include <cstring>
#include <numeric>
#include <shared_mutex>
#include <stack>
#include <unordered_set>
#include "staticjson/document.hpp"
#include "staticjson/staticjson.hpp"
#include "rapidjson/pointer.h"
#include "rapidjson/schema.h"
#include "rapidjson/prettywriter.h"

class Nssai
{
public:
    std::string sst;
    std::string sd;
    void staticjson_init(staticjson::ObjectHandler* h)
    {
        h->add_property("sst", &this->sst);
        h->add_property("sd", &this->sd);
    }
    std::string to_string()
    {
        return sst + "-" + sd;
    }
    void from_string(const std::string& source)
    {
        auto pos = source.find("-");
        if ((pos != std::string::npos) && (pos != (source.size() -1)))
        {
            sst = source.substr(0, pos);
            sd = source.substr(pos + 1, std::string::npos);
        }
    }
};

class PlmnId
{
public:
    std::string mcc;
    std::string mnc;
    void staticjson_init(staticjson::ObjectHandler* h)
    {
        h->add_property("mcc", &this->mcc);
        h->add_property("mnc", &this->mnc);
    }
};


class AcuOperationItem
{
public:
    std::string updateFlag;
    Nssai snssai;
    PlmnId plmnId;
    void staticjson_init(staticjson::ObjectHandler* h)
    {
        h->add_property("updateFlag", &this->updateFlag);
        h->add_property("snssai", &this->snssai);
        h->add_property("plmnId", &this->plmnId, staticjson::Flags::Optional);
    }
};

class UeACRequestInfo
{
public:
    std::string supi;
    std::string anType;
    std::string additionalAnType;
    std::vector<AcuOperationItem> acuOperationList;
    void staticjson_init(staticjson::ObjectHandler* h)
    {
        h->add_property("supi", &this->supi);
        h->add_property("anType", &this->anType);
        h->add_property("acuOperationList", &this->acuOperationList);
        h->add_property("additionalAnType", &this->additionalAnType, staticjson::Flags::Optional);
    }
};

class UeACRequestData
{
public:
    std::vector<UeACRequestInfo> ueACRequestInfo;
    std::string nfId;
    std::string nfType;
    std::string eacNotificationUri;
    void staticjson_init(staticjson::ObjectHandler* h)
    {
        h->add_property("ueACRequestInfo", &this->ueACRequestInfo);
        h->add_property("nfId", &this->nfId);
        h->add_property("nfType", &this->nfType, staticjson::Flags::Optional);
        h->add_property("eacNotificationUri", &this->eacNotificationUri, staticjson::Flags::Optional);
    }
};

class AcuFailureItem
{
public:
    Nssai snssai;
    std::string reason;
    PlmnId plmnId;
    std::string pduSessionId;
    unsigned reasonFlag = staticjson::Flags::Optional | staticjson::Flags::IgnoreWrite;
    unsigned plmnIdFlag = staticjson::Flags::Optional | staticjson::Flags::IgnoreWrite;
    unsigned pduSessionIdFlag = staticjson::Flags::Optional | staticjson::Flags::IgnoreWrite;
    void staticjson_init(staticjson::ObjectHandler* h)
    {
        h->add_property("snssai", &this->snssai);
        h->add_property("reason", &this->reason, reasonFlag);
        h->add_property("plmnId", &this->plmnId, plmnIdFlag);
        h->add_property("pduSessionId", &this->pduSessionId, pduSessionIdFlag);
    }
};

class UeACResponseData
{
public:
    std::map<std::string, std::vector<AcuFailureItem>> acuFailureList;
    void staticjson_init(staticjson::ObjectHandler* h)
    {
        h->add_property("acuFailureList", &this->acuFailureList);
    }
};

#endif
