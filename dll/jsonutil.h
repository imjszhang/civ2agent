#pragma once

#include <string>
#include <windows.h>

inline void JsonEscape(const std::string& in, std::string& out) {
  for (unsigned char c : in) {
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          wsprintfA(buf, "\\u%04x", c);
          out += buf;
        } else {
          out += static_cast<char>(c);
        }
    }
  }
}

inline std::string JsonString(const std::string& in) {
  std::string out;
  out.push_back('"');
  JsonEscape(in, out);
  out.push_back('"');
  return out;
}

inline std::string ErrJson(const char* code, const std::string& reason) {
  return std::string("{\"ok\":false,\"error\":") + JsonString(code) + ",\"reason\":" +
         JsonString(reason) + "}";
}

inline std::string OkJson(const std::string& body_inside_braces) {
  if (body_inside_braces.empty()) return "{\"ok\":true}";
  return std::string("{\"ok\":true,") + body_inside_braces + "}";
}
