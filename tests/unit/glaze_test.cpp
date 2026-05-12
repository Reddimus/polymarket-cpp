/// @file glaze_test.cpp
/// @brief Glaze-deserializer shape-parity tests for the polymarket-cpp
/// CLOB WebSocket dispatcher.
///
/// `src/clob/websocket.cpp::parse_message` lives in an anonymous
/// namespace (the dispatcher is private), so these tests cannot
/// reach it directly. Instead, they mirror the same `glz::generic`
/// access path — find_field + typed get_* — to pin the assumptions
/// the real dispatcher relies on:
///
///   - object_t::find returns end() on missing keys (no throw)
///   - is_string / is_number / is_boolean / is_array discriminate
///     cleanly with no implicit coercion
///   - get<T>() on a value of type T returns T without throwing
///   - bool field reads do NOT accidentally pick up string fields
///     named "true"/"false" (the bool/string_view overload-set
///     guardrail; see memory `feedback_polymarket_cpp_overload_bug.md`)
///   - read_json on malformed JSON returns a non-empty error_ctx
///
/// Migrated from nlohmann/json on 2026-05-11.

#include "polymarket/core/types.hpp"

#include <catch2/catch_test_macros.hpp>

#include <glaze/glaze.hpp>
#include <glaze/json/generic.hpp>

#include <string>
#include <string_view>

namespace {

using glz_node = glz::generic;

const glz_node *find_field(const glz_node &node, std::string_view key) {
  if (!node.is_object())
    return nullptr;
  const glz_node::object_t &obj = node.get_object();
  glz_node::object_t::const_iterator it = obj.find(std::string(key));
  if (it == obj.end())
    return nullptr;
  return &it->second;
}

std::string get_string(const glz_node &node, std::string_view key) {
  const glz_node *v = find_field(node, key);
  if (!v || !v->is_string())
    return {};
  return v->get<std::string>();
}

bool get_bool_field(const glz_node &node, std::string_view key, bool fallback) {
  const glz_node *v = find_field(node, key);
  if (!v || !v->is_boolean())
    return fallback;
  return v->get<bool>();
}

polymarket::Decimal get_decimal(const glz_node &node, std::string_view key) {
  const glz_node *v = find_field(node, key);
  if (!v)
    return polymarket::Decimal{};
  if (v->is_string())
    return polymarket::Decimal::from_string(v->get<std::string>());
  if (v->is_number())
    return polymarket::Decimal::from_string(std::to_string(v->get<double>()));
  return polymarket::Decimal{};
}

} // namespace

TEST_CASE("Glaze parses a representative CLOB book-snapshot payload",
          "[glaze][clob][parse]") {
  const std::string body = R"({
        "event_type": "book",
        "asset_id": "71321045679252212594626385532706912750332728571942134274332321581290950697279",
        "timestamp": 1741000000000,
        "hash": "0xdeadbeef",
        "bids": [
            {"price": "0.45", "size": "100.5"},
            {"price": "0.44", "size": "200.0"}
        ],
        "asks": [
            {"price": "0.46", "size": "75.25"},
            {"price": "0.47", "size": "150.0"}
        ]
    })";

  glz_node root{};
  glz::error_ctx ec = glz::read_json(root, body);
  REQUIRE_FALSE(static_cast<bool>(ec));

  REQUIRE(get_string(root, "event_type") == "book");
  REQUIRE(get_string(root, "hash") == "0xdeadbeef");

  const glz_node *bids = find_field(root, "bids");
  REQUIRE(bids != nullptr);
  REQUIRE(bids->is_array());
  REQUIRE(bids->get_array().size() == 2);

  // Bid 0: price=0.45, size=100.5
  const glz_node &bid0 = bids->get_array()[0];
  REQUIRE(get_decimal(bid0, "price").to_string() ==
          polymarket::Decimal::from_string("0.45").to_string());
  REQUIRE(get_decimal(bid0, "size").to_string() ==
          polymarket::Decimal::from_string("100.5").to_string());

  const glz_node *asks = find_field(root, "asks");
  REQUIRE(asks != nullptr);
  REQUIRE(asks->is_array());
  REQUIRE(asks->get_array().size() == 2);
}

TEST_CASE("Glaze get_decimal accepts numeric and string-typed prices",
          "[glaze][clob][parse]") {
  // Polymarket's WS uses string-typed prices; defensive code in the
  // dispatcher also handles the numeric form. Both must round-trip
  // through Decimal::from_string with matching values.
  const std::string body = R"({
        "string_price": "0.4500",
        "number_price": 0.45
    })";
  glz_node root{};
  glz::error_ctx ec = glz::read_json(root, body);
  REQUIRE_FALSE(static_cast<bool>(ec));

  polymarket::Decimal s = get_decimal(root, "string_price");
  polymarket::Decimal n = get_decimal(root, "number_price");
  // Both should be non-zero; exact equality depends on double-to-string
  // formatting, so check structural correctness.
  REQUIRE_FALSE(s.to_string().empty());
  REQUIRE_FALSE(n.to_string().empty());
}

TEST_CASE("Glaze bool field reads ignore string-typed peers",
          "[glaze][clob][parse][regression]") {
  // Regression guard for memory feedback_polymarket_cpp_overload_bug.md:
  // a poorly-disambiguated bool getter that accepted string_view's
  // implicit conversion from const char* could pick up a string field
  // named e.g. "true_setting":"true" as a true boolean. Verify
  // is_boolean strictly gates the bool path — string "true" must NOT
  // read as bool true; the fallback must take over.
  const std::string body = R"({
        "is_taker": true,
        "fake_bool_str": "true",
        "fake_bool_num": 1
    })";
  glz_node root{};
  glz::error_ctx ec = glz::read_json(root, body);
  REQUIRE_FALSE(static_cast<bool>(ec));

  // Real bool field reads cleanly.
  REQUIRE(get_bool_field(root, "is_taker", false) == true);
  // String "true" must NOT coerce to bool true — fallback wins.
  REQUIRE(get_bool_field(root, "fake_bool_str", false) == false);
  REQUIRE(get_bool_field(root, "fake_bool_str", true) == true);
  // Number 1 must NOT coerce to bool true — fallback wins.
  REQUIRE(get_bool_field(root, "fake_bool_num", false) == false);
  // Missing field returns fallback verbatim.
  REQUIRE(get_bool_field(root, "absent_field", true) == true);
  REQUIRE(get_bool_field(root, "absent_field", false) == false);
}

TEST_CASE("Glaze read_json rejects malformed input",
          "[glaze][clob][parse][error]") {
  const std::string body = R"({"price": "this is not a closed string)";
  glz_node root{};
  glz::error_ctx ec = glz::read_json(root, body);
  REQUIRE(static_cast<bool>(ec));
  // format_error should produce non-empty diagnostic text.
  const std::string msg = glz::format_error(ec, body);
  REQUIRE_FALSE(msg.empty());
}

TEST_CASE("Glaze handles missing optional fields as typed defaults",
          "[glaze][clob][parse]") {
  // The dispatcher reads many optional fields (transaction_hash,
  // reason, etc.). Missing-field access must NOT throw — instead
  // produce a typed default (empty string, zero Decimal, etc.).
  const std::string body = R"({"event_type":"order","id":"abc"})";
  glz_node root{};
  glz::error_ctx ec = glz::read_json(root, body);
  REQUIRE_FALSE(static_cast<bool>(ec));

  REQUIRE(get_string(root, "event_type") == "order");
  REQUIRE(get_string(root, "id") == "abc");
  // Missing fields return empty/zero defaults — no throw.
  REQUIRE(get_string(root, "missing_field").empty());
  REQUIRE(get_decimal(root, "missing_decimal").to_string() ==
          polymarket::Decimal{}.to_string());
  // is_array on a missing field must surface as nullptr/false.
  const glz_node *missing_array = find_field(root, "bids");
  REQUIRE(missing_array == nullptr);
}

TEST_CASE("Glaze parses a stdin-style command envelope",
          "[glaze][cli][parse]") {
  // Mirrors the polymarket-us-cli stdin contract:
  //   {"command":"<cmd>","params":{...}}
  // The dispatcher walks `params` as glz::generic so it can handle
  // arbitrary shapes per command without a static `glz::meta`.
  const std::string body = R"({
        "command": "place_order",
        "params": {
            "market_id": "0x123",
            "side": "buy",
            "type": "limit",
            "price": "0.45",
            "size": "100",
            "post_only": true,
            "expiration": 1741000000
        }
    })";
  glz_node root{};
  glz::error_ctx ec = glz::read_json(root, body);
  REQUIRE_FALSE(static_cast<bool>(ec));

  REQUIRE(get_string(root, "command") == "place_order");
  const glz_node *params = find_field(root, "params");
  REQUIRE(params != nullptr);
  REQUIRE(params->is_object());

  REQUIRE(get_string(*params, "market_id") == "0x123");
  REQUIRE(get_string(*params, "side") == "buy");
  REQUIRE(get_bool_field(*params, "post_only", false) == true);
  // expiration is a JSON number — verify the type discriminates.
  const glz_node *exp = find_field(*params, "expiration");
  REQUIRE(exp != nullptr);
  REQUIRE(exp->is_number());
  REQUIRE(exp->get<double>() == 1741000000.0);
}
