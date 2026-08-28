/**
 * @file src/invites.cpp
 * @brief Guest invite storage and redemption.
 */
#include "invites.h"

#include "crypto.h"
#include "file_handler.h"
#include "logging.h"
#include "state_storage.h"
#include "uuid.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace invite {

  namespace fs = std::filesystem;
  using namespace std::literals;

  namespace {
    std::mutex g_mutex;
    std::vector<invite_t> g_invites;
    bool g_loaded = false;

    /// Link tokens use a URL-safe alphabet with no lookalike characters removed: the
    /// token is copied and pasted, never read aloud, so ambiguity does not matter and
    /// the full alphabet buys entropy. 32 characters over 64 symbols is 192 bits.
    constexpr std::string_view token_alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    constexpr std::size_t token_length = 32;

    /// PINs are the opposite: read off a message and typed by hand, so digits only.
    constexpr std::string_view pin_alphabet = "0123456789";
    constexpr std::size_t pin_length = 6;

    constexpr long long default_expiry_seconds = 24 * 60 * 60;

    const std::string &invites_path() {
      static const std::string path = [] {
        const auto state = fs::path(statefile::sunshine_state_path());
        return (state.parent_path() / "invites.json").string();
      }();
      return path;
    }

    /// Caller holds g_mutex.
    void save_locked() {
      nlohmann::json root = nlohmann::json::object();
      nlohmann::json items = nlohmann::json::array();
      for (const auto &invite : g_invites) {
        items.push_back(policy::to_json(invite));
      }
      root["invites"] = items;

      std::lock_guard<std::mutex> state_lock(statefile::state_mutex());
      if (file_handler::write_file(invites_path().c_str(), root.dump(4)) != 0) {
        BOOST_LOG(error) << "Couldn't write "sv << invites_path();
        return;
      }
      // The file holds link tokens and PINs in plaintext so the owner can send them
      // again — see docs/guest-invites.md — so it must not be world-readable even if the
      // directory is.
      std::error_code ec;
      fs::permissions(
        invites_path(),
        fs::perms::owner_read | fs::perms::owner_write,
        fs::perm_options::replace,
        ec
      );
    }

    /// Caller holds g_mutex.
    invite_t *find_locked(const std::string &id) {
      const auto it = std::find_if(g_invites.begin(), g_invites.end(), [&](const invite_t &i) {
        return i.id == id;
      });
      return it == g_invites.end() ? nullptr : &*it;
    }
  }  // namespace

  void load() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_loaded = true;
    g_invites.clear();

    if (!fs::exists(invites_path())) {
      return;
    }

    nlohmann::json root;
    try {
      std::ifstream in(invites_path());
      in >> root;
    } catch (const std::exception &e) {
      // Deliberately not quarantining or rewriting: an unreadable invite file is
      // recoverable by hand, and silently replacing it would destroy every live link.
      BOOST_LOG(error) << "Couldn't read "sv << invites_path() << ": "sv << e.what();
      return;
    }

    if (!root.contains("invites") || !root["invites"].is_array()) {
      return;
    }
    for (const auto &node : root["invites"]) {
      if (auto parsed = policy::from_json(node)) {
        g_invites.push_back(std::move(*parsed));
      }
    }
    BOOST_LOG(info) << "Invites: loaded "sv << g_invites.size() << " invite(s)"sv;
  }

  std::vector<invite_t> list() {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto copy = g_invites;
    std::sort(copy.begin(), copy.end(), [](const invite_t &a, const invite_t &b) {
      return a.created_at > b.created_at;
    });
    return copy;
  }

  std::optional<invite_t> find(const std::string &id) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto *found = find_locked(id);
    return found ? std::optional<invite_t> {*found} : std::nullopt;
  }

  std::optional<invite_t> find_by_token(const std::string &token) {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (const auto &invite : g_invites) {
      if (policy::constant_time_equals(invite.token, token)) {
        return invite;
      }
    }
    return std::nullopt;
  }

  invite_t create(const spec_t &spec) {
    invite_t invite;
    invite.id = uuid_util::uuid_t::generate().string();
    invite.token = crypto::rand_alphabet(token_length, token_alphabet);
    invite.pin = crypto::rand_alphabet(pin_length, pin_alphabet);
    invite.created_at = policy::clock_t::now();

    invite.label = spec.label.value_or("Guest"s);
    // Default to gamepad-only plus the two action bits a guest needs to see and start
    // anything. Deliberately not PERM::_all: the safe grant should be what you get by
    // forgetting to choose, not what you get by asking.
    invite.perm = spec.perm.value_or(0u);
    invite.gamepad_base_slot = spec.gamepad_base_slot.value_or(1);
    invite.app_id = spec.app_id.value_or(-1);
    invite.allow_browser = spec.allow_browser.value_or(true);
    invite.allow_pairing = spec.allow_pairing.value_or(false);
    invite.max_uses = spec.max_uses.value_or(0);

    const auto expires_in = spec.expires_in_seconds.value_or(default_expiry_seconds);
    invite.expires_at = expires_in > 0 ? invite.created_at + std::chrono::seconds {expires_in} : policy::time_point_t {};

    std::lock_guard<std::mutex> lock(g_mutex);
    g_invites.push_back(invite);
    save_locked();
    return invite;
  }

  std::optional<invite_t> update(const std::string &id, const spec_t &spec) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto *invite = find_locked(id);
    if (!invite) {
      return std::nullopt;
    }

    if (spec.label) {
      invite->label = *spec.label;
    }
    if (spec.perm) {
      invite->perm = *spec.perm;
    }
    if (spec.gamepad_base_slot) {
      invite->gamepad_base_slot = *spec.gamepad_base_slot;
    }
    if (spec.app_id) {
      invite->app_id = *spec.app_id;
    }
    if (spec.allow_browser) {
      invite->allow_browser = *spec.allow_browser;
    }
    if (spec.allow_pairing) {
      invite->allow_pairing = *spec.allow_pairing;
    }
    if (spec.revoked) {
      invite->revoked = *spec.revoked;
    }
    if (spec.max_uses) {
      invite->max_uses = *spec.max_uses;
    }
    if (spec.expires_in_seconds) {
      const auto seconds = *spec.expires_in_seconds;
      invite->expires_at = seconds > 0 ? policy::clock_t::now() + std::chrono::seconds {seconds} : policy::time_point_t {};
      // Extending an expiry is the owner saying "let them back in", so it also lifts a
      // lockout that would otherwise keep refusing them for the next hour.
      invite->locked_until = {};
      invite->failed_attempts = 0;
    }

    const auto copy = *invite;
    save_locked();
    return copy;
  }

  std::optional<invite_t> rotate(const std::string &id) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto *invite = find_locked(id);
    if (!invite) {
      return std::nullopt;
    }
    invite->token = crypto::rand_alphabet(token_length, token_alphabet);
    invite->pin = crypto::rand_alphabet(pin_length, pin_alphabet);
    // A rotated invite is a fresh one for everyone who had the old link, including
    // whoever was locked out guessing at it.
    invite->locked_until = {};
    invite->failed_attempts = 0;

    const auto copy = *invite;
    save_locked();
    return copy;
  }

  bool remove(const std::string &id) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto before = g_invites.size();
    g_invites.erase(
      std::remove_if(g_invites.begin(), g_invites.end(), [&](const invite_t &i) {
        return i.id == id;
      }),
      g_invites.end()
    );
    if (g_invites.size() == before) {
      return false;
    }
    save_locked();
    return true;
  }

  redemption_t redeem(const std::string &token, mode_e mode, const std::string &pin) {
    const auto now = policy::clock_t::now();

    std::lock_guard<std::mutex> lock(g_mutex);

    invite_t *found = nullptr;
    for (auto &invite : g_invites) {
      if (policy::constant_time_equals(invite.token, token)) {
        found = &invite;
        break;
      }
    }

    if (!found) {
      // Same shape as a real refusal. Returning early here without touching the PIN is
      // fine — the token is 192 bits, so an attacker is not distinguishing timings on a
      // value they cannot reach in the first place.
      return {result_e::unknown, {}, 0};
    }

    const auto result = policy::evaluate(*found, mode, pin, now);
    policy::apply(*found, result, now);

    redemption_t out;
    out.result = result;
    out.retry_after_seconds = policy::lockout_remaining_seconds(*found, now);
    if (result == result_e::ok) {
      out.invite = *found;
    }

    // Persist whichever counter moved. Both success and a wrong PIN change state that
    // must survive a restart, or the use limit and the lockout would both be resettable
    // by bouncing the host.
    if (result == result_e::ok || result == result_e::bad_pin) {
      save_locked();
    }
    return out;
  }

}  // namespace invite
