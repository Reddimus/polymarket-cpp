#include <catch2/catch_test_macros.hpp>

#include "polymarket/data/client.hpp"
#include "polymarket/gamma/client.hpp"

TEST_CASE("All unsupported Gamma and Data methods link and fail explicitly", "[scope]") {
	polymarket::gamma::Client gamma;
	REQUIRE_FALSE(gamma.get_events());
	REQUIRE_FALSE(gamma.get_event("id"));
	REQUIRE_FALSE(gamma.get_event_by_slug("slug"));
	REQUIRE_FALSE(gamma.get_markets());
	REQUIRE_FALSE(gamma.get_market("id"));
	REQUIRE_FALSE(gamma.get_series());
	REQUIRE_FALSE(gamma.get_series_by_ticker("ticker"));
	REQUIRE_FALSE(gamma.get_tags());
	REQUIRE_FALSE(gamma.get_sports());
	REQUIRE_FALSE(gamma.search("query"));
	polymarket::data::Client data;
	REQUIRE_FALSE(data.get_positions("address"));
	REQUIRE_FALSE(data.get_activity("address"));
	REQUIRE_FALSE(data.get_trades());
	REQUIRE_FALSE(data.get_leaderboard());
}
