#ifndef H2SERVER_REQUEST_MATCH_H
#define H2SERVER_REQUEST_MATCH_H


#include <vector>
#include <list>
#include <map>
#include <set>
#include <regex>

#include <rapidjson/pointer.h>
#include <rapidjson/document.h>

#include "H2Server_Config_Schema.h"
#include "H2Server_Response.h"
#include "H2Server_Request_Message.h"

struct ci_less
{
    // case-independent (ci) compare_less binary function
    struct nocase_compare
    {
        bool operator()(const unsigned char& c1, const unsigned char& c2) const
        {
            return tolower(c1) < tolower(c2);
        }
    };
    bool operator()(const std::string& s1, const std::string& s2) const
    {
        return std::lexicographical_compare
               (s1.begin(), s1.end(),     // source range
                s2.begin(), s2.end(),     // dest range
                nocase_compare());   // comparison
    }
};

class Match_Rule
{
public:
    enum Match_Type
    {
        EQUALS_TO = 0,
        START_WITH,
        END_WITH,
        CONTAINS,
        REGEX_MATCH
    };

    Match_Type match_type;
    std::string header_name;
    std::string json_pointer;
    std::string object;
    mutable uint64_t unique_id;
    std::regex reg_exp;
    Match_Rule(const Schema_Header_Match& header_match)
    {
        object = header_match.input;
        header_name = header_match.header;
        json_pointer = "";
        match_type = string_to_match_type[header_match.matchType];
        try
        {
            if (REGEX_MATCH == match_type)
            {
                reg_exp.assign(object, std::regex_constants::ECMAScript|std::regex_constants::optimize);
            }
        }
        catch (std::regex_error e)
        {
            std::cerr<<"invalid reg exp: "<<object<<" reason: "<<e.what()<<std::endl;
        }
    }
    Match_Rule(const Schema_Payload_Match& payload_match)
    {
        object = payload_match.input;
        header_name = "";
        json_pointer = payload_match.jsonPointer;
        match_type = string_to_match_type[payload_match.matchType];
        try
        {
            if (REGEX_MATCH == match_type)
            {
                reg_exp.assign(object, std::regex_constants::ECMAScript|std::regex_constants::optimize);
            }
        }
        catch (std::regex_error e)
        {
            std::cerr<<"invalid reg exp: "<<object<<" "<<e.what()<<std::endl;
        }
    }

    std::map<std::string, Match_Type> string_to_match_type {{"EqualsTo", EQUALS_TO}, {"StartsWith", START_WITH}, {"EndsWith", END_WITH}, {"Contains", CONTAINS}, {"RegexMatch", REGEX_MATCH}};

    bool match(const std::string& subject, Match_Type verb, const std::string& object) const
    {
        if (debug_mode)
        {
            std::cout<<"subject: "<<subject<<", match type: "<<verb<<", object: "<<object<<std::endl;
        }
        switch (verb)
        {
            case EQUALS_TO:
            {
                return (subject == object);
            }
            case START_WITH:
            {
                return (subject.find(object) == 0);
            }
            case END_WITH:
            {
                return (subject.size() >= object.size() && 0 == subject.compare(subject.size() - object.size(), object.size(), object));
            }
            case CONTAINS:
            {
                return (subject.find(object) != std::string::npos);
            }
            case REGEX_MATCH:
            {
                return std::regex_match(subject, reg_exp);
            }
        }
        return false;
    }

    bool match(const std::string& subject) const
    {
        return match(subject, match_type, object);
    }

    bool match(const rapidjson::Document& d) const
    {
        return match(getValueFromJsonPtr(d, json_pointer), match_type, object);
    }

    bool match(H2Server_Request_Message& request) const
    {
        if (request.match_result.count(unique_id))
        {
            return request.match_result[unique_id];
        }
        else
        {
            bool matched = false;
            if (header_name.size())
            {
                auto range = request.headers.equal_range(header_name);
                for (auto iter = range.first; iter != range.second; ++iter)
                {
                    auto& header_val = iter->second;
                    matched = match(header_val);
                    if (matched)
                    {
                        break;
                    }
                }
            }
            else
            {
                request.decode_json_if_not_yet();
                matched = match(request.json_payload);
            }
            request.match_result[unique_id] = matched;
            return matched;
        }
    }

    bool match_header(const std::map<std::string, std::string, ci_less>& response_headers) const
    {
        auto it = response_headers.find(header_name);
        if (it != response_headers.end())
        {
            return match(it->second, match_type, object);
        }
        return false;
    }

    bool match_header(const std::vector<std::map<std::string, std::string, ci_less>>& response_headers) const
    {
        for (auto& response_header_frame: response_headers)
        {
            auto it = response_header_frame.find(header_name);
            if (it != response_header_frame.end())
            {
                return match(it->second, match_type, object);
            }
        }
        return false;
    }

    bool match_json_doc(const rapidjson::Document& d) const
    {
        return match(getValueFromJsonPtr(d, json_pointer), match_type, object);
    }

    bool match(const std::map<std::string, std::string, ci_less>& response_headers, const rapidjson::Document& d)
    {
        if (header_name.size())
        {
            return match_header(response_headers);
        }
        else
        {
            return match_json_doc(d);
        }
    }

    bool match(const std::vector<std::map<std::string, std::string, ci_less>>& response_headers, const rapidjson::Document& d)
    {
        if (header_name.size())
        {
            return match_header(response_headers);
        }
        else
        {
            return match_json_doc(d);
        }
    }

    bool operator<(const Match_Rule& rhs) const
    {
        if (!header_name.empty() && rhs.header_name.empty())
        {
            return false;
        }
        else if (header_name.empty() && !rhs.header_name.empty())
        {
            return true;
        }
        else if ((match_type == EQUALS_TO) && (rhs.match_type != EQUALS_TO))
        {
            return false;
        }
        else if ((rhs.match_type == EQUALS_TO) && (match_type != EQUALS_TO))
        {
            return true;
        }
        else
        {
            std::string mine = std::to_string(match_type);
            mine.append(header_name);
            mine.append(json_pointer);
            mine.append(object);

            std::string other = std::to_string(rhs.match_type);
            other.append(rhs.header_name);
            other.append(rhs.json_pointer);
            other.append(rhs.object);
            return (mine < other);
        }
    }

};


class H2Server_Request
{
public:
    std::set<Match_Rule> match_rules;
    std::string name;
    size_t request_index;

    H2Server_Request(const Schema_Request_Match& request_match, size_t index)
    {
        for (auto& schema_header_match : request_match.header_match)
        {
            match_rules.emplace(Match_Rule(schema_header_match));
        }

        for (auto& schema_payload_match : request_match.payload_match)
        {
            match_rules.emplace(Match_Rule(schema_payload_match));
        }
        name = request_match.name;
        request_index = index;
    }

    bool match(H2Server_Request_Message& request) const
    {
        for (auto match_rule = match_rules.rbegin(); match_rule != match_rules.rend(); match_rule++)
        {
            if (!match_rule->match(request))
            {
                return false;
            }
        }
        return true;
    }
    bool operator<(const H2Server_Request& rhs) const
    {
        if (match_rules.size() < rhs.match_rules.size())
        {
            return true;
        }
        else if (match_rules.size() > rhs.match_rules.size())
        {
            return false;
        }
        else
        {
            auto match = match_rules.rbegin();
            auto other_match = rhs.match_rules.rbegin();
            while (match != match_rules.rend() && other_match != rhs.match_rules.rend())
            {
                if (*match < *other_match)
                {
                    return true;
                }
                else if (*other_match < *match)
                {
                    return false;
                }
                else
                {
                    match++;
                    other_match++;
                }
            }
        }
        return false;
    }
};



#endif
