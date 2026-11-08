#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <openssl/crypto.h>
#include <openssl/sha.h>

namespace confighttp::csrf {
  enum class identity_source : unsigned char { authorization, peer };
  using client_id_t = std::array<unsigned char, SHA256_DIGEST_LENGTH + 1>;

  inline client_id_t client_id(identity_source source, std::string_view value) {
    client_id_t result {};
    result[0] = static_cast<unsigned char>(source);
    if (!SHA256(reinterpret_cast<const unsigned char *>(value.data()), value.size(), result.data() + 1)) {
      throw std::runtime_error("Unable to hash CSRF client identity");
    }
    return result;
  }

  enum class validation { valid, invalid, expired };

  class TokenStore {
  public:
    using clock = std::chrono::steady_clock;
    static constexpr std::size_t token_size = 32;
    static constexpr std::size_t max_entries = 4096;
    static constexpr auto lifetime = std::chrono::hours(1);

    explicit TokenStore(std::function<std::string()> generate,
                        std::function<clock::time_point()> now = [] { return clock::now(); },
                        std::size_t capacity = max_entries):
        _generate(std::move(generate)), _now(std::move(now)), _capacity(std::clamp(capacity, std::size_t {1}, max_entries)) {}

    std::string issue(const client_id_t &identity) {
      std::scoped_lock lock(_mutex);
      const auto now = _now();
      std::erase_if(_entries, [&](const auto &entry) { return entry.second.expires_at <= now; });
      if (const auto existing = _entries.find(identity); existing != _entries.end()) {
        return existing->second.token;
      }
      auto token = _generate();
      if (token.size() != token_size) {
        throw std::runtime_error("Invalid CSRF token length");
      }
      if (_entries.size() >= _capacity) {
        // Admit new clients under pressure instead of locking them out for an hour.
        const auto oldest = std::min_element(_entries.begin(), _entries.end(), [](const auto &left, const auto &right) {
          return left.second.expires_at < right.second.expires_at;
        });
        _entries.erase(oldest);
      }
      _entries.emplace(identity, Entry {token, now + lifetime});
      return token;
    }

    validation validate(const client_id_t &identity, std::string_view candidate) {
      std::scoped_lock lock(_mutex);
      const auto entry = _entries.find(identity);
      if (entry == _entries.end()) {
        return validation::invalid;
      }
      if (entry->second.expires_at <= _now()) {
        _entries.erase(entry);
        return validation::expired;
      }
      return candidate.size() == token_size && CRYPTO_memcmp(entry->second.token.data(), candidate.data(), token_size) == 0 ?
        validation::valid : validation::invalid;
    }

    std::size_t size() const {
      std::scoped_lock lock(_mutex);
      return _entries.size();
    }

  private:
    struct Entry {
      std::string token;
      clock::time_point expires_at;
    };
    std::function<std::string()> _generate;
    std::function<clock::time_point()> _now;
    const std::size_t _capacity;
    mutable std::mutex _mutex;
    std::map<client_id_t, Entry> _entries;
  };
}  // namespace confighttp::csrf
