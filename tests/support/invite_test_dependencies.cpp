#include <src/crypto.h>
#include <src/file_handler.h>
#include <src/logging.h>
#include <src/state_storage.h>

#include <atomic>
#include <filesystem>
#include <fstream>

boost::log::sources::severity_logger<int> verbose, debug, info, warning, error, fatal;

namespace crypto {
  std::string rand_alphabet(std::size_t, const std::string_view &) {
    static std::atomic<unsigned> sequence {0};
    return "test-token-" + std::to_string(++sequence);
  }
}

namespace statefile {
  const std::string &sunshine_state_path() {
    static const std::string path = (std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() / ".autodev/tmp/security-hardening/guest/lifecycle-state/state.json").string();
    return path;
  }
  std::mutex &state_mutex() {
    static std::mutex mutex;
    return mutex;
  }
}

namespace file_handler {
  int write_file(const char *path, const std::string_view &contents) {
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    std::ofstream out(path);
    out << contents;
    return out.good() ? 0 : -1;
  }
}
