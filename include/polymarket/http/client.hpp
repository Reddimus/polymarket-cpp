/// @file http/client.hpp
/// @brief HTTP client wrapper using libcurl

#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "polymarket/core/error.hpp"

namespace polymarket::http {

/// HTTP method.
///
/// Note: ``DEL`` (not ``DELETE``) is the enumerator for the wire
/// verb ``"DELETE"``. ``<windows.h>`` defines ``DELETE`` as a macro
/// for an access-rights constant (``0x00010000L``); any indirect
/// inclusion via libcurl/MSVC headers makes ``Method::DELETE``
/// expand to ``Method::0x00010000L`` and the build dies with
/// C2589/C2059. ``method_to_string`` still emits ``"DELETE"`` so
/// the wire protocol is unchanged.
enum class Method { GET, POST, PUT, DEL, PATCH };

/// Convert method to string
constexpr std::string_view method_to_string(Method method) {
	switch (method) {
		case Method::GET:
			return "GET";
		case Method::POST:
			return "POST";
		case Method::PUT:
			return "PUT";
		case Method::DEL:
			return "DELETE";
		case Method::PATCH:
			return "PATCH";
	}
	return "GET";
}

/// HTTP response
struct Response {
	int status_code{0};
	std::map<std::string, std::string> headers;
	std::string body;

	/// Check if response indicates success (2xx)
	[[nodiscard]] bool is_success() const { return status_code >= 200 && status_code < 300; }

	/// Check if response indicates client error (4xx)
	[[nodiscard]] bool is_client_error() const { return status_code >= 400 && status_code < 500; }

	/// Check if response indicates server error (5xx)
	[[nodiscard]] bool is_server_error() const { return status_code >= 500; }

	/// Check if response indicates rate limiting (429)
	[[nodiscard]] bool is_rate_limited() const { return status_code == 429; }
};

/// HTTP request configuration
struct Request {
	Method method{Method::GET};
	std::string url;
	std::map<std::string, std::string> headers;
	std::string body;
	std::chrono::milliseconds timeout{30000};

	/// Add a header
	Request& header(std::string_view name, std::string_view value) {
		headers[std::string(name)] = std::string(value);
		return *this;
	}

	/// Set JSON content type
	Request& json_content() {
		headers["Content-Type"] = "application/json";
		return *this;
	}

	/// Set the request body
	Request& set_body(std::string_view data) {
		body = std::string(data);
		return *this;
	}
};

/// URL query string builder
class QueryBuilder {
public:
	QueryBuilder& add(std::string_view key, std::string_view value);
	QueryBuilder& add(std::string_view key, int64_t value);
	QueryBuilder& add(std::string_view key, double value);
	QueryBuilder& add_if(bool condition, std::string_view key, std::string_view value);

	/// Build the query string (including leading '?')
	[[nodiscard]] std::string build() const;

	/// Check if empty
	[[nodiscard]] bool empty() const { return params_.empty(); }

private:
	std::vector<std::pair<std::string, std::string>> params_;
};

/// HTTP client configuration
struct ClientConfig {
	std::chrono::milliseconds default_timeout{30000};
	std::chrono::milliseconds connect_timeout{10000};
	bool tcp_nodelay{true};
	bool tcp_keepalive{true};
	std::chrono::seconds keepalive_interval{60};
	bool follow_redirects{true};
	int max_redirects{5};
	std::optional<std::string> proxy_url;
	std::optional<std::string> proxy_username;
	std::optional<std::string> proxy_password;
	std::string user_agent{"polymarket-cpp/0.1.0"};
};

/// HTTP client using libcurl
class Client {
public:
	/// Initialize the global curl context (call once at program start)
	static void global_init();

	/// Cleanup the global curl context (call once at program end)
	static void global_cleanup();

	/// Create a new HTTP client
	explicit Client(ClientConfig config = {});
	~Client();

	Client(const Client&) = delete;
	Client& operator=(const Client&) = delete;
	Client(Client&&) noexcept;
	Client& operator=(Client&&) noexcept;

	/// Execute an HTTP request
	[[nodiscard]] Result<Response> execute(const Request& request);

	/// Convenience methods
	[[nodiscard]] Result<Response> get(std::string_view url);
	[[nodiscard]] Result<Response> post(std::string_view url, std::string_view body);
	[[nodiscard]] Result<Response> del(std::string_view url);

	/// Get the underlying config
	[[nodiscard]] const ClientConfig& config() const { return config_; }

	/// Set a default header that will be sent with every request
	void set_default_header(std::string_view name, std::string_view value);

	/// Statistics
	struct Stats {
		uint64_t request_count{0};
		uint64_t bytes_sent{0};
		uint64_t bytes_received{0};
		std::chrono::nanoseconds total_time{0};
	};

	[[nodiscard]] const Stats& stats() const { return stats_; }

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
	ClientConfig config_;
	std::map<std::string, std::string> default_headers_;
	Stats stats_;
};

/// URL encode a string
[[nodiscard]] std::string url_encode(std::string_view input);

/// URL decode a string
[[nodiscard]] std::string url_decode(std::string_view input);

} // namespace polymarket::http
