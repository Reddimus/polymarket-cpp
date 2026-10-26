#include <charconv>

#include "../detail/json.hpp"
/// @file client.cpp
/// @brief Legacy CLOB REST client and public metadata helpers.

#include <algorithm>
#include <chrono>
#include <limits>
#include <map>

#include "polymarket/clob/client.hpp"
#include "polymarket/crypto/eip712.hpp"
#include "polymarket/crypto/hmac.hpp"
#include "polymarket/http/client.hpp"

namespace polymarket::clob {

// =========================================================================
// JSON Helpers
// =========================================================================

namespace json {

std::string escape(std::string_view text) {
	std::string quoted;
	(void)glz::write_json(text, quoted);
	return quoted.substr(1, quoted.size() - 2);
}

class ObjectBuilder {
public:
	ObjectBuilder& add(std::string_view key, std::string_view value) {
		if (!first_)
			out_ += ',';
		first_ = false;
		out_ += "\"";
		out_ += key;
		out_ += "\":\"";
		out_ += escape(value);
		out_ += "\"";
		return *this;
	}

	ObjectBuilder& add(std::string_view key, int64_t value) {
		if (!first_)
			out_ += ',';
		first_ = false;
		out_ += "\"";
		out_ += key;
		out_ += "\":";
		out_ += std::to_string(value);
		return *this;
	}

	ObjectBuilder& add_raw(std::string_view key, std::string_view raw) {
		if (!first_)
			out_ += ',';
		first_ = false;
		out_ += "\"";
		out_ += key;
		out_ += "\":";
		out_ += raw;
		return *this;
	}

	std::string build() const { return "{" + out_ + "}"; }

private:
	std::string out_;
	bool first_{true};
};

std::optional<glz::generic_u64> field(std::string_view text, std::string_view key) {
	glz::generic_u64 root;
	if (glz::read<detail::kReadOptions>(root, text) || !root.is_object())
		return std::nullopt;
	const auto found = root.get_object().find(key);
	if (found == root.get_object().end())
		return std::nullopt;
	return found->second;
}
std::optional<std::string> extract_string(std::string_view text, std::string_view key) {
	const std::optional<glz::generic_u64> value = field(text, key);
	if (!value || !value->is_string())
		return std::nullopt;
	return value->get<std::string>();
}
std::optional<int64_t> extract_int(std::string_view text, std::string_view key) {
	const std::optional<glz::generic_u64> value = field(text, key);
	if (!value)
		return std::nullopt;
	std::int64_t number = 0;
	if (value->is_string()) {
		const std::string& input = value->get<std::string>();
		const std::from_chars_result parsed =
			std::from_chars(input.data(), input.data() + input.size(), number);
		if (parsed.ec != std::errc{} || parsed.ptr != input.data() + input.size())
			return std::nullopt;
	} else if (!detail::exact_integer(*value, number))
		return std::nullopt;
	return number;
}
std::optional<bool> extract_bool(std::string_view text, std::string_view key) {
	const std::optional<glz::generic_u64> value = field(text, key);
	if (!value || !value->is_boolean())
		return std::nullopt;
	return value->get<bool>();
}

Result<Decimal> extract_decimal(std::string_view text, std::string_view key) {
	const std::optional<std::string> value = extract_string(text, key);
	if (!value)
		return std::unexpected(Error::parse("Missing decimal field: " + std::string(key)));
	try {
		return Decimal::from_string(*value);
	} catch (const std::exception&) {
		return std::unexpected(Error::parse("Invalid decimal field: " + std::string(key)));
	}
}

} // namespace json

// =========================================================================
// Implementation
// =========================================================================

namespace {
class Transport {
public:
	explicit Transport(const ClientConfig& config)
		: client_({.default_timeout =
					   std::chrono::milliseconds{
						   std::clamp<std::int64_t>(config.timeout.count(), 1, 86400) * 1000}}),
		  retry_(config.retry_policy), limiter_(config.rate_limit) {}
	Result<http::Response> execute(const http::Request& request) {
		const unsigned attempts =
			request.method == http::Method::GET ? std::max<unsigned>(retry_.max_attempts, 1) : 1;
		for (unsigned attempt = 1;; ++attempt) {
			if (!limiter_.acquire())
				return std::unexpected(Error::rate_limit("Rate limiter wait exceeded"));
			Result<http::Response> result = client_.execute(request);
			const bool retryable =
				result ? should_retry(HttpResponse{.status_code = result->status_code}, retry_)
					   : should_retry(result.error(), retry_);
			if (!retryable || attempt >= attempts)
				return result;
			std::this_thread::sleep_for(
				calculate_retry_delay(static_cast<std::uint8_t>(attempt), retry_));
		}
	}
	Result<http::Response> get(std::string_view url) {
		http::Request request;
		request.url = url;
		return execute(request);
	}
	Result<http::Response> post(std::string_view url, std::string_view body) {
		http::Request request;
		request.method = http::Method::POST;
		request.url = url;
		request.body = body;
		request.json_content();
		return execute(request);
	}
	Result<http::Response> del(std::string_view url) {
		http::Request request;
		request.method = http::Method::DEL;
		request.url = url;
		return execute(request);
	}

private:
	http::Client client_;
	RetryPolicy retry_;
	RateLimiter limiter_;
};
} // namespace

struct Client::Impl {
	Transport http;
	ClientConfig config;

	bool authenticated{false};
	bool is_builder_mode{false};
	SignatureType signature_type{SignatureType::EOA};
	std::optional<Address> signer_addr;
	std::optional<Address> funder_addr;
	std::optional<ApiKeyCredentials> api_creds;
	std::optional<BuilderKeyCredentials> builder_creds;

	std::map<std::string, TickSize> tick_size_cache;
	std::map<std::string, bool> neg_risk_cache;
	std::map<std::string, std::uint32_t> fee_rate_cache;

	explicit Impl(ClientConfig cfg) : http(cfg), config(std::move(cfg)) {}
	Result<std::string> get_timestamp(bool force_server = false) {
		if (!force_server && !config.use_server_time) {
			const auto now = std::chrono::system_clock::now().time_since_epoch();
			return std::to_string(std::chrono::duration_cast<std::chrono::seconds>(now).count());
		}
		const Result<http::Response> response = http.get(config.base_url + "/time");
		if (!response)
			return std::unexpected(response.error());
		if (!response->is_success())
			return std::unexpected(Error::from_http_status(response->status_code, response->body));
		glz::generic_u64 value;
		if (glz::read<detail::kReadOptions>(value, response->body))
			return std::unexpected(Error::parse("Invalid server time"));
		std::int64_t seconds = 0;
		if (value.is_string()) {
			const std::string& text = value.get<std::string>();
			const auto parsed = std::from_chars(text.data(), text.data() + text.size(), seconds);
			if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
				return std::unexpected(Error::parse("Invalid server time"));
		} else if (!detail::exact_integer(value, seconds))
			return std::unexpected(Error::parse("Invalid server time"));
		if (seconds < 0)
			return std::unexpected(Error::parse("Negative server time"));
		return std::to_string(seconds);
	}

	Result<void> add_l2_auth(http::Request& req) {
		if (!authenticated || !api_creds) {
			return std::unexpected(Error::auth("API credentials not available"));
		}

		const Result<std::string> clock = get_timestamp();
		if (!clock)
			return std::unexpected(clock.error());
		const std::string timestamp = *clock;
		std::string method = std::string(http::method_to_string(req.method));

		std::string path;
		auto pos = req.url.find("://");
		if (pos != std::string::npos) {
			pos = req.url.find('/', pos + 3);
			if (pos != std::string::npos)
				path = req.url.substr(pos);
		}
		if (path.empty())
			path = "/";

		auto sig =
			crypto::generate_l2_signature(api_creds->api_secret, timestamp, method, path, req.body);
		if (!sig)
			return std::unexpected(sig.error());

		req.header("POLY_API_KEY", api_creds->api_key);
		req.header("POLY_SIGNATURE", *sig);
		req.header("POLY_TIMESTAMP", timestamp);
		req.header("POLY_PASSPHRASE", api_creds->api_passphrase);

		return {};
	}
};

// =========================================================================
// Client
// =========================================================================

Result<Client> Client::create(ClientConfig config) {
	if (config.timeout <= std::chrono::seconds::zero())
		return std::unexpected(Error::validation("Timeout must be positive"));
	return Client(std::make_unique<Impl>(std::move(config)));
}

Client::Client(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Client::~Client() = default;
Client::Client(Client&&) noexcept = default;
Client& Client::operator=(Client&&) noexcept = default;

bool Client::is_authenticated() const noexcept {
	return impl_ && impl_->authenticated;
}
bool Client::is_builder() const noexcept {
	return impl_ && impl_->is_builder_mode;
}

Result<void> Client::authenticate(const crypto::PrivateKey& signer, std::optional<Address> funder,
								  SignatureType sig_type) {
	if (!impl_)
		return std::unexpected(Error::validation("Client moved-from"));
	auto addr_result = signer.address();
	if (!addr_result)
		return std::unexpected(addr_result.error());

	// Derive API key via L1 auth
	const Result<std::string> clock = impl_->get_timestamp();
	if (!clock)
		return std::unexpected(clock.error());
	const std::string timestamp = *clock;

	crypto::ClobAuth auth;
	auth.address = *addr_result;
	auth.timestamp = timestamp;
	auth.nonce = 0;

	crypto::EIP712Domain domain;
	domain.name = "ClobAuthDomain";
	domain.version = "1";
	domain.chain_id = impl_->config.chain_id;

	auto hash = auth.type_hash(domain);
	auto sig = signer.sign(hash);
	if (!sig)
		return std::unexpected(sig.error());

	http::Request req;
	req.method = http::Method::GET;
	req.url = impl_->config.base_url + "/auth/derive-api-key";
	req.header("POLY_ADDRESS", addr_result->to_checksum());
	req.header("POLY_SIGNATURE", sig->to_hex());
	req.header("POLY_TIMESTAMP", timestamp);
	req.header("POLY_NONCE", "0");

	auto resp = impl_->http.execute(req);
	if (!resp)
		return std::unexpected(resp.error());

	if (!resp->is_success()) {
		return std::unexpected(Error::auth("Failed to derive API key: " + resp->body));
	}

	ApiKeyCredentials credentials{
		.api_key = json::extract_string(resp->body, "apiKey").value_or(""),
		.api_secret = json::extract_string(resp->body, "secret").value_or(""),
		.api_passphrase = json::extract_string(resp->body, "passphrase").value_or("")};
	if (credentials.api_key.empty() || credentials.api_secret.empty() ||
		credentials.api_passphrase.empty())
		return std::unexpected(Error::parse("Incomplete API credentials"));
	impl_->signer_addr = *addr_result;
	impl_->funder_addr = funder.value_or(*addr_result);
	impl_->signature_type = sig_type;
	impl_->api_creds = std::move(credentials);
	impl_->authenticated = true;
	return {};
}

Result<void> Client::authenticate(const ApiKeyCredentials& credentials,
								  const Address& signer_address, std::optional<Address> funder,
								  SignatureType sig_type) {
	if (!impl_)
		return std::unexpected(Error::validation("Client moved-from"));
	if (credentials.api_key.empty() || credentials.api_secret.empty() ||
		credentials.api_passphrase.empty() || !crypto::base64_decode(credentials.api_secret))
		return std::unexpected(Error::validation("Invalid API credentials"));
	impl_->signature_type = sig_type;
	impl_->api_creds = credentials;
	impl_->signer_addr = signer_address;
	impl_->funder_addr = funder.value_or(signer_address);
	impl_->authenticated = true;
	return {};
}

Result<void> Client::set_builder_credentials(const BuilderKeyCredentials& credentials) {
	if (!impl_)
		return std::unexpected(Error::validation("Client moved-from"));
	impl_->builder_creds = credentials;
	impl_->is_builder_mode = true;
	return {};
}

void Client::deauthenticate() {
	if (!impl_)
		return;
	impl_->authenticated = false;
	impl_->is_builder_mode = false;
	impl_->api_creds.reset();
	impl_->builder_creds.reset();
	impl_->signer_addr.reset();
	impl_->funder_addr.reset();
}

// =========================================================================
// Public Endpoints
// =========================================================================

Result<HealthResponse> Client::ok() {
	if (!impl_)
		return std::unexpected(Error::validation("Client moved-from"));
	auto resp = impl_->http.get(impl_->config.base_url + "/");
	if (!resp)
		return std::unexpected(resp.error());
	if (!resp->is_success())
		return std::unexpected(Error::server("Health check failed"));
	return HealthResponse{.status = "OK"};
}

Result<ServerTimeResponse> Client::server_time() {
	if (!impl_)
		return std::unexpected(Error::validation("Client moved-from"));
	const Result<std::string> text = impl_->get_timestamp(true);
	if (!text)
		return std::unexpected(text.error());
	std::int64_t timestamp = 0;
	const auto parsed = std::from_chars(text->data(), text->data() + text->size(), timestamp);
	if (parsed.ec != std::errc{} || parsed.ptr != text->data() + text->size())
		return std::unexpected(Error::parse("Invalid server timestamp"));
	return ServerTimeResponse{.timestamp = timestamp};
}

Result<PriceResponse> Client::price(const PriceRequest& request) {
	if (!impl_)
		return std::unexpected(Error::validation("Client moved-from"));
	http::QueryBuilder query;
	query.add("token_id", request.token_id.to_decimal_string());
	query.add("side", request.side == Side::Buy ? "BUY" : "SELL");

	auto resp = impl_->http.get(impl_->config.base_url + "/price" + query.build());
	if (!resp)
		return std::unexpected(resp.error());
	if (!resp->is_success())
		return std::unexpected(Error::server("Get price failed"));

	const Result<Decimal> price_value = json::extract_decimal(resp->body, "price");
	if (!price_value)
		return std::unexpected(price_value.error());
	return PriceResponse{.price = *price_value, .token_id = request.token_id, .side = request.side};
}

Result<std::vector<PriceResponse>> Client::prices(const std::vector<PriceRequest>& requests) {
	if (!impl_)
		return std::unexpected(Error::validation("Client moved-from"));
	std::vector<PriceResponse> results;
	for (const auto& req : requests) {
		auto result = price(req);
		if (!result)
			return std::unexpected(result.error());
		results.push_back(*result);
	}
	return results;
}

Result<MidpointResponse> Client::midpoint(const MidpointRequest& request) {
	if (!impl_)
		return std::unexpected(Error::validation("Client moved-from"));
	http::QueryBuilder query;
	query.add("token_id", request.token_id.to_decimal_string());

	auto resp = impl_->http.get(impl_->config.base_url + "/midpoint" + query.build());
	if (!resp)
		return std::unexpected(resp.error());
	if (!resp->is_success())
		return std::unexpected(Error::server("Get midpoint failed"));

	const Result<Decimal> mid_value = json::extract_decimal(resp->body, "mid");
	if (!mid_value)
		return std::unexpected(mid_value.error());
	return MidpointResponse{.mid = *mid_value, .token_id = request.token_id};
}

Result<std::vector<MidpointResponse>>
Client::midpoints(const std::vector<MidpointRequest>& requests) {
	if (!impl_)
		return std::unexpected(Error::validation("Client moved-from"));
	std::vector<MidpointResponse> results;
	for (const auto& req : requests) {
		auto result = midpoint(req);
		if (!result)
			return std::unexpected(result.error());
		results.push_back(*result);
	}
	return results;
}

Result<SpreadResponse> Client::spread(const SpreadRequest& request) {
	if (!impl_)
		return std::unexpected(Error::validation("Client moved-from"));
	http::QueryBuilder query;
	query.add("token_id", request.token_id.to_decimal_string());

	auto resp = impl_->http.get(impl_->config.base_url + "/spread" + query.build());
	if (!resp)
		return std::unexpected(resp.error());
	if (!resp->is_success())
		return std::unexpected(Error::server("Get spread failed"));

	const Result<Decimal> spread_value = json::extract_decimal(resp->body, "spread");
	if (!spread_value)
		return std::unexpected(spread_value.error());
	return SpreadResponse{.spread = *spread_value, .token_id = request.token_id};
}

Result<std::vector<SpreadResponse>> Client::spreads(const std::vector<SpreadRequest>& requests) {
	if (!impl_)
		return std::unexpected(Error::validation("Client moved-from"));
	std::vector<SpreadResponse> results;
	for (const auto& req : requests) {
		auto result = spread(req);
		if (!result)
			return std::unexpected(result.error());
		results.push_back(*result);
	}
	return results;
}

Result<LastTradePriceResponse> Client::last_trade_price(uint256_t token_id) {
	if (!impl_)
		return std::unexpected(Error::validation("Client moved-from"));
	http::QueryBuilder query;
	query.add("token_id", token_id.to_decimal_string());

	auto resp = impl_->http.get(impl_->config.base_url + "/last-trade-price" + query.build());
	if (!resp)
		return std::unexpected(resp.error());
	if (!resp->is_success())
		return std::unexpected(Error::server("Get last trade price failed"));

	const Result<Decimal> price_value = json::extract_decimal(resp->body, "price");
	if (!price_value)
		return std::unexpected(price_value.error());
	const std::optional<std::string> side = json::extract_string(resp->body, "side");
	if (!side || (*side != "BUY" && *side != "SELL" && !side->empty()))
		return std::unexpected(Error::parse("Invalid last-trade side"));
	const std::optional<std::int64_t> ts = json::extract_int(resp->body, "timestamp");
	return LastTradePriceResponse{
		.price = *price_value, .token_id = token_id, .timestamp = ts.value_or(0), .side = *side};
}

Result<OrderBookResponse> Client::book(const OrderBookRequest& request) {
	if (!impl_)
		return std::unexpected(Error::validation("Client moved-from"));
	(void)request;
	return std::unexpected(Error::validation("Legacy book is not implemented"));
}

Result<std::vector<OrderBookResponse>>
Client::books(const std::vector<OrderBookRequest>& requests) {
	if (!impl_)
		return std::unexpected(Error::validation("Client moved-from"));
	std::vector<OrderBookResponse> results;
	for (const auto& req : requests) {
		auto result = book(req);
		if (!result)
			return std::unexpected(result.error());
		results.push_back(*result);
	}
	return results;
}

Result<TickSizeResponse> Client::tick_size(uint256_t token_id) {
	if (!impl_)
		return std::unexpected(Error::validation("Client moved-from"));
	std::string key = token_id.to_decimal_string();

	auto it = impl_->tick_size_cache.find(key);
	if (it != impl_->tick_size_cache.end()) {
		return TickSizeResponse{.token_id = token_id, .minimum_tick_size = it->second};
	}

	http::QueryBuilder query;
	query.add("token_id", key);

	auto resp = impl_->http.get(impl_->config.base_url + "/tick-size" + query.build());
	if (!resp)
		return std::unexpected(resp.error());
	if (!resp->is_success())
		return std::unexpected(Error::server("Get tick size failed"));

	std::optional<std::string> ts_str = json::extract_string(resp->body, "minimum_tick_size");
	if (!ts_str) {
		const std::optional<glz::generic_u64> value = json::field(resp->body, "minimum_tick_size");
		if (value && value->is_number()) {
			const double n = value->as_number();
			if (n == 0.1)
				ts_str = "0.1";
			else if (n == 0.01)
				ts_str = "0.01";
			else if (n == 0.001)
				ts_str = "0.001";
			else if (n == 0.0001)
				ts_str = "0.0001";
		}
	}
	if (!ts_str ||
		(*ts_str != "0.1" && *ts_str != "0.01" && *ts_str != "0.001" && *ts_str != "0.0001"))
		return std::unexpected(Error::parse("Missing or invalid tick size"));
	const TickSize ts = *ts_str == "0.1"	  ? TickSize::Tenth
						: *ts_str == "0.001"  ? TickSize::Thousandth
						: *ts_str == "0.0001" ? TickSize::TenThousandth
											  : TickSize::Hundredth;

	impl_->tick_size_cache[key] = ts;
	return TickSizeResponse{.token_id = token_id, .minimum_tick_size = ts};
}

Result<NegRiskResponse> Client::neg_risk(uint256_t token_id) {
	if (!impl_)
		return std::unexpected(Error::validation("Client moved-from"));
	std::string key = token_id.to_decimal_string();

	auto it = impl_->neg_risk_cache.find(key);
	if (it != impl_->neg_risk_cache.end()) {
		return NegRiskResponse{.token_id = token_id, .neg_risk = it->second};
	}

	http::QueryBuilder query;
	query.add("token_id", key);

	auto resp = impl_->http.get(impl_->config.base_url + "/neg-risk" + query.build());
	if (!resp)
		return std::unexpected(resp.error());
	if (!resp->is_success())
		return std::unexpected(Error::server("Get neg risk failed"));

	const std::optional<bool> nr = json::extract_bool(resp->body, "neg_risk");
	if (!nr)
		return std::unexpected(Error::parse("Missing or invalid neg_risk"));
	impl_->neg_risk_cache[key] = *nr;
	return NegRiskResponse{.token_id = token_id, .neg_risk = *nr};
}

Result<FeeRateResponse> Client::fee_rate_bps(uint256_t token_id) {
	if (!impl_)
		return std::unexpected(Error::validation("Client moved-from"));
	std::string key = token_id.to_decimal_string();

	auto it = impl_->fee_rate_cache.find(key);
	if (it != impl_->fee_rate_cache.end()) {
		return FeeRateResponse{
			.token_id = token_id, .base_fee = it->second, .maker_fee = 0, .taker_fee = it->second};
	}

	http::QueryBuilder query;
	query.add("token_id", key);

	auto resp = impl_->http.get(impl_->config.base_url + "/fee-rate" + query.build());
	if (!resp)
		return std::unexpected(resp.error());
	if (!resp->is_success())
		return std::unexpected(Error::server("Get fee rate failed"));

	std::optional<std::int64_t> fr = json::extract_int(resp->body, "fee_rate_bps");
	if (!fr)
		fr = json::extract_int(resp->body, "base_fee");
	if (!fr || *fr < 0 || *fr > std::numeric_limits<std::uint32_t>::max())
		return std::unexpected(Error::parse("Invalid fee rate"));
	const std::uint32_t fee_rate = static_cast<std::uint32_t>(*fr);
	impl_->fee_rate_cache[key] = fee_rate;

	return FeeRateResponse{
		.token_id = token_id, .base_fee = fee_rate, .maker_fee = 0, .taker_fee = fee_rate};
}

Result<PriceHistoryResponse> Client::price_history(const PriceHistoryRequest& request) {
	if (!impl_)
		return std::unexpected(Error::validation("Client moved-from"));
	(void)request;
	return std::unexpected(Error::validation("Legacy price_history is not implemented"));
}

Result<PaginatedResponse<MarketResponse>> Client::markets(std::optional<Cursor> cursor) {
	if (!impl_)
		return std::unexpected(Error::validation("Client moved-from"));
	(void)cursor;
	return std::unexpected(Error::validation("Legacy markets is not implemented"));
}

Result<GeoblockResponse> Client::check_geoblock() {
	if (!impl_)
		return std::unexpected(Error::validation("Client moved-from"));
	auto resp = impl_->http.get(impl_->config.geoblock_url + "/api/geoblock");
	if (!resp)
		return std::unexpected(resp.error());
	if (!resp->is_success())
		return std::unexpected(Error::server("Geoblock check failed"));

	std::optional<bool> blocked = json::extract_bool(resp->body, "blocked");
	if (!blocked)
		blocked = json::extract_bool(resp->body, "restricted");
	if (!blocked)
		return std::unexpected(Error::parse("Missing or invalid geoblock result"));
	auto country = json::extract_string(resp->body, "country").value_or("");
	return GeoblockResponse{.blocked = *blocked, .country_code = country};
}

// =========================================================================
// Cache Management
// =========================================================================

void Client::set_tick_size(uint256_t token_id, TickSize tick_size) {
	if (!impl_)
		return;
	impl_->tick_size_cache[token_id.to_decimal_string()] = tick_size;
}

void Client::set_neg_risk(uint256_t token_id, bool neg_risk) {
	if (!impl_)
		return;
	impl_->neg_risk_cache[token_id.to_decimal_string()] = neg_risk;
}

void Client::set_fee_rate_bps(uint256_t token_id, std::uint32_t fee_rate) {
	if (!impl_)
		return;
	impl_->fee_rate_cache[token_id.to_decimal_string()] = fee_rate;
}

void Client::invalidate_caches() {
	if (!impl_)
		return;
	impl_->tick_size_cache.clear();
	impl_->neg_risk_cache.clear();
	impl_->fee_rate_cache.clear();
}

// =========================================================================
// Authenticated Endpoints (stubs)
// =========================================================================

Result<std::vector<ApiKeyCredentials>> Client::api_keys() {
	if (!impl_)
		return std::unexpected(Error::validation("Client moved-from"));
	if (!impl_->authenticated)
		return std::unexpected(Error::auth("Not authenticated"));
	return std::unexpected(Error::validation("Legacy API-key listing is not implemented"));
}

VoidResult Client::delete_api_key() {
	if (!impl_)
		return std::unexpected(Error::validation("Client moved-from"));
	if (!impl_->authenticated)
		return std::unexpected(Error::auth("Not authenticated"));
	return std::unexpected(Error::validation("Legacy API-key deletion is not implemented"));
}

Result<LimitOrderBuilder> Client::limit_order(uint256_t token_id) {
	if (!impl_)
		return std::unexpected(Error::validation("Client moved-from"));
	if (!impl_->authenticated || !impl_->signer_addr) {
		return std::unexpected(Error::auth("Not authenticated"));
	}

	auto ts_result = tick_size(token_id);
	auto nr_result = neg_risk(token_id);
	auto fr_result = fee_rate_bps(token_id);

	if (!ts_result)
		return std::unexpected(ts_result.error());
	if (!nr_result)
		return std::unexpected(nr_result.error());
	if (!fr_result)
		return std::unexpected(fr_result.error());
	const TickSize ts = ts_result->minimum_tick_size;
	const bool nr = nr_result->neg_risk;
	const std::uint32_t fr = fr_result->base_fee;

	return LimitOrderBuilder(*impl_->signer_addr, impl_->funder_addr.value_or(*impl_->signer_addr),
							 impl_->signature_type, ts, fr, nr);
}

Result<MarketOrderBuilder> Client::market_order(uint256_t token_id) {
	if (!impl_)
		return std::unexpected(Error::validation("Client moved-from"));
	if (!impl_->authenticated || !impl_->signer_addr) {
		return std::unexpected(Error::auth("Not authenticated"));
	}

	auto ts_result = tick_size(token_id);
	auto nr_result = neg_risk(token_id);
	auto fr_result = fee_rate_bps(token_id);

	if (!ts_result)
		return std::unexpected(ts_result.error());
	if (!nr_result)
		return std::unexpected(nr_result.error());
	if (!fr_result)
		return std::unexpected(fr_result.error());
	const TickSize ts = ts_result->minimum_tick_size;
	const bool nr = nr_result->neg_risk;
	const std::uint32_t fr = fr_result->base_fee;

	return MarketOrderBuilder(*impl_->signer_addr, impl_->funder_addr.value_or(*impl_->signer_addr),
							  impl_->signature_type, ts, fr, nr);
}

Result<SignedOrder> Client::sign_order(const crypto::PrivateKey& key, const SignableOrder& order) {
	if (!impl_)
		return std::unexpected(Error::validation("Client moved-from"));
	bool neg = impl_->neg_risk_cache.count(order.order.token_id.to_decimal_string()) > 0
				   ? impl_->neg_risk_cache[order.order.token_id.to_decimal_string()]
				   : false;

	auto type_hash = order.order.type_hash(neg, impl_->config.chain_id);
	auto sig = key.sign(type_hash);
	if (!sig)
		return std::unexpected(sig.error());

	return SignedOrder{.order = order.order,
					   .signature = *sig,
					   .order_type = order.order_type,
					   .owner = impl_->api_creds ? impl_->api_creds->api_key : "",
					   .post_only = order.post_only};
}

Result<PostOrderResponse> Client::post_order(const SignedOrder& order) {
	if (!impl_)
		return std::unexpected(Error::validation("Client moved-from"));
	if (!impl_->authenticated)
		return std::unexpected(Error::auth("Not authenticated"));

	json::ObjectBuilder inner;
	inner.add("salt", order.order.salt.to_decimal_string());
	inner.add("maker", order.order.maker.to_checksum());
	inner.add("signer", order.order.signer.to_checksum());
	inner.add("taker", order.order.taker.to_checksum());
	inner.add("tokenId", order.order.token_id.to_decimal_string());
	inner.add("makerAmount", order.order.maker_amount.to_decimal_string());
	inner.add("takerAmount", order.order.taker_amount.to_decimal_string());
	inner.add("expiration", order.order.expiration.to_decimal_string());
	inner.add("nonce", order.order.nonce.to_decimal_string());
	inner.add("feeRateBps", order.order.fee_rate_bps.to_decimal_string());
	inner.add("side", static_cast<int64_t>(order.order.side));
	inner.add("signatureType", static_cast<int64_t>(order.order.signature_type));

	json::ObjectBuilder outer;
	outer.add_raw("order", inner.build());
	outer.add("owner", order.owner);
	outer.add("signature", order.signature.to_hex());
	outer.add("orderType", to_order_type_string(order.order_type));

	http::Request req;
	req.method = http::Method::POST;
	req.url = impl_->config.base_url + "/order";
	req.body = outer.build();
	req.header("Content-Type", "application/json");

	auto auth = impl_->add_l2_auth(req);
	if (!auth)
		return std::unexpected(auth.error());

	auto resp = impl_->http.execute(req);
	if (!resp)
		return std::unexpected(resp.error());
	if (!resp->is_success())
		return std::unexpected(Error::server("Post order failed: " + resp->body));

	const std::optional<std::string> order_id = json::extract_string(resp->body, "orderID");
	const std::optional<bool> success = json::extract_bool(resp->body, "success");
	if (!success || !*success || !order_id || order_id->empty())
		return std::unexpected(Error::parse("Order response does not confirm acceptance"));
	return PostOrderResponse{.order_id = *order_id, .success = true};
}

Result<std::vector<PostOrderResponse>> Client::post_orders(const std::vector<SignedOrder>& orders) {
	if (!impl_)
		return std::unexpected(Error::validation("Client moved-from"));
	std::vector<PostOrderResponse> results;
	for (const auto& order : orders) {
		auto result = post_order(order);
		if (!result)
			return std::unexpected(result.error());
		results.push_back(*result);
	}
	return results;
}

Result<OpenOrderResponse> Client::order(std::string_view order_id) {
	if (!impl_)
		return std::unexpected(Error::validation("Client moved-from"));
	(void)order_id;
	return std::unexpected(Error::validation("Legacy order is not implemented"));
}

Result<PaginatedResponse<OpenOrderResponse>> Client::orders(const OrdersRequest& request,
															std::optional<Cursor> cursor) {
	if (!impl_)
		return std::unexpected(Error::validation("Client moved-from"));
	(void)request;
	(void)cursor;
	return std::unexpected(Error::validation("Legacy orders is not implemented"));
}

Result<CancelOrdersResponse> Client::cancel_order(std::string_view order_id) {
	if (!impl_)
		return std::unexpected(Error::validation("Client moved-from"));
	(void)order_id;
	return std::unexpected(Error::validation("Legacy cancel_order is not implemented"));
}

Result<CancelOrdersResponse> Client::cancel_orders(const std::vector<std::string>& order_ids) {
	if (!impl_)
		return std::unexpected(Error::validation("Client moved-from"));
	(void)order_ids;
	return std::unexpected(Error::validation("Legacy cancel_orders is not implemented"));
}

Result<CancelOrdersResponse> Client::cancel_all_orders() {
	if (!impl_)
		return std::unexpected(Error::validation("Client moved-from"));
	return std::unexpected(Error::validation("Legacy cancel_all_orders is not implemented"));
}

Result<CancelOrdersResponse> Client::cancel_market_orders(std::optional<Bytes32> market,
														  std::optional<uint256_t> asset_id) {
	if (!impl_)
		return std::unexpected(Error::validation("Client moved-from"));
	(void)market;
	(void)asset_id;
	return std::unexpected(Error::validation("Legacy cancel_market_orders is not implemented"));
}

Result<PaginatedResponse<TradeResponse>> Client::trades(const TradesRequest& request,
														std::optional<Cursor> cursor) {
	if (!impl_)
		return std::unexpected(Error::validation("Client moved-from"));
	(void)request;
	(void)cursor;
	return std::unexpected(Error::validation("Legacy trades is not implemented"));
}

Result<BalanceAllowanceResponse> Client::balance_allowance(const BalanceAllowanceRequest& request) {
	if (!impl_)
		return std::unexpected(Error::validation("Client moved-from"));
	(void)request;
	return std::unexpected(Error::validation("Legacy balance_allowance is not implemented"));
}

VoidResult Client::update_balance_allowance(const BalanceAllowanceRequest& request) {
	if (!impl_)
		return std::unexpected(Error::validation("Client moved-from"));
	(void)request;
	return std::unexpected(Error::validation("Legacy update_balance_allowance is not implemented"));
}

Result<std::vector<NotificationResponse>> Client::notifications() {
	if (!impl_)
		return std::unexpected(Error::validation("Client moved-from"));
	return std::unexpected(Error::validation("Legacy notifications is not implemented"));
}

VoidResult Client::delete_notifications(const std::vector<std::string>& notification_ids) {
	if (!impl_)
		return std::unexpected(Error::validation("Client moved-from"));
	(void)notification_ids;
	return std::unexpected(Error::validation("Legacy delete_notifications is not implemented"));
}

Result<OrderScoringResponse> Client::is_order_scoring(std::string_view order_id) {
	if (!impl_)
		return std::unexpected(Error::validation("Client moved-from"));
	(void)order_id;
	return std::unexpected(Error::validation("Legacy is_order_scoring is not implemented"));
}

Result<std::vector<OrderScoringResponse>>
Client::are_orders_scoring(const std::vector<std::string>& order_ids) {
	if (!impl_)
		return std::unexpected(Error::validation("Client moved-from"));
	(void)order_ids;
	return std::unexpected(Error::validation("Legacy are_orders_scoring is not implemented"));
}

Result<bool> Client::closed_only_mode() {
	if (!impl_)
		return std::unexpected(Error::validation("Client moved-from"));
	return std::unexpected(Error::validation("Legacy closed_only_mode is not implemented"));
}

// =========================================================================
// Builder Endpoints
// =========================================================================

Result<HeartbeatResponse> Client::post_heartbeat(std::optional<std::string> heartbeat_id) {
	if (!impl_)
		return std::unexpected(Error::validation("Client moved-from"));
	(void)heartbeat_id;
	return std::unexpected(Error::validation("Legacy post_heartbeat is not implemented"));
}

Result<std::vector<BuilderKeyCredentials>> Client::builder_api_keys() {
	if (!impl_)
		return std::unexpected(Error::validation("Client moved-from"));
	return std::unexpected(Error::validation("Legacy builder_api_keys is not implemented"));
}

VoidResult Client::revoke_builder_api_key() {
	if (!impl_)
		return std::unexpected(Error::validation("Client moved-from"));
	return std::unexpected(Error::validation("Legacy revoke_builder_api_key is not implemented"));
}

// =========================================================================
// Accessors
// =========================================================================

const std::string& Client::host() const noexcept {
	static const std::string empty;
	return impl_ ? impl_->config.base_url : empty;
}
const ClientConfig& Client::config() const noexcept {
	static const ClientConfig empty;
	return impl_ ? impl_->config : empty;
}
std::optional<Address> Client::signer_address() const noexcept {
	return impl_ ? impl_->signer_addr : std::nullopt;
}
std::optional<Address> Client::funder_address() const noexcept {
	return impl_ ? impl_->funder_addr : std::nullopt;
}

} // namespace polymarket::clob
