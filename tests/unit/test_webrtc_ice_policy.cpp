#include "src/webrtc_ice_policy.h"

#include <gtest/gtest.h>

using webrtc_ice_policy::host_stun_urls;

TEST(WebrtcIcePolicy, KeepsOnlyStunUrlsFromStringsAndArrays) {
  const auto servers = nlohmann::json::parse(R"([
    {"urls": "stun:example.com:3480"},
    {"urls": ["turn:example.com:3480", "turn:example.com:3480?transport=tcp", "STUNS:example.com:5349"]},
    {"urls": "turns:example.com:5350", "username": "u", "credential": "c"}
  ])");
  EXPECT_EQ(host_stun_urls(servers), (std::vector<std::string> {"stun:example.com:3480", "STUNS:example.com:5349"}));
}

TEST(WebrtcIcePolicy, IgnoresMalformedEntriesAndDuplicates) {
  const auto servers = nlohmann::json::parse(R"([
    "stun:not-an-object",
    {"url": "stun:wrong-key"},
    {"urls": 42},
    {"urls": "stun:"},
    {"urls": "stun:a"},
    {"urls": ["stun:a", null, "stunx:a"]}
  ])");
  EXPECT_EQ(host_stun_urls(servers), (std::vector<std::string> {"stun:a"}));
  EXPECT_TRUE(host_stun_urls(nlohmann::json::object()).empty());
  EXPECT_TRUE(host_stun_urls(nlohmann::json()).empty());
}

TEST(WebrtcIcePolicy, CapsAtTheLibraryLimit) {
  auto servers = nlohmann::json::array();
  for (int i = 0; i < 12; ++i) {
    servers.push_back({{"urls", "stun:host" + std::to_string(i)}});
  }
  EXPECT_EQ(host_stun_urls(servers).size(), webrtc_ice_policy::max_host_servers);
}
