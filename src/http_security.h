#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/address_v4.hpp>

namespace http::security {
  inline constexpr std::size_t max_request_bytes = 16 * 1024 * 1024;

  template<class Config>
  void configure_request_limits(Config &config) {
    config.max_request_streambuf_size = max_request_bytes;
    config.timeout_content = 30;
  }

  inline bool is_loopback(std::string_view remote_address) {
    boost::system::error_code ec;
    auto address = boost::asio::ip::make_address(std::string(remote_address), ec);
    if (ec) {
      return false;
    }
    if (address.is_v6() && address.to_v6().is_v4_mapped()) {
      return boost::asio::ip::make_address_v4(boost::asio::ip::v4_mapped, address.to_v6()).is_loopback();
    }
    return address.is_loopback();
  }

  inline std::string log_text(std::string_view text) {
    std::string result(text.substr(0, 512));
    std::replace_if(result.begin(), result.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; }, ' ');
    return result;
  }

  inline std::string log_path(std::string_view path) {
    path = path.substr(0, path.find('?'));
    for (const auto prefix : {std::string_view {"/join/"}, std::string_view {"/api/join/"}}) {
      if (path.starts_with(prefix)) {
        const auto end = path.find('/', prefix.size());
        return std::string(prefix) + "[REDACTED]" + (end == std::string_view::npos ? "" : log_text(path.substr(end)));
      }
    }
    return log_text(path);
  }

  inline std::string log_header(std::string_view name, std::string_view value) {
    // An allowlist also protects credentials carried by future/custom headers.
    if (boost::iequals(name, "Content-Length") || boost::iequals(name, "Content-Type")) {
      return log_text(value);
    }
    return "[REDACTED]";
  }
}  // namespace http::security
