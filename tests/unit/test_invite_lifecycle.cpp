#include "../tests_common.h"
#include <src/invites.h>

#include <algorithm>
#include <future>
#include <set>

namespace {
  using namespace std::chrono_literals;

  class InviteLifecycle: public testing::Test {
  protected:
    void SetUp() override {
      for (const auto &grant : invite::list()) {
        invite::revoked_access_t revoked;
        invite::remove(grant.id, revoked);
      }
    }

    invite::invite_t grant() {
      invite::spec_t spec;
      spec.perm = 0x1234;
      spec.app_id = 42;
      spec.allow_pairing = true;
      spec.expires_in_seconds = 0;
      return invite::create(spec);
    }

    bool start(const std::string &token, const std::string &id) {
      return invite::guest::create_stream(token, [&](const auto &) { return std::optional<std::string> {id}; });
    }
  };

  TEST_F(InviteLifecycle, RevocationOwnsEveryStreamFromEveryCredential) {
    const auto initial = grant();
    const auto first = invite::guest::issue(initial);
    const auto second = invite::guest::issue(initial);
    ASSERT_TRUE(start(first, "A"));
    ASSERT_TRUE(start(first, "B"));
    ASSERT_TRUE(start(second, "C"));
    EXPECT_TRUE(invite::guest::owns_stream_session(first, "A"));
    EXPECT_TRUE(invite::guest::owns_stream_session(first, "B"));
    EXPECT_FALSE(invite::guest::owns_stream_session(second, "A"));

    invite::spec_t change;
    change.revoked = true;
    invite::revoked_access_t revoked;
    ASSERT_TRUE(invite::update(initial.id, change, revoked));
    EXPECT_EQ((std::set<std::string> {revoked.stream_ids.begin(), revoked.stream_ids.end()}), (std::set<std::string> {"A", "B", "C"}));
    EXPECT_FALSE(invite::guest::lookup(first));
    EXPECT_FALSE(start(first, "D"));
  }

  TEST_F(InviteLifecycle, GuestTtlClosesStreamsEvenWhenInviteNeverExpires) {
    const auto initial = grant();
    const auto token = invite::guest::issue(initial);
    ASSERT_TRUE(start(token, "A"));
    ASSERT_TRUE(start(token, "B"));
    EXPECT_EQ(invite::guest::active_count(initial.id), 1);
    const auto revoked = invite::expire(invite::policy::clock_t::now() + invite::guest::session_ttl + 1s);
    EXPECT_EQ((std::set<std::string> {revoked.stream_ids.begin(), revoked.stream_ids.end()}), (std::set<std::string> {"A", "B"}));
    EXPECT_FALSE(invite::guest::lookup(token));
    EXPECT_TRUE(invite::expire(invite::policy::clock_t::now() + 10h).stream_ids.empty());
    EXPECT_TRUE(invite::find(initial.id));
  }

  TEST_F(InviteLifecycle, ClosedStreamsAreRemovedFromOwnership) {
    const auto initial = grant();
    const auto token = invite::guest::issue(initial);
    ASSERT_TRUE(start(token, "A"));
    ASSERT_TRUE(start(token, "B"));
    invite::guest::forget_stream_session("A");
    EXPECT_FALSE(invite::guest::owns_stream_session(token, "A"));
    EXPECT_EQ(invite::guest::revoke(token), std::vector<std::string> {"B"});
  }

  TEST_F(InviteLifecycle, RotationRejectsStaleRedemptionAndPairingBeforePublication) {
    const auto initial = grant();
    invite::revoked_access_t revoked;
    const auto rotated = invite::rotate(initial.id, revoked);
    ASSERT_TRUE(rotated);
    EXPECT_GT(rotated->generation, initial.generation);
    EXPECT_TRUE(invite::guest::issue(initial).empty());
    bool published = false;
    EXPECT_FALSE(invite::commit_pairing(initial.id, initial.generation, "device", [&] { published = true; }));
    EXPECT_FALSE(published);
    EXPECT_FALSE(invite::paired_grant(initial.id, initial.generation));
    EXPECT_FALSE(invite::guest::issue(*rotated).empty());
  }

  TEST_F(InviteLifecycle, DeletionNeverPromotesAnInviteCertificateToOwner) {
    const auto initial = grant();
    ASSERT_TRUE(invite::commit_pairing(initial.id, initial.generation, "device", [] {}));
    invite::revoked_access_t revoked;
    ASSERT_TRUE(invite::remove(initial.id, revoked));
    EXPECT_EQ(revoked.device_uuids, std::vector<std::string> {"device"});
    EXPECT_FALSE(invite::paired_grant(initial.id, initial.generation));
    bool published = false;
    EXPECT_FALSE(invite::commit_pairing(initial.id, initial.generation, "late-device", [&] { published = true; }));
    EXPECT_FALSE(published);
    EXPECT_TRUE(invite::guest::issue(initial).empty());
  }

  TEST_F(InviteLifecycle, ModeChangesAndScopeChangesInvalidateIssuedGrants) {
    for (int change_kind = 0; change_kind < 5; ++change_kind) {
      const auto initial = grant();
      const auto token = invite::guest::issue(initial);
      ASSERT_TRUE(start(token, "stream"));
      ASSERT_TRUE(invite::commit_pairing(initial.id, initial.generation, "device", [] {}));
      invite::spec_t change;
      if (change_kind == 0) change.allow_browser = false;
      if (change_kind == 1) change.allow_pairing = false;
      if (change_kind == 2) change.app_id = 43;
      if (change_kind == 3) change.perm = 0;
      if (change_kind == 4) change.expires_in_seconds = 1;
      invite::revoked_access_t revoked;
      ASSERT_TRUE(invite::update(initial.id, change, revoked));
      EXPECT_EQ(revoked.stream_ids, std::vector<std::string> {"stream"});
      EXPECT_EQ(revoked.device_uuids, std::vector<std::string> {"device"});
      EXPECT_FALSE(invite::guest::lookup(token));
      EXPECT_FALSE(invite::paired_grant(initial.id, initial.generation));
      EXPECT_FALSE(invite::commit_pairing(initial.id, initial.generation, "late", [] {}));
    }
  }

  TEST_F(InviteLifecycle, InviteExpiryCollectsNativeAndBrowserResources) {
    const auto initial = grant();
    invite::spec_t change;
    change.expires_in_seconds = 1;
    invite::revoked_access_t ignored;
    const auto expiring = invite::update(initial.id, change, ignored);
    ASSERT_TRUE(expiring);
    const auto token = invite::guest::issue(*expiring);
    ASSERT_TRUE(start(token, "stream"));
    ASSERT_TRUE(invite::commit_pairing(expiring->id, expiring->generation, "device", [] {}));
    const auto revoked = invite::expire(expiring->expires_at);
    EXPECT_EQ(revoked.stream_ids, std::vector<std::string> {"stream"});
    EXPECT_EQ(revoked.device_uuids, std::vector<std::string> {"device"});
    EXPECT_FALSE(invite::guest::lookup(token));
  }

  TEST_F(InviteLifecycle, GenerationAndNativeAssociationSurvivePersistence) {
    const auto initial = grant();
    invite::revoked_access_t ignored;
    const auto rotated = invite::rotate(initial.id, ignored);
    ASSERT_TRUE(rotated);
    ASSERT_TRUE(invite::commit_pairing(rotated->id, rotated->generation, "device", [] {}));
    invite::load();
    EXPECT_FALSE(invite::paired_grant(initial.id, initial.generation));
    const auto restored = invite::paired_grant(rotated->id, rotated->generation);
    ASSERT_TRUE(restored);
    EXPECT_EQ(restored->paired_device_uuids, std::vector<std::string> {"device"});
  }

  TEST_F(InviteLifecycle, DeletionWaitsForStartedStreamAndCannotMissItsId) {
    const auto initial = grant();
    const auto token = invite::guest::issue(initial);
    std::promise<void> entered, release;
    auto released = release.get_future().share();
    auto starting = std::async(std::launch::async, [&] {
      return invite::guest::create_stream(token, [&](const auto &) {
        // Call back into the store to catch an accidental storage lock held over startup.
        EXPECT_TRUE(invite::find(initial.id));
        entered.set_value();
        released.wait();
        return std::optional<std::string> {"late-stream"};
      });
    });
    entered.get_future().wait();
    auto deletion = std::async(std::launch::async, [&] {
      invite::revoked_access_t revoked;
      invite::remove(initial.id, revoked);
      return revoked;
    });
    EXPECT_EQ(deletion.wait_for(30ms), std::future_status::timeout);
    release.set_value();
    EXPECT_TRUE(starting.get());
    EXPECT_EQ(deletion.get().stream_ids, std::vector<std::string> {"late-stream"});
    bool ran = false;
    EXPECT_FALSE(invite::guest::create_stream(token, [&](const auto &) { ran = true; return std::optional<std::string> {"escaped"}; }));
    EXPECT_FALSE(ran);
  }

  TEST_F(InviteLifecycle, RevocationWaitsForCertificatePublicationAndReturnsDevice) {
    const auto initial = grant();
    std::promise<void> entered, release;
    auto released = release.get_future().share();
    auto pairing = std::async(std::launch::async, [&] {
      return invite::commit_pairing(initial.id, initial.generation, "late-device", [&] {
        EXPECT_TRUE(invite::find(initial.id));
        entered.set_value();
        released.wait();
      });
    });
    entered.get_future().wait();
    auto rotation = std::async(std::launch::async, [&] {
      invite::revoked_access_t revoked;
      invite::rotate(initial.id, revoked);
      return revoked;
    });
    EXPECT_EQ(rotation.wait_for(30ms), std::future_status::timeout);
    release.set_value();
    EXPECT_TRUE(pairing.get());
    EXPECT_EQ(rotation.get().device_uuids, std::vector<std::string> {"late-device"});
    EXPECT_FALSE(invite::paired_grant(initial.id, initial.generation));
  }
}
