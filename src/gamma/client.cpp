/// Gamma endpoints are unsupported; every declared method fails explicitly.

#include "polymarket/gamma/client.hpp"

namespace polymarket::gamma {

struct Client::Impl {
	std::string base_url;
	// Reserved for a future implementation.
};

Client::Client(std::string_view base_url) : impl_(std::make_unique<Impl>()) {
	impl_->base_url = std::string(base_url);
}

Client::~Client() = default;

Client::Client(Client&&) noexcept = default;
Client& Client::operator=(Client&&) noexcept = default;

Result<std::string> Client::get_events(const EventFilter&) {
	// Deferred — see file-level docstring.
	return std::unexpected(Error::network("Gamma API not yet implemented"));
}

Result<std::string> Client::get_event(std::string_view) {
	return std::unexpected(Error::network("Gamma API not yet implemented"));
}

Result<std::string> Client::get_markets(const MarketFilter&) {
	return std::unexpected(Error::network("Gamma API not yet implemented"));
}

Result<std::string> Client::get_market(std::string_view) {
	return std::unexpected(Error::network("Gamma API not yet implemented"));
}

Result<std::string> Client::search(std::string_view) {
	return std::unexpected(Error::network("Gamma API not yet implemented"));
}

Result<std::string> Client::get_event_by_slug(std::string_view) {
	return std::unexpected(Error::network("Gamma API not yet implemented"));
}
Result<std::string> Client::get_series() {
	return std::unexpected(Error::network("Gamma API not yet implemented"));
}
Result<std::string> Client::get_series_by_ticker(std::string_view) {
	return std::unexpected(Error::network("Gamma API not yet implemented"));
}
Result<std::string> Client::get_tags() {
	return std::unexpected(Error::network("Gamma API not yet implemented"));
}
Result<std::string> Client::get_sports() {
	return std::unexpected(Error::network("Gamma API not yet implemented"));
}

} // namespace polymarket::gamma
