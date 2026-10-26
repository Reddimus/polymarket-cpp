#include "../detail/callback_slot.hpp"
#include "../detail/json.hpp"
#include "../detail/ws_runtime.hpp"
/// @file websocket.cpp
/// @brief CLOB market/user WebSockets sharing the private lifecycle runtime.
#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstring>
#include <deque>
#include <glaze/glaze.hpp>
#include <glaze/json/generic.hpp>
#include <iostream>
#include <libwebsockets.h>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include "polymarket/clob/websocket.hpp"
#include "polymarket/core/error.hpp"

namespace polymarket::clob {

namespace {

using glz_node = glz::generic_u64;

constexpr std::size_t kMaxFrameSize = 1 << 20; // 1 MiB
constexpr const char* kClobProtocolName = "polymarket-clob-ws";

/// Tokenize the ``wss://host[:port]/path`` form into its three pieces.
struct ParsedUrl {
	std::string host;
	int port = 443;
	std::string path = "/ws/market";
	bool ssl = true;
};

ParsedUrl parse_url(std::string_view url) {
	ParsedUrl result;
	if (url.find_first_of(std::string_view{" \t\r\n\0#", 6}) != url.npos)
		return result;
	if (url.starts_with("wss://"))
		url.remove_prefix(6);
	else if (url.starts_with("ws://")) {
		url.remove_prefix(5);
		result.ssl = false;
		result.port = 80;
	} else
		return result;
	const std::size_t slash = url.find_first_of("/?");
	const std::string_view authority = url.substr(0, slash);
	if (authority.empty() || authority.find('@') != authority.npos)
		return result;
	if (slash != url.npos)
		result.path = (url[slash] == '?' ? "/" : "") + std::string(url.substr(slash));
	std::string_view host = authority, port;
	if (authority.starts_with('[')) {
		const std::size_t bracket = authority.find(']');
		if (bracket == authority.npos)
			return result;
		host = authority.substr(1, bracket - 1);
		if (bracket + 1 < authority.size()) {
			if (authority[bracket + 1] != ':')
				return result;
			port = authority.substr(bracket + 2);
		}
	} else {
		const std::size_t colon = authority.find(':');
		if (colon != authority.npos) {
			host = authority.substr(0, colon);
			port = authority.substr(colon + 1);
		}
	}
	if (!port.empty()) {
		const std::from_chars_result parsed =
			std::from_chars(port.data(), port.data() + port.size(), result.port);
		if (parsed.ec != std::errc{} || parsed.ptr != port.data() + port.size() ||
			result.port < 1 || result.port > 65535)
			return result;
	} else if (authority.ends_with(':'))
		return result;
	result.host = host;
	return result;
}

// Typed field access for selected market messages.

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
	if (!v)
		throw std::invalid_argument("Missing decimal field: " + std::string(key));
	if (v->is_string()) {
		return Decimal::from_string(v->get<std::string>());
	}
	if (v->is_number()) {
		return Decimal::from_double(v->as_number());
	}
	throw std::invalid_argument("Invalid decimal field: " + std::string(key));
}

uint256_t get_uint256(const glz_node& node, const char* key) {
	const glz_node* v = find_field(node, key);
	if (!v || !v->is_string())
		throw std::invalid_argument("Invalid token ID field: " + std::string(key));
	return uint256_t::from_string(v->get<std::string>());
}

Timestamp get_timestamp(const glz_node& node, const char* key) {
	const glz_node* v = find_field(node, key);
	if (!v) {
		return Timestamp{};
	}
	if (v->is_string()) {
		Timestamp value = 0;
		const std::string& text = v->get<std::string>();
		const std::from_chars_result parsed =
			std::from_chars(text.data(), text.data() + text.size(), value);
		if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
			return Timestamp{};
		return value;
	}
	if (v->is_number()) {
		Timestamp number = 0;
		if (!detail::exact_integer(*v, number))
			return Timestamp{};
		return number;
	}
	return Timestamp{};
}

// Preserve events without a typed decoder as complete JSON.
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
			throw std::invalid_argument("Unsupported WebSocket tick size: " + nts);
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
	if (ev == "order" || ev == "trade") {
		std::string payload;
		(void)glz::write_json(j, payload);
		return UserEvent{ev, std::move(payload)};
	}
	if (!ev.empty()) {
		std::string payload;
		(void)glz::write_json(j, payload);
		return MarketEvent{ev, std::move(payload)};
	}
	return std::nullopt;
}

} // namespace

// ─── Impl ──────────────────────────────────────────────────────────

static int clob_ws_callback(struct lws* wsi, enum lws_callback_reasons reason, void* user, void* in,
							std::size_t len);

struct WebSocketClient::Impl : detail::WsRuntime {
	WsConfig config;
	std::optional<Credentials> credentials;
	ParsedUrl url;

	// Connection.

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
	detail::CallbackSlot<void(const WsMessage&)> message_cb;
	detail::CallbackSlot<void(const WsError&)> error_cb;
	detail::CallbackSlot<void(bool)> state_cb;

	bool initial_subscription{false};

	void enqueue(std::string frame) {
		{
			std::lock_guard<std::mutex> g(send_mutex);
			send_queue.push_back(std::move(frame));
		}
		wake();
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
	try {
		(void)user;
		auto* impl = static_cast<WebSocketClient::Impl*>(lws_context_user(lws_get_context(wsi)));

		switch (reason) {
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
				if (len > kMaxFrameSize - impl->fragment_buf.size())
					return -1;
				impl->fragment_buf.append(static_cast<const char*>(in), len);
				if (!lws_is_final_fragment(wsi) || lws_remaining_packet_payload(wsi)) {
					return 0;
				}
				std::string payload = std::move(impl->fragment_buf);
				impl->fragment_buf.clear();

				if (payload == "PONG")
					return 0;
				glz_node parsed{};
				glz::error_ctx ec = glz::read<polymarket::detail::kReadOptions>(parsed, payload);
				if (ec) {
					impl->emit_error(0, std::string{"invalid JSON: "} +
											glz::format_error(ec, payload));
					return 0;
				}

				auto dispatch_one = [&](const glz_node& j) {
					try {
						const std::optional<WsMessage> msg = parse_message(j);
						if (msg && impl->message_cb)
							impl->message_cb(*msg);
					} catch (const std::exception& error) {
						impl->emit_error(0, error.what());
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
				if (written < 0 || static_cast<std::size_t>(written) != frame.size()) {
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
				const std::string err = in ? std::string(static_cast<const char*>(in), len)
										   : std::string{"(no detail)"};
				impl->emit_error(0, "connection error: " + err);
				impl->connected.store(false, std::memory_order_release);
				impl->should_stop.store(true, std::memory_order_release);
				if (impl->state_cb) {
					impl->state_cb(false);
				}
				return -1;
			}

			case LWS_CALLBACK_CLIENT_CLOSED: {
				impl->connected.store(false, std::memory_order_release);
				impl->should_stop.store(true, std::memory_order_release);
				if (impl->state_cb) {
					impl->state_cb(false);
				}
				return 0;
			}

			default:
				return 0;
		}
	} catch (...) {
		return -1;
	}
}

static const struct lws_protocols kProtocols[] = {
	{kClobProtocolName, clob_ws_callback, 0, kMaxFrameSize, 0, nullptr, 0},
	{nullptr, nullptr, 0, 0, 0, nullptr, 0}};

// ─── WebSocketClient public surface ────────────────────────────────

WebSocketClient::WebSocketClient(WsConfig config) : impl_(std::make_shared<Impl>()) {
	impl_->config = std::move(config);
	impl_->url = parse_url(impl_->config.url);
}

WebSocketClient::WebSocketClient(WsConfig config, Credentials credentials)
	: impl_(std::make_shared<Impl>()) {
	if (config.url == WsConfig{}.url)
		config.url = "wss://ws-subscriptions-clob.polymarket.com/ws/user";
	impl_->config = std::move(config);
	impl_->url = parse_url(impl_->config.url);
	impl_->credentials = std::move(credentials);
}

WebSocketClient::~WebSocketClient() {
	disconnect();
}

WebSocketClient::WebSocketClient(WebSocketClient&&) noexcept = default;
WebSocketClient& WebSocketClient::operator=(WebSocketClient&& other) noexcept {
	if (this != &other) {
		disconnect();
		impl_ = std::move(other.impl_);
	}
	return *this;
}

VoidResult WebSocketClient::connect() {
	const std::shared_ptr<Impl> state = impl_;
	if (!state)
		return std::unexpected(Error::validation("Client moved-from"));
	if (state && (state->url.host.empty() || state->url.port < 1 || state->url.port > 65535))
		return std::unexpected(Error::validation("Invalid WebSocket URL"));

	const bool user_channel = state->url.path == "/ws/user";
	if (!user_channel && state->url.path != "/ws/market")
		return std::unexpected(Error::validation("Expected /ws/market or /ws/user endpoint"));
	if (user_channel &&
		(!state->credentials || state->credentials->api_key.empty() ||
		 state->credentials->api_secret.empty() || state->credentials->passphrase.empty()))
		return std::unexpected(Error::validation("User channel requires API credentials"));
	state->stop();
	if (state->worker_running.load(std::memory_order_acquire))
		return std::unexpected(Error::network("Previous worker is stopping"));
	if (!detail::keep_tls_initialized())
		return std::unexpected(Error::network("TLS initialization failed"));
	detail::RetiredThreads::instance().reap();
	state->should_stop.store(false, std::memory_order_release);
	{
		const std::lock_guard lock(state->send_mutex);
		state->send_queue.clear();
	}
	state->fragment_buf.clear();
	state->initial_subscription = false;
	state->config.ping_interval =
		std::clamp(state->config.ping_interval, std::chrono::seconds{1}, std::chrono::seconds{10});
	{
		const std::lock_guard lock(state->sub_mutex);
		state->active_subs.clear();
	}

	struct lws_context_creation_info ctx_info {};
	std::memset(&ctx_info, 0, sizeof(ctx_info));
	ctx_info.port = CONTEXT_PORT_NO_LISTEN;
	ctx_info.protocols = kProtocols;
	ctx_info.user = state.get();
	ctx_info.options = state->url.ssl ? LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT : 0;

	state->context = lws_create_context(&ctx_info);
	if (!state->context) {
		return std::unexpected(Error::network("lws_create_context failed"));
	}

	struct lws_client_connect_info ci {};
	std::memset(&ci, 0, sizeof(ci));
	ci.context = state->context;
	ci.address = state->url.host.c_str();
	ci.port = state->url.port;
	ci.path = state->url.path.c_str();
	ci.host = state->url.host.c_str();
	ci.origin = state->url.host.c_str();
	ci.protocol = kProtocols[0].name;
	ci.ssl_connection = state->url.ssl ? LCCSCF_USE_SSL : 0;

	state->wsi = lws_client_connect_via_info(&ci);
	if (!state->wsi) {
		state->release_context();
		return std::unexpected(Error::network("lws_client_connect_via_info failed"));
	}

	if (state->should_stop.load(std::memory_order_acquire)) {
		state->release_context();
		return std::unexpected(Error::network("Disconnected while connecting"));
	}
	try {
		state->start(state, [](Impl& impl) {
			if (impl.connected.load(std::memory_order_acquire)) {
				const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
				if (now - impl.last_ping >= impl.config.ping_interval) {
					impl.enqueue("PING");
					impl.last_ping = now;
				}
			}
			const std::lock_guard lock(impl.send_mutex);
			if (!impl.send_queue.empty() && impl.wsi)
				lws_callback_on_writable(impl.wsi);
		});

	} catch (const std::exception& error) {
		state->release_context();
		return std::unexpected(Error::network(error.what()));
	}

	// Wait up to 10s for the upgrade — same shape as
	// us/ws/subscriber.cpp.
	const auto start = std::chrono::steady_clock::now();
	while (!state->connected.load(std::memory_order_acquire) &&
		   !state->should_stop.load(std::memory_order_acquire)) {
		if (std::chrono::steady_clock::now() - start > std::chrono::seconds(10)) {
			state->stop();
			return std::unexpected(Error::network("WS upgrade timed out after 10s"));
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	if (!state->connected.load(std::memory_order_acquire))
		return std::unexpected(Error::network("WebSocket closed during handshake"));
	return {};
}

void WebSocketClient::disconnect() {
	const std::shared_ptr<Impl> state = impl_;
	if (state) {
		state->stop();
		const std::lock_guard lock(state->sub_mutex);
		state->active_subs.clear();
	}
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
	if (!impl_)
		return std::unexpected(Error::validation("Client moved-from"));
	return subscribe_market_with_options(asset_ids, impl_->config.enable_custom_features);
}

Result<SubscriptionId>
WebSocketClient::subscribe_market_with_options(const std::vector<uint256_t>& asset_ids,
											   bool custom_features) {
	if (!impl_ || impl_->url.path != "/ws/market")
		return std::unexpected(Error::validation("Requires /ws/market endpoint"));
	if (!is_connected()) {
		return std::unexpected(Error::network("not connected"));
	}
	if (asset_ids.empty()) {
		return std::unexpected(
			Error::validation("subscribe_market requires at least one asset_id"));
	}
	std::ostringstream o;
	o << (impl_->initial_subscription ? R"({"operation":"subscribe","assets_ids":)"
									  : R"({"type":"market","assets_ids":)")
	  << asset_ids_array_json(asset_ids);
	if (custom_features) {
		o << R"(,"custom_feature_enabled":true)";
	}
	o << '}';
	impl_->enqueue(o.str());
	impl_->initial_subscription = true;

	SubscriptionId sub{impl_->next_sub_id.fetch_add(1, std::memory_order_relaxed)};
	std::lock_guard<std::mutex> g(impl_->sub_mutex);
	impl_->active_subs.push_back(sub);
	return sub;
}

VoidResult WebSocketClient::unsubscribe_market(const std::vector<uint256_t>& asset_ids) {
	if (!impl_ || impl_->url.path != "/ws/market")
		return std::unexpected(Error::validation("Requires /ws/market endpoint"));
	if (!is_connected()) {
		return std::unexpected(Error::network("not connected"));
	}
	std::ostringstream o;
	o << R"({"operation":"unsubscribe","assets_ids":)" << asset_ids_array_json(asset_ids) << '}';
	impl_->enqueue(o.str());
	return {};
}

Result<SubscriptionId> WebSocketClient::subscribe_user(const std::vector<Bytes32>& markets) {
	if (!impl_ || impl_->url.path != "/ws/user")
		return std::unexpected(Error::validation("Requires /ws/user endpoint"));
	if (!is_connected()) {
		return std::unexpected(Error::network("not connected"));
	}
	if (!impl_->credentials.has_value()) {
		return std::unexpected(Error::validation("subscribe_user requires Credentials"));
	}
	std::ostringstream o;
	if (impl_->initial_subscription)
		o << R"({"operation":"subscribe","markets":)" << bytes32_array_json(markets) << '}';
	else {
		glz_node auth;
		auth["apiKey"] = impl_->credentials->api_key;
		auth["secret"] = impl_->credentials->api_secret;
		auth["passphrase"] = impl_->credentials->passphrase;
		std::string encoded;
		(void)glz::write_json(auth, encoded);
		o << R"({"type":"user","auth":)" << encoded << R"(,"markets":)"
		  << bytes32_array_json(markets) << '}';
	}
	impl_->enqueue(o.str());
	impl_->initial_subscription = true;
	SubscriptionId sub{impl_->next_sub_id.fetch_add(1, std::memory_order_relaxed)};
	std::lock_guard<std::mutex> g(impl_->sub_mutex);
	impl_->active_subs.push_back(sub);
	return sub;
}

VoidResult WebSocketClient::unsubscribe_user(const std::vector<Bytes32>& markets) {
	if (!impl_ || impl_->url.path != "/ws/user")
		return std::unexpected(Error::validation("Requires /ws/user endpoint"));
	if (!is_connected()) {
		return std::unexpected(Error::network("not connected"));
	}
	std::ostringstream o;
	o << R"({"operation":"unsubscribe","markets":)" << bytes32_array_json(markets) << '}';
	impl_->enqueue(o.str());
	return {};
}

void WebSocketClient::on_message(WsMessageCallback callback) {
	if (impl_)
		impl_->message_cb = std::move(callback);
}

void WebSocketClient::on_error(WsErrorCallback callback) {
	if (impl_)
		impl_->error_cb = std::move(callback);
}

void WebSocketClient::on_state_change(WsStateCallback callback) {
	if (impl_)
		impl_->state_cb = std::move(callback);
}

const WsConfig& WebSocketClient::config() const noexcept {
	// Moved-from accessors expose a stable default.
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
