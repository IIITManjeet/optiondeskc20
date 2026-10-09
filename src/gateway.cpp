#include "od/gateway.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace od {

namespace {
constexpr double kUnknownQueue = std::numeric_limits<double>::infinity();
bool same_price(double a, double b) { return std::abs(a - b) < 1e-9; }
}  // namespace

bool SimGateway::crosses(const OrderRequest& r) const {
    const auto it = books_.find(r.instrument);
    if (it == books_.end()) return false;
    const auto& b = it->second;
    return r.side == Side::Buy ? (b.ask > 0.0 && r.price >= b.ask) : (b.bid > 0.0 && r.price <= b.bid);
}

// Where an order arriving now would stand in the queue at its price.
double SimGateway::initial_queue(const OrderRequest& r) const {
    const auto it = books_.find(r.instrument);
    if (it == books_.end()) return kUnknownQueue;
    const auto& b = it->second;
    const double best = r.side == Side::Buy ? b.bid : b.ask;
    const double shown = r.side == Side::Buy ? b.bid_amount : b.ask_amount;
    if (best <= 0.0) return 0.0;                     // empty side: we'd be the market
    if (same_price(r.price, best)) return shown;     // join: behind what's displayed
    const bool improves = r.side == Side::Buy ? r.price > best : r.price < best;
    return improves ? 0.0 : kUnknownQueue;           // improve: first; behind: unknown
}

void SimGateway::fill(Resting& r, double qty) {
    if (qty <= 0.0) return;
    qty = std::min(qty, r.remaining);
    events_.push_back(ExecEvent::fill(r.req.client_id, qty, r.req.price, option_fee_coin(r.req.price, qty)));
    r.remaining -= qty;
}

void SimGateway::place(const OrderRequest& req) {
    if (is_perpetual(req.instrument)) {
        events_.push_back(ExecEvent::ack(req.client_id));
        events_.push_back(
            ExecEvent::fill(req.client_id, req.qty, req.price, perp_taker_fee_coin(req.qty, req.price)));
        return;
    }
    if (req.post_only && crosses(req)) {
        events_.push_back(ExecEvent::rejected(req.client_id, "post_only_would_cross"));
        return;
    }
    resting_[req.client_id] = {req, req.qty, initial_queue(req)};
    events_.push_back(ExecEvent::ack(req.client_id));
}

void SimGateway::amend(std::uint64_t id, double price, double qty) {
    const auto it = resting_.find(id);
    if (it == resting_.end()) {
        events_.push_back(ExecEvent::rejected(id, "unknown_order"));
        return;
    }
    OrderRequest next = it->second.req;
    next.price = price;
    next.qty = qty;
    if (next.post_only && crosses(next)) {
        // Like the exchange: a rejected amend leaves the original order working.
        events_.push_back(ExecEvent::ack(id, it->second.req.price));
        return;
    }
    auto& r = it->second;
    const bool repriced = !same_price(r.req.price, price);
    r.req = next;
    r.remaining = qty;
    if (repriced) r.queue = initial_queue(next);  // a new price goes to the back
    events_.push_back(ExecEvent::ack(id, price));
}

void SimGateway::cancel(std::uint64_t id) {
    if (resting_.erase(id)) events_.push_back(ExecEvent::cancelled(id));
}

void SimGateway::cancel_all() {
    for (const auto& [id, _] : resting_) events_.push_back(ExecEvent::cancelled(id));
    resting_.clear();
}

void SimGateway::poll(std::vector<ExecEvent>& out) {
    out.insert(out.end(), events_.begin(), events_.end());
    events_.clear();
}

double SimGateway::queue_ahead(std::uint64_t id) const {
    const auto it = resting_.find(id);
    return it == resting_.end() ? kUnknownQueue : it->second.queue;
}

void SimGateway::on_market(const std::string& instrument, double best_bid, double best_ask,
                           double bid_amount, double ask_amount) {
    books_[instrument] = {best_bid, best_ask, bid_amount, ask_amount};
    for (auto it = resting_.begin(); it != resting_.end();) {
        auto& r = it->second;
        if (r.req.instrument != instrument) {
            ++it;
            continue;
        }
        const bool buy = r.req.side == Side::Buy;
        const double opposite = buy ? best_ask : best_bid;
        const double same = buy ? best_bid : best_ask;
        const double shown = buy ? bid_amount : ask_amount;

        // The other side moved onto our price: we would have traded.
        if (opposite > 0.0 && (buy ? opposite <= r.req.price : opposite >= r.req.price)) {
            fill(r, r.remaining);
        } else if (same <= 0.0 || (buy ? same < r.req.price : same > r.req.price)) {
            r.queue = 0.0;  // nobody else at or better than our price: we're first
        } else if (same_price(same, r.req.price)) {
            // At the best price. Coming from "behind", assume we're last; otherwise
            // the displayed size can only have shrunk in front of us.
            r.queue = std::isinf(r.queue) ? shown : std::min(r.queue, shown);
        } else {
            r.queue = kUnknownQueue;  // the best price moved past us
        }
        it = r.remaining <= 1e-12 ? resting_.erase(it) : std::next(it);
    }
}

void SimGateway::on_trade(const std::string& instrument, double price, double amount, bool taker_buy) {
    for (auto it = resting_.begin(); it != resting_.end();) {
        auto& r = it->second;
        // Aggressive buyers trade against resting sells and vice versa.
        const bool exposed = r.req.instrument == instrument && (r.req.side == Side::Sell) == taker_buy;
        if (exposed) {
            const bool through = taker_buy ? price > r.req.price : price < r.req.price;
            if (through) {
                fill(r, r.remaining);
            } else if (same_price(price, r.req.price) && !std::isinf(r.queue)) {
                const double reaches_us = amount - r.queue;
                r.queue = std::max(0.0, r.queue - amount);
                fill(r, reaches_us);
            }
        }
        it = r.remaining <= 1e-12 ? resting_.erase(it) : std::next(it);
    }
}

double hedge_notional_usd(double delta, double index, const HedgeParams& p) {
    if (std::abs(delta) <= p.threshold_coin || !(index > 0.0)) return 0.0;
    // Long delta -> sell perp. delta (coin) * index = USD notional to offset.
    const double usd = -delta * index;
    return std::round(usd / p.contract_usd) * p.contract_usd;
}

}  // namespace od
