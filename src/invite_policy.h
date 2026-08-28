/**
 * @file src/invite_policy.h
 * @brief Guest invite rules — expiry, use accounting, PIN lockout, permission clamping.
 *
 * Compiled apart from the store and the HTTP routes so the rules that decide whether a
 * stranger gets into the machine are unit-testable without a network, a filesystem, or
 * the application's globals. Every function here is pure: `now` is a parameter, nothing
 * reads a clock, and nothing does I/O.
 *
 * See docs/guest-invites.md for the design these rules implement.
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace invite::policy {

  using clock_t = std::chrono::system_clock;
  using time_point_t = clock_t::time_point;

  /// How a guest is trying to use the invite. An invite may permit either, both, or
  /// neither — "neither" being a link the owner has effectively parked.
  enum class mode_e {
    browser,  ///< redeem into a /webrtc session
    pairing,  ///< complete a native Moonlight pairing
  };

  /// Why a redemption was refused. Distinguished internally so the host can log the real
  /// reason; what is told to the guest is deliberately coarser (see public_reason).
  enum class result_e {
    ok,
    unknown,      ///< no invite with that token
    revoked,      ///< owner turned it off
    expired,      ///< past expires_at
    exhausted,    ///< max_uses reached
    locked_out,   ///< too many wrong PINs, still inside the lockout window
    bad_pin,      ///< wrong PIN, not yet locked out
    not_allowed,  ///< this invite does not offer the mode being attempted
  };

  /// Wrong PINs tolerated before the first lockout. Generous because a guest is typing a
  /// code read off a chat message, and the PIN is only the *second* factor — an attacker
  /// without the token cannot reach this check at all.
  inline constexpr int lockout_threshold = 5;

  /// Lockout doubles with each threshold crossed, from this base, so a patient attacker
  /// is pushed into hours while a fat-fingered guest waits under a minute.
  inline constexpr std::chrono::seconds lockout_base {30};

  /// Ceiling on the doubling, so a lockout can never become a silent permanent ban the
  /// owner cannot explain.
  inline constexpr std::chrono::seconds lockout_max {3600};

  struct invite_t {
    std::string id;     ///< uuid, owner-facing
    std::string label;  ///< what the owner calls it
    std::string token;  ///< the secret in the link
    std::string pin;    ///< second factor, sent by another channel

    /// crypto::PERM bitmask. The ceiling for anything this invite grants; kept as a
    /// plain integer so this module does not depend on crypto.h.
    std::uint32_t perm = 0;

    int gamepad_base_slot = 0;  ///< 0-15, so two players do not share a virtual pad
    int app_id = -1;            ///< -1 = whatever is already running

    bool allow_browser = true;
    bool allow_pairing = false;
    bool revoked = false;

    time_point_t created_at {};
    time_point_t expires_at {};   ///< epoch = never expires
    time_point_t locked_until {}; ///< epoch = not locked

    int max_uses = 0;  ///< 0 = unlimited
    int uses = 0;
    int failed_attempts = 0;  ///< consecutive; reset by a correct PIN
  };

  /**
   * @brief Decide whether this invite admits a guest right now. Does not mutate.
   *
   * Checks run cheapest-and-least-informative first: an expired invite is refused as
   * expired whether or not the PIN was right, so the PIN check never becomes an oracle
   * for a link that would be refused anyway.
   */
  result_e evaluate(const invite_t &invite, mode_e mode, std::string_view pin, time_point_t now);

  /**
   * @brief Record the outcome of an evaluate() on the invite.
   *
   * Success bumps `uses` and clears the failure count. A wrong PIN increments the failure
   * count and, on crossing the threshold, sets `locked_until`. Every other refusal is
   * stateless — being refused for an expiry that already passed must not accumulate into
   * a lockout, or an owner extending the expiry would find the link still dead.
   */
  void apply(invite_t &invite, result_e result, time_point_t now);

  /**
   * @brief Reduce a requested permission bitmask to what the invite actually grants.
   *
   * Always an intersection, never a union: a guest that asks for more gets less, and an
   * invite can only ever narrow. The caller passes what it would have used by default,
   * so a bug that forgets to pass the guest's request still cannot widen the grant.
   */
  std::uint32_t clamp_permission(const invite_t &invite, std::uint32_t requested);

  /**
   * @brief Whether the invite is usable at all, ignoring the PIN and the mode.
   *
   * Drives what the landing page shows before anyone types anything, so a guest with a
   * dead link is told so instead of being asked for a PIN that cannot work.
   */
  bool is_live(const invite_t &invite, time_point_t now);

  /// Length-independent comparison, so a wrong PIN reveals nothing through timing.
  bool constant_time_equals(std::string_view a, std::string_view b);

  /**
   * @brief What the guest is told.
   *
   * Deliberately lossy. `unknown`, `revoked` and `expired` all collapse to the same
   * message: telling a stranger that a token was real but expired confirms the token,
   * which is the secret. The owner sees the precise reason in the log.
   */
  std::string public_reason(result_e result);

  /// Seconds a locked-out guest must wait, or 0 when not locked.
  int lockout_remaining_seconds(const invite_t &invite, time_point_t now);

}  // namespace invite::policy
