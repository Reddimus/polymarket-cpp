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

#include <polymarket/us/client.hpp>

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

namespace pm = polymarket;
using json = nlohmann::json;

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

void emit_ok(const json &result) {
  json out = {{"ok", true}, {"result", result}};
  std::cout << out.dump() << '\n';
}

int emit_err(const std::string &code, const std::string &message) {
  json out = {{"ok", false}, {"error", {{"code", code}, {"message", message}}}};
  std::cout << out.dump() << '\n';
  return exit_code_for(code);
}

/// Result<string> from the SDK can hold either a parseable JSON body
/// or an opaque payload (e.g., POST /v1/order/.../cancel returns
/// empty). Wrap in {"raw_response": <body>} when not parseable.
json wrap_body(const std::string &body) {
  if (body.empty())
    return json::object();
  try {
    return json::parse(body);
  } catch (const std::exception &) {
    return json{{"raw_response", body}};
  }
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
    try {
      json j = json::parse(text);
      out.key_id = j.at("key_id").get<std::string>();
      out.secret_key = j.at("secret_key").get<std::string>();
      return true;
    } catch (const std::exception &e) {
      err_out = std::string("credentials JSON parse: ") + e.what();
      return false;
    }
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
  auto r = c.get_account();
  if (!r.has_value())
    return emit_err(classify_error(r.error().message()), r.error().message());
  emit_ok(wrap_body(*r));
  return 0;
}

int cmd_get_balance(pm::us::Client &c) {
  auto r = c.get_balance();
  if (!r.has_value())
    return emit_err(classify_error(r.error().message()), r.error().message());
  emit_ok(wrap_body(*r));
  return 0;
}

int cmd_get_positions(pm::us::Client &c) {
  auto r = c.get_positions();
  if (!r.has_value())
    return emit_err(classify_error(r.error().message()), r.error().message());
  emit_ok(wrap_body(*r));
  return 0;
}

int cmd_get_order(pm::us::Client &c, const json &params) {
  if (!params.contains("order_id"))
    return emit_err("bad_request", "get_order requires params.order_id");
  auto r = c.get_order_by_id(params.at("order_id").get<std::string>());
  if (!r.has_value())
    return emit_err(classify_error(r.error().message()), r.error().message());
  emit_ok(wrap_body(*r));
  return 0;
}

int cmd_get_orders(pm::us::Client &c, const json &params) {
  pm::us::OrderFilter f;
  if (params.contains("market_id"))
    f.market_id = params.at("market_id").get<std::string>();
  if (params.contains("status"))
    f.status = params.at("status").get<std::string>();
  if (params.contains("limit"))
    f.limit = params.at("limit").get<int>();
  if (params.contains("cursor"))
    f.cursor = params.at("cursor").get<std::string>();
  auto r = c.get_orders(f);
  if (!r.has_value())
    return emit_err(classify_error(r.error().message()), r.error().message());
  emit_ok(wrap_body(*r));
  return 0;
}

int cmd_cancel_order(pm::us::Client &c, const json &params) {
  if (!params.contains("order_id"))
    return emit_err("bad_request", "cancel_order requires params.order_id");
  auto r = c.cancel_order(params.at("order_id").get<std::string>());
  if (!r.has_value())
    return emit_err(classify_error(r.error().message()), r.error().message());
  emit_ok(json::object());
  return 0;
}

int cmd_place_order(pm::us::Client &c, const json &params) {
  pm::us::OrderRequest req;
  try {
    req.market_id = params.at("market_id").get<std::string>();
    req.side = params.at("side").get<std::string>();
    req.type = params.at("type").get<std::string>();
    // price and size accepted as strings (preferred — Decimal
    // preserves precision) or numbers (convenience).
    const auto pull_decimal = [](const json &j) -> pm::Decimal {
      if (j.is_string())
        return pm::Decimal::from_string(j.get<std::string>());
      if (j.is_number())
        return pm::Decimal::from_string(std::to_string(j.get<double>()));
      throw std::runtime_error("price/size must be string or number");
    };
    req.price = pull_decimal(params.at("price"));
    req.size = pull_decimal(params.at("size"));
    if (params.contains("post_only"))
      req.post_only = params.at("post_only").get<bool>();
    if (params.contains("expiration"))
      req.expiration = params.at("expiration").get<int64_t>();
  } catch (const std::exception &e) {
    return emit_err("bad_request",
                    std::string("place_order params: ") + e.what());
  }
  auto r = c.place_order(req);
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
  if (auto rc = client.set_credentials(creds); !rc.has_value())
    return emit_err("auth", rc.error().message());

  std::ostringstream buf;
  buf << std::cin.rdbuf();
  const std::string stdin_text = buf.str();
  if (stdin_text.empty())
    return emit_err("bad_request", "stdin is empty; expected JSON request");

  json req;
  try {
    req = json::parse(stdin_text);
  } catch (const std::exception &e) {
    return emit_err("bad_request",
                    std::string("stdin JSON parse: ") + e.what());
  }

  if (!req.contains("command") || !req.at("command").is_string())
    return emit_err("bad_request", "missing string field: command");
  const std::string cmd = req.at("command").get<std::string>();
  const json params = req.value("params", json::object());

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
