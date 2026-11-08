#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <functional>
#include <future>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>

#include <Simple-Web-Server/server_http.hpp>
#include <src/http_security.h>

namespace {
  using namespace std::chrono_literals;
  using HttpServer = SimpleWeb::Server<SimpleWeb::HTTP>;
  namespace asio = boost::asio;

  TEST(HttpSecurityPolicy, RecognizesLoopbackIncludingIpv4MappedAddresses) {
    for (const auto address : {"127.0.0.1", "127.42.0.19", "::1", "0:0:0:0:0:0:0:1", "::ffff:127.0.0.1", "::ffff:7f2a:13"}) {
      EXPECT_TRUE(http::security::is_loopback(address)) << address;
    }
    for (const auto address : {"", "localhost", "127.0.0.1.example", "127.0.0.1:1234", "[::1]", "0.0.0.0", "::", "192.168.1.2", "::ffff:192.168.1.2", "2001:db8::1", "::127.0.0.1"}) {
      EXPECT_FALSE(http::security::is_loopback(address)) << address;
    }
  }

  TEST(HttpSecurityLogging, RemovesQueriesAndInvitationTokensFromPaths) {
    EXPECT_EQ(http::security::log_path("/api/apps?token=secret&password=private"), "/api/apps");
    EXPECT_EQ(http::security::log_path("/join/invitation-secret?token=other-secret"), "/join/[REDACTED]");
    EXPECT_EQ(http::security::log_path("/api/join/invitation-secret/sessions?token=other-secret"), "/api/join/[REDACTED]/sessions");
    EXPECT_EQ(http::security::log_path("/api/join/invitation-secret/\r\nforged-log"), "/api/join/[REDACTED]/  forged-log");
  }

  TEST(HttpSecurityLogging, RedactsCredentialAndUnrecognizedHeadersRegardlessOfCase) {
    for (const auto name : {"Authorization", "aUtHoRiZaTiOn", "Proxy-Authorization", "Cookie", "Set-Cookie", "Referer", "X-API-Key", "X-CSRF-Token", "X-Custom-Credential"}) {
      EXPECT_EQ(http::security::log_header(name, "private-secret\r\nforged-log"), "[REDACTED]") << name;
    }
    EXPECT_EQ(http::security::log_header("cOnTeNt-TyPe", "application/json\r\nforged-log"), "application/json  forged-log");
    EXPECT_EQ(http::security::log_header("Content-Length", "123"), "123");
  }

  TEST(HttpSecurityLogging, BoundsTextAndRemovesEveryAsciiControlCharacter) {
    std::string controls;
    for (int c = 0; c < 32; ++c) {
      controls.push_back(static_cast<char>(c));
    }
    controls.push_back('\x7f');
    EXPECT_EQ(http::security::log_text(controls), std::string(controls.size(), ' '));
    EXPECT_EQ(http::security::log_text(std::string(1024, 'x')), std::string(512, 'x'));
    EXPECT_EQ(http::security::log_header("Content-Type", std::string(1024, 'x')).size(), 512);
    EXPECT_EQ(http::security::log_path(std::string(1024, 'x') + "?secret").size(), 512);
  }

  class HttpSecurityRequestLimits: public testing::Test {
  protected:
    struct Exchange {
      std::string response;
      bool timed_out = false;
    };

    HttpServer server;
    std::thread server_thread;
    unsigned short port = 0;
    std::atomic<unsigned int> handler_calls {0};
    std::mutex state_mutex;
    std::string received_body;
    boost::system::error_code request_error;
    std::size_t rejected_body_bytes = 0;
    std::exception_ptr server_exception;

    void SetUp() override {
      server.config.address = "127.0.0.1";
      server.config.port = 0;
      http::security::configure_request_limits(server.config);
      server.io_service = std::make_shared<SimpleWeb::io_context>();
      server.default_resource["POST"] = [this](auto response, auto request) {
        ++handler_calls;
        {
          std::lock_guard lock(state_mutex);
          received_body = request->content.string();
        }
        response->write(SimpleWeb::StatusCode::success_ok, "accepted");
      };
      server.on_error = [this](auto request, const auto &error) {
        std::lock_guard lock(state_mutex);
        request_error = error;
        rejected_body_bytes = request->content.size();
      };

      auto listening = std::make_shared<std::promise<unsigned short>>();
      auto ready = listening->get_future();
      server.start([listening](unsigned short assigned_port) { listening->set_value(assigned_port); });
      server_thread = std::thread([this] {
        try {
          server.io_service->run();
        }
        catch (...) {
          std::lock_guard lock(state_mutex);
          server_exception = std::current_exception();
        }
      });
      ASSERT_EQ(ready.wait_for(5s), std::future_status::ready);
      port = ready.get();
    }

    void TearDown() override {
      server.stop();
      server.io_service->stop();
      if (server_thread.joinable()) {
        server_thread.join();
      }
      EXPECT_FALSE(server_exception) << "The server threw while parsing a request";
    }

    Exchange exchange(const std::string &wire) {
      asio::io_context context;
      asio::ip::tcp::socket socket(context);
      socket.connect({asio::ip::make_address("127.0.0.1"), port});
      asio::steady_timer deadline(context, 5s);
      Exchange result;
      std::array<char, 4096> buffer {};
      std::function<void()> read;
      read = [&] {
        socket.async_read_some(asio::buffer(buffer), [&](const auto &error, std::size_t size) {
          result.response.append(buffer.data(), size);
          if (!error && result.response.size() < 8192) {
            read();
          }
          else {
            deadline.cancel();
            boost::system::error_code ignored;
            socket.close(ignored);
          }
        });
      };
      deadline.async_wait([&](const auto &error) {
        if (!error) {
          result.timed_out = true;
          boost::system::error_code ignored;
          socket.close(ignored);
        }
      });
      // Read concurrently with writing: an oversized header can be rejected
      // before the complete client buffer has been transmitted.
      asio::async_write(socket, asio::buffer(wire), [](const auto &, std::size_t) {});
      read();
      context.run();
      return result;
    }

    static std::string request(std::string_view headers, std::string_view body = {}) {
      return "POST /test HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n" + std::string(headers) + "\r\n" + std::string(body);
    }

    static std::string hex(std::size_t size) {
      std::ostringstream stream;
      stream << std::hex << size;
      return stream.str();
    }

    void expect_rejected(const Exchange &result, std::size_t maximum_buffered_body, bool content_limit = true) {
      EXPECT_FALSE(result.timed_out) << "The server waited for a body it should reject from its declared size";
      EXPECT_EQ(handler_calls.load(), 0);
      EXPECT_EQ(result.response.find("200 OK"), std::string::npos);
      std::lock_guard lock(state_mutex);
      EXPECT_TRUE(request_error);
      if (content_limit) {
        EXPECT_EQ(request_error, boost::system::errc::make_error_code(boost::system::errc::message_size));
      }
      EXPECT_LE(rejected_body_bytes, maximum_buffered_body);
    }
  };

  TEST_F(HttpSecurityRequestLimits, OrdinaryContentLengthRequestReachesHandler) {
    const auto result = exchange(request("Content-Length: 5\r\n", "hello"));
    EXPECT_FALSE(result.timed_out);
    EXPECT_NE(result.response.find("200 OK"), std::string::npos);
    EXPECT_NE(result.response.find("accepted"), std::string::npos);
    EXPECT_EQ(handler_calls.load(), 1);
    std::lock_guard lock(state_mutex);
    EXPECT_EQ(received_body, "hello");
    EXPECT_FALSE(request_error);
  }

  TEST_F(HttpSecurityRequestLimits, OrdinaryChunkedRequestReachesHandler) {
    const auto result = exchange(request("Transfer-Encoding: chunked\r\n", "3\r\nabc\r\n2\r\nde\r\n0\r\n\r\n"));
    EXPECT_FALSE(result.timed_out);
    EXPECT_NE(result.response.find("200 OK"), std::string::npos);
    EXPECT_EQ(handler_calls.load(), 1);
    std::lock_guard lock(state_mutex);
    EXPECT_EQ(received_body, "abcde");
  }

  TEST_F(HttpSecurityRequestLimits, OversizedContentLengthIsRejectedWithoutSendingABody) {
    const auto result = exchange(request("Content-Length: " + std::to_string(http::security::max_request_bytes + 1) + "\r\n"));
    expect_rejected(result, 0);
  }

  TEST_F(HttpSecurityRequestLimits, OversizedChunkIsRejectedWithoutSendingItsBody) {
    const auto result = exchange(request("Transfer-Encoding: chunked\r\n", hex(http::security::max_request_bytes + 1) + "\r\n"));
    expect_rejected(result, 0);
  }

  TEST_F(HttpSecurityRequestLimits, ChunkSizesAreLimitedCumulatively) {
    const auto body = "1\r\nx\r\n" + hex(http::security::max_request_bytes) + "\r\n";
    expect_rejected(exchange(request("Transfer-Encoding: chunked\r\n", body)), 1);
  }

  TEST_F(HttpSecurityRequestLimits, ChunkSizeCannotWrapTheCumulativeLimit) {
    const auto body = "1\r\nx\r\n" + hex((std::numeric_limits<unsigned long>::max)()) + "\r\n";
    expect_rejected(exchange(request("Transfer-Encoding: chunked\r\n", body)), 1);
  }

  TEST_F(HttpSecurityRequestLimits, OversizedHeaderIsRejectedWithoutInvokingHandler) {
    const auto wire = "POST /test HTTP/1.1\r\nHost: localhost\r\nX-Padding: " + std::string(http::security::max_request_bytes, 'x') + "\r\nContent-Length: 0\r\n\r\n";
    expect_rejected(exchange(wire), 0, false);
  }
}  // namespace
