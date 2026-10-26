#include <array>
#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <polymarket/clob/websocket.hpp>
#include <polymarket/crypto/ed25519.hpp>
#include <polymarket/us/ws/subscriber.hpp>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>

using namespace std::chrono_literals;

template <class Factory>
int exercise(Factory factory, std::string_view mode) {
	auto client = factory();
	if (mode == "move") {
		if (!client->connect())
			return 10;
		auto second = factory();
		if (!second->connect())
			return 11;
		*client = std::move(*second);
		if (!client->is_connected() || second->is_connected())
			return 12;
		client->disconnect();
	} else if (mode == "reconnect") {
		for (int i = 0; i < 3; ++i) {
			if (!client->connect())
				return 13;
			client->disconnect();
		}
	} else if (mode == "subscribe") {
		if (!client->connect())
			return 14;
		if constexpr (std::is_same_v<typename decltype(client)::element_type,
									 polymarket::us::ws::Subscriber>) {
			if (!client->subscribe_market_data({"quoted\"\\\nslug"}))
				return 15;
		} else {
			if (!client->subscribe_market_with_options({polymarket::uint256_t::from_u64(1)},
													   true) ||
				!client->subscribe_market({polymarket::uint256_t::from_u64(2)}) ||
				!client->unsubscribe_market({polymarket::uint256_t::from_u64(1)}) ||
				client->subscribe_user())
				return 18;
		}
		std::this_thread::sleep_for(100ms);
		client->disconnect();
	} else {
		std::atomic<int> messages = 0;
		std::atomic<bool> complete = false;
		client->on_message([&](const auto& message) {
			if constexpr (std::is_same_v<typename decltype(client)::element_type,
										 polymarket::us::ws::Subscriber>) {
				if (!message.starts_with('{') || !message.ends_with('}'))
					return;
			} else {
				const auto* trade = std::get_if<polymarket::clob::LastTradePriceMessage>(&message);
				if (!trade || trade->asset_id.to_string() != "18446744073709551616")
					return;
			}
			++messages;
			if (mode == "destroy")
				client.reset();
			complete.store(true, std::memory_order_release);
			if (mode == "throw")
				throw std::runtime_error("fixture callback exception");
		});
		(void)
			client->connect(); // Callback destruction can stop the connection during its handshake.
		const auto deadline = std::chrono::steady_clock::now() + 3s;
		while (!complete.load(std::memory_order_acquire) &&
			   std::chrono::steady_clock::now() < deadline)
			std::this_thread::sleep_for(5ms);
		if (!complete || messages != 1)
			return 16;
		if (client)
			client->disconnect();
		std::this_thread::sleep_for(100ms);
	}
	const auto key =
		polymarket::crypto::Ed25519PrivateKey::from_seed(std::array<std::uint8_t, 32>{});
	return key && key->sign(std::array<std::uint8_t, 1>{42}) ? 0 : 17;
}
int main(int argc, char** argv) {
	if (argc != 4)
		return 2;
	const int port = std::stoi(argv[2]);
	const std::string mode = argv[3];
	if (std::string_view(argv[1]) == "us") {
		return exercise(
			[&] {
				auto client = std::make_unique<polymarket::us::ws::Subscriber>();
				polymarket::us::ws::SubscriberConfig config;
				config.key_id = "fixture-key";
				config.secret_key = std::string(86, 'A') + "==";
				config.host = "127.0.0.1";
				config.port = port;
				config.use_tls = false;
				if (!client->configure(std::move(config)))
					throw std::runtime_error("fixture configure failed");
				return client;
			},
			mode);
	}
	if (mode == "user" || mode == "tick") {
		polymarket::clob::WsConfig config;
		config.url =
			"ws://127.0.0.1:" + std::to_string(port) + (mode == "user" ? "/ws/user" : "/ws/market");
		polymarket::clob::WebSocketClient client(config, {"quoted\"key", "AA==", "pass\\phrase"});
		std::atomic<int> events = 0, errors = 0, ticks = 0, raw = 0;
		client.on_error([&](const auto& error) {
			errors += error.message.find("Unsupported WebSocket tick size") != std::string::npos
						  ? 1
						  : 1000;
		});
		client.on_message([&](const auto& message) {
			if (const auto* user = std::get_if<polymarket::clob::UserEvent>(&message)) {
				if ((user->event_type == "order" || user->event_type == "trade") &&
					user->json.find("PLACEMENT") != std::string::npos)
					++events;
			}
			if (const auto* market = std::get_if<polymarket::clob::MarketEvent>(&message))
				if (market->event_type == "price_change" &&
					market->json.find("price_changes") != std::string::npos)
					++raw;
			if (std::get_if<polymarket::clob::TickSizeChange>(&message))
				++ticks;
		});
		if (!client.connect())
			return 19;
		if (mode == "user" && (!client.subscribe_user() ||
							   !client.subscribe_user({polymarket::Bytes32::from_hex("1")}) ||
							   !client.unsubscribe_user({polymarket::Bytes32::from_hex("1")}) ||
							   client.subscribe_market({polymarket::uint256_t::from_u64(1)})))
			return 20;
		const auto deadline = std::chrono::steady_clock::now() + 3s;
		while ((mode == "user" ? events.load() < 2 : errors.load() < 1) &&
			   std::chrono::steady_clock::now() < deadline)
			std::this_thread::sleep_for(5ms);
		std::this_thread::sleep_for(100ms);
		client.disconnect();
		return mode == "user" ? (events == 2 && errors == 0 ? 0 : 21)
							  : (errors == 1 && ticks == 0 && raw == 1 ? 0 : 22);
	}
	return exercise(
		[&] {
			polymarket::clob::WsConfig config;
			config.url = "ws://127.0.0.1:" + std::to_string(port) + "/ws/market";
			return std::make_unique<polymarket::clob::WebSocketClient>(config);
		},
		mode);
}
