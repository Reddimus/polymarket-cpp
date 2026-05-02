/// @file us/ws/subscriber.cpp
/// @brief Implementation of polymarket::us::ws::Subscriber.
///
/// Cribs the libwebsockets boilerplate from kalshi-cpp's
/// `src/ws/websocket.cpp` (685 LOC, battle-tested) and adapts to
/// Polymarket's auth + subscribe shape:
///   - Ed25519 (seed||pub) instead of RSA-PSS
///   - X-PM-Access-Key/Timestamp/Signature headers
///   - {"subscribe":{"requestId":..., "subscriptionType":..., ...}} frame
///   - {"heartbeat":{}} ping frame

#include "polymarket/us/ws/subscriber.hpp"

#include "polymarket/crypto/ed25519.hpp"
#include "polymarket/crypto/hmac.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <iostream>
#include <libwebsockets.h>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

namespace polymarket::us::ws {

namespace {

/// Build the canonical signing message — same format as the REST
/// client (literal {timestamp}{method}{path}, no separators).
std::string canonical_message(std::string_view ts, std::string_view method,
                              std::string_view path) {
  std::string m;
  m.reserve(ts.size() + method.size() + path.size());
  m.append(ts);
  m.append(method);
  m.append(path);
  return m;
}

std::string now_unix_ms() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return std::to_string(
      std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

/// Build the SUBSCRIPTION_TYPE_MARKET_DATA frame. Hand-rolled JSON is
/// fine here — flat shape, escape-free strings (slugs are kebab-case
/// ASCII).
std::string build_subscribe_frame(const std::vector<std::string> &slugs,
                                  std::string_view request_id) {
  std::ostringstream o;
  o << R"({"subscribe":{"requestId":")" << request_id
    << R"(","subscriptionType":"SUBSCRIPTION_TYPE_MARKET_DATA","marketSlugs":[)";
  for (std::size_t i = 0; i < slugs.size(); ++i) {
    if (i)
      o << ',';
    o << '"' << slugs[i] << '"';
  }
  o << R"(],"responsesDebounced":true}})";
  return o.str();
}

constexpr const char *kHeartbeatFrame = R"({"heartbeat":{}})";
constexpr std::size_t kMaxFrameSize = 1 << 20; // 1 MiB

} // namespace

// Forward declaration so the C-style libwebsockets callback can take
// our struct as user data.
struct Impl;
static int subscriber_callback(struct lws *wsi,
                               enum lws_callback_reasons reason, void *user,
                               void *in, std::size_t len);

struct Subscriber::Impl {
  SubscriberConfig cfg;
  crypto::Ed25519PrivateKey signing_key;

  // Pre-computed handshake auth headers — the upgrade fires once at
  // connect time and we hand these to libwebsockets.
  std::string auth_ts;
  std::string auth_sig;

  // Connection state.
  struct lws_context *context = nullptr;
  struct lws *wsi = nullptr;
  std::thread service_thread;
  std::atomic<bool> connected{false};
  std::atomic<bool> should_stop{false};

  // Heartbeat tracking.
  std::chrono::steady_clock::time_point last_heartbeat{};

  // Outbound queue. lws_write must be called from the service thread
  // in response to a writable callback, so subscribe/heartbeat enqueue
  // here and the LWS_CALLBACK_CLIENT_WRITEABLE handler drains.
  std::mutex send_mutex;
  std::deque<std::string> send_queue;

  OnMessage message_cb;
  OnStateChange state_cb;

  // signing_key is set in configure(); this placeholder is a 32-byte
  // zero seed (a valid Ed25519 seed shape — the resulting key is
  // never used because configure() replaces it before connect()).
  Impl()
      : signing_key(
            crypto::Ed25519PrivateKey::from_seed(std::array<std::uint8_t, 32>{})
                .value()) {}

  void enqueue(std::string frame) {
    {
      std::lock_guard<std::mutex> g(send_mutex);
      send_queue.push_back(std::move(frame));
    }
    if (wsi)
      lws_callback_on_writable(wsi);
  }

  /// Compute fresh handshake-time auth (called on every connect so
  /// the timestamp stays inside Polymarket's ±30 s tolerance window).
  Result<void> refresh_auth() {
    auth_ts = now_unix_ms();
    const std::string msg = canonical_message(auth_ts, "GET", "/v1/ws/markets");
    auto sig = signing_key.sign(msg);
    if (!sig.has_value())
      return std::unexpected(sig.error());
    auth_sig = crypto::base64_encode(
        std::span<const std::uint8_t>{sig->data(), sig->size()});
    return {};
  }
};

// ─── lws callback ──────────────────────────────────────────────────

static int subscriber_callback(struct lws *wsi,
                               enum lws_callback_reasons reason, void *user,
                               void *in, std::size_t len) {
  auto *impl =
      static_cast<Subscriber::Impl *>(lws_context_user(lws_get_context(wsi)));
  (void)user;

  switch (reason) {
  case LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER: {
    auto **p = reinterpret_cast<unsigned char **>(in);
    auto *end = (*p) + len;
    auto add = [&](const char *name, const std::string &value) -> bool {
      return lws_add_http_header_by_name(
                 wsi, reinterpret_cast<const unsigned char *>(name),
                 reinterpret_cast<const unsigned char *>(value.c_str()),
                 static_cast<int>(value.size()), p, end) == 0;
    };
    if (!add("X-PM-Access-Key:", impl->cfg.key_id) ||
        !add("X-PM-Timestamp:", impl->auth_ts) ||
        !add("X-PM-Signature:", impl->auth_sig)) {
      std::cerr << "[ws] failed to add auth headers (buffer full)\n";
      return -1;
    }
    return 0;
  }

  case LWS_CALLBACK_CLIENT_ESTABLISHED: {
    impl->connected.store(true, std::memory_order_release);
    impl->last_heartbeat = std::chrono::steady_clock::now();
    if (impl->state_cb)
      impl->state_cb(true);
    return 0;
  }

  case LWS_CALLBACK_CLIENT_RECEIVE: {
    if (in && len) {
      const auto *bytes = static_cast<const char *>(in);
      if (impl->message_cb)
        impl->message_cb(std::string_view(bytes, len));
    }
    return 0;
  }

  case LWS_CALLBACK_CLIENT_WRITEABLE: {
    std::string frame;
    {
      std::lock_guard<std::mutex> g(impl->send_mutex);
      if (impl->send_queue.empty())
        return 0;
      frame = std::move(impl->send_queue.front());
      impl->send_queue.pop_front();
    }
    // libwebsockets requires LWS_PRE bytes of pre-padding for headers.
    std::vector<unsigned char> buf(LWS_PRE + frame.size());
    std::memcpy(buf.data() + LWS_PRE, frame.data(), frame.size());
    int written =
        lws_write(wsi, buf.data() + LWS_PRE, frame.size(), LWS_WRITE_TEXT);
    if (written < 0) {
      std::cerr << "[ws] lws_write failed: " << written << '\n';
      return -1;
    }
    // Re-arm if more queued.
    {
      std::lock_guard<std::mutex> g(impl->send_mutex);
      if (!impl->send_queue.empty())
        lws_callback_on_writable(wsi);
    }
    return 0;
  }

  case LWS_CALLBACK_CLIENT_CONNECTION_ERROR: {
    const char *err = in ? static_cast<const char *>(in) : "(no detail)";
    std::cerr << "[ws] connection error: " << std::string(err, len) << '\n';
    impl->connected.store(false, std::memory_order_release);
    if (impl->state_cb)
      impl->state_cb(false);
    return -1;
  }

  case LWS_CALLBACK_CLIENT_CLOSED: {
    impl->connected.store(false, std::memory_order_release);
    if (impl->state_cb)
      impl->state_cb(false);
    return 0;
  }

  default:
    return 0;
  }
}

static const struct lws_protocols kProtocols[] = {
    {"polymarket-us-ws", subscriber_callback, 0, kMaxFrameSize, 0, nullptr, 0},
    {nullptr, nullptr, 0, 0, 0, nullptr, 0}};

// ─── Subscriber ────────────────────────────────────────────────────

Subscriber::Subscriber() : impl_(std::make_unique<Impl>()) {}
Subscriber::~Subscriber() { disconnect(); }
Subscriber::Subscriber(Subscriber &&) noexcept = default;
Subscriber &Subscriber::operator=(Subscriber &&) noexcept = default;

Result<void> Subscriber::configure(SubscriberConfig cfg) {
  // Decode + validate the secret (same path as REST Client).
  auto decoded = crypto::base64_decode(cfg.secret_key);
  if (!decoded.has_value())
    return std::unexpected(decoded.error());
  if (decoded->size() != 64) {
    std::ostringstream m;
    m << "Polymarket US secret must decode to 64 bytes, got "
      << decoded->size();
    return std::unexpected(Error::validation(m.str()));
  }
  std::array<std::uint8_t, 32> seed{};
  std::memcpy(seed.data(), decoded->data(), 32);
  auto key = crypto::Ed25519PrivateKey::from_seed(seed);
  if (!key.has_value())
    return std::unexpected(key.error());
  impl_->signing_key = std::move(*key);
  impl_->cfg = std::move(cfg);
  return {};
}

Result<void> Subscriber::connect() {
  if (impl_->cfg.key_id.empty())
    return std::unexpected(
        Error::validation("call configure() before connect()"));

  if (auto rc = impl_->refresh_auth(); !rc.has_value())
    return std::unexpected(rc.error());

  struct lws_context_creation_info ctx_info {};
  std::memset(&ctx_info, 0, sizeof(ctx_info));
  ctx_info.port = CONTEXT_PORT_NO_LISTEN;
  ctx_info.protocols = kProtocols;
  ctx_info.user = impl_.get();
  ctx_info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;

  impl_->context = lws_create_context(&ctx_info);
  if (!impl_->context)
    return std::unexpected(Error::network("lws_create_context failed"));

  struct lws_client_connect_info ci {};
  std::memset(&ci, 0, sizeof(ci));
  ci.context = impl_->context;
  ci.address = impl_->cfg.host.c_str();
  ci.port = 443;
  ci.path = "/v1/ws/markets";
  ci.host = impl_->cfg.host.c_str();
  ci.origin = impl_->cfg.host.c_str();
  ci.protocol = kProtocols[0].name;
  ci.ssl_connection = LCCSCF_USE_SSL;

  impl_->wsi = lws_client_connect_via_info(&ci);
  if (!impl_->wsi) {
    lws_context_destroy(impl_->context);
    impl_->context = nullptr;
    return std::unexpected(
        Error::network("lws_client_connect_via_info failed"));
  }

  // Service thread: pumps lws + sends periodic heartbeats.
  impl_->should_stop.store(false, std::memory_order_release);
  impl_->service_thread = std::thread([impl = impl_.get()]() {
    while (!impl->should_stop.load(std::memory_order_acquire) &&
           impl->context) {
      lws_service(impl->context, 50);
      // Heartbeat tick (only after connect, only if cadence elapsed).
      if (impl->connected.load(std::memory_order_acquire)) {
        const auto now = std::chrono::steady_clock::now();
        if (now - impl->last_heartbeat >= impl->cfg.heartbeat) {
          impl->enqueue(kHeartbeatFrame);
          impl->last_heartbeat = now;
        }
      }
    }
  });

  // Wait briefly for the upgrade to complete.
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

void Subscriber::disconnect() {
  impl_->should_stop.store(true, std::memory_order_release);
  impl_->connected.store(false, std::memory_order_release);
  if (impl_->service_thread.joinable())
    impl_->service_thread.join();
  if (impl_->context) {
    lws_context_destroy(impl_->context);
    impl_->context = nullptr;
    impl_->wsi = nullptr;
  }
}

bool Subscriber::is_connected() const noexcept {
  return impl_->connected.load(std::memory_order_acquire);
}

Result<void> Subscriber::subscribe_market_data(
    const std::vector<std::string> &market_slugs) {
  if (!is_connected())
    return std::unexpected(Error::network("not connected"));
  if (market_slugs.empty())
    return std::unexpected(
        Error::validation("subscribe requires at least one market slug"));
  if (market_slugs.size() > 100)
    return std::unexpected(
        Error::validation("Polymarket caps a single subscription at 100 "
                          "markets; shard upstream"));

  // Use the timestamp as the request id — debugging-friendly and
  // unique-enough for our single-Subscriber-per-shard scope.
  const std::string request_id = "sub-" + now_unix_ms();
  impl_->enqueue(build_subscribe_frame(market_slugs, request_id));
  return {};
}

void Subscriber::on_message(OnMessage cb) { impl_->message_cb = std::move(cb); }
void Subscriber::on_state_change(OnStateChange cb) {
  impl_->state_cb = std::move(cb);
}

} // namespace polymarket::us::ws
