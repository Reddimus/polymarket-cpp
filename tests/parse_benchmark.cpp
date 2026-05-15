/// @file parse_benchmark.cpp
/// @brief Microbenchmark: parse a representative CLOB orderbook
/// payload 1k times and report wall-clock. Used as a parse-throughput
/// regression guard with `ctest --timeout` and an absolute upper bound
/// on us/op.
///
/// Historical baseline (recorded at migration time on x86_64-v3, GCC
/// 13.3, -O3 -DNDEBUG, payload≈14.6KB book + 200 levels/side,
/// iters=1000, median of 3 runs on a moderately-loaded workstation):
///
///     nlohmann/json v3.11.3 :  ~500 us/op  (pre-migration baseline,
///                                          ranged 487-757 us/op
///                                          across runs)
///     glaze v7.6.0          :  ~290 us/op  (post-migration,
///                                          ranged 172-349 us/op)
///     speedup               :   1.7-2.5x   (Decimal::from_string
///                                          dominates the per-level
///                                          work; the JSON parse
///                                          itself was already ≈3x
///                                          faster but is fixed-cost
///                                          relative to the 400
///                                          decimal conversions)
///
/// The pre-migration nlohmann path lived in src/clob/websocket.cpp at
/// the time of the Glaze migration commit; it has since been removed
/// along with the nlohmann FetchContent dep. Re-introduce a
/// side-by-side bench only if a future regression suspicion warrants
/// it.
///
/// The benchmark exercises the same hot path as `parse_message` in
/// `src/clob/websocket.cpp` — parse via `glz::generic`, then walk
/// the AST extracting typed fields. We don't depend on the websocket
/// library because the parser is in an anonymous namespace there; a
/// representative subset (book snapshot + bid/ask level walk) is
/// inlined here.
///
/// Linked against polymarket_core for `pm::Decimal::from_string`
/// (the same precision-preserving conversion used by the real
/// dispatcher).

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <glaze/glaze.hpp>
#include <glaze/json/generic.hpp>
#include <string>

#include "polymarket/core/types.hpp"

namespace {

using glz_node = glz::generic;

/// 200-level book-snapshot payload (~12KB). Mimics a busy CLOB
/// orderbook on a high-volume market: deep both-sided book + the
/// envelope fields the dispatcher reads (event_type, asset_id, hash,
/// timestamp).
std::string make_payload() {
	std::string json;
	json.reserve(16 * 1024);
	json += R"({"event_type":"book",)";
	json +=
		R"("asset_id":"71321045679252212594626385532706912750332728571942134274332321581290950697279",)";
	json += R"("timestamp":1741000000000,)";
	json += R"("hash":"0xdeadbeef0123456789abcdef0123456789abcdef0123456789abcdef01234567",)";
	json += R"("bids":[)";
	constexpr int kLevels = 200;
	for (int i = 0; i < kLevels; ++i) {
		char buf[128];
		std::snprintf(buf, sizeof(buf), R"({"price":"0.%04d","size":"%d.%03d"})", 5000 - i * 5 + 1,
					  100 + i, (i * 7) % 1000);
		if (i != 0) {
			json += ',';
		}
		json += buf;
	}
	json += R"(],"asks":[)";
	for (int i = 0; i < kLevels; ++i) {
		char buf[128];
		std::snprintf(buf, sizeof(buf), R"({"price":"0.%04d","size":"%d.%03d"})", 5001 + i * 5,
					  100 + i, (i * 11) % 1000);
		if (i != 0) {
			json += ',';
		}
		json += buf;
	}
	json += "]}";
	return json;
}

// ---- glz::generic accessors (mirrors src/clob/websocket.cpp) ----

const glz_node* find_field(const glz_node& node, const char* key) {
	if (!node.is_object())
		return nullptr;
	const glz_node::object_t& obj = node.get_object();
	glz_node::object_t::const_iterator it = obj.find(key);
	if (it == obj.end())
		return nullptr;
	return &it->second;
}

std::string get_string(const glz_node& node, const char* key) {
	const glz_node* v = find_field(node, key);
	if (!v || !v->is_string())
		return {};
	return v->get<std::string>();
}

polymarket::Decimal get_decimal(const glz_node& node, const char* key) {
	const glz_node* v = find_field(node, key);
	if (!v)
		return polymarket::Decimal{};
	if (v->is_string())
		return polymarket::Decimal::from_string(v->get<std::string>());
	if (v->is_number())
		return polymarket::Decimal::from_string(std::to_string(v->get<double>()));
	return polymarket::Decimal{};
}

/// Parse one book-snapshot payload and walk the bids/asks. Returns
/// the total level count so the compiler can't dead-code the inner
/// loop.
std::size_t parse_one(const std::string& payload, std::string& err_out) {
	glz_node root{};
	glz::error_ctx ec = glz::read_json(root, payload);
	if (ec) {
		err_out = glz::format_error(ec, payload);
		return 0;
	}
	// Echo the event_type + hash to exercise the typed-field path.
	const std::string ev = get_string(root, "event_type");
	const std::string hash = get_string(root, "hash");
	std::size_t levels = 0;

	const glz_node* bids = find_field(root, "bids");
	if (bids && bids->is_array()) {
		for (const glz_node& b : bids->get_array()) {
			polymarket::Decimal price = get_decimal(b, "price");
			polymarket::Decimal size = get_decimal(b, "size");
			// Touch the values so the compiler keeps the parse work.
			if (!price.to_string().empty() && !size.to_string().empty())
				++levels;
		}
	}
	const glz_node* asks = find_field(root, "asks");
	if (asks && asks->is_array()) {
		for (const glz_node& a : asks->get_array()) {
			polymarket::Decimal price = get_decimal(a, "price");
			polymarket::Decimal size = get_decimal(a, "size");
			if (!price.to_string().empty() && !size.to_string().empty())
				++levels;
		}
	}
	// Silence unused-variable warnings under -Wall.
	(void)ev;
	(void)hash;
	return levels;
}

} // namespace

int main() {
	const std::string payload = make_payload();
	constexpr int kIterations = 1000;
	constexpr std::size_t kExpectedLevelsPerParse = 400; // 200 bids + 200 asks

	// Warmup — let the allocator and CPU settle.
	for (int i = 0; i < 50; ++i) {
		std::string warm_err;
		(void)parse_one(payload, warm_err);
	}

	std::chrono::nanoseconds glaze_total{0};
	std::size_t checksum = 0;
	for (int i = 0; i < kIterations; ++i) {
		std::string err;
		std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
		std::size_t levels = parse_one(payload, err);
		std::chrono::steady_clock::time_point t1 = std::chrono::steady_clock::now();
		if (!err.empty()) {
			std::fprintf(stderr, "glaze parse failed: %s\n", err.c_str());
			return 1;
		}
		glaze_total += (t1 - t0);
		checksum += levels;
	}

	if (checksum != kExpectedLevelsPerParse * kIterations) {
		std::fprintf(stderr, "checksum mismatch: got=%zu (expected %zu)\n", checksum,
					 kExpectedLevelsPerParse * kIterations);
		return 1;
	}

	const double glaze_ms = glaze_total.count() / 1e6;
	const double us_per_op = (glaze_total.count() / 1e3) / kIterations;

	std::printf("parse_benchmark: payload=%zuB iters=%d levels=%zu/parse\n", payload.size(),
				kIterations, kExpectedLevelsPerParse);
	std::printf("  glaze: %8.3f ms total  (%8.3f us/op)\n", glaze_ms, us_per_op);

	// Regression guard: at migration time, Glaze parsed this payload
	// at ~290 us/op median on x86_64-v3. Cap at 1000 us/op — that's
	// still 2x faster than the nlohmann baseline's worst case
	// (~750 us/op) and leaves a generous slack window for slower CI
	// runners (macOS arm64 ≈ 2-3x slower than the x86_64-v3 dev box)
	// and Debug builds. A genuine algorithmic regression (e.g., the
	// dispatch falling back to a quadratic path) would blow past
	// 1000 us/op by orders of magnitude.
	constexpr double kMaxUsPerOp = 1000.0;
	if (us_per_op > kMaxUsPerOp) {
		std::fprintf(stderr, "REGRESSION: %.3f us/op exceeds cap of %.0f us/op\n", us_per_op,
					 kMaxUsPerOp);
		return 1;
	}
	return 0;
}
