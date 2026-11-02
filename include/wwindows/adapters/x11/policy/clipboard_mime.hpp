#pragma once
#include <string_view>
namespace ww::x11::policy {
inline bool is_text_mime(std::string_view mime) noexcept {
  return mime == "text/plain;charset=utf-8" || mime == "UTF8_STRING" || mime == "text/plain" || mime == "STRING" || mime == "TEXT";
}
inline std::string_view preferred_text_mime(const auto& mimes) noexcept {
  for (const auto& mime : mimes) if (is_text_mime(mime)) return mime;
  return {};
}
}
