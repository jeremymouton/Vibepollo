/**
 * @file src/invites.h
 * @brief Guest invite storage — the owner's list of links, and redeeming one.
 *
 * The rules live in invite_policy.h and are pure. This is the part that has state: an
 * in-memory list guarded by a mutex, persisted to invites.json beside the state file.
 *
 * Kept out of nvhttp's own state file on purpose. Invites are written far more often
 * than pairings — every redemption bumps a counter, every wrong PIN bumps another — and
 * a corrupt invite file must never be able to cost the owner their paired clients.
 *
 * See docs/guest-invites.md.
 */
#pragma once

#include "invite_policy.h"

#include <optional>
#include <string>
#include <vector>

namespace invite {

  using policy::invite_t;
  using policy::mode_e;
  using policy::result_e;

  /// What an owner may set when creating or editing an invite. Everything is optional on
  /// a PATCH; on create, the defaults produce a browser-only, gamepad-only invite that
  /// expires in a day, which is the shape almost every invite wants.
  struct spec_t {
    std::optional<std::string> label;
    std::optional<std::uint32_t> perm;
    std::optional<int> gamepad_base_slot;
    std::optional<int> app_id;
    std::optional<bool> allow_browser;
    std::optional<bool> allow_pairing;
    std::optional<bool> revoked;
    std::optional<int> max_uses;
    /// Seconds from now. 0 means "never expires"; absent on PATCH leaves it alone.
    std::optional<long long> expires_in_seconds;
  };

  /// Outcome of a redemption attempt, carrying what the caller needs to act on it.
  struct redemption_t {
    result_e result;
    /// Populated only when result == ok. A copy, so the caller never holds the lock.
    invite_t invite;
    /// Seconds to wait, when refused for a lockout.
    int retry_after_seconds = 0;
  };

  /// Read invites.json. Safe to call when the file does not exist. Called once at start.
  void load();

  /// Every invite, newest first. Includes the token and PIN — this is the owner's view,
  /// and callers must never expose it to a guest.
  std::vector<invite_t> list();

  /// One invite by id, or nothing.
  std::optional<invite_t> find(const std::string &id);

  /// Create an invite, generating its id, token and PIN. Persists before returning.
  invite_t create(const spec_t &spec);

  /// Apply the set fields of @p spec to an existing invite. Persists on success.
  std::optional<invite_t> update(const std::string &id, const spec_t &spec);

  /// New token and PIN for an existing invite, invalidating the link already sent.
  std::optional<invite_t> rotate(const std::string &id);

  /// Forget an invite entirely. Prefer setting revoked, which keeps the refusal explicable.
  bool remove(const std::string &id);

  /**
   * @brief Remember that this invite paired a Moonlight client.
   *
   * Called from the far end of the pairing handshake, where the device finally gets
   * a uuid — several round trips after the invite was redeemed. Without the record
   * the pairing cannot be taken back, and an expiring link would hand out permanent
   * access to anyone who chose the native path.
   */
  void record_paired_device(const std::string &invite_id, const std::string &device_uuid);

  /**
   * @brief Hand back the devices this invite paired, and forget them.
   *
   * Take-and-clear in one locked step so two callers cannot both try to unpair the
   * same device. The actual unpairing happens in the caller: this module does not
   * depend on nvhttp, and calling into it while holding the invite lock would invite
   * a deadlock.
   */
  std::vector<std::string> take_paired_devices(const std::string &invite_id);

  /**
   * @brief Look up an invite by its link token, without redeeming it.
   *
   * For the landing page, which has to say what the link offers before anyone types a
   * PIN. The returned invite still carries its secrets, so callers must project it down
   * to the guest-safe fields.
   */
  std::optional<invite_t> find_by_token(const std::string &token);

  /**
   * @brief Attempt to redeem a link token with a PIN.
   *
   * Evaluates and records the outcome under one lock, so two guests racing on the last
   * use of an invite cannot both win, and persists whatever changed. An unknown token
   * takes the same path as a known one, so a token that does not exist is not answered
   * faster than one that does.
   */
  redemption_t redeem(const std::string &token, mode_e mode, const std::string &pin);


  /**
   * @brief Sessions belonging to redeemed guests.
   *
   * The most important property in this whole feature: a guest credential is never
   * accepted by confighttp's authenticate(). Guests get their own cookie and their own
   * store, and only the handful of routes that explicitly ask for a guest session accept
   * one. That is fail-closed by construction — an admin route added later cannot become
   * reachable by a guest, because a guest credential is not an authentication token
   * anywhere in the system.
   *
   * Reusing the owner's SessionToken with a "guest:" username was considered and
   * rejected for the mirror-image reason: it would make every existing authenticate()
   * call site a place a guest might slip through, and keeping that correct would mean
   * auditing all of them forever.
   *
   * In memory only. A guest session that does not survive a restart costs its holder one
   * re-redeem with a link they already have, which is not worth persisting state for.
   */
  namespace guest {

    inline constexpr std::string_view cookie_name {"__Host-apollo_guest"};

    /// How long a redeemed session lasts. Long enough to finish a game, short enough
    /// that a borrowed laptop does not stay admitted for a week.
    inline constexpr std::chrono::hours session_ttl {8};

    struct session_t {
      std::string invite_id;
      std::string label;  ///< carried for logging, so a guest is nameable in the log
      std::uint32_t perm = 0;
      int gamepad_base_slot = 0;
      int app_id = -1;
      policy::time_point_t expires_at {};
      /// The WebRTC session this guest created, once they have created one.
      /// Signalling is authorised against this and nothing else, so a guest can
      /// drive their own stream and cannot touch anyone else's.
      std::string stream_session_id;
    };

    /// Mint a session for a successfully redeemed invite. Returns the cookie value.
    std::string issue(const invite_t &invite);

    /// Resolve a cookie value, or nothing when unknown or expired.
    std::optional<session_t> lookup(const std::string &token);

    /// Drop a session early — the guest leaving, or the owner revoking.
    void revoke(const std::string &token);

    /// Record the WebRTC session a guest just created, so the signalling routes can
    /// tell "their own stream" from "somebody else's".
    void bind_stream_session(const std::string &token, const std::string &stream_session_id);

    /// Whether this cookie owns this WebRTC session. False for an unknown or expired
    /// cookie, for a guest who has not created a session, and for any mismatch — the
    /// three cases are deliberately indistinguishable to the caller.
    bool owns_stream_session(const std::string &token, const std::string &stream_session_id);

    /// How many guests are connected on this invite right now. Drives the owner's
    /// "someone is in" indicator, which is a different question from `uses` —
    /// that counts redemptions ever, this counts people currently here.
    int active_count(const std::string &invite_id);

    /// Drop every session issued from one invite. Called when the owner revokes it, so
    /// revoking a link also ejects whoever is already using it.
    ///
    /// Ejects them from the CREDENTIAL, not from a stream already running: a
    /// negotiated WebRTC session keeps flowing because nothing re-checks the guest
    /// afterwards. Call stream_sessions_for_invite first and close those too.
    void revoke_for_invite(const std::string &invite_id);

    /// The WebRTC sessions this invite's guests are currently running.
    ///
    /// Must be read BEFORE revoking: revoke_for_invite drops the guest sessions, and
    /// the stream ids live on them.
    std::vector<std::string> stream_sessions_for_invite(const std::string &invite_id);

  }  // namespace guest

}  // namespace invite
