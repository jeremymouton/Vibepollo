#include "http_auth_request_policy.h"
#include "http_security.h"

#include <boost/algorithm/string.hpp>
#include <filesystem>
#include <format>
#include <nlohmann/json.hpp>
#include <ranges>

namespace confighttp::policy {
  PasswordAuthResult PasswordAttemptLimiter::verify(const std::string &remote_address, const boost::function<bool()> &verify_credentials) {
    std::scoped_lock lock(_mutex);
    const auto now = _now();
    constexpr auto window = std::chrono::minutes(1);
    std::erase_if(_peers, [&](const auto &entry) { return entry.second.reset_at <= now; });
    if (_global.reset_at <= now) {
      _global = {now + window, 0};
      _next_global_attempt = {};
    }
    auto retry = [&](clock::time_point reset_at) {
      return PasswordAuthResult {false, static_cast<int>(std::chrono::ceil<std::chrono::seconds>(reset_at - now).count())};
    };
    // Keep local recovery available even during a distributed remote attack.
    if (_global.failures >= 100 && !http::security::is_loopback(remote_address)) {
      if (now < _next_global_attempt) {
        return retry(_next_global_attempt);
      }
      _next_global_attempt = now + std::chrono::seconds(1);
    }
    auto it = _peers.find(remote_address);
    if (it == _peers.end()) {
      if (_peers.size() >= 1024) {
        return retry(_global.reset_at);
      }
      it = _peers.emplace(remote_address, Attempts {now + window, 0}).first;
    }
    if (it->second.failures >= 5) {
      return retry(it->second.reset_at);
    }
    if (verify_credentials()) {
      _peers.erase(it);
      return {true, 0};
    }
    ++it->second.failures;
    ++_global.failures;
    return {false, 0};
  }

  AuthResult RequestAuthPolicy::make_error(StatusCode code, const std::string &error) const {
    AuthResult result {false, code, {}, {}};
    result.headers.emplace("Access-Control-Allow-Origin", std::format("https://localhost:{}", _dependencies.https_port()));
    if (!error.empty()) {
      result.body = nlohmann::json {{"status", false}, {"error", error}}.dump();
      result.headers.emplace("Content-Type", "application/json");
    }
    return result;
  }

  AuthResult RequestAuthPolicy::basic_error() const {
    auto result = make_error(StatusCode::client_error_unauthorized, "Unauthorized");
    result.headers.emplace("WWW-Authenticate", "Basic realm=\"Sunshine\"");
    return result;
  }

  AuthResult RequestAuthPolicy::check_password(const std::string &remote_address, const std::string &username, const std::string &password) const {
    if (!_dependencies.remote_allowed(remote_address)) return make_error(StatusCode::client_error_forbidden, "Forbidden");
    if (!_dependencies.credentials_configured()) return make_error(StatusCode::client_error_unauthorized, "Credentials not configured");
    const auto result = _dependencies.password_auth ?
      _dependencies.password_auth(remote_address, username, password) :
      PasswordAuthResult {_dependencies.credentials_valid(username, password), 0};
    if (result.retry_after > 0) {
      auto error = make_error(StatusCode::client_error_too_many_requests, "Too many authentication attempts. Try again shortly.");
      error.headers.emplace("Retry-After", std::to_string(result.retry_after));
      return error;
    }
    if (!result.accepted) return make_error(StatusCode::client_error_unauthorized, "Invalid credentials");
    return {true, StatusCode::success_ok, {}, {}};
  }

  AuthResult RequestAuthPolicy::check_setup(const std::string &remote_address) const {
    if (!http::security::is_loopback(remote_address)) return make_error(StatusCode::client_error_forbidden, "Initial setup requires access from this computer");
    if (_dependencies.credentials_configured()) return make_error(StatusCode::client_error_forbidden, "Credentials already configured");
    return {true, StatusCode::success_ok, {}, {}};
  }

  AuthResult RequestAuthPolicy::check_basic(const std::string &remote_address, const std::string &raw_auth) const {
    const auto space = raw_auth.find(' ');
    if (space == std::string::npos || !boost::iequals(raw_auth.substr(0, space), "Basic")) return basic_error();
    try {
      const auto decoded = _dependencies.decode_base64(raw_auth.substr(space + 1));
      const auto colon = decoded.find(':');
      if (colon == std::string::npos) return basic_error();
      auto result = check_password(remote_address, decoded.substr(0, colon), decoded.substr(colon + 1));
      if (!result.ok && result.code == StatusCode::client_error_unauthorized) return basic_error();
      return result;
    } catch (...) { return basic_error(); }
  }

  AuthResult RequestAuthPolicy::check_bearer(const std::string &raw_auth, const std::string &path, const std::string &method) const {
    if (!_dependencies.bearer_valid(raw_auth, path, method)) return make_error(StatusCode::client_error_forbidden, "Forbidden: Token does not have permission for this path/method.");
    return {true, StatusCode::success_ok, {}, {}};
  }

  AuthResult RequestAuthPolicy::check_session(const std::string &raw_auth) const {
    if (!raw_auth.starts_with("Session ")) return make_error(StatusCode::client_error_unauthorized, "Invalid session token format");
    if (!_dependencies.session_valid(raw_auth.substr(8))) return make_error(StatusCode::client_error_unauthorized, "Invalid or expired session token");
    return {true, StatusCode::success_ok, {}, {}};
  }

  bool RequestAuthPolicy::is_html_request(const std::string &path) const {
    if (path.starts_with("/api/") || path.starts_with("/assets/") || path.starts_with("/images/")) return false;
    auto ext = boost::algorithm::to_lower_copy(std::filesystem::path(path).extension().string());
    static const std::vector<std::string> non_html {".js", ".css", ".map", ".json", ".woff", ".woff2", ".ttf", ".eot", ".ico", ".png", ".jpg", ".jpeg", ".svg"};
    return std::ranges::find(non_html, ext) == non_html.end();
  }

  AuthResult RequestAuthPolicy::check(const std::string &remote_address, const std::string &auth_header, const std::string &path, const std::string &method) const {
    auto base_path = path.substr(0, path.find('?'));
    if (base_path == "/welcome" || base_path == "/welcome/") return {true, StatusCode::success_ok, {}, {}};
    if (!_dependencies.remote_allowed(remote_address)) return make_error(StatusCode::client_error_forbidden, "Forbidden");
    const bool is_api = base_path.starts_with("/api/");
    if (!is_api || base_path == "/api/auth/login" || base_path == "/api/auth/logout" || base_path == "/api/csrf-token") return {true, StatusCode::success_ok, {}, {}};
    if (!_dependencies.credentials_configured()) return make_error(StatusCode::client_error_unauthorized, "Credentials not configured");
    if (auth_header.empty()) return make_error(StatusCode::client_error_unauthorized, "Unauthorized");
    if (auth_header.starts_with("Bearer ")) return check_bearer(auth_header, path, method);
    if (auth_header.starts_with("Session ")) return check_session(auth_header);
    if (const auto space = auth_header.find(' '); space != std::string::npos && boost::iequals(auth_header.substr(0, space), "Basic")) return check_basic(remote_address, auth_header);
    return make_error(StatusCode::client_error_unauthorized, "Unauthorized");
  }

  std::string RequestAuthPolicy::extract_cookie(const SimpleWeb::CaseInsensitiveMultimap &headers, std::string_view name) const {
    const auto it = headers.find("Cookie");
    if (it == headers.end()) return {};
    const auto prefix = std::string(name) + "=";
    auto start = it->second.find(prefix);
    if (start == std::string::npos) return {};
    start += prefix.size();
    const auto end = it->second.find(';', start);
    return _dependencies.cookie_unescape(it->second.substr(start, end == std::string::npos ? std::string::npos : end - start));
  }
}  // namespace confighttp::policy
