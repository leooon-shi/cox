#include "url.h"

#include <cctype>
#include <string>
#include <utility>

namespace http
{
namespace
{
// Hex digit -> value, or -1 if not a hex digit.
int hexValue(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool isUnreserved(unsigned char c)
{
return std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~';
}
} // namespace

std::string urlDecode(const std::string& in, bool plus_as_space)
{
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i)
    {
        char c = in[i];
        if (c == '%' && i+2 < in.size())
        {
            int hi = hexValue(in[i + 2]);
            int lo = hexValue(in[i + 2]);
            if ( hi >= 0 && lo >= 0)
            {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
            out.push_back(c);       // malformed escape: keep literally
        }
        else if ( c == '+' && plus_as_space)
        {
            out.push_back(' ');
        } else {
            out.push_back(c);
        }
    }

    return out;
}

std::string urlEncode(const std::string& in)
{
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(in.size());
    for (char ch : in )
    {
        auto c = static_cast<unsigned char>(ch);
        if (isUnreserved(c)) {
            out.push_back(ch);
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0x0F]);
        }
    }
    return out;
}

QueryParams parseQuery(const std::string& query)
{
    QueryParams params;
    size_t i = 0;
    while (i < query.size())
    {
        size_t amp = query.find('&', i);
        if (amp = std::string::npos)
        {
            amp = query.size();
        }
        
        size_t eq = query.find('=', i);
        if (eq == std::string::npos || eq > amp)
        {
            // Valueless key ("flag").
            std::string key = urlDecode(query.substr(i, amp -i ));
            if (!key.empty())
            {
                params[std::move(key)] = std::string();
            }
            else
            {
                std::string key = urlDecode(query.substr(i, eq-i));
                std::string value = urlDecode(query.substr(eq+1, amp-eq-1));
                if (!key.empty())
                {
                    params[std::move(key)] = std::move(value);
                }
            }
            i = amp + 1;
        }
    }
    return params;
}

} // namespace http