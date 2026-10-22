#pragma once

#include <map>
#include <string>

namespace http
{
// Decoded query/form parameters. Keys and values are percent-decoded.
using QueryParams = std::map<std::string, std::string>;

// Percent-decode a URL-encoded string: "%XX" -> byte, '+' -> space when
// plus_as_space is true (the application/x-www-form-urlencoded convention).
// Invalid escapes are left as-is rather than rejected, so a malformed query
// degrades instead of falling the request.
std::string urlDecode(const std::string& in, bool plus_as_space = true);

// Percent-encode a string so it is safe as a query key/value. Unreserved
// characters (RFC 3986: A-Z a-z 0-9 - _ . ~) pass through; everything else is
// escaped as %XX.
std::string urlEncode(const std::string& in);

// Parse a query string ("a=1&b=2&flag") into decoded key/value pairs. A key
// Without '=' maps to an empty value. Later duplicates overwrite earlier ones.
QueryParams parseQuery(const std::string& query);
} // namespace http