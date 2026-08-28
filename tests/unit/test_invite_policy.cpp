/**
 * @file tests/unit/test_invite_policy.cpp
 * @brief Unit tests for guest invite admission rules.
 *
 * These rules decide whether a stranger holding a link gets into the machine, so the
 * cases that matter most here are the refusals — and specifically that a refusal never
 * leaks more than it should.
 */
#include "../tests_common.h"

#include <src/invite_policy.h>

namespace {
  namespace policy = invite::policy;

  using policy::mode_e;
  using policy::result_e;

  /// A fixed instant, so every expiry and lockout in these tests is exact.
  policy::time_point_t at(int seconds) {
    return policy::time_point_t {} + std::chrono::seconds {1000000 + seconds};
  }

  /// A healthy browser invite: live, unexpired, unlimited, PIN "123456".
  policy::invite_t make_invite() {
    policy::invite_t invite;
    invite.id = "11111111-2222-3333-4444-555555555555";
    invite.label = "Brother";
    invite.token = "vJ8kQ2mN4pR7sT9wY1bD3fH5jL6nP0qS";
    invite.pin = "123456";
    invite.perm = 0xFFFFFFFFu;
    invite.gamepad_base_slot = 1;
    invite.allow_browser = true;
    invite.allow_pairing = false;
    invite.created_at = at(0);
    invite.expires_at = at(3600);
    return invite;
  }

  TEST(InvitePolicy, AdmitsACorrectPinOnALiveInvite) {
    const auto invite = make_invite();
    EXPECT_EQ(policy::evaluate(invite, mode_e::browser, "123456", at(10)), result_e::ok);
  }

  TEST(InvitePolicy, RefusesAWrongPin) {
    const auto invite = make_invite();
    EXPECT_EQ(policy::evaluate(invite, mode_e::browser, "000000", at(10)), result_e::bad_pin);
  }

  TEST(InvitePolicy, RefusesAnEmptyPin) {
    const auto invite = make_invite();
    EXPECT_EQ(policy::evaluate(invite, mode_e::browser, "", at(10)), result_e::bad_pin);
  }

  TEST(InvitePolicy, RefusesAPinThatIsAPrefixOfTheRealOne) {
    const auto invite = make_invite();
    EXPECT_EQ(policy::evaluate(invite, mode_e::browser, "123", at(10)), result_e::bad_pin);
  }

  TEST(InvitePolicy, RefusesAfterExpiry) {
    const auto invite = make_invite();
    EXPECT_EQ(policy::evaluate(invite, mode_e::browser, "123456", at(3601)), result_e::expired);
  }

  TEST(InvitePolicy, TreatsAnUnsetExpiryAsNeverExpiring) {
    auto invite = make_invite();
    invite.expires_at = {};
    EXPECT_EQ(policy::evaluate(invite, mode_e::browser, "123456", at(999999)), result_e::ok);
  }

  TEST(InvitePolicy, RefusesWhenRevoked) {
    auto invite = make_invite();
    invite.revoked = true;
    EXPECT_EQ(policy::evaluate(invite, mode_e::browser, "123456", at(10)), result_e::revoked);
  }

  TEST(InvitePolicy, RefusesOnceUsesAreExhausted) {
    auto invite = make_invite();
    invite.max_uses = 2;
    invite.uses = 2;
    EXPECT_EQ(policy::evaluate(invite, mode_e::browser, "123456", at(10)), result_e::exhausted);
  }

  TEST(InvitePolicy, TreatsZeroMaxUsesAsUnlimited) {
    auto invite = make_invite();
    invite.max_uses = 0;
    invite.uses = 500;
    EXPECT_EQ(policy::evaluate(invite, mode_e::browser, "123456", at(10)), result_e::ok);
  }

  TEST(InvitePolicy, RefusesAModeTheInviteDoesNotOffer) {
    const auto invite = make_invite();  // allow_pairing is false
    EXPECT_EQ(policy::evaluate(invite, mode_e::pairing, "123456", at(10)), result_e::not_allowed);
  }

  TEST(InvitePolicy, AdmitsPairingWhenTheInviteOffersIt) {
    auto invite = make_invite();
    invite.allow_pairing = true;
    EXPECT_EQ(policy::evaluate(invite, mode_e::pairing, "123456", at(10)), result_e::ok);
  }

  // A dead invite must be refused for being dead whatever the guest typed. If a wrong PIN
  // on an expired invite reported bad_pin, the response would confirm the token was real.
  TEST(InvitePolicy, ChecksInviteStateBeforeThePinSoRefusalsDoNotLeak) {
    auto invite = make_invite();
    invite.revoked = true;
    EXPECT_EQ(policy::evaluate(invite, mode_e::browser, "wrong", at(10)), result_e::revoked);

    auto expired = make_invite();
    EXPECT_EQ(policy::evaluate(expired, mode_e::browser, "wrong", at(9999)), result_e::expired);
  }

  TEST(InvitePolicy, TellsTheGuestTheSameThingForUnknownRevokedAndExpired) {
    // The three must be indistinguishable to someone holding a guessed token.
    const auto unknown = policy::public_reason(result_e::unknown);
    EXPECT_EQ(policy::public_reason(result_e::revoked), unknown);
    EXPECT_EQ(policy::public_reason(result_e::expired), unknown);
    // ...and must not be the message a real token with a wrong PIN gets.
    EXPECT_NE(policy::public_reason(result_e::bad_pin), unknown);
  }

  TEST(InvitePolicy, LocksOutAfterTheThresholdOfWrongPins) {
    auto invite = make_invite();
    for (int i = 0; i < policy::lockout_threshold; ++i) {
      const auto result = policy::evaluate(invite, mode_e::browser, "000000", at(10));
      ASSERT_EQ(result, result_e::bad_pin) << "attempt " << i;
      policy::apply(invite, result, at(10));
    }
    EXPECT_EQ(policy::evaluate(invite, mode_e::browser, "123456", at(11)), result_e::locked_out);
  }

  TEST(InvitePolicy, ReleasesTheLockoutOnceItsWindowPasses) {
    auto invite = make_invite();
    for (int i = 0; i < policy::lockout_threshold; ++i) {
      policy::apply(invite, result_e::bad_pin, at(10));
    }
    ASSERT_GT(policy::lockout_remaining_seconds(invite, at(11)), 0);

    const auto after = at(10) + policy::lockout_base + std::chrono::seconds {1};
    EXPECT_EQ(policy::lockout_remaining_seconds(invite, after), 0);
    EXPECT_EQ(policy::evaluate(invite, mode_e::browser, "123456", after), result_e::ok);
  }

  TEST(InvitePolicy, LengthensTheLockoutEachTimeTheThresholdIsCrossedAgain) {
    auto invite = make_invite();
    for (int i = 0; i < policy::lockout_threshold; ++i) {
      policy::apply(invite, result_e::bad_pin, at(10));
    }
    const auto first = invite.locked_until;

    for (int i = 0; i < policy::lockout_threshold; ++i) {
      policy::apply(invite, result_e::bad_pin, at(200));
    }
    const auto second = invite.locked_until;

    EXPECT_GT(second - at(200), first - at(10));
  }

  TEST(InvitePolicy, CapsTheLockoutSoItNeverBecomesPermanent) {
    auto invite = make_invite();
    for (int i = 0; i < policy::lockout_threshold * 40; ++i) {
      policy::apply(invite, result_e::bad_pin, at(10));
    }
    const auto locked_for = invite.locked_until - at(10);
    EXPECT_LE(locked_for, policy::lockout_max);
  }

  TEST(InvitePolicy, ClearsTheFailureCountOnASuccessfulRedeem) {
    auto invite = make_invite();
    policy::apply(invite, result_e::bad_pin, at(10));
    policy::apply(invite, result_e::bad_pin, at(11));
    ASSERT_EQ(invite.failed_attempts, 2);

    policy::apply(invite, result_e::ok, at(12));
    EXPECT_EQ(invite.failed_attempts, 0);
    EXPECT_EQ(policy::lockout_remaining_seconds(invite, at(12)), 0);
  }

  TEST(InvitePolicy, CountsASuccessfulRedeemAgainstTheUseLimit) {
    auto invite = make_invite();
    invite.max_uses = 1;
    policy::apply(invite, result_e::ok, at(10));
    EXPECT_EQ(invite.uses, 1);
    EXPECT_EQ(policy::evaluate(invite, mode_e::browser, "123456", at(11)), result_e::exhausted);
  }

  // An invite refused for a reason the guest did not cause must not accumulate into a
  // lockout, or extending the expiry would leave the link mysteriously still dead.
  TEST(InvitePolicy, DoesNotCountStatelessRefusalsAsFailedAttempts) {
    auto invite = make_invite();
    for (const auto result : {result_e::expired, result_e::revoked, result_e::exhausted, result_e::not_allowed, result_e::unknown}) {
      policy::apply(invite, result, at(10));
    }
    EXPECT_EQ(invite.failed_attempts, 0);
    EXPECT_EQ(policy::lockout_remaining_seconds(invite, at(10)), 0);
  }

  TEST(InvitePolicy, ClampsAPermissionDownNeverUp) {
    auto invite = make_invite();
    invite.perm = 0b0101u;
    EXPECT_EQ(policy::clamp_permission(invite, 0b1111u), 0b0101u);
    EXPECT_EQ(policy::clamp_permission(invite, 0b0100u), 0b0100u);
    EXPECT_EQ(policy::clamp_permission(invite, 0u), 0u);
  }

  TEST(InvitePolicy, GrantsNothingWhenTheInviteGrantsNothing) {
    auto invite = make_invite();
    invite.perm = 0u;
    EXPECT_EQ(policy::clamp_permission(invite, 0xFFFFFFFFu), 0u);
  }

  TEST(InvitePolicy, ReportsLivenessForTheLandingPage) {
    auto invite = make_invite();
    EXPECT_TRUE(policy::is_live(invite, at(10)));
    EXPECT_FALSE(policy::is_live(invite, at(3601)));

    auto revoked = make_invite();
    revoked.revoked = true;
    EXPECT_FALSE(policy::is_live(revoked, at(10)));

    auto spent = make_invite();
    spent.max_uses = 1;
    spent.uses = 1;
    EXPECT_FALSE(policy::is_live(spent, at(10)));
  }

  // Liveness is about the invite, not the guest — a lockout is temporary and must not
  // make the landing page claim the link is dead.
  TEST(InvitePolicy, StillReportsALockedOutInviteAsLive) {
    auto invite = make_invite();
    for (int i = 0; i < policy::lockout_threshold; ++i) {
      policy::apply(invite, result_e::bad_pin, at(10));
    }
    EXPECT_TRUE(policy::is_live(invite, at(11)));
  }

  TEST(InvitePolicy, ComparesEqualStringsAsEqual) {
    EXPECT_TRUE(policy::constant_time_equals("123456", "123456"));
    EXPECT_TRUE(policy::constant_time_equals("", ""));
  }

  TEST(InvitePolicy, ComparesDifferingStringsAsUnequalIncludingByLength) {
    EXPECT_FALSE(policy::constant_time_equals("123456", "123457"));
    EXPECT_FALSE(policy::constant_time_equals("123456", "12345"));
    EXPECT_FALSE(policy::constant_time_equals("12345", "123456"));
    EXPECT_FALSE(policy::constant_time_equals("", "1"));
    EXPECT_FALSE(policy::constant_time_equals("1", ""));
  }

  // A NUL inside either side must not truncate the comparison the way strcmp would.
  TEST(InvitePolicy, ComparesPastEmbeddedNuls) {
    using namespace std::string_view_literals;
    EXPECT_FALSE(policy::constant_time_equals("12\0" "45"sv, "12\0" "46"sv));
    EXPECT_TRUE(policy::constant_time_equals("12\0" "45"sv, "12\0" "45"sv));
  }
}  // namespace
