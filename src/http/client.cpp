/// @file http_client.cpp
/// @brief HTTP client implementation using libcurl

#include "polymarket/http/client.hpp"

#include <curl/curl.h>

#include <cstring>
#include <sstream>

namespace polymarket::http {

// =========================================================================
// QueryBuilder Implementation
// =========================================================================

QueryBuilder &QueryBuilder::add(std::string_view key, std::string_view value) {
  params_.emplace_back(std::string(key), std::string(value));
  return *this;
}

QueryBuilder &QueryBuilder::add(std::string_view key, int64_t value) {
  return add(key, std::to_string(value));
}

QueryBuilder &QueryBuilder::add(std::string_view key, double value) {
  std::ostringstream oss;
  oss << value;
  return add(key, oss.str());
}

QueryBuilder &QueryBuilder::add_if(bool condition, std::string_view key,
                                   std::string_view value) {
  if (condition) {
    add(key, value);
  }
  return *this;
}

std::string QueryBuilder::build() const {
  if (params_.empty()) {
    return "";
  }

  std::string result = "?";
  bool first = true;
  for (const auto &[key, value] : params_) {
    if (!first) {
      result += '&';
    }
    first = false;
    result += url_encode(key);
    result += '=';
    result += url_encode(value);
  }
  return result;
}

// =========================================================================
// URL Encoding
// =========================================================================

std::string url_encode(std::string_view input) {
  CURL *curl = curl_easy_init();
  if (!curl) {
    // Fallback: return as-is
    return std::string(input);
  }

  char *encoded =
      curl_easy_escape(curl, input.data(), static_cast<int>(input.size()));
  std::string result;
  if (encoded) {
    result = encoded;
    curl_free(encoded);
  }
  curl_easy_cleanup(curl);
  return result;
}

std::string url_decode(std::string_view input) {
  CURL *curl = curl_easy_init();
  if (!curl) {
    return std::string(input);
  }

  int out_len = 0;
  char *decoded = curl_easy_unescape(curl, input.data(),
                                     static_cast<int>(input.size()), &out_len);
  std::string result;
  if (decoded) {
    result.assign(decoded, out_len);
    curl_free(decoded);
  }
  curl_easy_cleanup(curl);
  return result;
}

// =========================================================================
// Client Implementation
// =========================================================================

struct Client::Impl {
  CURL *curl{nullptr};
  struct curl_slist *headers{nullptr};

  Impl() { curl = curl_easy_init(); }

  ~Impl() {
    if (headers) {
      curl_slist_free_all(headers);
    }
    if (curl) {
      curl_easy_cleanup(curl);
    }
  }
};

void Client::global_init() { curl_global_init(CURL_GLOBAL_DEFAULT); }

void Client::global_cleanup() { curl_global_cleanup(); }

Client::Client(ClientConfig config)
    : impl_(std::make_unique<Impl>()), config_(std::move(config)) {}

Client::~Client() = default;

Client::Client(Client &&) noexcept = default;
Client &Client::operator=(Client &&) noexcept = default;

namespace {

// Write callback for curl
size_t write_callback(char *ptr, size_t size, size_t nmemb, void *userdata) {
  auto *response = static_cast<std::string *>(userdata);
  size_t total = size * nmemb;
  response->append(ptr, total);
  return total;
}

// Header callback for curl
size_t header_callback(char *buffer, size_t size, size_t nitems,
                       void *userdata) {
  auto *headers = static_cast<std::map<std::string, std::string> *>(userdata);
  size_t total = size * nitems;

  std::string line(buffer, total);
  // Trim trailing whitespace
  while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
    line.pop_back();
  }

  // Parse header
  auto colon = line.find(':');
  if (colon != std::string::npos) {
    std::string name = line.substr(0, colon);
    std::string value = line.substr(colon + 1);
    // Trim leading whitespace from value
    while (!value.empty() && value.front() == ' ') {
      value.erase(value.begin());
    }
    // Convert header name to lowercase for consistent access
    for (char &c : name) {
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    (*headers)[std::move(name)] = std::move(value);
  }

  return total;
}

} // anonymous namespace

Result<Response> Client::execute(const Request &request) {
  if (!impl_->curl) {
    return std::unexpected(Error::network("CURL not initialized"));
  }

  CURL *curl = impl_->curl;
  curl_easy_reset(curl);

  // Set URL
  curl_easy_setopt(curl, CURLOPT_URL, request.url.c_str());

  // Set method
  switch (request.method) {
  case Method::GET:
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    break;
  case Method::POST:
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    break;
  case Method::PUT:
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
    break;
  case Method::DELETE:
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    break;
  case Method::PATCH:
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PATCH");
    break;
  }

  // Set body
  if (!request.body.empty()) {
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                     static_cast<long>(request.body.size()));
  }

  // Build headers
  if (impl_->headers) {
    curl_slist_free_all(impl_->headers);
    impl_->headers = nullptr;
  }

  // Add default headers
  for (const auto &[name, value] : default_headers_) {
    std::string header = name + ": " + value;
    impl_->headers = curl_slist_append(impl_->headers, header.c_str());
  }

  // Add request headers
  for (const auto &[name, value] : request.headers) {
    std::string header = name + ": " + value;
    impl_->headers = curl_slist_append(impl_->headers, header.c_str());
  }

  if (impl_->headers) {
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, impl_->headers);
  }

  // Set timeouts
  auto timeout_ms = request.timeout.count() > 0
                        ? request.timeout.count()
                        : config_.default_timeout.count();
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout_ms));
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS,
                   static_cast<long>(config_.connect_timeout.count()));

  // Force IPv4 — Docker bridge networks resolve AAAA records but
  // route only IPv4. Without this, libcurl tries the IPv6 addresses
  // first, blocks on connect for the full timeout (or worse, spins
  // in a tight reconnect loop on certain libcurl/glibc combos),
  // never reaching the IPv4 fallback. Setting CURL_IPRESOLVE_V4 is
  // the same fix Docker recommends for in-container HTTPS clients.
  curl_easy_setopt(curl, CURLOPT_IPRESOLVE, (long)CURL_IPRESOLVE_V4);

  // TCP options
  if (config_.tcp_nodelay) {
    curl_easy_setopt(curl, CURLOPT_TCP_NODELAY, 1L);
  }
  if (config_.tcp_keepalive) {
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPIDLE,
                     static_cast<long>(config_.keepalive_interval.count()));
  }

  // Redirects
  if (config_.follow_redirects) {
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS,
                     static_cast<long>(config_.max_redirects));
  }

  // Proxy
  if (config_.proxy_url) {
    curl_easy_setopt(curl, CURLOPT_PROXY, config_.proxy_url->c_str());
    if (config_.proxy_username && config_.proxy_password) {
      std::string auth =
          *config_.proxy_username + ":" + *config_.proxy_password;
      curl_easy_setopt(curl, CURLOPT_PROXYUSERPWD, auth.c_str());
    }
  }

  // User agent
  curl_easy_setopt(curl, CURLOPT_USERAGENT, config_.user_agent.c_str());

  // Response handling
  Response response;
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response.headers);

  // Execute request
  auto start = std::chrono::steady_clock::now();
  CURLcode res = curl_easy_perform(curl);
  auto end = std::chrono::steady_clock::now();

  // Update stats
  stats_.request_count++;
  stats_.bytes_sent += request.body.size();
  stats_.bytes_received += response.body.size();
  stats_.total_time +=
      std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);

  if (res != CURLE_OK) {
    return std::unexpected(Error::network(curl_easy_strerror(res)));
  }

  // Get status code
  long status_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);
  response.status_code = static_cast<int>(status_code);

  return response;
}

Result<Response> Client::get(std::string_view url) {
  Request req;
  req.method = Method::GET;
  req.url = std::string(url);
  return execute(req);
}

Result<Response> Client::post(std::string_view url, std::string_view body) {
  Request req;
  req.method = Method::POST;
  req.url = std::string(url);
  req.body = std::string(body);
  req.json_content();
  return execute(req);
}

Result<Response> Client::del(std::string_view url) {
  Request req;
  req.method = Method::DELETE;
  req.url = std::string(url);
  return execute(req);
}

void Client::set_default_header(std::string_view name, std::string_view value) {
  default_headers_[std::string(name)] = std::string(value);
}

} // namespace polymarket::http
