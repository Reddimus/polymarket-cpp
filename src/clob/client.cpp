/// @file client.cpp
/// @brief CLOB API REST client implementation (simplified)

#include "polymarket/clob/client.hpp"
#include "polymarket/crypto/eip712.hpp"
#include "polymarket/crypto/hmac.hpp"
#include "polymarket/http/client.hpp"

#include <chrono>
#include <map>

namespace polymarket::clob {

// =========================================================================
// JSON Helpers
// =========================================================================

namespace json {

std::string escape(std::string_view s) {
  std::string result;
  result.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
    case '"':
      result += "\\\"";
      break;
    case '\\':
      result += "\\\\";
      break;
    case '\n':
      result += "\\n";
      break;
    case '\r':
      result += "\\r";
      break;
    case '\t':
      result += "\\t";
      break;
    default:
      result += c;
    }
  }
  return result;
}

class ObjectBuilder {
public:
  ObjectBuilder &add(std::string_view key, std::string_view value) {
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

  ObjectBuilder &add(std::string_view key, int64_t value) {
    if (!first_)
      out_ += ',';
    first_ = false;
    out_ += "\"";
    out_ += key;
    out_ += "\":";
    out_ += std::to_string(value);
    return *this;
  }

  ObjectBuilder &add_raw(std::string_view key, std::string_view raw) {
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

std::optional<std::string> extract_string(std::string_view json,
                                          std::string_view key) {
  std::string pattern = "\"" + std::string(key) + "\":";
  auto pos = json.find(pattern);
  if (pos == std::string_view::npos)
    return std::nullopt;

  pos += pattern.size();
  while (pos < json.size() && json[pos] == ' ')
    pos++;

  if (pos >= json.size() || json[pos] != '"')
    return std::nullopt;
  pos++;

  std::string result;
  while (pos < json.size() && json[pos] != '"') {
    result += json[pos++];
  }
  return result;
}

std::optional<int64_t> extract_int(std::string_view json,
                                   std::string_view key) {
  auto str = extract_string(json, key);
  if (!str)
    return std::nullopt;
  try {
    return std::stoll(*str);
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<bool> extract_bool(std::string_view json, std::string_view key) {
  std::string pattern = "\"" + std::string(key) + "\":";
  auto pos = json.find(pattern);
  if (pos == std::string_view::npos)
    return std::nullopt;
  pos += pattern.size();
  while (pos < json.size() && json[pos] == ' ')
    pos++;
  if (pos + 4 <= json.size() && json.substr(pos, 4) == "true")
    return true;
  if (pos + 5 <= json.size() && json.substr(pos, 5) == "false")
    return false;
  return std::nullopt;
}

} // namespace json

// =========================================================================
// Implementation
// =========================================================================

struct Client::Impl {
  http::Client http;
  ClientConfig config;

  bool authenticated{false};
  bool is_builder_mode{false};
  std::optional<Address> signer_addr;
  std::optional<Address> funder_addr;
  std::optional<ApiKeyCredentials> api_creds;
  std::optional<BuilderKeyCredentials> builder_creds;

  std::map<std::string, TickSize> tick_size_cache;
  std::map<std::string, bool> neg_risk_cache;
  std::map<std::string, std::uint32_t> fee_rate_cache;

  explicit Impl(ClientConfig cfg)
      : http(http::ClientConfig{.tcp_nodelay = true,
                                .tcp_keepalive = true,
                                .user_agent = "polymarket-cpp/0.1.0"}),
        config(std::move(cfg)) {}

  std::string get_timestamp() const {
    auto now = std::chrono::system_clock::now();
    auto ts =
        std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch())
            .count();
    return std::to_string(ts);
  }

  Result<void> add_l2_auth(http::Request &req) {
    if (!authenticated || !api_creds) {
      return std::unexpected(Error::auth("API credentials not available"));
    }

    std::string timestamp = get_timestamp();
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

    auto sig = crypto::generate_l2_signature(api_creds->api_secret, timestamp,
                                             method, path, req.body);
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
  return Client(std::make_unique<Impl>(std::move(config)));
}

Client::Client(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Client::~Client() = default;
Client::Client(Client &&) noexcept = default;
Client &Client::operator=(Client &&) noexcept = default;

bool Client::is_authenticated() const noexcept { return impl_->authenticated; }
bool Client::is_builder() const noexcept { return impl_->is_builder_mode; }

Result<void> Client::authenticate(const crypto::PrivateKey &signer,
                                  std::optional<Address> funder,
                                  SignatureType sig_type) {

  auto addr_result = signer.address();
  if (!addr_result)
    return std::unexpected(addr_result.error());

  impl_->signer_addr = *addr_result;
  impl_->funder_addr = funder.value_or(*addr_result);

  // Derive API key via L1 auth
  std::string timestamp = impl_->get_timestamp();

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
  req.url = impl_->config.base_url + "/auth/api-key";
  req.header("POLY_ADDRESS", addr_result->to_checksum());
  req.header("POLY_SIGNATURE", "0x" + sig->to_hex());
  req.header("POLY_TIMESTAMP", timestamp);
  req.header("POLY_NONCE", "0");

  auto resp = impl_->http.execute(req);
  if (!resp)
    return std::unexpected(resp.error());

  if (!resp->is_success()) {
    return std::unexpected(
        Error::auth("Failed to derive API key: " + resp->body));
  }

  impl_->api_creds = ApiKeyCredentials{
      .api_key = json::extract_string(resp->body, "apiKey").value_or(""),
      .api_secret = json::extract_string(resp->body, "secret").value_or(""),
      .api_passphrase =
          json::extract_string(resp->body, "passphrase").value_or("")};
  impl_->authenticated = true;
  return {};
}

Result<void> Client::authenticate(const ApiKeyCredentials &credentials,
                                  const Address &signer_address,
                                  std::optional<Address> funder) {

  impl_->api_creds = credentials;
  impl_->signer_addr = signer_address;
  impl_->funder_addr = funder.value_or(signer_address);
  impl_->authenticated = true;
  return {};
}

Result<void>
Client::set_builder_credentials(const BuilderKeyCredentials &credentials) {
  impl_->builder_creds = credentials;
  impl_->is_builder_mode = true;
  return {};
}

void Client::deauthenticate() {
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
  auto resp = impl_->http.get(impl_->config.base_url + "/");
  if (!resp)
    return std::unexpected(resp.error());
  if (!resp->is_success())
    return std::unexpected(Error::server("Health check failed"));
  return HealthResponse{.status = "OK"};
}

Result<ServerTimeResponse> Client::server_time() {
  auto resp = impl_->http.get(impl_->config.base_url + "/time");
  if (!resp)
    return std::unexpected(resp.error());
  if (!resp->is_success())
    return std::unexpected(Error::server("Server time failed"));
  auto ts = json::extract_int(resp->body, "timestamp");
  return ServerTimeResponse{.timestamp = ts.value_or(0)};
}

Result<PriceResponse> Client::price(const PriceRequest &request) {
  http::QueryBuilder query;
  query.add("token_id", request.token_id.to_decimal_string());
  query.add("side", request.side == Side::Buy ? "BUY" : "SELL");

  auto resp =
      impl_->http.get(impl_->config.base_url + "/price" + query.build());
  if (!resp)
    return std::unexpected(resp.error());
  if (!resp->is_success())
    return std::unexpected(Error::server("Get price failed"));

  auto price_str = json::extract_string(resp->body, "price");
  return PriceResponse{.price = Decimal::from_string(price_str.value_or("0")),
                       .token_id = request.token_id,
                       .side = request.side};
}

Result<std::vector<PriceResponse>>
Client::prices(const std::vector<PriceRequest> &requests) {
  std::vector<PriceResponse> results;
  for (const auto &req : requests) {
    auto result = price(req);
    if (!result)
      return std::unexpected(result.error());
    results.push_back(*result);
  }
  return results;
}

Result<MidpointResponse> Client::midpoint(const MidpointRequest &request) {
  http::QueryBuilder query;
  query.add("token_id", request.token_id.to_decimal_string());

  auto resp =
      impl_->http.get(impl_->config.base_url + "/midpoint" + query.build());
  if (!resp)
    return std::unexpected(resp.error());
  if (!resp->is_success())
    return std::unexpected(Error::server("Get midpoint failed"));

  auto mid_str = json::extract_string(resp->body, "mid");
  return MidpointResponse{.mid = Decimal::from_string(mid_str.value_or("0")),
                          .token_id = request.token_id};
}

Result<std::vector<MidpointResponse>>
Client::midpoints(const std::vector<MidpointRequest> &requests) {
  std::vector<MidpointResponse> results;
  for (const auto &req : requests) {
    auto result = midpoint(req);
    if (!result)
      return std::unexpected(result.error());
    results.push_back(*result);
  }
  return results;
}

Result<SpreadResponse> Client::spread(const SpreadRequest &request) {
  http::QueryBuilder query;
  query.add("token_id", request.token_id.to_decimal_string());

  auto resp =
      impl_->http.get(impl_->config.base_url + "/spread" + query.build());
  if (!resp)
    return std::unexpected(resp.error());
  if (!resp->is_success())
    return std::unexpected(Error::server("Get spread failed"));

  auto spread_str = json::extract_string(resp->body, "spread");
  return SpreadResponse{.spread =
                            Decimal::from_string(spread_str.value_or("0")),
                        .token_id = request.token_id};
}

Result<std::vector<SpreadResponse>>
Client::spreads(const std::vector<SpreadRequest> &requests) {
  std::vector<SpreadResponse> results;
  for (const auto &req : requests) {
    auto result = spread(req);
    if (!result)
      return std::unexpected(result.error());
    results.push_back(*result);
  }
  return results;
}

Result<LastTradePriceResponse> Client::last_trade_price(uint256_t token_id) {
  http::QueryBuilder query;
  query.add("token_id", token_id.to_decimal_string());

  auto resp = impl_->http.get(impl_->config.base_url + "/last-trade-price" +
                              query.build());
  if (!resp)
    return std::unexpected(resp.error());
  if (!resp->is_success())
    return std::unexpected(Error::server("Get last trade price failed"));

  auto price_str = json::extract_string(resp->body, "price");
  auto ts = json::extract_int(resp->body, "timestamp");
  return LastTradePriceResponse{
      .price = Decimal::from_string(price_str.value_or("0")),
      .token_id = token_id,
      .timestamp = ts.value_or(0)};
}

Result<OrderBookResponse> Client::book(const OrderBookRequest &request) {
  http::QueryBuilder query;
  query.add("token_id", request.token_id.to_decimal_string());

  auto resp = impl_->http.get(impl_->config.base_url + "/book" + query.build());
  if (!resp)
    return std::unexpected(resp.error());
  if (!resp->is_success())
    return std::unexpected(Error::server("Get book failed"));

  return OrderBookResponse{.token_id = request.token_id,
                           .bids = {},
                           .asks = {},
                           .timestamp = now_ms(),
                           .hash = ""};
}

Result<std::vector<OrderBookResponse>>
Client::books(const std::vector<OrderBookRequest> &requests) {
  std::vector<OrderBookResponse> results;
  for (const auto &req : requests) {
    auto result = book(req);
    if (!result)
      return std::unexpected(result.error());
    results.push_back(*result);
  }
  return results;
}

Result<TickSizeResponse> Client::tick_size(uint256_t token_id) {
  std::string key = token_id.to_decimal_string();

  auto it = impl_->tick_size_cache.find(key);
  if (it != impl_->tick_size_cache.end()) {
    return TickSizeResponse{.token_id = token_id,
                            .minimum_tick_size = it->second};
  }

  http::QueryBuilder query;
  query.add("token_id", key);

  auto resp =
      impl_->http.get(impl_->config.base_url + "/tick-size" + query.build());
  if (!resp)
    return std::unexpected(resp.error());
  if (!resp->is_success())
    return std::unexpected(Error::server("Get tick size failed"));

  auto ts_str = json::extract_string(resp->body, "minimum_tick_size");
  TickSize ts = TickSize::Hundredth;
  if (ts_str) {
    if (*ts_str == "0.1")
      ts = TickSize::Tenth;
    else if (*ts_str == "0.001")
      ts = TickSize::Thousandth;
    else if (*ts_str == "0.0001")
      ts = TickSize::TenThousandth;
  }

  impl_->tick_size_cache[key] = ts;
  return TickSizeResponse{.token_id = token_id, .minimum_tick_size = ts};
}

Result<NegRiskResponse> Client::neg_risk(uint256_t token_id) {
  std::string key = token_id.to_decimal_string();

  auto it = impl_->neg_risk_cache.find(key);
  if (it != impl_->neg_risk_cache.end()) {
    return NegRiskResponse{.token_id = token_id, .neg_risk = it->second};
  }

  http::QueryBuilder query;
  query.add("token_id", key);

  auto resp =
      impl_->http.get(impl_->config.base_url + "/neg-risk" + query.build());
  if (!resp)
    return std::unexpected(resp.error());
  if (!resp->is_success())
    return std::unexpected(Error::server("Get neg risk failed"));

  auto nr = json::extract_bool(resp->body, "neg_risk").value_or(false);
  impl_->neg_risk_cache[key] = nr;
  return NegRiskResponse{.token_id = token_id, .neg_risk = nr};
}

Result<FeeRateResponse> Client::fee_rate_bps(uint256_t token_id) {
  std::string key = token_id.to_decimal_string();

  auto it = impl_->fee_rate_cache.find(key);
  if (it != impl_->fee_rate_cache.end()) {
    return FeeRateResponse{.token_id = token_id,
                           .base_fee = it->second,
                           .maker_fee = 0,
                           .taker_fee = it->second};
  }

  http::QueryBuilder query;
  query.add("token_id", key);

  auto resp =
      impl_->http.get(impl_->config.base_url + "/fee-rate-bps" + query.build());
  if (!resp)
    return std::unexpected(resp.error());
  if (!resp->is_success())
    return std::unexpected(Error::server("Get fee rate failed"));

  auto fr = json::extract_int(resp->body, "fee_rate_bps");
  std::uint32_t fee_rate = fr ? static_cast<std::uint32_t>(*fr) : 0;
  impl_->fee_rate_cache[key] = fee_rate;

  return FeeRateResponse{.token_id = token_id,
                         .base_fee = fee_rate,
                         .maker_fee = 0,
                         .taker_fee = fee_rate};
}

Result<PriceHistoryResponse>
Client::price_history(const PriceHistoryRequest &request) {
  http::QueryBuilder query;
  query.add("token_id", request.token_id.to_decimal_string());
  if (request.interval) {
    query.add("interval", to_interval_string(*request.interval));
  }

  auto resp = impl_->http.get(impl_->config.base_url + "/price-history" +
                              query.build());
  if (!resp)
    return std::unexpected(resp.error());
  if (!resp->is_success())
    return std::unexpected(Error::server("Get price history failed"));

  return PriceHistoryResponse{.history = {}};
}

Result<PaginatedResponse<MarketResponse>>
Client::markets(std::optional<Cursor> cursor) {
  http::QueryBuilder query;
  if (cursor)
    query.add("next_cursor", cursor->value);

  auto resp =
      impl_->http.get(impl_->config.base_url + "/markets" + query.build());
  if (!resp)
    return std::unexpected(resp.error());
  if (!resp->is_success())
    return std::unexpected(Error::server("Get markets failed"));

  return PaginatedResponse<MarketResponse>{.items = {},
                                           .next_cursor = std::nullopt};
}

Result<GeoblockResponse> Client::check_geoblock() {
  auto resp = impl_->http.get(impl_->config.geoblock_url + "/api/geoblock");
  if (!resp)
    return std::unexpected(resp.error());
  if (!resp->is_success())
    return std::unexpected(Error::server("Geoblock check failed"));

  auto blocked = json::extract_bool(resp->body, "restricted").value_or(false);
  auto country = json::extract_string(resp->body, "country").value_or("");
  return GeoblockResponse{.blocked = blocked, .country_code = country};
}

// =========================================================================
// Cache Management
// =========================================================================

void Client::set_tick_size(uint256_t token_id, TickSize tick_size) {
  impl_->tick_size_cache[token_id.to_decimal_string()] = tick_size;
}

void Client::set_neg_risk(uint256_t token_id, bool neg_risk) {
  impl_->neg_risk_cache[token_id.to_decimal_string()] = neg_risk;
}

void Client::set_fee_rate_bps(uint256_t token_id, std::uint32_t fee_rate) {
  impl_->fee_rate_cache[token_id.to_decimal_string()] = fee_rate;
}

void Client::invalidate_caches() {
  impl_->tick_size_cache.clear();
  impl_->neg_risk_cache.clear();
  impl_->fee_rate_cache.clear();
}

// =========================================================================
// Authenticated Endpoints (stubs)
// =========================================================================

Result<std::vector<ApiKeyCredentials>> Client::api_keys() {
  if (!impl_->authenticated)
    return std::unexpected(Error::auth("Not authenticated"));
  return std::vector<ApiKeyCredentials>{};
}

VoidResult Client::delete_api_key() {
  if (!impl_->authenticated)
    return std::unexpected(Error::auth("Not authenticated"));
  return {};
}

Result<LimitOrderBuilder> Client::limit_order(uint256_t token_id) {
  if (!impl_->authenticated || !impl_->signer_addr) {
    return std::unexpected(Error::auth("Not authenticated"));
  }

  auto ts_result = tick_size(token_id);
  auto nr_result = neg_risk(token_id);
  auto fr_result = fee_rate_bps(token_id);

  TickSize ts = ts_result ? ts_result->minimum_tick_size : TickSize::Hundredth;
  bool nr = nr_result ? nr_result->neg_risk : false;
  std::uint32_t fr = fr_result ? fr_result->base_fee : 0;

  return LimitOrderBuilder(*impl_->signer_addr,
                           impl_->funder_addr.value_or(*impl_->signer_addr),
                           SignatureType::EOA, ts, fr, nr);
}

Result<MarketOrderBuilder> Client::market_order(uint256_t token_id) {
  if (!impl_->authenticated || !impl_->signer_addr) {
    return std::unexpected(Error::auth("Not authenticated"));
  }

  auto ts_result = tick_size(token_id);
  auto nr_result = neg_risk(token_id);
  auto fr_result = fee_rate_bps(token_id);

  TickSize ts = ts_result ? ts_result->minimum_tick_size : TickSize::Hundredth;
  bool nr = nr_result ? nr_result->neg_risk : false;
  std::uint32_t fr = fr_result ? fr_result->base_fee : 0;

  return MarketOrderBuilder(*impl_->signer_addr,
                            impl_->funder_addr.value_or(*impl_->signer_addr),
                            SignatureType::EOA, ts, fr, nr);
}

Result<SignedOrder> Client::sign_order(const crypto::PrivateKey &key,
                                       const SignableOrder &order) {
  bool neg =
      impl_->neg_risk_cache.count(order.order.token_id.to_decimal_string()) > 0
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

Result<PostOrderResponse> Client::post_order(const SignedOrder &order) {
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
  outer.add("signature", "0x" + order.signature.to_hex());
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

  auto order_id = json::extract_string(resp->body, "orderID");
  return PostOrderResponse{.order_id = order_id.value_or(""), .success = true};
}

Result<std::vector<PostOrderResponse>>
Client::post_orders(const std::vector<SignedOrder> &orders) {
  std::vector<PostOrderResponse> results;
  for (const auto &order : orders) {
    auto result = post_order(order);
    if (!result)
      return std::unexpected(result.error());
    results.push_back(*result);
  }
  return results;
}

Result<OpenOrderResponse> Client::order(std::string_view order_id) {
  if (!impl_->authenticated)
    return std::unexpected(Error::auth("Not authenticated"));
  return OpenOrderResponse{};
}

Result<PaginatedResponse<OpenOrderResponse>>
Client::orders(const OrdersRequest &request, std::optional<Cursor> cursor) {
  if (!impl_->authenticated)
    return std::unexpected(Error::auth("Not authenticated"));
  return PaginatedResponse<OpenOrderResponse>{.items = {},
                                              .next_cursor = std::nullopt};
}

Result<CancelOrdersResponse> Client::cancel_order(std::string_view order_id) {
  if (!impl_->authenticated)
    return std::unexpected(Error::auth("Not authenticated"));
  return CancelOrdersResponse{.canceled = {std::string(order_id)},
                              .not_canceled = {}};
}

Result<CancelOrdersResponse>
Client::cancel_orders(const std::vector<std::string> &order_ids) {
  CancelOrdersResponse response;
  for (const auto &id : order_ids) {
    auto result = cancel_order(id);
    if (result)
      response.canceled.push_back(id);
    else
      response.not_canceled.push_back(id);
  }
  return response;
}

Result<CancelOrdersResponse> Client::cancel_all_orders() {
  if (!impl_->authenticated)
    return std::unexpected(Error::auth("Not authenticated"));
  return CancelOrdersResponse{.canceled = {}, .not_canceled = {}};
}

Result<CancelOrdersResponse>
Client::cancel_market_orders(std::optional<Bytes32> market,
                             std::optional<uint256_t> asset_id) {
  if (!impl_->authenticated)
    return std::unexpected(Error::auth("Not authenticated"));
  return CancelOrdersResponse{.canceled = {}, .not_canceled = {}};
}

Result<PaginatedResponse<TradeResponse>>
Client::trades(const TradesRequest &request, std::optional<Cursor> cursor) {
  if (!impl_->authenticated)
    return std::unexpected(Error::auth("Not authenticated"));
  return PaginatedResponse<TradeResponse>{.items = {},
                                          .next_cursor = std::nullopt};
}

Result<BalanceAllowanceResponse>
Client::balance_allowance(const BalanceAllowanceRequest &request) {
  if (!impl_->authenticated)
    return std::unexpected(Error::auth("Not authenticated"));
  return BalanceAllowanceResponse{};
}

VoidResult
Client::update_balance_allowance(const BalanceAllowanceRequest &request) {
  if (!impl_->authenticated)
    return std::unexpected(Error::auth("Not authenticated"));
  return {};
}

Result<std::vector<NotificationResponse>> Client::notifications() {
  if (!impl_->authenticated)
    return std::unexpected(Error::auth("Not authenticated"));
  return std::vector<NotificationResponse>{};
}

VoidResult
Client::delete_notifications(const std::vector<std::string> &notification_ids) {
  if (!impl_->authenticated)
    return std::unexpected(Error::auth("Not authenticated"));
  return {};
}

Result<OrderScoringResponse>
Client::is_order_scoring(std::string_view order_id) {
  if (!impl_->authenticated)
    return std::unexpected(Error::auth("Not authenticated"));
  return OrderScoringResponse{};
}

Result<std::vector<OrderScoringResponse>>
Client::are_orders_scoring(const std::vector<std::string> &order_ids) {
  if (!impl_->authenticated)
    return std::unexpected(Error::auth("Not authenticated"));
  return std::vector<OrderScoringResponse>{};
}

Result<bool> Client::closed_only_mode() {
  if (!impl_->authenticated)
    return std::unexpected(Error::auth("Not authenticated"));
  return false;
}

// =========================================================================
// Builder Endpoints
// =========================================================================

Result<HeartbeatResponse>
Client::post_heartbeat(std::optional<std::string> heartbeat_id) {
  if (!impl_->is_builder_mode)
    return std::unexpected(Error::auth("Builder credentials not set"));
  return HeartbeatResponse{};
}

Result<std::vector<BuilderKeyCredentials>> Client::builder_api_keys() {
  if (!impl_->is_builder_mode)
    return std::unexpected(Error::auth("Builder credentials not set"));
  return std::vector<BuilderKeyCredentials>{};
}

VoidResult Client::revoke_builder_api_key() {
  if (!impl_->is_builder_mode)
    return std::unexpected(Error::auth("Builder credentials not set"));
  return {};
}

// =========================================================================
// Accessors
// =========================================================================

const std::string &Client::host() const noexcept {
  return impl_->config.base_url;
}
const ClientConfig &Client::config() const noexcept { return impl_->config; }
std::optional<Address> Client::signer_address() const noexcept {
  return impl_->signer_addr;
}
std::optional<Address> Client::funder_address() const noexcept {
  return impl_->funder_addr;
}

} // namespace polymarket::clob
