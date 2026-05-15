/// @file websocket.cpp
/// @brief CLOB WebSocket client implementation (offshore CLOB endpoint).
///
/// Connects to ``wss://ws-subscriptions-clob.polymarket.com/ws/`` and
/// emits typed messages on the supplied callback.
///
/// Cribs the libwebsockets event-loop + send-queue plumbing from
/// ``src/us/ws/subscriber.cpp`` (the production-shipping US-gateway
/// subscriber). The transport layer is identical; what differs is:
///
///   - URL path: ``/ws/`` for the offshore CLOB (vs ``/v1/ws/markets``)
///   - Auth: optional HMAC-SHA256 + L2 headers for the user channel
///     (vs Ed25519 for the US gateway)
///   - Subscribe frame: ``{"type":"market","assets_ids":[...]}`` /
///     ``{"type":"user","markets":[...]}``
///   - Inbound parsing: dispatch onto the WsMessage variant (one
///     case per ``event_type`` field — book snapshots, deltas, price
///     changes, trades, fills, cancels)
///
/// The variant-based message dispatch handles the most common
/// ``event_type`` values; an unknown type is logged via the error
/// callback but does not tear down the connection. Parsing uses
/// Glaze's `glz::generic` AST: the message shape is dynamic-per-type
/// (different ``event_type`` values yield different field sets), so we
/// parse once into the generic AST and walk it. Migrated from
/// nlohmann/json on 2026-05-11.

#include "polymarket/clob/websocket.hpp"

#include "polymarket/core/error.hpp"
#include "polymarket/crypto/hmac.hpp"

namespace polymarket::clob {
using polymarket::crypto::generate_l2_signature;
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <glaze/glaze.hpp>
#include <glaze/json/generic.hpp>
#include <iostream>
#include <libwebsockets.h>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

namespace polymarket::clob {

namespace {

using glz_node = glz::generic;

constexpr std::size_t kMaxFrameSize = 1 << 20; // 1 MiB
constexpr const char* kClobProtocolName = "polymarket-clob-ws";

std::string now_unix_ms() {
	const auto now = std::chrono::system_clock::now().time_since_epoch();
	return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

/// Tokenize the ``wss://host[:port]/path`` form into its three pieces.
struct ParsedUrl {
	std::string host;
	int port = 443;
	std::string path = "/ws/";
	bool ssl = true;
};

ParsedUrl parse_url(std::string_view url) {
	ParsedUrl r;
	std::string_view rest = url;
	if (rest.starts_with("wss://")) {
		r.ssl = true;
		rest.remove_prefix(6);
	} else if (rest.starts_with("ws://")) {
		r.ssl = false;
		r.port = 80;
		rest.remove_prefix(5);
	}
	const auto slash = rest.find('/');
	std::string_view host_port = rest.substr(0, slash);
	if (slash != std::string_view::npos) {
		r.path = std::string(rest.substr(slash));
	}
	const auto colon = host_port.find(':');
	if (colon != std::string_view::npos) {
		r.host = std::string(host_port.substr(0, colon));
		auto port_sv = host_port.substr(colon + 1);
		try {
			r.port = std::stoi(std::string(port_sv));
		} catch (...) {
			// leave default
		}
	} else {
		r.host = std::string(host_port);
	}
	return r;
}

// ---- glz::generic access helpers ----
//
// All field reads go through these so a missing or wrong-typed field
// produces a typed default rather than a throwing access. The bool
// overload below is named ``get_bool_field`` (NOT another overload of
// ``get_*``) to avoid the bool/string_view overload-set ambiguity that
// bit the US client's ``append_query`` in v0.1.0 (see memory
// ``feedback_polymarket_cpp_overload_bug.md``). The dispatcher's
// inputs are always typed JSON, not raw literals, so the risk is
// lower here — but we keep the names distinct to be safe.

const glz_node* find_field(const glz_node& node, const char* key) {
	if (!node.is_object()) {
		return nullptr;
	}
	const glz_node::object_t& obj = node.get_object();
	glz_node::object_t::const_iterator it = obj.find(key);
	if (it == obj.end()) {
		return nullptr;
	}
	return &it->second;
}

std::string get_string(const glz_node& node, const char* key) {
	const glz_node* v = find_field(node, key);
	if (!v || !v->is_string()) {
		return {};
	}
	return v->get<std::string>();
}

Decimal get_decimal(const glz_node& node, const char* key) {
	const glz_node* v = find_field(node, key);
	if (!v) {
		return Decimal{};
	}
	if (v->is_string()) {
		return Decimal::from_string(v->get<std::string>());
	}
	if (v->is_number()) {
		return Decimal::from_string(std::to_string(v->get<double>()));
	}
	return Decimal{};
}

uint256_t get_uint256(const glz_node& node, const char* key) {
	const glz_node* v = find_field(node, key);
	if (!v || !v->is_string()) {
		return uint256_t{};
	}
	return uint256_t::from_string(v->get<std::string>());
}

Side get_side(const glz_node& node, const char* key) {
	const std::string s = get_string(node, key);
	if (s == "BUY" || s == "buy") {
		return Side::Buy;
	}
	return Side::Sell;
}

Timestamp get_timestamp(const glz_node& node, const char* key) {
	const glz_node* v = find_field(node, key);
	if (!v) {
		return Timestamp{};
	}
	if (v->is_string()) {
		try {
			return static_cast<Timestamp>(std::stoll(v->get<std::string>()));
		} catch (...) {
			return Timestamp{};
		}
	}
	if (v->is_number()) {
		return static_cast<Timestamp>(v->get<double>());
	}
	return Timestamp{};
}

bool get_bool_field(const glz_node& node, const char* key, bool fallback) {
	const glz_node* v = find_field(node, key);
	if (!v || !v->is_boolean()) {
		return fallback;
	}
	return v->get<bool>();
}

/// Parse one inbound JSON message into a typed ``WsMessage`` variant.
/// Returns ``std::nullopt`` for unknown ``event_type`` values; the
/// caller logs and drops. Field reads all go through ``find_field``
/// so missing optional fields produce a well-typed default rather
/// than throwing.
std::optional<WsMessage> parse_message(const glz_node& j) {
	const std::string ev = get_string(j, "event_type");

	if (ev == "book") {
		BookSnapshot s;
		s.asset_id = get_uint256(j, "asset_id");
		s.timestamp = get_timestamp(j, "timestamp");
		s.hash = get_string(j, "hash");
		const glz_node* bids = find_field(j, "bids");
		if (bids && bids->is_array()) {
			for (const glz_node& b : bids->get_array()) {
				s.bids.push_back({get_decimal(b, "price"), get_decimal(b, "size")});
			}
		}
		const glz_node* asks = find_field(j, "asks");
		if (asks && asks->is_array()) {
			for (const glz_node& a : asks->get_array()) {
				s.asks.push_back({get_decimal(a, "price"), get_decimal(a, "size")});
			}
		}
		return s;
	}
	if (ev == "price_change") {
		BookDelta d;
		d.asset_id = get_uint256(j, "asset_id");
		d.price = get_decimal(j, "price");
		d.size = get_decimal(j, "size");
		d.side = get_side(j, "side");
		d.timestamp = get_timestamp(j, "timestamp");
		return d;
	}
	if (ev == "tick_size_change") {
		TickSizeChange t;
		t.asset_id = get_uint256(j, "asset_id");
		t.timestamp = get_timestamp(j, "timestamp");
		// ``new_tick_size`` arrives as a numeric string ("0.01", "0.001"
		// etc). The SDK's TickSize is a closed enum; map by literal.
		const std::string nts = get_string(j, "new_tick_size");
		if (nts == "0.1") {
			t.new_tick_size = TickSize::Tenth;
		} else if (nts == "0.01") {
			t.new_tick_size = TickSize::Hundredth;
		} else if (nts == "0.001") {
			t.new_tick_size = TickSize::Thousandth;
		} else if (nts == "0.0001") {
			t.new_tick_size = TickSize::TenThousandth;
		} else {
			t.new_tick_size = TickSize::Hundredth; // safe default
		}
		return t;
	}
	if (ev == "last_trade_price") {
		LastTradePriceMessage m;
		m.asset_id = get_uint256(j, "asset_id");
		m.price = get_decimal(j, "price");
		m.timestamp = get_timestamp(j, "timestamp");
		return m;
	}
	if (ev == "order") {
		OrderFill f;
		f.order_id = get_string(j, "id");
		f.asset_id = get_uint256(j, "asset_id");
		f.price = get_decimal(j, "price");
		f.size = get_decimal(j, "size");
		f.side = get_side(j, "side");
		f.is_taker = get_bool_field(j, "is_taker", false);
		f.timestamp = get_timestamp(j, "timestamp");
		const std::string tx = get_string(j, "transaction_hash");
		if (!tx.empty()) {
			f.transaction_hash = tx;
		}
		return f;
	}
	if (ev == "cancel") {
		OrderCancel c;
		c.order_id = get_string(j, "id");
		c.asset_id = get_uint256(j, "asset_id");
		c.reason = get_string(j, "reason");
		c.timestamp = get_timestamp(j, "timestamp");
		return c;
	}
	if (ev == "trade") {
		TradeConfirm t;
		t.trade_id = get_string(j, "id");
		t.order_id = get_string(j, "order_id");
		t.asset_id = get_uint256(j, "asset_id");
		t.price = get_decimal(j, "price");
		t.size = get_decimal(j, "size");
		t.timestamp = get_timestamp(j, "timestamp");
		const std::string st = get_string(j, "status");
		if (st == "MATCHED" || st == "matched") {
			t.status = TradeStatus::Matched;
		} else if (st == "MINED" || st == "mined") {
			t.status = TradeStatus::Mined;
		} else if (st == "CONFIRMED" || st == "confirmed") {
			t.status = TradeStatus::Confirmed;
		} else if (st == "FAILED" || st == "failed") {
			t.status = TradeStatus::Failed;
		} else {
			t.status = TradeStatus::Matched;
		}
		return t;
	}
	return std::nullopt;
}

} // namespace

// ─── Impl ──────────────────────────────────────────────────────────

static int clob_ws_callback(struct lws* wsi, enum lws_callback_reasons reason, void* user, void* in,
							std::size_t len);

struct WebSocketClient::Impl {
	WsConfig config;
	std::optional<Credentials> credentials;
	ParsedUrl url;

	// Connection.
	struct lws_context* context = nullptr;
	struct lws* wsi = nullptr;
	std::thread service_thread;
	std::atomic<bool> connected{false};
	std::atomic<bool> should_stop{false};

	// Application-level keepalive (libwebsockets has built-in PING but
	// Polymarket's gateway expects an application-level frame every
	// ``ping_interval``).
	std::chrono::steady_clock::time_point last_ping{};

	// Outbound queue. lws_write must be called from the service thread
	// in response to a CLIENT_WRITEABLE callback.
	std::mutex send_mutex;
	std::deque<std::string> send_queue;

	// Reassembly buffer for fragmented frames.
	std::string fragment_buf;

	// Subscriptions tracked for subscription_count().
	mutable std::mutex sub_mutex;
	std::vector<SubscriptionId> active_subs;
	std::atomic<std::uint64_t> next_sub_id{1};

	// Callbacks.
	WsMessageCallback message_cb;
	WsErrorCallback error_cb;
	WsStateCallback state_cb;

	/// Compute timestamp + signature for the user-channel auth payload.
	/// Polymarket CLOB uses HMAC-SHA256 over ``ts + GET + path`` with
	/// the L2 secret. Returns ``{ts, sig_base64}``.
	std::optional<std::pair<std::string, std::string>> compute_auth() const {
		if (!credentials.has_value()) {
			return std::nullopt;
		}
		const std::string ts = now_unix_ms();
		auto sig = generate_l2_signature(credentials->api_secret, ts, "GET", url.path, /*body=*/"");
		if (!sig.has_value()) {
			return std::nullopt;
		}
		return std::make_pair(ts, *sig);
	}

	void enqueue(std::string frame) {
		{
			std::lock_guard<std::mutex> g(send_mutex);
			send_queue.push_back(std::move(frame));
		}
		if (wsi) {
			lws_callback_on_writable(wsi);
		}
	}

	void emit_error(int code, const std::string& msg) {
		if (error_cb) {
			error_cb(WsError{code, msg});
		}
	}
};

// ─── lws callback ──────────────────────────────────────────────────

static int clob_ws_callback(struct lws* wsi, enum lws_callback_reasons reason, void* user, void* in,
							std::size_t len) {
	(void)user;
	auto* impl = static_cast<WebSocketClient::Impl*>(lws_context_user(lws_get_context(wsi)));

	switch (reason) {
		case LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER: {
			auto auth = impl->compute_auth();
			if (!auth.has_value()) {
				return 0;
			}
			auto** p = reinterpret_cast<unsigned char**>(in);
			auto* end = (*p) + len;
			auto add = [&](const char* name, const std::string& value) {
				return lws_add_http_header_by_name(
					wsi, reinterpret_cast<const unsigned char*>(name),
					reinterpret_cast<const unsigned char*>(value.c_str()),
					static_cast<int>(value.size()), p, end);
			};
			if (add("POLY_ADDRESS:", impl->credentials->api_key) != 0 ||
				add("POLY_TIMESTAMP:", auth->first) != 0 ||
				add("POLY_SIGNATURE:", auth->second) != 0 ||
				add("POLY_API_KEY:", impl->credentials->api_key) != 0 ||
				add("POLY_PASSPHRASE:", impl->credentials->passphrase) != 0) {
				impl->emit_error(0, "failed to attach CLOB auth headers");
				return -1;
			}
			return 0;
		}

		case LWS_CALLBACK_CLIENT_ESTABLISHED: {
			impl->connected.store(true, std::memory_order_release);
			impl->last_ping = std::chrono::steady_clock::now();
			if (impl->state_cb) {
				impl->state_cb(true);
			}
			return 0;
		}

		case LWS_CALLBACK_CLIENT_RECEIVE: {
			if (!in || !len) {
				return 0;
			}
			impl->fragment_buf.append(static_cast<const char*>(in), len);
			if (!lws_is_final_fragment(wsi)) {
				return 0;
			}
			std::string payload = std::move(impl->fragment_buf);
			impl->fragment_buf.clear();

			glz_node parsed{};
			glz::error_ctx ec = glz::read_json(parsed, payload);
			if (ec) {
				impl->emit_error(0, std::string{"invalid JSON: "} + glz::format_error(ec, payload));
				return 0;
			}

			auto dispatch_one = [&](const glz_node& j) {
				std::optional<WsMessage> msg = parse_message(j);
				if (!msg.has_value()) {
					return;
				}
				if (impl->message_cb) {
					impl->message_cb(*msg);
				}
			};
			if (parsed.is_array()) {
				for (const glz_node& el : parsed.get_array()) {
					dispatch_one(el);
				}
			} else {
				dispatch_one(parsed);
			}
			return 0;
		}

		case LWS_CALLBACK_CLIENT_WRITEABLE: {
			std::string frame;
			{
				std::lock_guard<std::mutex> g(impl->send_mutex);
				if (impl->send_queue.empty()) {
					return 0;
				}
				frame = std::move(impl->send_queue.front());
				impl->send_queue.pop_front();
			}
			std::vector<unsigned char> buf(LWS_PRE + frame.size());
			std::memcpy(buf.data() + LWS_PRE, frame.data(), frame.size());
			int written = lws_write(wsi, buf.data() + LWS_PRE, frame.size(), LWS_WRITE_TEXT);
			if (written < 0) {
				impl->emit_error(0, "lws_write failed");
				return -1;
			}
			{
				std::lock_guard<std::mutex> g(impl->send_mutex);
				if (!impl->send_queue.empty()) {
					lws_callback_on_writable(wsi);
				}
			}
			return 0;
		}

		case LWS_CALLBACK_CLIENT_CONNECTION_ERROR: {
			const std::string err =
				in ? std::string(static_cast<const char*>(in), len) : std::string{"(no detail)"};
			impl->emit_error(0, "connection error: " + err);
			impl->connected.store(false, std::memory_order_release);
			if (impl->state_cb) {
				impl->state_cb(false);
			}
			return -1;
		}

		case LWS_CALLBACK_CLIENT_CLOSED: {
			impl->connected.store(false, std::memory_order_release);
			if (impl->state_cb) {
				impl->state_cb(false);
			}
			return 0;
		}

		default:
			return 0;
	}
}

static const struct lws_protocols kProtocols[] = {
	{kClobProtocolName, clob_ws_callback, 0, kMaxFrameSize, 0, nullptr, 0},
	{nullptr, nullptr, 0, 0, 0, nullptr, 0}};

// ─── WebSocketClient public surface ────────────────────────────────

WebSocketClient::WebSocketClient(WsConfig config) : impl_(std::make_unique<Impl>()) {
	impl_->config = std::move(config);
	impl_->url = parse_url(impl_->config.url);
}

WebSocketClient::WebSocketClient(WsConfig config, Credentials credentials)
	: impl_(std::make_unique<Impl>()) {
	impl_->config = std::move(config);
	impl_->url = parse_url(impl_->config.url);
	impl_->credentials = std::move(credentials);
}

WebSocketClient::~WebSocketClient() {
	disconnect();
}

WebSocketClient::WebSocketClient(WebSocketClient&&) noexcept = default;
WebSocketClient& WebSocketClient::operator=(WebSocketClient&&) noexcept = default;

VoidResult WebSocketClient::connect() {
	// A moved-from instance has impl_ == nullptr. Surface a clean error
	// rather than dereferencing through nullptr a few lines below.
	if (!impl_) {
		return std::unexpected(Error::network("client moved-from"));
	}
	// Idempotent: tear down any existing context before standing a new
	// one up.
	disconnect();

	struct lws_context_creation_info ctx_info {};
	std::memset(&ctx_info, 0, sizeof(ctx_info));
	ctx_info.port = CONTEXT_PORT_NO_LISTEN;
	ctx_info.protocols = kProtocols;
	ctx_info.user = impl_.get();
	ctx_info.options = impl_->url.ssl ? LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT : 0;

	impl_->context = lws_create_context(&ctx_info);
	if (!impl_->context) {
		return std::unexpected(Error::network("lws_create_context failed"));
	}

	struct lws_client_connect_info ci {};
	std::memset(&ci, 0, sizeof(ci));
	ci.context = impl_->context;
	ci.address = impl_->url.host.c_str();
	ci.port = impl_->url.port;
	ci.path = impl_->url.path.c_str();
	ci.host = impl_->url.host.c_str();
	ci.origin = impl_->url.host.c_str();
	ci.protocol = kProtocols[0].name;
	ci.ssl_connection = impl_->url.ssl ? LCCSCF_USE_SSL : 0;

	impl_->wsi = lws_client_connect_via_info(&ci);
	if (!impl_->wsi) {
		lws_context_destroy(impl_->context);
		impl_->context = nullptr;
		return std::unexpected(Error::network("lws_client_connect_via_info failed"));
	}

	impl_->should_stop.store(false, std::memory_order_release);
	impl_->service_thread = std::thread([impl = impl_.get()]() {
		while (!impl->should_stop.load(std::memory_order_acquire) && impl->context) {
			lws_service(impl->context, 50);
			if (impl->connected.load(std::memory_order_acquire)) {
				const auto now = std::chrono::steady_clock::now();
				if (now - impl->last_ping >= impl->config.ping_interval) {
					impl->enqueue("PING");
					impl->last_ping = now;
				}
			}
		}
	});

	// Wait up to 10s for the upgrade — same shape as
	// us/ws/subscriber.cpp.
	const auto start = std::chrono::steady_clock::now();
	while (!impl_->connected.load(std::memory_order_acquire) &&
		   !impl_->should_stop.load(std::memory_order_acquire)) {
		if (std::chrono::steady_clock::now() - start > std::chrono::seconds(10)) {
			disconnect();
			return std::unexpected(Error::network("WS upgrade timed out after 10s"));
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	return {};
}

void WebSocketClient::disconnect() {
	if (!impl_) {
		return;
	}
	impl_->should_stop.store(true, std::memory_order_release);
	impl_->connected.store(false, std::memory_order_release);
	if (impl_->service_thread.joinable()) {
		impl_->service_thread.join();
	}
	if (impl_->context) {
		lws_context_destroy(impl_->context);
		impl_->context = nullptr;
		impl_->wsi = nullptr;
	}
	std::lock_guard<std::mutex> g(impl_->sub_mutex);
	impl_->active_subs.clear();
}

bool WebSocketClient::is_connected() const noexcept {
	if (!impl_) {
		return false;
	}
	return impl_->connected.load(std::memory_order_acquire);
}

namespace {

std::string asset_ids_array_json(const std::vector<uint256_t>& asset_ids) {
	std::ostringstream o;
	o << '[';
	for (std::size_t i = 0; i < asset_ids.size(); ++i) {
		if (i) {
			o << ',';
		}
		o << '"' << asset_ids[i].to_string() << '"';
	}
	o << ']';
	return o.str();
}

std::string bytes32_array_json(const std::vector<Bytes32>& v) {
	std::ostringstream o;
	o << '[';
	for (std::size_t i = 0; i < v.size(); ++i) {
		if (i) {
			o << ',';
		}
		o << '"' << v[i].to_hex() << '"';
	}
	o << ']';
	return o.str();
}

} // namespace

Result<SubscriptionId> WebSocketClient::subscribe_market(const std::vector<uint256_t>& asset_ids) {
	return subscribe_market_with_options(asset_ids, impl_->config.enable_custom_features);
}

Result<SubscriptionId>
WebSocketClient::subscribe_market_with_options(const std::vector<uint256_t>& asset_ids,
											   bool custom_features) {
	if (!is_connected()) {
		return std::unexpected(Error::network("not connected"));
	}
	if (asset_ids.empty()) {
		return std::unexpected(
			Error::validation("subscribe_market requires at least one asset_id"));
	}
	std::ostringstream o;
	o << R"({"type":"market","assets_ids":)" << asset_ids_array_json(asset_ids);
	if (custom_features) {
		o << R"(,"initial_dump":true)";
	}
	o << '}';
	impl_->enqueue(o.str());

	SubscriptionId sub{impl_->next_sub_id.fetch_add(1, std::memory_order_relaxed)};
	std::lock_guard<std::mutex> g(impl_->sub_mutex);
	impl_->active_subs.push_back(sub);
	return sub;
}

VoidResult WebSocketClient::unsubscribe_market(const std::vector<uint256_t>& asset_ids) {
	if (!is_connected()) {
		return std::unexpected(Error::network("not connected"));
	}
	std::ostringstream o;
	o << R"({"type":"unsubscribe","markets":)" << asset_ids_array_json(asset_ids) << '}';
	impl_->enqueue(o.str());
	return {};
}

Result<SubscriptionId> WebSocketClient::subscribe_user(const std::vector<Bytes32>& markets) {
	if (!is_connected()) {
		return std::unexpected(Error::network("not connected"));
	}
	if (!impl_->credentials.has_value()) {
		return std::unexpected(Error::validation("subscribe_user requires Credentials"));
	}
	std::ostringstream o;
	o << R"({"type":"user","markets":)" << bytes32_array_json(markets) << '}';
	impl_->enqueue(o.str());
	SubscriptionId sub{impl_->next_sub_id.fetch_add(1, std::memory_order_relaxed)};
	std::lock_guard<std::mutex> g(impl_->sub_mutex);
	impl_->active_subs.push_back(sub);
	return sub;
}

VoidResult WebSocketClient::unsubscribe_user(const std::vector<Bytes32>& markets) {
	if (!is_connected()) {
		return std::unexpected(Error::network("not connected"));
	}
	std::ostringstream o;
	o << R"({"type":"unsubscribe_user","markets":)" << bytes32_array_json(markets) << '}';
	impl_->enqueue(o.str());
	return {};
}

void WebSocketClient::on_message(WsMessageCallback callback) {
	impl_->message_cb = std::move(callback);
}

void WebSocketClient::on_error(WsErrorCallback callback) {
	impl_->error_cb = std::move(callback);
}

void WebSocketClient::on_state_change(WsStateCallback callback) {
	impl_->state_cb = std::move(callback);
}

const WsConfig& WebSocketClient::config() const noexcept {
	// The defaulted move ctor leaves the moved-from object's impl_ as
	// nullptr; returning a reference into the null would crash. Surface
	// a static empty config so accessors stay safe on moved-from
	// instances (matches the null-guard pattern in disconnect() /
	// is_connected()).
	if (!impl_) {
		static const WsConfig kEmpty{};
		return kEmpty;
	}
	return impl_->config;
}

std::size_t WebSocketClient::subscription_count() const noexcept {
	if (!impl_) {
		return 0;
	}
	std::lock_guard<std::mutex> g(impl_->sub_mutex);
	return impl_->active_subs.size();
}

bool WebSocketClient::is_authenticated() const noexcept {
	if (!impl_) {
		return false;
	}
	return impl_->credentials.has_value();
}

} // namespace polymarket::clob
