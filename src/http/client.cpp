#include "polymarket/http/client.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <curl/curl.h>
#include <limits>
#include <stdexcept>

namespace polymarket::http {
namespace {
struct Runtime {
	CURLcode status{CURLE_FAILED_INIT};
	Runtime() {
		const curl_version_info_data* info = curl_version_info(CURLVERSION_NOW);
		if (info && (info->features & CURL_VERSION_THREADSAFE))
			status = curl_global_init(CURL_GLOBAL_DEFAULT);
	}
	~Runtime() {
		if (status == CURLE_OK)
			curl_global_cleanup();
	}
};
Runtime& runtime() {
	static Runtime instance;
	return instance;
}

bool invalid_text(std::string_view text) {
	return text.find_first_of(std::string_view{"\r\n\0", 3}) != text.npos;
}
bool valid_header(std::string_view name, std::string_view value) {
	if (name.empty() || invalid_text(value))
		return false;
	for (const unsigned char c : name) {
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
			  std::string_view{"!#$%&'*+-.^_`|~"}.find(static_cast<char>(c)) !=
				  std::string_view::npos))
			return false;
	}
	return true;
}
std::string lower(std::string text) {
	for (char& c : text)
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return text;
}
struct Capture {
	Response response;
	std::size_t limit;
	std::size_t header_bytes{0};
	bool exceeded{false};
};
std::size_t write_body(char* data, std::size_t size, std::size_t count, void* context) noexcept {
	Capture& out = *static_cast<Capture*>(context);
	if (size && count > std::numeric_limits<std::size_t>::max() / size)
		return 0;
	const std::size_t length = size * count;
	if (out.limit &&
		(out.response.body.size() > out.limit || length > out.limit - out.response.body.size())) {
		out.exceeded = true;
		return 0;
	}
	try {
		out.response.body.append(data, length);
		return length;
	} catch (...) {
		return 0;
	}
}
std::size_t write_header(char* data, std::size_t size, std::size_t count, void* context) noexcept {
	Capture& out = *static_cast<Capture*>(context);
	if (size && count > std::numeric_limits<std::size_t>::max() / size)
		return 0;
	const std::size_t length = size * count;
	if (length > 65536 - out.header_bytes)
		return 0;
	out.header_bytes += length;
	try {
		std::string_view line(data, length);
		if (line.starts_with("HTTP/"))
			out.response.headers.clear();
		const std::size_t colon = line.find(':');
		if (colon != line.npos) {
			std::string_view value = line.substr(colon + 1);
			const std::size_t first = value.find_first_not_of(" \t");
			value = first == value.npos ? std::string_view{} : value.substr(first);
			const std::size_t last = value.find_last_not_of(" \t\r\n");
			value = last == value.npos ? std::string_view{} : value.substr(0, last + 1);
			out.response.headers[lower(std::string(line.substr(0, colon)))] = std::string(value);
		}
		return length;
	} catch (...) {
		return 0;
	}
}
long timeout(std::chrono::milliseconds value) {
	return static_cast<long>(
		std::clamp(value.count(), std::chrono::milliseconds::rep{1},
				   static_cast<std::chrono::milliseconds::rep>(std::numeric_limits<long>::max())));
}
} // namespace

QueryBuilder& QueryBuilder::add(std::string_view key, std::string_view value) {
	params_.emplace_back(key, value);
	return *this;
}
QueryBuilder& QueryBuilder::add(std::string_view key, int64_t value) {
	return add(key, std::to_string(value));
}
QueryBuilder& QueryBuilder::add(std::string_view key, double value) {
	if (!std::isfinite(value))
		throw std::invalid_argument("Query numbers must be finite");
	char buffer[64];
	const std::to_chars_result result = std::to_chars(buffer, buffer + sizeof(buffer), value);
	if (result.ec != std::errc{})
		throw std::invalid_argument("Invalid query number");
	return add(key, std::string_view(buffer, static_cast<std::size_t>(result.ptr - buffer)));
}
QueryBuilder& QueryBuilder::add_if(bool condition, std::string_view key, std::string_view value) {
	if (condition)
		add(key, value);
	return *this;
}
std::string QueryBuilder::build() const {
	std::string result;
	for (const auto& [key, value] : params_) {
		result += result.empty() ? '?' : '&';
		result += url_encode(key) + '=' + url_encode(value);
	}
	return result;
}
std::string url_encode(std::string_view input) {
	constexpr char hex[] = "0123456789ABCDEF";
	std::string result;
	for (const unsigned char c : input) {
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
			c == '-' || c == '_' || c == '.' || c == '~')
			result.push_back(static_cast<char>(c));
		else {
			result += '%';
			result += hex[c >> 4];
			result += hex[c & 15];
		}
	}
	return result;
}
std::string url_decode(std::string_view input) {
	const auto nibble = [](unsigned char c) -> int {
		if (c >= '0' && c <= '9')
			return c - '0';
		if (c >= 'a' && c <= 'f')
			return c - 'a' + 10;
		if (c >= 'A' && c <= 'F')
			return c - 'A' + 10;
		return -1;
	};
	std::string result;
	for (std::size_t i = 0; i < input.size(); ++i) {
		if (input[i] == '%' && input.size() - i >= 3) {
			const int high = nibble(static_cast<unsigned char>(input[i + 1]));
			const int low = nibble(static_cast<unsigned char>(input[i + 2]));
			if (high >= 0 && low >= 0) {
				result += static_cast<char>((high << 4) | low);
				i += 2;
				continue;
			}
		}
		result += input[i];
	}
	return result;
}
struct Client::Impl {
	CURL* curl{nullptr};
	Impl() {
		if (runtime().status == CURLE_OK)
			curl = curl_easy_init();
	}
	~Impl() {
		if (curl)
			curl_easy_cleanup(curl);
	}
};
void Client::global_init() {
	(void)runtime();
}
void Client::global_cleanup() {} // Automatic runtime lifetime; cannot invalidate active clients.
Client::Client(ClientConfig config) : impl_(std::make_unique<Impl>()), config_(std::move(config)) {}
Client::~Client() = default;
Client::Client(Client&&) noexcept = default;
Client& Client::operator=(Client&&) noexcept = default;

Result<Response> Client::execute(const Request& request) {
	if (!impl_ || !impl_->curl)
		return std::unexpected(Error::network("CURL not initialized"));
	if (invalid_text(request.url) || invalid_text(config_.user_agent) ||
		config_.default_timeout.count() <= 0 || config_.connect_timeout.count() <= 0 ||
		request.timeout.count() < 0 || config_.max_redirects < 0)
		return std::unexpected(
			Error::validation("Invalid URL, timeout, user agent, or redirect limit"));
	if (request.body.size() > static_cast<std::uintmax_t>(std::numeric_limits<curl_off_t>::max()))
		return std::unexpected(Error::validation("Request body is too large"));
	std::map<std::string, std::string> merged;
	for (const auto& [name, value] : default_headers_) {
		if (!valid_header(name, value))
			return std::unexpected(Error::validation("Invalid HTTP header"));
		merged[lower(name)] = value;
	}
	for (const auto& [name, value] : request.headers) {
		if (!valid_header(name, value))
			return std::unexpected(Error::validation("Invalid HTTP header"));
		merged[lower(name)] = value;
	}
	using HeaderList = std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)>;
	HeaderList headers(nullptr, &curl_slist_free_all);
	for (const auto& [name, value] : merged) {
		const std::string text = name + (value.empty() ? ";" : ": " + value);
		curl_slist* appended = curl_slist_append(headers.get(), text.c_str());
		if (!appended)
			return std::unexpected(Error::network("Could not allocate HTTP headers"));
		(void)headers.release();
		headers.reset(appended);
	}
	CURL* curl = impl_->curl;
	curl_easy_reset(curl);
	CURLcode option_error = CURLE_OK;
	const auto set = [&](CURLoption option, const auto& value) {
		if (option_error == CURLE_OK)
			option_error = curl_easy_setopt(curl, option, value);
	};
	Capture out{.limit = config_.max_response_bytes};
	set(CURLOPT_URL, request.url.c_str());
	set(CURLOPT_PROTOCOLS_STR, "http,https");
	set(CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
	set(CURLOPT_NOSIGNAL, 1L);
	set(CURLOPT_HTTPHEADER, headers.get());
	set(CURLOPT_TIMEOUT_MS,
		timeout(request.timeout.count() ? request.timeout : config_.default_timeout));
	set(CURLOPT_CONNECTTIMEOUT_MS, timeout(config_.connect_timeout));
	set(CURLOPT_TCP_NODELAY, config_.tcp_nodelay ? 1L : 0L);
	set(CURLOPT_TCP_KEEPALIVE, config_.tcp_keepalive ? 1L : 0L);
	set(CURLOPT_TCP_KEEPIDLE,
		static_cast<long>(
			std::clamp(config_.keepalive_interval.count(), std::chrono::seconds::rep{1},
					   static_cast<std::chrono::seconds::rep>(std::numeric_limits<long>::max()))));
	// Custom headers can contain credentials; do not forward them through redirects.
	set(CURLOPT_FOLLOWLOCATION, config_.follow_redirects && merged.empty() ? 1L : 0L);
	set(CURLOPT_MAXREDIRS, static_cast<long>(config_.max_redirects));
	if (config_.proxy_url) {
		if (invalid_text(*config_.proxy_url))
			return std::unexpected(Error::validation("Invalid proxy URL"));
		set(CURLOPT_PROXY, config_.proxy_url->c_str());
		if (config_.proxy_username)
			set(CURLOPT_PROXYUSERNAME, config_.proxy_username->c_str());
		if (config_.proxy_password)
			set(CURLOPT_PROXYPASSWORD, config_.proxy_password->c_str());
	}
	set(CURLOPT_USERAGENT, config_.user_agent.c_str());
	set(CURLOPT_ACCEPT_ENCODING, "");
	set(CURLOPT_WRITEFUNCTION, &write_body);
	set(CURLOPT_WRITEDATA, &out);
	set(CURLOPT_HEADERFUNCTION, &write_header);
	set(CURLOPT_HEADERDATA, &out);
	if (request.method == Method::POST)
		set(CURLOPT_POST, 1L);
	else if (request.method != Method::GET || !request.body.empty())
		set(CURLOPT_CUSTOMREQUEST, method_to_string(request.method).data());
	if (request.method != Method::GET || !request.body.empty()) {
		set(CURLOPT_POSTFIELDS, request.body.data());
		set(CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request.body.size()));
	}
	if (option_error != CURLE_OK)
		return std::unexpected(Error::network(curl_easy_strerror(option_error)));
	const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
	const CURLcode result = curl_easy_perform(curl);
	const std::chrono::nanoseconds elapsed = std::chrono::steady_clock::now() - start;
	++stats_.request_count;
	stats_.bytes_sent += request.body.size();
	stats_.bytes_received += out.response.body.size();
	stats_.total_time = elapsed > std::chrono::nanoseconds::max() - stats_.total_time
							? std::chrono::nanoseconds::max()
							: stats_.total_time + elapsed;
	if (out.exceeded)
		return std::unexpected(Error::validation("HTTP response exceeds configured size limit"));
	if (result == CURLE_OPERATION_TIMEDOUT)
		return std::unexpected(Error::timeout(curl_easy_strerror(result)));
	if (result != CURLE_OK)
		return std::unexpected(Error::network(curl_easy_strerror(result)));
	long status = 0;
	if (curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status) != CURLE_OK)
		return std::unexpected(Error::network("Cannot read HTTP status"));
	out.response.status_code = static_cast<int>(status);
	return std::move(out.response);
}
Result<Response> Client::get(std::string_view url) {
	Request request;
	request.url = url;
	return execute(request);
}
Result<Response> Client::post(std::string_view url, std::string_view body) {
	Request request;
	request.method = Method::POST;
	request.url = url;
	request.body = body;
	request.json_content();
	return execute(request);
}
Result<Response> Client::del(std::string_view url) {
	Request request;
	request.method = Method::DEL;
	request.url = url;
	return execute(request);
}
void Client::set_default_header(std::string_view name, std::string_view value) {
	default_headers_[std::string(name)] = value;
}
} // namespace polymarket::http
