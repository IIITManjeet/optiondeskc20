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

// Paper exchange driven by real top-of-book data.
//
// Fill model (deliberately pessimistic): a resting buy fills only when the best
// ask trades down *to or through* its price, a resting sell when the best bid
// rises to it. With top-of-book data alone there's no way to know when someone
// simply hits our quote, so every fill here is one where the market moved
// against us first: it shows adverse selection, not spread capture. Fills are
// full size at our price with maker fees. Perpetual orders (the hedge) fill
// immediately at the price given, with taker fees.
class SimGateway : public Gateway {
public:
    void place(const OrderRequest& req) override;
    void amend(std::uint64_t client_id, double price, double qty) override;
    void cancel(std::uint64_t client_id) override;
    void cancel_all() override;
    void poll(std::vector<ExecEvent>& out) override;

    // Feed the simulator the latest top of book for an instrument.
    void on_market(const std::string& instrument, double best_bid, double best_ask);

    std::size_t resting() const { return resting_.size(); }

private:
    struct Book {
        double bid = 0.0, ask = 0.0;
    };
    bool crosses(const OrderRequest& r) const;

    std::unordered_map<std::uint64_t, OrderRequest> resting_;
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
