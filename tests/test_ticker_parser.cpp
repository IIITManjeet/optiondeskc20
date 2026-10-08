#include <gtest/gtest.h>

#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>

#include "od/ticker_parser.hpp"

namespace {

std::string read_frame(const char* name) {
    std::ifstream in(std::string(OD_DATA_DIR) + "/frames/" + name);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void replace(std::string& s, const std::string& from, const std::string& to) {
    const auto pos = s.find(from);
    ASSERT_NE(pos, std::string::npos) << from;
    s.replace(pos, from.size(), to);
}

}  // namespace

// Real frame captured from wss://www.deribit.com/ws/api/v2 (2026-10-09).
TEST(TickerParser, MatchesGeneralParserOnRealFrame) {
    const std::string frame = read_frame("ticker_BTC-27NOV26-88000-C.json");
    ASSERT_FALSE(frame.empty());

    od::deribit::TickerParser parser;
    od::deribit::TickerFields f;
    ASSERT_TRUE(parser.parse(frame, f));

    const auto d = nlohmann::json::parse(frame)["params"]["data"];
    EXPECT_EQ(f.instrument_name, d["instrument_name"].get<std::string>());
    EXPECT_EQ(f.update.exch_ts_ms, d["timestamp"].get<std::int64_t>());
    EXPECT_DOUBLE_EQ(f.update.bid, d["best_bid_price"].get<double>());
    EXPECT_DOUBLE_EQ(f.update.ask, d["best_ask_price"].get<double>());
    EXPECT_DOUBLE_EQ(f.update.bid_amount, d["best_bid_amount"].get<double>());
    EXPECT_DOUBLE_EQ(f.update.ask_amount, d["best_ask_amount"].get<double>());
    EXPECT_DOUBLE_EQ(f.update.mark, d["mark_price"].get<double>());
    EXPECT_DOUBLE_EQ(f.update.mark_iv, d["mark_iv"].get<double>() / 100.0);
    EXPECT_DOUBLE_EQ(f.update.underlying, d["underlying_price"].get<double>());
    EXPECT_DOUBLE_EQ(f.update.index_price, d["index_price"].get<double>());
}

TEST(TickerParser, NullAndMissingFieldsReadAsZero) {
    std::string frame = read_frame("ticker_BTC-27NOV26-88000-C.json");
    const auto d = nlohmann::json::parse(frame)["params"]["data"];
    replace(frame, "\"best_bid_price\":" + d["best_bid_price"].dump(), "\"best_bid_price\":null");
    replace(frame, "\"best_ask_price\"", "\"renamed_field\"");

    od::deribit::TickerParser parser;
    od::deribit::TickerFields f;
    ASSERT_TRUE(parser.parse(frame, f));
    EXPECT_EQ(f.update.bid, 0.0);
    EXPECT_EQ(f.update.ask, 0.0);
    EXPECT_GT(f.update.mark, 0.0);
}

TEST(TickerParser, RejectsNonTickerMessages) {
    od::deribit::TickerParser parser;
    od::deribit::TickerFields f;
    EXPECT_FALSE(parser.parse(read_frame("subscribe_ack.json"), f));
    EXPECT_FALSE(parser.parse(R"({"jsonrpc":"2.0","method":"heartbeat","params":{"type":"test_request"}})", f));
    EXPECT_FALSE(parser.parse(R"({"jsonrpc":"2.0","method":"subscription","params":{"channel":"book.BTC-PERPETUAL.100ms","data":{}}})", f));
    EXPECT_FALSE(parser.parse("not json", f));
    EXPECT_FALSE(parser.parse("", f));
}

TEST(TickerParser, ReusableAcrossMessages) {
    const std::string frame = read_frame("ticker_BTC-27NOV26-88000-C.json");
    od::deribit::TickerParser parser;
    od::deribit::TickerFields f;
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(parser.parse(frame, f));
        EXPECT_FALSE(parser.parse("{}", f));
    }
    ASSERT_TRUE(parser.parse(frame, f));
    EXPECT_EQ(f.instrument_name, "BTC-27NOV26-88000-C");
}
