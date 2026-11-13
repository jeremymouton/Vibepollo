/**
 * @file src/webrtc_ice_policy.h
 * @brief Which configured ICE servers the host's own peer connections use.
 */
#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace webrtc_ice_policy {

  // libwebrtc's RTCConfiguration holds at most this many ICE servers.
  inline constexpr std::size_t max_host_servers = 8;

  inline bool is_stun_url(std::string_view url) {
    const auto starts_with = [&](std::string_view prefix) {
      if (url.size() <= prefix.size()) {
        return false;
      }
      for (std::size_t i = 0; i < prefix.size(); ++i) {
        const auto c = url[i];
        if ((c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c) != prefix[i]) {
          return false;
        }
      }
      return true;
    };
    return starts_with("stun:") || starts_with("stuns:");
  }

  /**
   * STUN URLs from a browser-style ICE server list (`[{"urls": ...}]`).
   *
   * STUN only: it is what gives a host behind NAT a server-reflexive candidate.
   * TURN credentials in the list are minted for clients, and a host-side relay
   * candidate would only duplicate the path the client's own relay provides.
   */
  inline std::vector<std::string> host_stun_urls(const nlohmann::json &servers) {
    std::vector<std::string> urls;
    if (!servers.is_array()) {
      return urls;
    }
    const auto add = [&](const nlohmann::json &value) {
      if (urls.size() < max_host_servers && value.is_string()) {
        auto url = value.get<std::string>();
        if (is_stun_url(url) && std::find(urls.begin(), urls.end(), url) == urls.end()) {
          urls.push_back(std::move(url));
        }
      }
    };
    for (const auto &server : servers) {
      if (!server.is_object() || !server.contains("urls")) {
        continue;
      }
      const auto &value = server["urls"];
      if (value.is_array()) {
        for (const auto &entry : value) {
          add(entry);
        }
      } else {
        add(value);
      }
    }
    return urls;
  }

}  // namespace webrtc_ice_policy
