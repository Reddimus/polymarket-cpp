/// Data endpoints are unsupported and fail explicitly.

#include "polymarket/data/client.hpp"

namespace polymarket::data {

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

Result<std::string> Client::get_positions(std::string_view) {
	return std::unexpected(Error::network("Data API not yet implemented"));
}

Result<std::string> Client::get_activity(std::string_view) {
	return std::unexpected(Error::network("Data API not yet implemented"));
}

Result<std::string> Client::get_trades(const TradeFilter&) {
	return std::unexpected(Error::network("Data API not yet implemented"));
}

Result<std::string> Client::get_leaderboard() {
	return std::unexpected(Error::network("Data API not yet implemented"));
}

} // namespace polymarket::data
