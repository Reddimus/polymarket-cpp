/// @file us/client.cpp
/// @brief Polymarket US API client implementation
///
/// Implements the docs.polymarket.us auth scheme verified 2026-05-02:
/// - Headers: X-PM-Access-Key, X-PM-Timestamp (unix ms), X-PM-Signature
/// - Canonical signed string: literal `{timestamp}{method}{path}` (no
///   separators, body NOT signed)
/// - Tolerance ±30s
/// - Secret = base64 of 64 bytes (seed||pub); first 32 = Ed25519 seed
///
/// Two hosts: authed (api.polymarket.us) for orders/positions/etc.,
/// public (gateway.polymarket.us) for discovery/books/settlement/candles.

#include "polymarket/us/client.hpp"
#include "polymarket/crypto/ed25519.hpp"
#include "polymarket/crypto/hmac.hpp"  // base64_encode/decode helpers
#include "polymarket/http/client.hpp"

#include <chrono>
#include <cstring>
#include <iostream>
#include <sstream>

namespace polymarket::us {

namespace {

/// Build the canonical signing message verbatim per docs.polymarket.us:
///   f"{timestamp}{method}{path}"  — no separators, body excluded.
std::string canonical_message(std::string_view timestamp_ms,
                              std::string_view method,
                              std::string_view path) {
    std::string msg;
    msg.reserve(timestamp_ms.size() + method.size() + path.size());
    msg.append(timestamp_ms);
    msg.append(method);
    msg.append(path);
    return msg;
}

/// Current unix time in milliseconds as a string.
std::string now_unix_ms() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    return std::to_string(ms);
}

/// Append a query parameter to a URL, handling the ?/& separator.
void append_query(std::string& url, std::string_view key, std::string_view value) {
    url += url.find('?') == std::string::npos ? '?' : '&';
    url += key;
    url += '=';
    url += value;
}

void append_query(std::string& url, std::string_view key, bool value) {
    append_query(url, key, value ? "true" : "false");
}

void append_query(std::string& url, std::string_view key, int value) {
    append_query(url, key, std::to_string(value));
}

}  // namespace

struct Client::Impl {
    std::string authed_host;
    std::string public_host;
    std::optional<Credentials> credentials;
    std::optional<crypto::Ed25519PrivateKey> signing_key;
    http::Client http_client;

    /// Sign the canonical message with the loaded Ed25519 key.
    /// Returns base64(signature).
    Result<std::string> sign(std::string_view message) {
        if (!signing_key.has_value()) {
            return std::unexpected(Error::validation(
                "Polymarket US client missing credentials; "
                "call set_credentials() first"));
        }
        Result<crypto::Ed25519Signature> sig_result =
            signing_key->sign(message);
        if (!sig_result.has_value()) {
            return std::unexpected(sig_result.error());
        }
        return crypto::base64_encode(std::span<const std::uint8_t>{
            sig_result->data(), sig_result->size()});
    }

    /// Issue a public (unauthenticated) GET against gateway.polymarket.us.
    Result<std::string> public_get(std::string_view path) {
        std::string url = public_host;
        url += path;
        std::cerr << "[pm::us::public_get] " << url << '\n';
        http::Request req;
        req.method = http::Method::GET;
        req.url = url;
        Result<http::Response> resp = http_client.execute(req);
        std::cerr << "[pm::us::public_get] returned has_value="
                  << resp.has_value() << '\n';
        if (!resp.has_value()) {
            return std::unexpected(resp.error());
        }
        if (!resp->is_success()) {
            std::ostringstream msg;
            msg << "Polymarket US public GET " << path
                << " returned HTTP " << resp->status_code << ": " << resp->body;
            return std::unexpected(Error::network(msg.str()));
        }
        return resp->body;
    }

    /// Issue an authenticated request. `path` must start with `/`.
    /// `body` is empty for GET / DELETE / no-body POSTs.
    Result<std::string> authed_request(http::Method method,
                                       std::string_view path,
                                       std::string_view body) {
        if (!credentials.has_value()) {
            return std::unexpected(Error::validation(
                "Polymarket US authed call requires credentials; "
                "call set_credentials() first"));
        }
        const std::string ts = now_unix_ms();
        const std::string method_str =
            std::string(http::method_to_string(method));
        // The docs spec is to sign only the path component, NOT the query
        // string (query inclusion not documented; we match the worked
        // examples).
        const std::string base_path =
            [path]() {
                const auto qpos = path.find('?');
                return qpos == std::string_view::npos
                    ? std::string(path)
                    : std::string(path.substr(0, qpos));
            }();
        const std::string msg = canonical_message(ts, method_str, base_path);
        Result<std::string> sig_b64 = sign(msg);
        if (!sig_b64.has_value()) {
            return std::unexpected(sig_b64.error());
        }

        std::string url = authed_host;
        url += path;
        http::Request req;
        req.method = method;
        req.url = url;
        req.header("X-PM-Access-Key", credentials->key_id);
        req.header("X-PM-Timestamp", ts);
        req.header("X-PM-Signature", *sig_b64);
        if (!body.empty()) {
            req.json_content();
            req.set_body(body);
        }

        Result<http::Response> resp = http_client.execute(req);
        if (!resp.has_value()) {
            return std::unexpected(resp.error());
        }
        if (!resp->is_success()) {
            std::ostringstream errmsg;
            errmsg << "Polymarket US authed " << method_str << " " << path
                   << " returned HTTP " << resp->status_code << ": "
                   << resp->body;
            return std::unexpected(Error::network(errmsg.str()));
        }
        return resp->body;
    }
};

Client::Client(std::string_view authed_host, std::string_view public_host)
    : impl_(std::make_unique<Impl>()) {
    impl_->authed_host = std::string(authed_host);
    impl_->public_host = std::string(public_host);
}

Client::~Client() = default;
Client::Client(Client&&) noexcept = default;
Client& Client::operator=(Client&&) noexcept = default;

Result<void> Client::set_credentials(Credentials creds) {
    // Decode the base64 secret; expect 64 bytes (seed||pub).
    Result<std::vector<std::uint8_t>> decoded =
        crypto::base64_decode(creds.secret_key);
    if (!decoded.has_value()) {
        return std::unexpected(decoded.error());
    }
    if (decoded->size() != 64) {
        std::ostringstream msg;
        msg << "Polymarket US secret must decode to 64 bytes (seed||pub); "
            << "got " << decoded->size() << " bytes";
        return std::unexpected(Error::validation(msg.str()));
    }
    // First 32 bytes are the Ed25519 seed per docs.polymarket.us.
    std::array<std::uint8_t, 32> seed{};
    std::memcpy(seed.data(), decoded->data(), 32);
    Result<crypto::Ed25519PrivateKey> key =
        crypto::Ed25519PrivateKey::from_seed(seed);
    if (!key.has_value()) {
        return std::unexpected(key.error());
    }
    impl_->signing_key.emplace(std::move(*key));
    impl_->credentials = std::move(creds);
    return {};
}

// ----- Public Endpoints (gateway.polymarket.us) -----

Result<std::string> Client::get_events(const EventFilter& filter) {
    std::string path = "/v1/events";
    if (filter.active) append_query(path, "active", *filter.active);
    if (filter.limit) append_query(path, "limit", *filter.limit);
    if (filter.cursor) append_query(path, "cursor", *filter.cursor);
    return impl_->public_get(path);
}

Result<std::string> Client::get_event(std::string_view event_id) {
    std::string path = "/v1/events/";
    path.append(event_id);
    return impl_->public_get(path);
}

Result<std::string> Client::get_markets(const MarketFilter& filter) {
    std::cerr << "[pm::us::get_markets] enter\n";
    std::string path = "/v1/markets";
    if (filter.event_id) append_query(path, "eventId", *filter.event_id);
    if (filter.active) append_query(path, "active", *filter.active);
    if (filter.closed) append_query(path, "closed", *filter.closed);
    if (filter.tag_id) append_query(path, "tagIds", *filter.tag_id);
    if (filter.end_date_min) append_query(path, "endDateMin", *filter.end_date_min);
    if (filter.end_date_max) append_query(path, "endDateMax", *filter.end_date_max);
    if (filter.limit) append_query(path, "limit", *filter.limit);
    if (filter.offset) append_query(path, "offset", *filter.offset);
    if (filter.cursor) append_query(path, "cursor", *filter.cursor);
    std::cerr << "[pm::us::get_markets] path built: " << path << '\n';
    return impl_->public_get(path);
}

Result<std::string> Client::get_market(std::string_view market_id) {
    std::string path = "/v1/markets/";
    path.append(market_id);
    return impl_->public_get(path);
}

Result<std::string> Client::get_orderbook(std::string_view market_id) {
    std::string path = "/v1/markets/";
    path.append(market_id);
    path.append("/book");
    return impl_->public_get(path);
}

Result<std::string> Client::get_series() {
    return impl_->public_get("/v1/series");
}

Result<std::string> Client::get_series_by_ticker(std::string_view ticker) {
    std::string path = "/v1/series/";
    path.append(ticker);
    return impl_->public_get(path);
}

Result<std::string> Client::get_sports() {
    return impl_->public_get("/v1/sports");
}

Result<std::string> Client::get_sport(std::string_view sport_id) {
    std::string path = "/v1/sports/";
    path.append(sport_id);
    return impl_->public_get(path);
}

Result<std::string> Client::search(std::string_view query) {
    std::string path = "/v1/search?q=";
    path.append(query);
    return impl_->public_get(path);
}

// ----- Tags / Settlement / Candles / Health -----

Result<std::string> Client::get_tag_by_slug(std::string_view slug) {
    std::string path = "/v2/tags/slug/";
    path.append(slug);
    return impl_->public_get(path);
}

Result<std::string> Client::get_tags() {
    return impl_->public_get("/v2/tags");
}

Result<std::string> Client::get_settlement(std::string_view market_slug) {
    std::string path = "/v1/markets/";
    path.append(market_slug);
    path.append("/settlement");
    return impl_->public_get(path);
}

Result<std::string> Client::get_candles(const CandleRequest& req) {
    // POST /v1beta1/report/trades/stats with JSON body. Public host.
    // Use a minimal handcrafted JSON to avoid pulling in a parser
    // dependency for the SDK consumer surface.
    std::ostringstream body;
    body << "{"
         << R"("symbol":")" << req.symbol << "\","
         << R"("start_time":)" << req.start_time_ms << ","
         << R"("end_time":)" << req.end_time_ms << ","
         << R"("interval":")" << req.interval << "\""
         << "}";

    std::string url = impl_->public_host;
    url += "/v1beta1/report/trades/stats";
    http::Request httpreq;
    httpreq.method = http::Method::POST;
    httpreq.url = url;
    httpreq.json_content();
    httpreq.set_body(body.str());

    Result<http::Response> resp = impl_->http_client.execute(httpreq);
    if (!resp.has_value()) return std::unexpected(resp.error());
    if (!resp->is_success()) {
        std::ostringstream errmsg;
        errmsg << "Polymarket US public POST /v1beta1/report/trades/stats "
               << "returned HTTP " << resp->status_code << ": " << resp->body;
        return std::unexpected(Error::network(errmsg.str()));
    }
    return resp->body;
}

Result<std::string> Client::get_health() {
    return impl_->public_get("/v1/health");
}

// ----- Authenticated Endpoints (api.polymarket.us) -----

Result<std::string> Client::place_order(const OrderRequest& request) {
    // Build the order JSON. Caller-side schema mirrors docs:
    //   marketSlug, intent, quantity, price, timeInForce, type
    std::ostringstream body;
    body << "{"
         << R"("marketSlug":")" << request.market_id << "\","
         << R"("intent":"ORDER_INTENT_BUY_LONG",)"  // TODO map side->intent
         << R"("type":"ORDER_TYPE_LIMIT",)"
         << R"("timeInForce":"TIME_IN_FORCE_GOOD_TILL_CANCEL",)"
         << R"("quantity":")" << request.size.to_string() << "\","
         << R"("price":")" << request.price.to_string() << "\""
         << "}";
    return impl_->authed_request(http::Method::POST, "/v1/orders", body.str());
}

Result<void> Client::cancel_order(std::string_view order_id) {
    std::string path = "/v1/order/";
    path.append(order_id);
    path.append("/cancel");
    Result<std::string> r =
        impl_->authed_request(http::Method::POST, path, "");
    if (!r.has_value()) return std::unexpected(r.error());
    return {};
}

Result<std::string> Client::get_orders(const OrderFilter& filter) {
    std::string path = "/v1/orders/open";
    if (filter.market_id) append_query(path, "marketSlug", *filter.market_id);
    if (filter.status) append_query(path, "status", *filter.status);
    if (filter.limit) append_query(path, "limit", *filter.limit);
    if (filter.cursor) append_query(path, "cursor", *filter.cursor);
    return impl_->authed_request(http::Method::GET, path, "");
}

Result<std::string> Client::get_positions() {
    return impl_->authed_request(http::Method::GET, "/v1/portfolio/positions",
                                 "");
}

Result<std::string> Client::get_balance() {
    return impl_->authed_request(http::Method::GET, "/v1/account/balances", "");
}

Result<std::string> Client::get_account() {
    return impl_->authed_request(http::Method::GET, "/v1/account", "");
}

}  // namespace polymarket::us
