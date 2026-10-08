#include "od/ticker_parser.hpp"

#include <simdjson.h>

namespace od::deribit {

namespace od_json = simdjson::ondemand;

struct TickerParser::Impl {
    od_json::parser parser;
    // simdjson reads in SIMD-width blocks and needs SIMDJSON_PADDING readable bytes
    // past the end of the input; the WebSocket buffer doesn't guarantee that, so
    // the frame is copied into this (reused, never shrinking) buffer.
    std::string padded;
};

TickerParser::TickerParser() : impl_(std::make_unique<Impl>()) {}
TickerParser::~TickerParser() = default;

namespace {

// Missing or null fields (e.g. no bid) read as 0, matching the REST path.
double num(od_json::object& obj, std::string_view key) {
    double v = 0.0;
    if (obj[key].get_double().get(v)) v = 0.0;
    return v;
}

}  // namespace

bool TickerParser::parse(std::string_view frame, TickerFields& out) {
    auto& pad = impl_->padded;
    pad.assign(frame);
    pad.append(simdjson::SIMDJSON_PADDING, '\0');

    od_json::document doc;
    if (impl_->parser.iterate(pad.data(), frame.size(), pad.size()).get(doc)) return false;

    std::string_view method;
    if (doc["method"].get_string().get(method) || method != "subscription") return false;

    od_json::object params;
    if (doc["params"].get_object().get(params)) return false;
    std::string_view channel;
    if (params["channel"].get_string().get(channel) || !channel.starts_with("ticker.")) return false;
    od_json::object data;
    if (params["data"].get_object().get(data)) return false;

    if (data["instrument_name"].get_string().get(out.instrument_name)) return false;
    auto& u = out.update;
    std::int64_t ts = 0;
    if (data["timestamp"].get_int64().get(ts)) ts = 0;
    u.exch_ts_ms = ts;
    u.bid = num(data, "best_bid_price");
    u.ask = num(data, "best_ask_price");
    u.bid_amount = num(data, "best_bid_amount");
    u.ask_amount = num(data, "best_ask_amount");
    u.mark = num(data, "mark_price");
    u.mark_iv = num(data, "mark_iv") / 100.0;
    u.underlying = num(data, "underlying_price");
    u.index_price = num(data, "index_price");
    return true;
}

}  // namespace od::deribit
