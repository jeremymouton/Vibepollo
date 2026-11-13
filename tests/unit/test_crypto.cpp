#include "src/crypto.h"

#include <gtest/gtest.h>

TEST(GcmInput, RejectsEveryTruncatedAuthenticationTagWithoutThrowing) {
  crypto::cipher::gcm_t cipher(crypto::aes_t(16, 0x31), false);
  crypto::aes_t iv(16, 0x42);
  for (std::size_t size = 0; size < crypto::cipher::tag_size; ++size) {
    const std::string packet(size, '\0');
    std::vector<std::uint8_t> plaintext;
    SCOPED_TRACE(size);
    EXPECT_NO_THROW(EXPECT_NE(cipher.decrypt(packet, plaintext, &iv), 0));
  }
}

TEST(GcmInput, RejectsCipherContextInitializationFailure) {
  crypto::cipher::gcm_t cipher(crypto::aes_t(16, 0x31), false);
  crypto::aes_t iv(16, 0x42);
  std::vector<std::uint8_t> plaintext;
  const std::string packet(crypto::cipher::tag_size, '\0');
  EXPECT_NE(cipher.decrypt(packet, plaintext, &iv), 0);
  ASSERT_TRUE(cipher.decrypt_ctx);
  ASSERT_EQ(EVP_CIPHER_CTX_reset(cipher.decrypt_ctx.get()), 1);
  EXPECT_NE(cipher.decrypt(packet, plaintext, &iv), 0);
}

TEST(GcmInput, ValidPacketsStillDecryptAfterMalformedPackets) {
  const crypto::aes_t key(16, 0x31);
  crypto::cipher::gcm_t encryptor(key, false);
  crypto::cipher::gcm_t decryptor(key, false);
  crypto::aes_t iv(16, 0x42);
  for (const std::string message : {std::string(), std::string("input event")}) {
    std::vector<std::uint8_t> packet(crypto::cipher::tag_size + message.size());
    ASSERT_EQ(encryptor.encrypt(message, packet.data(), &iv), message.size());
    const std::string_view wire(reinterpret_cast<const char *>(packet.data()), packet.size());
    std::vector<std::uint8_t> plaintext;
    packet[0] ^= 1;
    EXPECT_NE(decryptor.decrypt(wire, plaintext, &iv), 0);
    packet[0] ^= 1;
    ASSERT_EQ(decryptor.decrypt(wire, plaintext, &iv), 0);
    EXPECT_EQ(std::string(plaintext.begin(), plaintext.end()), message);
  }
}
