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
#include <unordered_map>

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
#ifndef _WIN32
      // The file holds link tokens and PINs in plaintext so the owner can send them
      // again — see docs/guest-invites.md — so narrow it to the owner even if the
      // directory is not.
      //
      // POSIX only, and deliberately so: on Windows std::filesystem::permissions merely
      // toggles the read-only attribute and grants no access control whatsoever, so
      // calling it there would look like protection while providing none. Windows is
      // handled by statefile::repair_config_permissions(), which puts this file under
      // the same config-directory ACL as sunshine_state.json.
      std::error_code ec;
      fs::permissions(
        invites_path(),
        fs::perms::owner_read | fs::perms::owner_write,
        fs::perm_options::replace,
        ec
      );
#endif
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
    // Narrowing an invite must not leave a wider session already running on it.
    // Sessions snapshot their grant at redemption, so the only way to take one
    // back is to end it; revoking or re-scoping is exactly that intent.
    const bool narrowed = (spec.revoked && *spec.revoked) || spec.perm || spec.gamepad_base_slot || spec.app_id;
    if (narrowed) {
      guest::revoke_for_invite(invite->id);
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
    // The old link is dead, so anything holding a session from it is too.
    guest::revoke_for_invite(invite->id);

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
    guest::revoke_for_invite(id);
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


  namespace guest {

    namespace {
      /// Lock ordering: the invite list's g_mutex is always taken BEFORE this one
      /// — update(), rotate() and remove() all call into here while holding it.
      /// Nothing in this namespace takes g_mutex, which is what keeps that safe.
      std::mutex g_guest_mutex;
      std::unordered_map<std::string, session_t> g_sessions;

      /// Same alphabet and length as a link token: this cookie is the whole credential
      /// for a session, so it gets the same 192 bits.
      constexpr std::size_t guest_token_length = 32;

      /// Caller holds g_guest_mutex. Expired sessions are dropped lazily rather than on a
      /// timer — the map only grows when someone redeems, and every lookup prunes.
      void sweep_locked(policy::time_point_t now) {
        for (auto it = g_sessions.begin(); it != g_sessions.end();) {
          it = it->second.expires_at <= now ? g_sessions.erase(it) : std::next(it);
        }
      }
    }  // namespace

    std::string issue(const invite_t &invite) {
      session_t session;
      session.invite_id = invite.id;
      session.label = invite.label;
      // Snapshot the grant rather than pointing at the invite. Editing an invite must
      // not silently widen a session already in flight; a guest who should lose access
      // is ejected by revoke_for_invite instead, which is explicit.
      session.perm = invite.perm;
      session.gamepad_base_slot = invite.gamepad_base_slot;
      session.app_id = invite.app_id;
      session.expires_at = policy::clock_t::now() + session_ttl;

      auto token = crypto::rand_alphabet(guest_token_length, token_alphabet);

      std::lock_guard<std::mutex> lock(g_guest_mutex);
      sweep_locked(policy::clock_t::now());
      g_sessions.emplace(token, std::move(session));
      return token;
    }

    std::optional<session_t> lookup(const std::string &token) {
      if (token.empty()) {
        return std::nullopt;
      }
      const auto now = policy::clock_t::now();
      std::lock_guard<std::mutex> lock(g_guest_mutex);
      sweep_locked(now);
      const auto it = g_sessions.find(token);
      if (it == g_sessions.end()) {
        return std::nullopt;
      }
      return it->second;
    }

    void bind_stream_session(const std::string &token, const std::string &stream_session_id) {
      if (token.empty() || stream_session_id.empty()) {
        return;
      }
      std::lock_guard<std::mutex> lock(g_guest_mutex);
      prune_expired(policy::clock_t::now());
      if (const auto it = g_sessions.find(token); it != g_sessions.end()) {
        // Last one wins. A guest who reloads the page creates a fresh session and the
        // old one is already being torn down, so there is nothing to keep.
        it->second.stream_session_id = stream_session_id;
      }
    }

    bool owns_stream_session(const std::string &token, const std::string &stream_session_id) {
      if (token.empty() || stream_session_id.empty()) {
        return false;
      }
      std::lock_guard<std::mutex> lock(g_guest_mutex);
      prune_expired(policy::clock_t::now());
      const auto it = g_sessions.find(token);
      return it != g_sessions.end() && it->second.stream_session_id == stream_session_id;
    }

    void revoke(const std::string &token) {
      std::lock_guard<std::mutex> lock(g_guest_mutex);
      g_sessions.erase(token);
    }

    int active_count(const std::string &invite_id) {
      const auto now = policy::clock_t::now();
      std::lock_guard<std::mutex> lock(g_guest_mutex);
      sweep_locked(now);
      int count = 0;
      for (const auto &entry : g_sessions) {
        if (entry.second.invite_id == invite_id) {
          ++count;
        }
      }
      return count;
    }

    void revoke_for_invite(const std::string &invite_id) {
      std::lock_guard<std::mutex> lock(g_guest_mutex);
      for (auto it = g_sessions.begin(); it != g_sessions.end();) {
        it = it->second.invite_id == invite_id ? g_sessions.erase(it) : std::next(it);
      }
    }

  }  // namespace guest

}  // namespace invite
