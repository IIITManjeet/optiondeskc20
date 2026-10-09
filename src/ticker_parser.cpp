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
    std::vector<TradeFields> scratch;
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

std::int64_t int64(od_json::object& obj, std::string_view key) {
    std::int64_t v = 0;
    if (obj[key].get_int64().get(v)) v = 0;
    return v;
}

bool parse_ticker(od_json::object& data, TickerFields& out) {
    if (data["instrument_name"].get_string().get(out.instrument_name)) return false;
    auto& u = out.update;
    u.exch_ts_ms = int64(data, "timestamp");
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

bool parse_trade(od_json::object& t, TradeFields& out) {
    if (t["instrument_name"].get_string().get(out.instrument_name)) return false;
    std::string_view direction;
    if (t["direction"].get_string().get(direction)) return false;
    auto& u = out.update;
    u.taker_buy = direction == "buy";
    u.exch_ts_ms = int64(t, "timestamp");
    std::uint64_t seq = 0;
    if (t["trade_seq"].get_uint64().get(seq)) seq = 0;
    u.trade_seq = seq;
    u.price = num(t, "price");
    u.amount = num(t, "amount");
    u.iv = num(t, "iv") / 100.0;
    u.index_price = num(t, "index_price");
    return u.price > 0.0 && u.amount > 0.0;
}

}  // namespace

TickerParser::Kind TickerParser::parse(std::string_view frame, TickerFields& tick,
                                       std::vector<TradeFields>& trades) {
    auto& pad = impl_->padded;
    pad.assign(frame);
    pad.append(simdjson::SIMDJSON_PADDING, '\0');

    od_json::document doc;
    if (impl_->parser.iterate(pad.data(), frame.size(), pad.size()).get(doc)) return Kind::Other;

    std::string_view method;
    if (doc["method"].get_string().get(method) || method != "subscription") return Kind::Other;

    od_json::object params;
    if (doc["params"].get_object().get(params)) return Kind::Other;
    std::string_view channel;
    if (params["channel"].get_string().get(channel)) return Kind::Other;

    if (channel.starts_with("ticker.")) {
        od_json::object data;
        if (params["data"].get_object().get(data)) return Kind::Other;
        return parse_ticker(data, tick) ? Kind::Ticker : Kind::Other;
    }
    if (channel.starts_with("trades.")) {
        od_json::array data;
        if (params["data"].get_array().get(data)) return Kind::Other;
        trades.clear();
        for (auto element : data) {
            od_json::object t;
            if (element.get_object().get(t)) continue;
            TradeFields f;
            if (parse_trade(t, f)) trades.push_back(f);
        }
        return Kind::Trades;
    }
    return Kind::Other;
}

bool TickerParser::parse(std::string_view frame, TickerFields& tick) {
    return parse(frame, tick, impl_->scratch) == Kind::Ticker;
}

}  // namespace od::deribit
