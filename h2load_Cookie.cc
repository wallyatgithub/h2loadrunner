#include <set>

#include "h2load_Cookie.h"
#include "config_schema.h"
#include "h2load_utils.h"


namespace h2load
{

std::vector<h2load::Cookie> Cookie::parse_cookie_string(const std::string& cookie_string, const std::string& origin_authority, const std::string& origin_schema)
{
    static std::set<std::string, ci_less> cookie_attributes {"Secure", "HttpOnly", "Expires", "Domain", "Path", "SameSite"};
    std::vector <Cookie> parsed_cookies;

    bool secure_origin = (origin_schema == "https");
    std::vector<std::string> tokens = tokenize_string(cookie_string, "; ");

    for (auto& token: tokens)
    {
        std::vector<std::string> key_value_pair = tokenize_string(token, "=");
        if ((cookie_attributes.count(key_value_pair[0]) == 0) && (key_value_pair.size() == 2))
        {
            parsed_cookies.emplace_back(h2load::Cookie());
            parsed_cookies.back().origin = origin_authority;
            util::inp_strlower(parsed_cookies.back().origin);
            parsed_cookies.back().secure_origin = secure_origin;
            parsed_cookies.back().cookie_key = key_value_pair[0];
            parsed_cookies.back().cookie_value = key_value_pair[1];
            parsed_cookies.back().sameSite = "Lax";
        }
        else if (cookie_attributes.count(key_value_pair[0]) == 1)
        {
            if (parsed_cookies.size() == 0)
            {
                continue;
            }
            if (key_value_pair[0] == "Expires" && key_value_pair.size() == 2)
            {
                parsed_cookies.back().expires = key_value_pair[1];
            }
            else if (key_value_pair[0] == "Secure" && key_value_pair.size() == 1)
            {
                parsed_cookies.back().secure = true;
            }
            else if (key_value_pair[0] == "HttpOnly" && key_value_pair.size() == 1)
            {
                parsed_cookies.back().httpOnly = true;
            }
            else if (key_value_pair[0] == "Domain" && key_value_pair.size() == 2)
            {
                parsed_cookies.back().domain = key_value_pair[1];
                util::inp_strlower(parsed_cookies.back().domain);
            }
            else if (key_value_pair[0] == "Path" && key_value_pair.size() == 2)
            {
                parsed_cookies.back().path = key_value_pair[1];
            }
            else if (key_value_pair[0] == "SameSite" && key_value_pair.size() == 2)
            {
                parsed_cookies.back().sameSite = key_value_pair[1];
            }
        }
        else
        {
            std::cerr<<"invalid token in cookie string:"<<key_value_pair[0]<<std::endl;
        }
    }
    return parsed_cookies;
}

bool Cookie::is_cookie_acceptable(const Cookie& cookie)
{
    if (cookie.cookie_key.find("__Host-") == 0)
    {
        /*
        If a cookie name has this prefix,
        it is accepted in a Set-Cookie header only if
        it is also marked with the Secure attribute,
        was sent from a secure origin,
        does not include a Domain attribute,
        and has the Path attribute set to "/"
        */
        if (!cookie.secure || !cookie.secure_origin || !cookie.domain.empty() || cookie.path != "/")
        {
            return false;
        }
    }
    if (cookie.cookie_key.find("__Secure-") == 0)
    {
        /*
        If a cookie name has this prefix,
        it is accepted in a Set-Cookie header only if
        it is marked with the Secure attribute and was sent from a secure origin
        */
        if (!cookie.secure || !cookie.secure_origin)
        {
            // _Secure- check failed, this cookie is not accepted
            return false;
        }
    }
    if (cookie.sameSite == "None" && !cookie.secure)
    {
        // if SameSite=None then the Secure attribute must also be set
        return false;
    }
    return true;
}

bool Cookie::is_cookie_allowed_to_be_sent(Cookie cookie, const std::string dest_schema,
                                                     const std::string dest_authority,
                                                     const std::string dest_path)
{
    if (cookie.secure && dest_schema == "http")
    {
        return false;
    }
    
    std::string authority = dest_authority;
    util::inp_strlower(authority);

    if (cookie.domain.size())
    {
        std::string dest_host = tokenize_string(authority, ":")[0];
        std::string domain = cookie.domain;
        size_t pos = dest_host.rfind(domain);
        if (pos == std::string::npos || ((dest_host.size() - pos) != domain.size()))
        {
            return false;
        }
    }
    else // domain empty, origin exact match with no sub domain allowed
    {
      std::string origin = cookie.origin;
      if (origin != authority)
      {
          return false;
      }
    }

    if (cookie.path.size() && dest_path.size())
    {
        size_t pos = dest_path.find(cookie.path);
        if (pos != 0)
        {
            return false;
        }
    }

    if (cookie.sameSite.size() && cookie.sameSite != "None")
    {
        if (cookie.origin != authority)
        {
            return false;
        }
        /* not a browser, difference between Strict and Lax is not considered
        else if (cookie.sameSite == "Strict" && previous URL is not same with cookie origin)
        {
            return false;
        }
        */
    }
    return true;
}

std::ostream& operator<<(std::ostream& o, const Cookie& cookie)
{
    o << "Cookie: { "
      << "key:" << cookie.cookie_key
      << ", value:" << cookie.cookie_value
      << ", origin:" << cookie.origin
      << ", secure_origin:" << cookie.secure_origin
      << ", expires:" << cookie.expires
      << ", secure:" << cookie.secure
      << ", httpOnly:" << cookie.httpOnly
      << ", domain:" << cookie.domain
      << ", path:" << cookie.path
      << ", sameSite:" << cookie.sameSite
      << " }";
    return o;
}

}
