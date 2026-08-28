/**
 * @file src/invite_policy.cpp
 * @brief Implementation of the guest invite rules.
 */
#include "invite_policy.h"

#include <algorithm>

namespace invite::policy {

  namespace {
    /// An epoch time_point is the "unset" marker for expiry and lockout. Using a sentinel
    /// rather than an optional keeps the record trivially serialisable as two integers.
    bool is_unset(time_point_t t) {
      return t.time_since_epoch().count() == 0;
    }

    /// Lockout length after `crossings` full thresholds of consecutive failures.
    std::chrono::seconds lockout_for(int crossings) {
      auto seconds = lockout_base;
      for (int i = 1; i < crossings && seconds < lockout_max; ++i) {
        seconds *= 2;
      }
      return std::min(seconds, lockout_max);
    }
  }  // namespace

  bool constant_time_equals(std::string_view a, std::string_view b) {
    // Fold the length difference into the accumulator rather than returning early, so
    // the comparison takes the same shape whatever the inputs. Iterating to the longer
    // of the two keeps the loop count independent of where the first difference is.
    unsigned char diff = a.size() == b.size() ? 0 : 1;
    const std::size_t n = std::max(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i) {
      const unsigned char ca = i < a.size() ? static_cast<unsigned char>(a[i]) : 0;
      const unsigned char cb = i < b.size() ? static_cast<unsigned char>(b[i]) : 0;
      diff |= static_cast<unsigned char>(ca ^ cb);
    }
    return diff == 0;
  }

  bool is_live(const invite_t &invite, time_point_t now) {
    if (invite.revoked) {
      return false;
    }
    if (!is_unset(invite.expires_at) && now >= invite.expires_at) {
      return false;
    }
    if (invite.max_uses > 0 && invite.uses >= invite.max_uses) {
      return false;
    }
    return true;
  }

  int lockout_remaining_seconds(const invite_t &invite, time_point_t now) {
    if (is_unset(invite.locked_until) || now >= invite.locked_until) {
      return 0;
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::seconds>(invite.locked_until - now);
    // Round up: reporting 0 while still locked would invite an immediate pointless retry.
    return static_cast<int>(remaining.count()) + 1;
  }

  result_e evaluate(const invite_t &invite, mode_e mode, std::string_view pin, time_point_t now) {
    // Order matters. Everything that is true regardless of what the guest typed is
    // checked before the PIN, so a dead invite never reveals whether the PIN was right —
    // which would turn an expired link into a PIN oracle.
    if (invite.revoked) {
      return result_e::revoked;
    }
    if (!is_unset(invite.expires_at) && now >= invite.expires_at) {
      return result_e::expired;
    }
    if (invite.max_uses > 0 && invite.uses >= invite.max_uses) {
      return result_e::exhausted;
    }

    const bool mode_ok = mode == mode_e::browser ? invite.allow_browser : invite.allow_pairing;
    if (!mode_ok) {
      return result_e::not_allowed;
    }

    if (lockout_remaining_seconds(invite, now) > 0) {
      return result_e::locked_out;
    }

    if (!constant_time_equals(invite.pin, pin)) {
      return result_e::bad_pin;
    }

    return result_e::ok;
  }

  void apply(invite_t &invite, result_e result, time_point_t now) {
    switch (result) {
      case result_e::ok:
        ++invite.uses;
        // A correct PIN clears the record of wrong ones. Consecutive failures are what
        // the lockout is defending against; a guest who eventually gets it right should
        // not carry a penalty into their next session.
        invite.failed_attempts = 0;
        invite.locked_until = {};
        break;

      case result_e::bad_pin: {
        ++invite.failed_attempts;
        if (invite.failed_attempts % lockout_threshold == 0) {
          const int crossings = invite.failed_attempts / lockout_threshold;
          invite.locked_until = now + lockout_for(crossings);
        }
        break;
      }

      // Refusals that do not depend on what was typed leave no trace. Accumulating them
      // would mean an invite refused for an expiry it had already passed would still be
      // locked out after the owner extended it.
      case result_e::unknown:
      case result_e::revoked:
      case result_e::expired:
      case result_e::exhausted:
      case result_e::locked_out:
      case result_e::not_allowed:
        break;
    }
  }

  std::uint32_t clamp_permission(const invite_t &invite, std::uint32_t requested) {
    return invite.perm & requested;
  }

  std::string public_reason(result_e result) {
    switch (result) {
      case result_e::ok:
        return "ok";

      // Collapsed on purpose. Distinguishing them would tell a stranger holding a guessed
      // token that the token itself was real, which is the secret the PIN is protecting.
      case result_e::unknown:
      case result_e::revoked:
      case result_e::expired:
        return "This invite link is no longer valid.";

      case result_e::exhausted:
        return "This invite link has already been used.";
      case result_e::locked_out:
        return "Too many incorrect PINs. Try again shortly.";
      case result_e::bad_pin:
        return "Incorrect PIN.";
      case result_e::not_allowed:
        return "This invite does not allow that way of joining.";
    }
    return "This invite link is no longer valid.";
  }

}  // namespace invite::policy
