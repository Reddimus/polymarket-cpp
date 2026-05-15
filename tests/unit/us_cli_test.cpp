/// @file us_cli_test.cpp
/// @brief Smoke tests for the polymarket-us-cli binary.
///
/// We exec the built binary via popen and assert its JSON envelope
/// shape + exit-code mapping. These are integration-style tests
/// (require the binary to be built first); the unit tests for the
/// underlying pm::us::Client live in us_client_test.cpp.
///
/// Discovery: ctest's working directory is build/tests; the binary
/// is at ../apps/polymarket-us-cli relative to that. We pass the
/// resolved path via cmake's $<TARGET_FILE:polymarket_us_cli> at
/// configure time.

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

// POSIX uses ``popen``/``pclose`` and ``WEXITSTATUS`` from <sys/wait.h>.
// MSVC ships the same primitives renamed to ``_popen``/``_pclose``;
// the integer return from ``_pclose`` is already the exit code (no
// WEXITSTATUS wrapper needed). ``getpid`` lives in <process.h> on
// Windows but ``::getpid`` is also a Windows ``CRT`` alias for
// ``_getpid`` if we include <process.h>. Aliases below normalise both.
#if defined(_WIN32)
#include <process.h>
#define popen _popen
#define pclose _pclose
#define WEXITSTATUS(x) (x)
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

/// Path injected at compile time via CMake's POLYMARKET_US_CLI_PATH.
/// Falls back to a hard-coded relative path if the macro is not set.
#ifndef POLYMARKET_US_CLI_PATH
#define POLYMARKET_US_CLI_PATH "../apps/polymarket-us-cli"
#endif

constexpr const char* kCliPath = POLYMARKET_US_CLI_PATH;

/// Run `kCliPath args... < stdin_text` and capture stdout + exit code.
struct CliRun {
	int exit_code;
	std::string stdout_text;
};

CliRun run_cli(const std::string& args, const std::string& stdin_text) {
	// Write stdin to a temp file, then redirect via shell.
	const std::filesystem::path tmpdir = std::filesystem::temp_directory_path();
	const std::filesystem::path stdin_path =
		tmpdir / ("us_cli_test_stdin_" + std::to_string(::getpid()));
	{
		std::ofstream f(stdin_path);
		f << stdin_text;
	}
	// POSIX shell uses ``/dev/null``; Windows ``cmd.exe`` uses ``nul``.
	// ``_popen`` on Windows runs the command via cmd, so the right
	// null device differs by platform.
#if defined(_WIN32)
	constexpr const char* kNullDevice = "nul";
#else
	constexpr const char* kNullDevice = "/dev/null";
#endif
	std::string cmd =
		std::string(kCliPath) + " " + args + " < " + stdin_path.string() + " 2>" + kNullDevice;
	FILE* p = ::popen(cmd.c_str(), "r");
	REQUIRE(p != nullptr);
	std::ostringstream out;
	std::array<char, 4096> buf{};
	while (std::fgets(buf.data(), buf.size(), p) != nullptr)
		out << buf.data();
	int rc = ::pclose(p);
	std::filesystem::remove(stdin_path);
	int exit_code = (rc == -1) ? -1 : WEXITSTATUS(rc);
	return {exit_code, out.str()};
}

/// Synthesize a Polymarket-shape credential JSON file (key_id +
/// base64 of 64-byte seed||pub). Lets the binary boot past
/// set_credentials() without reaching the network.
std::filesystem::path make_synthetic_creds_file() {
	const std::filesystem::path path = std::filesystem::temp_directory_path() /
									   ("us_cli_test_creds_" + std::to_string(::getpid()));
	// base64 of 64 bytes filled with 0x42 (= 'B'). The pm::us::Client
	// secret must decode to exactly 64 bytes (seed||pub); the seed
	// half just needs to round-trip through Ed25519PrivateKey
	// successfully — we don't sign anything in these tests.
	const std::string secret_b64 =
		"QkJCQkJCQkJCQkJCQkJCQkJCQkJCQkJCQkJCQkJCQkJCQkJCQkJCQkJCQkJCQkJC"
		"QkJCQkJCQkJCQkJCQkJCQg==";
	std::ofstream f(path);
	f << R"({"key_id":"test-key","secret_key":")" << secret_b64 << R"("})";
	return path;
}

} // namespace

TEST_CASE("polymarket-us-cli --version emits version and exits 0", "[us_cli][version]") {
	CliRun r = run_cli("--version", "");
	REQUIRE(r.exit_code == 0);
	REQUIRE_THAT(r.stdout_text, Catch::Matchers::ContainsSubstring("polymarket-us-cli"));
}

TEST_CASE("polymarket-us-cli requires --credentials", "[us_cli][args]") {
	CliRun r = run_cli("", R"({"command":"get_account"})");
	REQUIRE(r.exit_code == 3);
	REQUIRE_THAT(r.stdout_text, Catch::Matchers::ContainsSubstring("\"ok\":false"));
	REQUIRE_THAT(r.stdout_text, Catch::Matchers::ContainsSubstring("--credentials"));
}

TEST_CASE("polymarket-us-cli rejects nonexistent credentials path", "[us_cli][creds]") {
	CliRun r = run_cli("--credentials /this/path/does/not/exist", R"({"command":"get_account"})");
	REQUIRE(r.exit_code == 2);
	REQUIRE_THAT(r.stdout_text, Catch::Matchers::ContainsSubstring("\"code\":\"auth\""));
}

TEST_CASE("polymarket-us-cli rejects empty stdin", "[us_cli][protocol]") {
	const auto creds = make_synthetic_creds_file();
	CliRun r = run_cli("--credentials " + creds.string(), "");
	std::filesystem::remove(creds);
	// 5 because empty-stdin classifies as bad_request → exit 5.
	REQUIRE((r.exit_code == 3 || r.exit_code == 5));
	REQUIRE_THAT(r.stdout_text, Catch::Matchers::ContainsSubstring("stdin is empty"));
}

TEST_CASE("polymarket-us-cli rejects malformed stdin JSON", "[us_cli][protocol]") {
	const auto creds = make_synthetic_creds_file();
	CliRun r = run_cli("--credentials " + creds.string(), "{not json}");
	std::filesystem::remove(creds);
	REQUIRE(r.exit_code == 5); // bad_request mapped to 5
	REQUIRE_THAT(r.stdout_text, Catch::Matchers::ContainsSubstring("stdin JSON parse"));
}

TEST_CASE("polymarket-us-cli rejects unknown command", "[us_cli][protocol]") {
	const auto creds = make_synthetic_creds_file();
	CliRun r =
		run_cli("--credentials " + creds.string(), R"({"command":"this_command_does_not_exist"})");
	std::filesystem::remove(creds);
	REQUIRE(r.exit_code == 5);
	REQUIRE_THAT(r.stdout_text, Catch::Matchers::ContainsSubstring("unknown command"));
}

TEST_CASE("polymarket-us-cli place_order requires market_id, side, type, "
		  "price, size",
		  "[us_cli][place_order][validation]") {
	const auto creds = make_synthetic_creds_file();
	// Missing every required param except command.
	CliRun r =
		run_cli("--credentials " + creds.string(), R"({"command":"place_order","params":{}})");
	std::filesystem::remove(creds);
	REQUIRE(r.exit_code == 5);
	REQUIRE_THAT(r.stdout_text, Catch::Matchers::ContainsSubstring("place_order params"));
}

TEST_CASE("polymarket-us-cli cancel_order requires order_id",
		  "[us_cli][cancel_order][validation]") {
	const auto creds = make_synthetic_creds_file();
	CliRun r =
		run_cli("--credentials " + creds.string(), R"({"command":"cancel_order","params":{}})");
	std::filesystem::remove(creds);
	REQUIRE(r.exit_code == 5);
	REQUIRE_THAT(r.stdout_text, Catch::Matchers::ContainsSubstring("order_id"));
}

TEST_CASE("polymarket-us-cli get_order requires order_id", "[us_cli][get_order][validation]") {
	const auto creds = make_synthetic_creds_file();
	CliRun r = run_cli("--credentials " + creds.string(), R"({"command":"get_order","params":{}})");
	std::filesystem::remove(creds);
	REQUIRE(r.exit_code == 5);
	REQUIRE_THAT(r.stdout_text, Catch::Matchers::ContainsSubstring("order_id"));
}
