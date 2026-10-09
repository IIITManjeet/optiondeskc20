#pragma once
// Order gateway interface plus a simulated exchange for paper trading.
//
// The strategy talks only to Gateway, so the same code runs against the
// simulator or (M5) Deribit testnet. Execution reports come back through poll(),
// never as callbacks into the strategy mid-decision.

#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

#include "od/orders.hpp"

namespace od {

class Gateway {
public:
    virtual ~Gateway() = default;
    virtual void place(const OrderRequest& req) = 0;
    virtual void amend(std::uint64_t client_id, double price, double qty) = 0;
    virtual void cancel(std::uint64_t client_id) = 0;
    virtual void cancel_all() = 0;
    // Appends execution reports received since the last call.
    virtual void poll(std::vector<ExecEvent>& out) = 0;
};

// Paper exchange driven by real top-of-book data and real trade prints.
//
// Fill model, with a queue-position estimate per resting order:
//   - Joining the best price puts us behind everything displayed there
//     (queue_ahead = displayed size). Improving the best price puts us first
//     (queue_ahead = 0). Resting behind the best price, our place is unknown
//     until our price becomes the best again; then we assume we're last.
//   - Displayed size at our level shrinking moves us up (cancellations ahead of
//     us). Assuming cancels come from ahead is optimistic; it's the usual
//     simplification.
//   - A trade at our price consumes queue_ahead first, then fills us, possibly
//     partially. A trade *through* our price fills us fully. So does the opposite
//     best price moving onto ours.
//   - An amend that changes price loses priority, as on the exchange.
// Our orders aren't in the real book, so the liquidity we "take" from a trade is
// not taken from anyone else: fills are an estimate, not a replay. Fills are at
// our price with maker fees. Perpetual orders (the hedge) fill immediately at
// the price given, with taker fees.
class SimGateway : public Gateway {
public:
    void place(const OrderRequest& req) override;
    void amend(std::uint64_t client_id, double price, double qty) override;
    void cancel(std::uint64_t client_id) override;
    void cancel_all() override;
    void poll(std::vector<ExecEvent>& out) override;

    // Latest top of book for an instrument (sizes in contracts).
    void on_market(const std::string& instrument, double best_bid, double best_ask,
                   double bid_amount = 0.0, double ask_amount = 0.0);
    // A public trade. taker_buy = the aggressor bought (so resting sells trade).
    void on_trade(const std::string& instrument, double price, double amount, bool taker_buy);

    std::size_t resting() const { return resting_.size(); }
    // Estimated contracts ahead of an order at its price; infinity = behind the best price.
    double queue_ahead(std::uint64_t client_id) const;

private:
    struct Book {
        double bid = 0.0, ask = 0.0, bid_amount = 0.0, ask_amount = 0.0;
    };
    struct Resting {
        OrderRequest req;
        double remaining = 0.0;
        double queue = 0.0;
    };
    bool crosses(const OrderRequest& r) const;
    double initial_queue(const OrderRequest& r) const;
    void fill(Resting& r, double qty);

    std::unordered_map<std::uint64_t, Resting> resting_;
    std::unordered_map<std::string, Book> books_;
    std::deque<ExecEvent> events_;
};

// Delta hedge with the inverse perpetual: when the USD-view delta (in coin) leaves
// [-threshold, +threshold], trade the perp back to flat. Deribit perp contracts
// are $10, so the size is rounded to that.
struct HedgeParams {
    double threshold_coin = 0.25;
    double contract_usd = 10.0;
};

// Signed USD notional to trade (positive = buy), or 0 if within the threshold.
double hedge_notional_usd(double delta_usd_view_coin, double index, const HedgeParams& p);

}  // namespace od
