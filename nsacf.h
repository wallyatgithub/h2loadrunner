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

class AcuOperationItem
{
public:
    std::string updateFlag;
    std::string snssai;
    std::string plmnId;
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
    std::string snssai;
    std::string reason;
    std::string plmnId;
    std::string pduSessionId;
    void staticjson_init(staticjson::ObjectHandler* h)
    {
        h->add_property("snssai", &this->snssai);
        h->add_property("reason", &this->reason, staticjson::Flags::Optional);
        h->add_property("plmnId", &this->plmnId, staticjson::Flags::Optional);
        h->add_property("pduSessionId", &this->pduSessionId, staticjson::Flags::Optional);
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
