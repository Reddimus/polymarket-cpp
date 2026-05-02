/// @file us_smoke.cpp
/// @brief Smoke-test the Polymarket US client end-to-end.
///
/// Verifies the auth handshake + a handful of public + authed
/// endpoints in <1s. Useful when:
///   - Bringing up new credentials (proves the secret decodes + signs)
///   - Onboarding a new operator (no Postgres / no compose required)
///   - Debugging an upstream API change (vs. our pipeline behaviour)
///
/// Usage:
///   export PM_US_KEY_ID=348090aa-...
///   export PM_US_SECRET_FILE=/path/to/base64-secret
///   ./build/examples/example_us_smoke
///
/// Exit codes:
///   0 = all calls succeeded
///   1 = bad / missing env
///   2 = secret-load error
///   3 = credential validation error
///   4 = at least one API call failed (which one logged to stderr)

#include <polymarket/us/client.hpp>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

std::string env_or_die(const char *name) {
  const char *v = std::getenv(name);
  if (!v || !*v) {
    std::cerr << "FATAL: env var " << name << " is required\n";
    std::exit(1);
  }
  return v;
}

std::string load_secret_file(const std::string &path) {
  std::ifstream in(path);
  if (!in) {
    std::cerr << "FATAL: cannot read secret file at " << path << '\n';
    std::exit(2);
  }
  std::ostringstream buf;
  buf << in.rdbuf();
  std::string out = buf.str();
  // Tolerate trailing whitespace from `cat <<EOF` style operator workflows.
  while (!out.empty() &&
         (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) {
    out.pop_back();
  }
  return out;
}

} // namespace

int main() {
  using namespace polymarket;

  const std::string key_id = env_or_die("PM_US_KEY_ID");
  const std::string secret_path = env_or_die("PM_US_SECRET_FILE");
  const std::string secret = load_secret_file(secret_path);

  us::Client client;
  if (auto rc = client.set_credentials({key_id, secret}); !rc.has_value()) {
    std::cerr << "FATAL: set_credentials failed: " << rc.error().message()
              << '\n';
    return 3;
  }

  int failures = 0;

  // ----- Public endpoints -----
  {
    auto h = client.get_health();
    if (h.has_value()) {
      std::cout << "[OK] health: " << h->substr(0, 80) << '\n';
    } else {
      std::cerr << "[FAIL] health: " << h.error().message() << '\n';
      ++failures;
    }
  }
  // Resolve weather tag id from the tag response — same pattern as
  // production discovery loops. Hardcoding tag_id=38 would silently
  // break if Polymarket renumbered tags (the markets endpoint would
  // return an empty list with 200 OK, which the smoke wouldn't catch).
  int weather_tag_id = -1;
  {
    auto t = client.get_tag_by_slug("weather");
    if (t.has_value()) {
      std::cout << "[OK] tag(weather): " << t->substr(0, 120) << '\n';
      const auto pos = t->find("\"id\":\"");
      if (pos != std::string::npos) {
        const auto start = pos + 6;
        const auto end = t->find('"', start);
        if (end != std::string::npos) {
          try {
            weather_tag_id = std::stoi(t->substr(start, end - start));
          } catch (...) {
            // fall through; markets check will flag the missing id
          }
        }
      }
    } else {
      std::cerr << "[FAIL] tag(weather): " << t.error().message() << '\n';
      ++failures;
    }
  }
  if (weather_tag_id < 0) {
    std::cerr
        << "[FAIL] could not parse tag id from get_tag_by_slug response\n";
    ++failures;
  } else {
    us::MarketFilter f;
    f.tag_id = weather_tag_id;
    f.active = true;
    f.closed = false;
    f.limit = 3;
    auto m = client.get_markets(f);
    if (m.has_value() && !m->empty() &&
        m->find("\"slug\"") != std::string::npos) {
      std::cout << "[OK] markets(tag=" << weather_tag_id
                << ", limit=3) bytes=" << m->size() << '\n';
    } else if (m.has_value()) {
      std::cerr << "[FAIL] markets returned empty payload — tag id "
                << weather_tag_id << " may be wrong\n";
      ++failures;
    } else {
      std::cerr << "[FAIL] markets: " << m.error().message() << '\n';
      ++failures;
    }
  }

  // ----- Authed endpoints -----
  {
    auto b = client.get_balance();
    if (b.has_value()) {
      std::cout << "[OK] balance: " << b->substr(0, 200) << '\n';
    } else {
      std::cerr << "[FAIL] balance: " << b.error().message() << '\n';
      ++failures;
    }
  }
  {
    auto p = client.get_positions();
    if (p.has_value()) {
      std::cout << "[OK] positions: " << p->substr(0, 120) << '\n';
    } else {
      std::cerr << "[FAIL] positions: " << p.error().message() << '\n';
      ++failures;
    }
  }

  if (failures > 0) {
    std::cerr << "\n" << failures << " call(s) failed\n";
    return 4;
  }
  std::cout << "\nall checks passed\n";
  return 0;
}
