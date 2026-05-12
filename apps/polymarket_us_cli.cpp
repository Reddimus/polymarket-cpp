/// @file polymarket_us_cli.cpp
/// @brief JSON-stdin/JSON-stdout CLI wrapper around pm::us::Client.
///
/// Concentrates Polymarket-US protocol risk in C++ so downstream
/// Python services (polymarket-trader's LiveExecutor) can stay
/// crypto-and-HTTP-free. One subprocess invocation = one operation.
///
/// Usage
/// -----
///   polymarket-us-cli --credentials <path> [--api-base-url <url>]
///   polymarket-us-cli --version
///
/// The credential file holds a JSON object:
/// {"key_id":"...","secret_key":"..."}. Reading credentials from a file (rather
/// than env) avoids leaking the secret into `docker compose config` rendered
/// output (see memory feedback_compose_config_leaks_secrets.md).
///
/// Wire protocol
/// -------------
/// stdin  (one line / one JSON document):
///   {"command":"<cmd>","params":{...}}
/// stdout (one JSON document, ASCII-clean, trailing newline):
///   {"ok":true,"result":{...}}
/// or
///   {"ok":false,"error":{"code":"<cat>","message":"<text>"}}
///
/// Commands: get_account | get_balance | place_order | cancel_order |
///           get_order | get_orders | get_positions
///
/// Exit codes (intended for shell-friendly diagnosis; structured
/// error info is on stdout regardless):
///   0  success
///   1  unknown / unhandled error
///   2  bad credentials (file missing / parse / validation)
///   3  malformed stdin JSON or unknown command
///   4  network / transport error
///   5  Polymarket API rejected the request (non-2xx)
///
/// JSON: Glaze v7.6.0 (migrated from nlohmann/json v3.11.3 on
/// 2026-05-11). Inputs and outputs are dynamic-shaped (different
/// commands need different params; results echo whatever Polymarket
/// returns) so we use `glz::generic` for both — it preserves arbitrary
/// nesting without needing a static `glz::meta` per command.

#include <polymarket/us/client.hpp>

#include <glaze/glaze.hpp>
#include <glaze/json/generic.hpp>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

namespace pm = polymarket;
using glz_node = glz::generic;

namespace {

constexpr std::string_view kVersion = "0.2.0";

struct Args {
  std::string credentials_path;
  std::string api_base_url; ///< Optional override; empty → kAuthedHost
  bool show_version = false;
  std::string parse_error;
};

Args parse_args(int argc, char **argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    std::string_view tok{argv[i]};
    if (tok == "--version" || tok == "-v") {
      a.show_version = true;
    } else if (tok == "--credentials" && i + 1 < argc) {
      a.credentials_path = argv[++i];
    } else if (tok == "--api-base-url" && i + 1 < argc) {
      a.api_base_url = argv[++i];
    } else {
      a.parse_error = "unrecognized argument: ";
      a.parse_error.append(tok);
      return a;
    }
  }
  return a;
}

/// Categorize a polymarket::Error so the Python side can decide
/// retry-vs-not without parsing free-text. The classification is
/// best-effort string matching; clearer error categories on the
/// underlying Result type would be a future improvement.
std::string classify_error(const std::string &message) {
  if (message.find("credentials") != std::string::npos)
    return "auth";
  if (message.find("HTTP 401") != std::string::npos ||
      message.find("HTTP 403") != std::string::npos)
    return "auth";
  if (message.find("HTTP 4") != std::string::npos)
    return "bad_request";
  if (message.find("HTTP 5") != std::string::npos)
    return "transport";
  if (message.find("timeout") != std::string::npos ||
      message.find("CURLE_") != std::string::npos)
    return "transport";
  return "unknown";
}

int exit_code_for(const std::string &category) {
  if (category == "auth")
    return 2;
  if (category == "bad_request")
    return 5;
  if (category == "transport")
    return 4;
  return 1;
}

// ---- glz::generic helpers --------------------------------------------------
//
// Polymarket-US endpoints emit arbitrary JSON shapes (positions,
// account, orderbook all differ). We carry them through this CLI as
// `glz::generic` so the dispatch is shape-free.

const glz_node *find_field(const glz_node &node, std::string_view key) {
  if (!node.is_object())
    return nullptr;
  const glz_node::object_t &obj = node.get_object();
  glz_node::object_t::const_iterator it = obj.find(std::string(key));
  if (it == obj.end())
    return nullptr;
  return &it->second;
}

std::string require_string(const glz_node &node, std::string_view key,
                           std::string &err_out) {
  const glz_node *v = find_field(node, key);
  if (!v || !v->is_string()) {
    err_out = "missing or non-string field: ";
    err_out.append(key);
    return {};
  }
  return v->get<std::string>();
}

std::string string_or_empty(const glz_node &node, std::string_view key) {
  const glz_node *v = find_field(node, key);
  if (!v || !v->is_string())
    return {};
  return v->get<std::string>();
}

std::optional<std::string> opt_string(const glz_node &node,
                                      std::string_view key) {
  const glz_node *v = find_field(node, key);
  if (!v || !v->is_string())
    return std::nullopt;
  return v->get<std::string>();
}

std::optional<int> opt_int(const glz_node &node, std::string_view key) {
  const glz_node *v = find_field(node, key);
  if (!v || !v->is_number())
    return std::nullopt;
  return static_cast<int>(v->get<double>());
}

std::optional<bool> opt_bool(const glz_node &node, std::string_view key) {
  const glz_node *v = find_field(node, key);
  if (!v || !v->is_boolean())
    return std::nullopt;
  return v->get<bool>();
}

std::optional<std::int64_t> opt_i64(const glz_node &node,
                                    std::string_view key) {
  const glz_node *v = find_field(node, key);
  if (!v || !v->is_number())
    return std::nullopt;
  return static_cast<std::int64_t>(v->get<double>());
}

/// Pull a Decimal from a generic node. Accepts string (preferred —
/// preserves precision) or number (convenience). Throws via `err_out`
/// on a type mismatch so the dispatcher can produce a structured
/// bad_request response.
pm::Decimal pull_decimal(const glz_node &node, std::string_view key,
                         std::string &err_out) {
  const glz_node *v = find_field(node, key);
  if (!v) {
    err_out = "missing field: ";
    err_out.append(key);
    return pm::Decimal{};
  }
  if (v->is_string())
    return pm::Decimal::from_string(v->get<std::string>());
  if (v->is_number())
    return pm::Decimal::from_string(std::to_string(v->get<double>()));
  err_out = "field ";
  err_out.append(key);
  err_out.append(" must be string or number");
  return pm::Decimal{};
}

/// Render a glz_node back to a JSON string. Used for both stdout
/// envelopes and pass-through of Polymarket response bodies.
///
/// Note: we build envelopes via `operator[]` and
/// `operator=(const object_t&)` rather than `glz_node{object_t{...}}`.
/// The variant-converting constructor selects the underlying variant
/// alternative but `glz::write_json` then renders it through the
/// generic variant write path, which produces `[index, value]` rather
/// than `{...}`. Initializer-list construction and per-key assignment
/// route through `data.emplace<object_t>()` and serialize as plain
/// JSON. Caught during migration testing 2026-05-11.
std::string dump_json(const glz_node &node) {
  std::string out;
  glz::error_ctx ec = glz::write_json(node, out);
  // Glaze writing a generic node should not fail in practice; if it
  // somehow does, fall back to an explicit error literal so the
  // envelope stays valid JSON.
  if (ec)
    return R"({"error":"glaze write failed"})";
  return out;
}

void emit_ok(const glz_node &result) {
  glz_node envelope;
  envelope["ok"] = true;
  envelope["result"] = result;
  std::cout << dump_json(envelope) << '\n';
}

int emit_err(const std::string &code, const std::string &message) {
  glz_node envelope;
  envelope["ok"] = false;
  glz_node err;
  err["code"] = code;
  err["message"] = message;
  envelope["error"] = err;
  std::cout << dump_json(envelope) << '\n';
  return exit_code_for(code);
}

/// Result<string> from the SDK can hold either a parseable JSON body
/// or an opaque payload (e.g., POST /v1/order/.../cancel returns
/// empty). Wrap in {"raw_response": <body>} when not parseable.
glz_node wrap_body(const std::string &body) {
  glz_node out;
  if (body.empty()) {
    // Force the variant onto object_t via operator=, not via the
    // variant-converting constructor (which would write back as
    // [idx, value]).
    out = glz_node::object_t{};
    return out;
  }
  glz_node parsed{};
  glz::error_ctx ec = glz::read_json(parsed, body);
  if (ec) {
    out["raw_response"] = body;
    return out;
  }
  return parsed;
}

std::string load_text_file(const std::string &path, std::string &err_out) {
  std::ifstream in(path);
  if (!in) {
    err_out = "cannot read credentials file: ";
    err_out.append(path);
    return {};
  }
  std::ostringstream buf;
  buf << in.rdbuf();
  std::string s = buf.str();
  while (!s.empty() &&
         (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
    s.pop_back();
  return s;
}

bool load_credentials(const std::string &path, pm::us::Credentials &out,
                      std::string &err_out) {
  std::string text = load_text_file(path, err_out);
  if (!err_out.empty())
    return false;
  if (text.empty()) {
    err_out = "credentials file is empty";
    return false;
  }
  // Two accepted formats:
  //   1) JSON: {"key_id":"...","secret_key":"..."}
  //   2) Plain "key_id:secret_key" (single line) — convenience for
  //      operator paste-tests; real prod uses the JSON form.
  if (text.front() == '{') {
    glz_node parsed{};
    glz::error_ctx ec = glz::read_json(parsed, text);
    if (ec) {
      err_out = "credentials JSON parse: ";
      err_out.append(glz::format_error(ec, text));
      return false;
    }
    std::string field_err;
    out.key_id = require_string(parsed, "key_id", field_err);
    if (!field_err.empty()) {
      err_out = "credentials JSON: " + field_err;
      return false;
    }
    out.secret_key = require_string(parsed, "secret_key", field_err);
    if (!field_err.empty()) {
      err_out = "credentials JSON: " + field_err;
      return false;
    }
    return true;
  }
  const auto colon = text.find(':');
  if (colon == std::string::npos) {
    err_out = "credentials file must be JSON {key_id, secret_key} or "
              "'key_id:secret_key' single line";
    return false;
  }
  out.key_id = text.substr(0, colon);
  out.secret_key = text.substr(colon + 1);
  return true;
}

// ----- Command dispatchers ---------------------------------------------------

int cmd_get_account(pm::us::Client &c) {
  pm::Result<std::string> r = c.get_account();
  if (!r.has_value())
    return emit_err(classify_error(r.error().message()), r.error().message());
  emit_ok(wrap_body(*r));
  return 0;
}

int cmd_get_balance(pm::us::Client &c) {
  pm::Result<std::string> r = c.get_balance();
  if (!r.has_value())
    return emit_err(classify_error(r.error().message()), r.error().message());
  emit_ok(wrap_body(*r));
  return 0;
}

int cmd_get_positions(pm::us::Client &c) {
  pm::Result<std::string> r = c.get_positions();
  if (!r.has_value())
    return emit_err(classify_error(r.error().message()), r.error().message());
  emit_ok(wrap_body(*r));
  return 0;
}

int cmd_get_order(pm::us::Client &c, const glz_node &params) {
  std::optional<std::string> order_id = opt_string(params, "order_id");
  if (!order_id)
    return emit_err("bad_request", "get_order requires params.order_id");
  pm::Result<std::string> r = c.get_order_by_id(*order_id);
  if (!r.has_value())
    return emit_err(classify_error(r.error().message()), r.error().message());
  emit_ok(wrap_body(*r));
  return 0;
}

int cmd_get_orders(pm::us::Client &c, const glz_node &params) {
  pm::us::OrderFilter f;
  if (auto v = opt_string(params, "market_id"); v.has_value())
    f.market_id = *v;
  if (auto v = opt_string(params, "status"); v.has_value())
    f.status = *v;
  if (auto v = opt_int(params, "limit"); v.has_value())
    f.limit = *v;
  if (auto v = opt_string(params, "cursor"); v.has_value())
    f.cursor = *v;
  pm::Result<std::string> r = c.get_orders(f);
  if (!r.has_value())
    return emit_err(classify_error(r.error().message()), r.error().message());
  emit_ok(wrap_body(*r));
  return 0;
}

int cmd_cancel_order(pm::us::Client &c, const glz_node &params) {
  std::optional<std::string> order_id = opt_string(params, "order_id");
  if (!order_id)
    return emit_err("bad_request", "cancel_order requires params.order_id");
  pm::Result<void> r = c.cancel_order(*order_id);
  if (!r.has_value())
    return emit_err(classify_error(r.error().message()), r.error().message());
  glz_node empty;
  empty = glz_node::object_t{}; // operator=(const object_t&), NOT variant-ctor
  emit_ok(empty);
  return 0;
}

int cmd_place_order(pm::us::Client &c, const glz_node &params) {
  pm::us::OrderRequest req;
  std::string field_err;

  req.market_id = require_string(params, "market_id", field_err);
  if (!field_err.empty())
    return emit_err("bad_request", "place_order params: " + field_err);
  req.side = require_string(params, "side", field_err);
  if (!field_err.empty())
    return emit_err("bad_request", "place_order params: " + field_err);
  req.type = require_string(params, "type", field_err);
  if (!field_err.empty())
    return emit_err("bad_request", "place_order params: " + field_err);

  req.price = pull_decimal(params, "price", field_err);
  if (!field_err.empty())
    return emit_err("bad_request", "place_order params: " + field_err);
  req.size = pull_decimal(params, "size", field_err);
  if (!field_err.empty())
    return emit_err("bad_request", "place_order params: " + field_err);

  if (auto v = opt_bool(params, "post_only"); v.has_value())
    req.post_only = *v;
  if (auto v = opt_i64(params, "expiration"); v.has_value())
    req.expiration = *v;

  pm::Result<std::string> r = c.place_order(req);
  if (!r.has_value())
    return emit_err(classify_error(r.error().message()), r.error().message());
  emit_ok(wrap_body(*r));
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  Args args = parse_args(argc, argv);
  if (!args.parse_error.empty()) {
    emit_err("bad_request", args.parse_error);
    return 3;
  }
  if (args.show_version) {
    std::cout << "polymarket-us-cli " << kVersion << '\n';
    return 0;
  }
  if (args.credentials_path.empty()) {
    emit_err("bad_request", "--credentials <path> is required");
    return 3;
  }

  pm::us::Credentials creds;
  {
    std::string err;
    if (!load_credentials(args.credentials_path, creds, err))
      return emit_err("auth", err);
  }

  pm::us::Client client(args.api_base_url.empty()
                            ? pm::us::Client::kAuthedHost
                            : std::string_view{args.api_base_url});
  if (pm::Result<void> rc = client.set_credentials(creds); !rc.has_value())
    return emit_err("auth", rc.error().message());

  std::ostringstream buf;
  buf << std::cin.rdbuf();
  const std::string stdin_text = buf.str();
  if (stdin_text.empty())
    return emit_err("bad_request", "stdin is empty; expected JSON request");

  glz_node req{};
  glz::error_ctx ec = glz::read_json(req, stdin_text);
  if (ec)
    return emit_err("bad_request", std::string("stdin JSON parse: ") +
                                       glz::format_error(ec, stdin_text));

  std::string cmd_err;
  const std::string cmd = require_string(req, "command", cmd_err);
  if (!cmd_err.empty())
    return emit_err("bad_request", "missing string field: command");

  // params is optional; default to an empty object if missing.
  // Build the empty default via operator=(const object_t&) so the
  // type is object_t (the variant-converting constructor would store
  // the same value but downstream is_object()/operator[] checks would
  // still work — the bug only bites on write, but using the explicit
  // assignment path keeps the construction style consistent).
  const glz_node *params_ptr = find_field(req, "params");
  static const glz_node kEmptyParams = []() {
    glz_node n;
    n = glz_node::object_t{};
    return n;
  }();
  const glz_node &params =
      (params_ptr && params_ptr->is_object()) ? *params_ptr : kEmptyParams;

  if (cmd == "get_account")
    return cmd_get_account(client);
  if (cmd == "get_balance")
    return cmd_get_balance(client);
  if (cmd == "get_positions")
    return cmd_get_positions(client);
  if (cmd == "get_order")
    return cmd_get_order(client, params);
  if (cmd == "get_orders")
    return cmd_get_orders(client, params);
  if (cmd == "cancel_order")
    return cmd_cancel_order(client, params);
  if (cmd == "place_order")
    return cmd_place_order(client, params);

  return emit_err("bad_request", std::string("unknown command: ") + cmd);
}
