#include "src/csrf_token_store.h"

#include <gtest/gtest.h>
#include <thread>
#include <vector>

namespace {
  namespace csrf = confighttp::csrf;

  class CsrfTokenStoreTest: public testing::Test {
  protected:
    csrf::TokenStore::clock::time_point now {};
    std::size_t generated = 0;
    csrf::TokenStore store {[this] { return next_token(); }, [this] { return now; }};

    std::string next_token() {
      const auto number = std::to_string(++generated);
      return std::string(csrf::TokenStore::token_size - number.size(), 'x') + number;
    }

    static csrf::client_id_t identity(std::string_view value) {
      return csrf::client_id(csrf::identity_source::authorization, value);
    }
  };

  TEST_F(CsrfTokenStoreTest, TokensStayBoundToTheIdentityThatRequestedThem) {
    const auto alice = identity("Session alice-secret");
    const auto bob = identity("Session bob-secret");
    const auto token = store.issue(alice);
    EXPECT_EQ(store.validate(alice, token), csrf::validation::valid);
    EXPECT_EQ(store.validate(bob, token), csrf::validation::invalid);
    EXPECT_EQ(store.validate(alice, token + "extra"), csrf::validation::invalid);
    EXPECT_EQ(store.validate(alice, token.substr(1)), csrf::validation::invalid);
    auto wrong = token;
    wrong.back() ^= 1;
    EXPECT_EQ(store.validate(alice, wrong), csrf::validation::invalid);
    EXPECT_EQ(store.validate(alice, token), csrf::validation::valid);
  }

  TEST_F(CsrfTokenStoreTest, RefetchingDoesNotExtendTheOriginalExpiry) {
    const auto client = identity("Session reusable");
    const auto token = store.issue(client);
    now += std::chrono::minutes(59);
    EXPECT_EQ(store.issue(client), token);
    EXPECT_EQ(generated, 1);
    now += std::chrono::minutes(1);
    EXPECT_EQ(store.validate(client, token), csrf::validation::expired);
    EXPECT_EQ(store.size(), 0);
    const auto replacement = store.issue(client);
    EXPECT_NE(replacement, token);
    EXPECT_EQ(store.validate(client, token), csrf::validation::invalid);
    EXPECT_EQ(store.validate(client, replacement), csrf::validation::valid);
  }

  TEST_F(CsrfTokenStoreTest, IssuancePrunesExpiredEntries) {
    store.issue(identity("one"));
    now += std::chrono::minutes(30);
    const auto live = store.issue(identity("two"));
    now += std::chrono::minutes(30);
    store.issue(identity("three"));
    EXPECT_EQ(store.size(), 2);
    EXPECT_EQ(store.validate(identity("two"), live), csrf::validation::valid);
  }

  TEST_F(CsrfTokenStoreTest, FullCacheEvictsOldestExpiryAndAdmitsNewClients) {
    csrf::TokenStore small {[this] { return next_token(); }, [this] { return now; }, 3};
    const auto oldest = small.issue(identity("oldest"));
    now += std::chrono::seconds(1);
    const auto middle = small.issue(identity("middle"));
    now += std::chrono::seconds(1);
    const auto newest = small.issue(identity("newest"));
    EXPECT_EQ(small.issue(identity("oldest")), oldest);
    const auto admitted = small.issue(identity("admitted"));
    EXPECT_EQ(small.size(), 3);
    EXPECT_EQ(small.validate(identity("oldest"), oldest), csrf::validation::invalid);
    EXPECT_EQ(small.validate(identity("middle"), middle), csrf::validation::valid);
    EXPECT_EQ(small.validate(identity("newest"), newest), csrf::validation::valid);
    EXPECT_EQ(small.validate(identity("admitted"), admitted), csrf::validation::valid);
  }

  TEST_F(CsrfTokenStoreTest, ProductionCapacityStaysBoundedUnderDistinctAnonymousIdentities) {
    const auto first = store.issue(identity("first"));
    for (std::size_t i = 1; i <= csrf::TokenStore::max_entries + 10; ++i) {
      now += std::chrono::milliseconds(1);
      const auto id = identity(std::to_string(i));
      const auto token = store.issue(id);
      EXPECT_LE(store.size(), csrf::TokenStore::max_entries);
      EXPECT_EQ(store.validate(id, token), csrf::validation::valid);
    }
    EXPECT_EQ(store.size(), csrf::TokenStore::max_entries);
    EXPECT_EQ(store.validate(identity("first"), first), csrf::validation::invalid);
  }

  TEST_F(CsrfTokenStoreTest, LargeCredentialValuesBecomeFixedSizeDigestKeys) {
    static_assert(sizeof(csrf::client_id_t) == SHA256_DIGEST_LENGTH + 1);
    std::string secret(1024 * 1024, 's');
    const auto id = identity(secret);
    const auto token = store.issue(id);
    EXPECT_EQ(store.issue(identity(std::string(secret))), token);
    secret.back() = 't';
    const auto different = identity(secret);
    EXPECT_NE(different, id);
    EXPECT_NE(store.issue(different), token);
    secret.clear();
    secret.shrink_to_fit();
    EXPECT_EQ(store.validate(id, token), csrf::validation::valid);
    EXPECT_EQ(store.size(), 2);
  }

  TEST_F(CsrfTokenStoreTest, CredentialTextCannotCollideWithPeerIdentity) {
    const auto peer = csrf::client_id(csrf::identity_source::peer, "127.0.0.1");
    const auto auth = identity("127.0.0.1");
    EXPECT_NE(peer, auth);
    const auto peer_token = store.issue(peer);
    EXPECT_EQ(store.validate(auth, peer_token), csrf::validation::invalid);
  }

  TEST_F(CsrfTokenStoreTest, IdentityUsesSha256WithoutRetainingTheCredential) {
    const auto id = identity("abc");
    const std::array<unsigned char, SHA256_DIGEST_LENGTH> expected {
      0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
      0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
      0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
      0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad,
    };
    EXPECT_TRUE(std::equal(id.begin() + 1, id.end(), expected.begin()));
  }

  TEST_F(CsrfTokenStoreTest, ConcurrentFetchesReturnOneTokenForTheSameIdentity) {
    const auto id = identity("shared-client");
    std::vector<std::string> tokens(32);
    std::vector<std::thread> workers;
    for (std::size_t i = 0; i < tokens.size(); ++i) {
      workers.emplace_back([&, i] { tokens[i] = store.issue(id); });
    }
    for (auto &worker : workers) {
      worker.join();
    }
    for (const auto &token : tokens) {
      EXPECT_EQ(token, tokens.front());
      EXPECT_EQ(store.validate(id, token), csrf::validation::valid);
    }
    EXPECT_EQ(generated, 1);
    EXPECT_EQ(store.size(), 1);
  }

  TEST_F(CsrfTokenStoreTest, InvalidGeneratorOutputIsNotRetained) {
    csrf::TokenStore invalid {[] { return std::string(1024 * 1024, 's'); }};
    EXPECT_THROW(invalid.issue(identity("client")), std::runtime_error);
    EXPECT_EQ(invalid.size(), 0);
  }
}  // namespace
