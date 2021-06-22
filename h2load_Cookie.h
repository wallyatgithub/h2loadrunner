#ifndef H2LOAD_COOKIE_H
#define H2LOAD_COOKIE_H
#include <string>
#include <vector>
#include <iostream>


namespace h2load
{

struct Cookie
{
    std::string origin;
    bool        secure_origin;
    std::string cookie_key;
    std::string cookie_value;
    std::string expires; // parsing supported, but not handled as it is not possible to expire in such short interval during load test
    bool        secure;
    bool        httpOnly; // parsing supported, but not handled as no JavaScript interaction for load test
    std::string domain;
    std::string path;
    std::string sameSite;
    std::string maxAge; // parsing supported, but not handled, same reason with expires

    static std::vector<Cookie> parse_cookie_string(const std::string& cookie_string,
                                                   const std::string& origin_authority,
                                                   const std::string& origin_schema);
    static bool is_cookie_acceptable(const Cookie& cookie);
    static bool is_cookie_allowed_to_be_sent(Cookie cookie, const std::string dest_schema,
                                                     const std::string dest_authority,
                                                     const std::string dest_path);
};

std::ostream& operator<<(std::ostream& o, const Cookie& cookie);

}
#endif
