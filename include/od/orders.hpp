#pragma once
// Orders, executions and the position ledger.
//
// The ledger is the strategy's own record of what it owns, built only from
// execution events (never from what it *meant* to do). Cash is tracked in the
// coin: buying an option pays its premium out of the coin balance, so
// equity = coin balance + mark value of positions (+ perp P&L), which is exactly
// what the risk engine's Portfolio expects.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "od/risk.hpp"

namespace od {

enum class Side { Buy, Sell };
inline double sign(Side s) { return s == Side::Buy ? 1.0 : -1.0; }
const char* to_string(Side s);

struct OrderRequest {
    std::uint64_t client_id = 0;
    std::string instrument;
    Side side = Side::Buy;
    double price = 0.0;  // options: coin; perpetual: USD
    double qty = 0.0;    // options: contracts; perpetual: USD notional
    bool post_only = true;
};

enum class OrderState { PendingNew, Open, Filled, Cancelled, Rejected };
const char* to_string(OrderState s);

struct Order {
    OrderRequest req;
    OrderState state = OrderState::PendingNew;
    double filled = 0.0;
    std::string reject_reason;
    bool live() const { return state == OrderState::PendingNew || state == OrderState::Open; }
};

struct ExecEvent {
    enum class Kind { Ack, Fill, Cancelled, Rejected } kind = Kind::Ack;
    std::uint64_t client_id = 0;
    double qty = 0.0;    // fills
    double price = 0.0;  // fills; acks of a replace carry the new price
    double fee = 0.0;    // fills, in coin
    std::string reason;  // rejects

    static ExecEvent ack(std::uint64_t id, double price = 0.0) { return {Kind::Ack, id, 0, price, 0, {}}; }
    static ExecEvent fill(std::uint64_t id, double qty, double price, double fee) {
        return {Kind::Fill, id, qty, price, fee, {}};
    }
    static ExecEvent cancelled(std::uint64_t id) { return {Kind::Cancelled, id, 0, 0, 0, {}}; }
    static ExecEvent rejected(std::uint64_t id, std::string why) {
        return {Kind::Rejected, id, 0, 0, 0, std::move(why)};
    }
};

class OrderManager {
public:
    OrderManager(std::string currency, double coin_balance);

    std::uint64_t next_id() { return ++last_id_; }

    // Record an order as sent (state PendingNew) before handing it to the gateway.
    void on_sent(const OrderRequest& req);
    // Record a price/qty amendment as sent.
    void on_amend_sent(std::uint64_t id, double price, double qty);
    void apply(const ExecEvent& e);

    const Order* find(std::uint64_t id) const;
    std::vector<const Order*> live_orders() const;
    // Total remaining quantity of live orders on one side of an instrument.
    double open_qty(const std::string& instrument, Side side) const;

    double position(const std::string& instrument) const;
    double coin_balance() const { return coin_; }
    double fees_paid() const { return fees_; }
    std::uint64_t fills() const { return n_fills_; }
    double volume() const { return volume_; }  // option contracts traded

    // Positions in the form the risk engine values.
    Portfolio portfolio() const;

private:
    struct Holding {
        double qty = 0.0;
        double entry = 0.0;  // options: VWAP coin; perp: USD entry (harmonic mean)
        bool perp = false;
    };
    void fill_option(Holding& h, Side side, double qty, double price);
    void fill_perp(Holding& h, Side side, double usd, double price);

    std::string currency_;
    double coin_;
    double fees_ = 0.0;
    double volume_ = 0.0;
    std::uint64_t n_fills_ = 0;
    std::uint64_t last_id_ = 0;
    std::unordered_map<std::uint64_t, Order> orders_;
    std::map<std::string, Holding> holdings_;
};

bool is_perpetual(const std::string& instrument);

// Approximate Deribit fees (check the current schedule before relying on them):
// options 0.03% of the underlying per contract, i.e. 0.0003 coin, capped at 12.5%
// of the option price; perpetual taker 0.05% of notional.
double option_fee_coin(double price_coin, double qty);
double perp_taker_fee_coin(double usd, double price);

}  // namespace od
