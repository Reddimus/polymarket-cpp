// Runs the real libcurl transport against a loopback HTTP server.

#include "../../src/detail/json.hpp"
#include "polymarket/clob/client.hpp"
#include "polymarket/http/client.hpp"
#include "polymarket/us/client.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

#ifdef _WIN32
using Socket = SOCKET;
using AddressLength = int;
constexpr Socket kInvalidSocket = INVALID_SOCKET;
constexpr int kShutdownBoth = SD_BOTH;
void close_socket(Socket socket) {
	(void)::closesocket(socket);
}
void initialize_sockets() {
	struct Winsock {
		Winsock() {
			WSADATA data{};
			if (::WSAStartup(MAKEWORD(2, 2), &data) != 0) {
				throw std::runtime_error("WSAStartup() failed");
			}
		}
		~Winsock() { (void)::WSACleanup(); }
	};
	static const Winsock initialized;
}
#else
using Socket = int;
using AddressLength = socklen_t;
constexpr Socket kInvalidSocket = -1;
constexpr int kShutdownBoth = SHUT_RDWR;
void close_socket(Socket socket) {
	(void)::close(socket);
}
void initialize_sockets() {}
#endif
using SocketIoResult = decltype(::recv(Socket{}, nullptr, 0, 0));

struct RecordedRequest {
	std::string method;
	std::string target;
	std::map<std::string, std::string> headers; // lower-case names
	std::string body;
};

/// Accepts connections on 127.0.0.1, records each request, and replies with a
/// fixed response, or never replies when `response` is empty.
class LoopbackServer {
public:
	explicit LoopbackServer(std::string response) : response_(std::move(response)) {
		initialize_sockets();
		listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		address.sin_port = 0;
		if (listener_ == kInvalidSocket ||
			::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
			::listen(listener_, 8) != 0) {
			return;
		}
		AddressLength length = sizeof(address);
		::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length);
		port_ = ntohs(address.sin_port);
		thread_ = std::thread([this] { serve(); });
	}

	~LoopbackServer() {
		stopping_ = true;
		if (listener_ != kInvalidSocket) {
			::shutdown(listener_, kShutdownBoth);
			close_socket(listener_);
		}
		if (thread_.joinable()) {
			thread_.join();
		}
	}

	LoopbackServer(const LoopbackServer&) = delete;
	LoopbackServer& operator=(const LoopbackServer&) = delete;

	[[nodiscard]] std::string origin() const { return "http://127.0.0.1:" + std::to_string(port_); }

	[[nodiscard]] std::string base_url() const {
		return "http://127.0.0.1:" + std::to_string(port_) + "/trade-api/v2";
	}

	[[nodiscard]] RecordedRequest last() const {
		const std::scoped_lock lock(mutex_);
		return requests_.empty() ? RecordedRequest{} : requests_.back();
	}

	[[nodiscard]] std::size_t count() const {
		const std::scoped_lock lock(mutex_);
		return requests_.size();
	}

private:
	void serve() {
		while (!stopping_) {
			const Socket connection = ::accept(listener_, nullptr, nullptr);
			if (connection == kInvalidSocket) {
				return;
			}
			handle(connection);
			close_socket(connection);
		}
	}

	void handle(Socket connection) {
		std::string data;
		std::array<char, 4096> buffer{};
		std::size_t header_end = std::string::npos;
		while ((header_end = data.find("\r\n\r\n")) == std::string::npos) {
			const SocketIoResult received =
				::recv(connection, buffer.data(), static_cast<int>(buffer.size()), 0);
			if (received <= 0) {
				return;
			}
			data.append(buffer.data(), static_cast<std::size_t>(received));
		}

		RecordedRequest request;
		const std::string head = data.substr(0, header_end);
		const std::size_t line_end = head.find("\r\n");
		const std::string request_line = head.substr(0, line_end);
		request.method = request_line.substr(0, request_line.find(' '));
		const std::size_t target_start = request.method.size() + 1;
		request.target = request_line.substr(target_start, request_line.rfind(' ') - target_start);
		std::size_t position = line_end == std::string::npos ? head.size() : line_end + 2;
		while (position < head.size()) {
			const std::size_t next = std::min(head.find("\r\n", position), head.size());
			const std::string line = head.substr(position, next - position);
			const std::size_t colon = line.find(':');
			if (colon != std::string::npos) {
				std::string name = line.substr(0, colon);
				std::transform(name.begin(), name.end(), name.begin(),
							   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				request.headers[name] = line.substr(line.find_first_not_of(' ', colon + 1));
			}
			position = next + 2;
		}

		const std::size_t length = request.headers.contains("content-length")
									   ? std::stoul(request.headers["content-length"])
									   : 0;
		request.body = data.substr(header_end + 4);
		while (request.body.size() < length) {
			const SocketIoResult received =
				::recv(connection, buffer.data(), static_cast<int>(buffer.size()), 0);
			if (received <= 0) {
				break;
			}
			request.body.append(buffer.data(), static_cast<std::size_t>(received));
		}
		{
			const std::scoped_lock lock(mutex_);
			requests_.push_back(request);
		}

		if (response_.empty()) {
			while (!stopping_) {
				std::this_thread::sleep_for(10ms);
			}
			return;
		}
#ifdef SO_NOSIGPIPE
		const int no_sigpipe = 1;
		(void)::setsockopt(connection, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
#endif
#ifdef MSG_NOSIGNAL
		constexpr int kSendFlags = MSG_NOSIGNAL;
#else
		constexpr int kSendFlags = 0;
#endif
		std::size_t sent = 0;
		while (sent < response_.size()) {
			const int chunk =
				static_cast<int>(std::min(response_.size() - sent, std::size_t{65536}));
			const SocketIoResult written =
				::send(connection, response_.data() + sent, chunk, kSendFlags);
			if (written <= 0) {
				return;
			}
			sent += static_cast<std::size_t>(written);
		}
	}

	std::string response_;
	Socket listener_{kInvalidSocket};
	std::uint16_t port_{0};
	std::atomic<bool> stopping_{false};
	std::thread thread_;
	mutable std::mutex mutex_;
	std::vector<RecordedRequest> requests_;
};

std::string http_response(int status, std::string_view body, std::string_view extra = {}) {
	return "HTTP/1.1 " + std::to_string(status) +
		   " Status\r\nContent-Length: " + std::to_string(body.size()) +
		   "\r\nConnection: close\r\n" + std::string(extra) + "\r\n" + std::string(body);
}

TEST_CASE("HTTP transport preserves body and empty headers", "[http]") {
	LoopbackServer server(http_response(200, "hello", "X-Empty: \r\n"));
	polymarket::http::Client client;
	const polymarket::Result<polymarket::http::Response> result =
		client.get(server.base_url() + "/weather");
	REQUIRE(result);
	REQUIRE(result->body == "hello");
	REQUIRE(result->headers.at("x-empty").empty());
}
TEST_CASE("HTTP transport bounds decoded responses", "[http]") {
	LoopbackServer server(http_response(200, "12345"));
	polymarket::http::Client client({.max_response_bytes = 4});
	REQUIRE_FALSE(client.get(server.base_url()));
	polymarket::http::Client unlimited({.max_response_bytes = 0});
	REQUIRE(unlimited.get(server.base_url()));
}
TEST_CASE("HTTP transport rejects invalid input and moved clients", "[http]") {
	polymarket::http::Client first;
	polymarket::http::Client second(std::move(first));
	REQUIRE_FALSE(first.get("http://127.0.0.1"));
	REQUIRE_FALSE(second.get("file:///etc/hosts"));
	second.set_default_header("X-Key", "a\r\nb");
	REQUIRE_FALSE(second.get("http://127.0.0.1"));
}
TEST_CASE("HTTP global compatibility calls cannot invalidate active clients", "[http]") {
	LoopbackServer server(http_response(200, "{}"));
	polymarket::http::Client client;
	polymarket::http::Client::global_cleanup();
	REQUIRE(client.get(server.base_url()));
}
TEST_CASE("HTTP custom headers suppress credential forwarding redirects", "[http]") {
	LoopbackServer server(http_response(302, "", "Location: /other\r\n"));
	polymarket::http::Client client;
	client.set_default_header("Authorization", "fixture");
	const polymarket::Result<polymarket::http::Response> result = client.get(server.base_url());
	REQUIRE(result);
	REQUIRE(result->status_code == 302);
	REQUIRE(server.count() == 1);
}
TEST_CASE("HTTP uses configured timeout", "[http]") {
	LoopbackServer server("");
	polymarket::http::Client client({.default_timeout = 50ms});
	const polymarket::Result<polymarket::http::Response> result = client.get(server.base_url());
	REQUIRE_FALSE(result);
	REQUIRE(result.error().code() == polymarket::ErrorCode::TimeoutError);
}
TEST_CASE("URL components preserve byte values and query precision", "[http]") {
	const std::string bytes("a &/%\0z", 8);
	REQUIRE(polymarket::http::url_decode(polymarket::http::url_encode(bytes)) == bytes);
	REQUIRE(polymarket::http::url_decode("%xx%") == "%xx%");
	polymarket::http::QueryBuilder query;
	query.add("n", 1.2345678901234567);
	REQUIRE(query.build() == "?n=1.2345678901234567");
	REQUIRE_THROWS_AS(query.add("n", std::numeric_limits<double>::infinity()),
					  std::invalid_argument);
}

polymarket::us::Credentials dummy_credentials() {
	return {"fixture", std::string(86, 'A') + "=="};
}
TEST_CASE("US order body matches current amount and time-in-force schema", "[http][us][contract]") {
	LoopbackServer server(http_response(200, "{}"));
	polymarket::us::Client client(server.origin(), server.origin());
	REQUIRE(client.set_credentials(dummy_credentials()));
	polymarket::us::OrderRequest request;
	request.market_id = "quoted\"\\\nmarket";
	request.side = "buy_short";
	request.type = "limit";
	request.price = polymarket::Decimal("0.5");
	request.size = polymarket::Decimal("2.5");
	request.time_in_force = "ioc";
	request.post_only = true;
	request.manual_order_indicator = false;
	REQUIRE(client.place_order(request));
	const RecordedRequest sent = server.last();
	REQUIRE(sent.method == "POST");
	REQUIRE(sent.target == "/v1/orders");
	glz::generic_u64 body;
	REQUIRE_FALSE(glz::read_json(body, sent.body));
	const auto& fields = body.get_object();
	REQUIRE(fields.at("marketSlug").get<std::string>() == request.market_id);
	REQUIRE(fields.at("quantity").is_number());
	REQUIRE(fields.at("quantity").as_number() == 2.5);
	REQUIRE(fields.at("price").is_object());
	REQUIRE(fields.at("price").get_object().at("value").get<std::string>() == "0.5");
	REQUIRE(fields.at("price").get_object().at("currency").get<std::string>() == "USD");
	REQUIRE(fields.at("tif").get<std::string>() == "TIME_IN_FORCE_IMMEDIATE_OR_CANCEL");
	REQUIRE(fields.at("manualOrderIndicator").get<std::string>() ==
			"MANUAL_ORDER_INDICATOR_AUTOMATIC");
	REQUIRE_FALSE(fields.contains("timeInForce"));
}
TEST_CASE("US expiration is serialized and unsupported IDs fail before HTTP",
		  "[http][us][contract]") {
	LoopbackServer server(http_response(200, "{}"));
	polymarket::us::Client client(server.origin(), server.origin());
	REQUIRE(client.set_credentials(dummy_credentials()));
	polymarket::us::OrderRequest request;
	request.market_id = "fixture";
	request.side = "buy";
	request.type = "limit";
	request.price = polymarket::Decimal("0.5");
	request.size = polymarket::Decimal("1");
	request.expiration = 1700000000;
	REQUIRE(client.place_order(request));
	const RecordedRequest sent = server.last();
	glz::generic_u64 body;
	REQUIRE_FALSE(glz::read_json(body, sent.body));
	REQUIRE(body.get_object().at("goodTillTime").get<std::string>() == "2023-11-14T22:13:20Z");
	REQUIRE(body.get_object().at("tif").get<std::string>() == "TIME_IN_FORCE_GOOD_TILL_DATE");
	request.client_order_id = "unsupported";
	REQUIRE_FALSE(client.place_order(request));
	REQUIRE(server.count() == 1);
}
TEST_CASE("US URL values cannot become extra query parameters or paths", "[http][us]") {
	LoopbackServer server(http_response(200, "{}"));
	polymarket::us::Client client(server.origin(), server.origin());
	REQUIRE(client.search("x&limit=100"));
	REQUIRE(server.last().target == "/v1/search?q=x%26limit%3D100");
	REQUIRE(client.get_event("a/b?x=1"));
	REQUIRE(server.last().target == "/v1/events/a%2Fb%3Fx%3D1");
	REQUIRE_FALSE(client.get_event(".."));
	polymarket::us::Client moved(std::move(client));
	REQUIRE_FALSE(client.get_health());
	REQUIRE(moved.get_health());
}
TEST_CASE("CLOB typed metadata accepts numeric fee rates and preserves token IDs", "[http][clob]") {
	LoopbackServer server(
		http_response(200, R"({"fee_rate_bps":12,"minimum_tick_size":0.001,"neg_risk":true})"));
	auto client = polymarket::clob::Client::create({.base_url = server.origin()});
	REQUIRE(client);
	const auto token = polymarket::uint256_t::from_string("18446744073709551616");
	const auto fee = client->fee_rate_bps(token);
	REQUIRE(fee);
	REQUIRE(fee->base_fee == 12);
	REQUIRE(server.last().target == "/fee-rate?token_id=18446744073709551616");
	const auto tick = client->tick_size(token);
	REQUIRE(tick);
	REQUIRE(tick->minimum_tick_size == polymarket::clob::TickSize::Thousandth);
	const auto risk = client->neg_risk(token);
	REQUIRE(risk);
	REQUIRE(risk->neg_risk);
}
TEST_CASE("CLOB price fields fail explicitly on malformed data", "[http][clob]") {
	for (const std::string body :
		 {std::string("{}"),
		  std::string(R"({"price":"invalid","mid":"invalid","spread":"invalid"})"),
		  std::string(R"({"price":"0.5","mid":"0.5","spread":"0.1"} trailing)")}) {
		LoopbackServer server(http_response(200, body));
		auto client = polymarket::clob::Client::create({.base_url = server.origin()});
		REQUIRE(client);
		REQUIRE_FALSE(client->price({}));
		REQUIRE_FALSE(client->midpoint({}));
		REQUIRE_FALSE(client->spread({}));
		REQUIRE_FALSE(client->last_trade_price({}));
	}
	LoopbackServer server(
		http_response(200, R"({"price":"0.5","mid":"0.5","spread":"0.1","side":"SELL"})"));
	auto client = polymarket::clob::Client::create({.base_url = server.origin()});
	REQUIRE(client);
	const auto price = client->price({});
	REQUIRE(price);
	REQUIRE(price->price == polymarket::Decimal("0.5"));
	REQUIRE(client->midpoint({}));
	REQUIRE(client->spread({}));
	const auto trade = client->last_trade_price({});
	REQUIRE(trade);
	REQUIRE(trade->side == "SELL");
	REQUIRE(trade->timestamp == 0);
}
TEST_CASE("CLOB key derivation uses its read-only endpoint", "[http][clob][auth]") {
	LoopbackServer server(
		http_response(200, R"({"apiKey":"fixture","secret":"AA==","passphrase":"fixture"})"));
	auto client = polymarket::clob::Client::create({.base_url = server.origin()});
	REQUIRE(client);
	const auto key = polymarket::crypto::PrivateKey::from_hex(std::string(63, '0') + "1");
	REQUIRE(key);
	REQUIRE(client->authenticate(*key));
	REQUIRE(server.last().target == "/auth/derive-api-key");
	REQUIRE(server.last().method == "GET");
	REQUIRE(server.last().headers.at("poly_signature").size() == 132);
}
TEST_CASE("CLOB pacing accounts for every retry", "[http][clob][retry]") {
	LoopbackServer server(http_response(503, "busy"));
	polymarket::clob::ClientConfig config;
	config.base_url = server.origin();
	config.retry_policy.initial_delay = std::chrono::milliseconds{0};
	config.rate_limit.max_tokens = 1;
	config.rate_limit.initial_tokens = 1;
	config.rate_limit.refill_interval = std::chrono::hours{1};
	config.rate_limit.max_wait = std::chrono::milliseconds{0};
	auto client = polymarket::clob::Client::create(config);
	REQUIRE(client);
	const auto result = client->ok();
	REQUIRE_FALSE(result);
	REQUIRE(result.error().code() == polymarket::ErrorCode::RateLimitError);
	REQUIRE(server.count() == 1);
}
TEST_CASE("Unimplemented CLOB operations cannot fabricate cancellations or empty accounts",
		  "[http][clob][scope]") {
	LoopbackServer server(http_response(200, "{}"));
	auto client = polymarket::clob::Client::create({.base_url = server.origin()});
	REQUIRE(client);
	REQUIRE(
		client->authenticate({"fixture", "AA==", "fixture"}, polymarket::Address::from_hex("1")));
	REQUIRE_FALSE(client->api_keys());
	REQUIRE_FALSE(client->delete_api_key());
	REQUIRE_FALSE(client->order("fixture"));
	REQUIRE_FALSE(client->orders({}));
	REQUIRE_FALSE(client->cancel_order("fixture"));
	REQUIRE_FALSE(client->cancel_orders({"fixture"}));
	REQUIRE_FALSE(client->cancel_all_orders());
	REQUIRE_FALSE(client->cancel_market_orders());
	REQUIRE_FALSE(client->trades({}));
	REQUIRE_FALSE(client->balance_allowance({}));
	REQUIRE_FALSE(client->update_balance_allowance({}));
	REQUIRE_FALSE(client->notifications());
	REQUIRE_FALSE(client->delete_notifications({}));
	REQUIRE_FALSE(client->is_order_scoring("fixture"));
	REQUIRE_FALSE(client->are_orders_scoring({}));
	REQUIRE_FALSE(client->closed_only_mode());
	REQUIRE_FALSE(client->post_heartbeat());
	REQUIRE_FALSE(client->builder_api_keys());
	REQUIRE_FALSE(client->revoke_builder_api_key());
	REQUIRE_FALSE(client->book({}));
	REQUIRE_FALSE(client->price_history({}));
	REQUIRE_FALSE(client->markets());
	REQUIRE(server.count() == 0);
}
TEST_CASE("Legacy order writes use one attempt and require confirmation", "[http][clob]") {
	LoopbackServer server(http_response(503, "busy"));
	auto client = polymarket::clob::Client::create({.base_url = server.origin()});
	REQUIRE(client);
	REQUIRE(
		client->authenticate({"fixture", "AA==", "fixture"}, polymarket::Address::from_hex("1")));
	polymarket::clob::SignedOrder order{};
	order.signature.v = 27;
	REQUIRE_FALSE(client->post_order(order));
	REQUIRE(server.count() == 1);
	REQUIRE(server.last().method == "POST");
	glz::generic_u64 body;
	REQUIRE_FALSE(glz::read_json(body, server.last().body));
	const std::string signature = body.get_object().at("signature").get<std::string>();
	REQUIRE(signature.starts_with("0x"));
	REQUIRE_FALSE(signature.starts_with("0x0x"));
	REQUIRE(signature.size() == 132);
}

TEST_CASE("CLOB server clocks and geoblock results cannot fabricate defaults", "[http][clob]") {
	LoopbackServer clock(http_response(200, "1700000000"));
	auto client = polymarket::clob::Client::create({.base_url = clock.origin()});
	REQUIRE(client);
	const auto time = client->server_time();
	REQUIRE(time);
	REQUIRE(time->timestamp == 1700000000);
	REQUIRE(clock.last().target == "/time");
	LoopbackServer denied(http_response(200, R"({"blocked":true,"country":"US"})"));
	auto restricted = polymarket::clob::Client::create({.geoblock_url = denied.origin()});
	REQUIRE(restricted);
	const auto result = restricted->check_geoblock();
	REQUIRE(result);
	REQUIRE(result->blocked);
	LoopbackServer invalid(http_response(200, "{}"));
	auto unknown = polymarket::clob::Client::create({.geoblock_url = invalid.origin()});
	REQUIRE(unknown);
	REQUIRE_FALSE(unknown->check_geoblock());
}
TEST_CASE("CLOB configured server time is used for request authentication", "[http][clob]") {
	LoopbackServer server(http_response(200, "1700000000"));
	auto client =
		polymarket::clob::Client::create({.base_url = server.origin(), .use_server_time = true});
	REQUIRE(client);
	REQUIRE(
		client->authenticate({"fixture", "AA==", "fixture"}, polymarket::Address::from_hex("1")));
	polymarket::clob::SignedOrder order{};
	order.signature.v = 27;
	REQUIRE_FALSE(client->post_order(order));
	REQUIRE(server.last().headers.at("poly_timestamp") == "1700000000");
	REQUIRE(server.count() == 2);
}
} // namespace
