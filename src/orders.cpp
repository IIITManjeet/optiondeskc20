#include "od/orders.hpp"

#include <algorithm>
#include <cmath>

namespace od {

const char* to_string(Side s) { return s == Side::Buy ? "buy" : "sell"; }

const char* to_string(OrderState s) {
    switch (s) {
        case OrderState::PendingNew: return "pending";
        case OrderState::Open: return "open";
        case OrderState::Filled: return "filled";
        case OrderState::Cancelled: return "cancelled";
        case OrderState::Rejected: return "rejected";
    }
    return "?";
}

bool is_perpetual(const std::string& instrument) { return instrument.ends_with("PERPETUAL"); }

double option_fee_coin(double price_coin, double qty) {
    return std::min(0.0003, 0.125 * price_coin) * qty;
}

double perp_taker_fee_coin(double usd, double price) { return 0.0005 * std::abs(usd) / price; }

OrderManager::OrderManager(std::string currency, double coin_balance)
    : currency_(std::move(currency)), coin_(coin_balance) {}

void OrderManager::on_sent(const OrderRequest& req) {
    Order o;
    o.req = req;
    orders_[req.client_id] = o;
}

void OrderManager::on_amend_sent(std::uint64_t id, double price, double qty) {
    if (auto it = orders_.find(id); it != orders_.end() && it->second.live()) {
        it->second.req.price = price;
        it->second.req.qty = it->second.filled + qty;
    }
}

const Order* OrderManager::find(std::uint64_t id) const {
    const auto it = orders_.find(id);
    return it == orders_.end() ? nullptr : &it->second;
}

std::vector<const Order*> OrderManager::live_orders() const {
    std::vector<const Order*> out;
    for (const auto& [_, o] : orders_)
        if (o.live()) out.push_back(&o);
    return out;
}

double OrderManager::open_qty(const std::string& instrument, Side side) const {
    double q = 0.0;
    for (const auto& [_, o] : orders_)
        if (o.live() && o.req.side == side && o.req.instrument == instrument) q += o.req.qty - o.filled;
    return q;
}

double OrderManager::position(const std::string& instrument) const {
    const auto it = holdings_.find(instrument);
    return it == holdings_.end() ? 0.0 : it->second.qty;
}

void OrderManager::fill_option(Holding& h, Side side, double qty, double price) {
    const double signed_qty = sign(side) * qty;
    coin_ -= signed_qty * price;  // pay premium when buying, receive when selling
    const double new_qty = h.qty + signed_qty;
    if (h.qty == 0.0 || (h.qty > 0) == (signed_qty > 0)) {
        h.entry = (h.entry * std::abs(h.qty) + price * qty) / (std::abs(h.qty) + qty);
    } else if (std::abs(signed_qty) > std::abs(h.qty)) {
        h.entry = price;  // flipped through zero: the remainder opened at this price
    }
    h.qty = std::abs(new_qty) < 1e-12 ? 0.0 : new_qty;
}

// Inverse perp: coin P&L of a position of S USD from entry E to price P is
// S * (1/E - 1/P). Adding to a position keeps that formula exact if the new entry
// is the notional-weighted harmonic mean. Reducing realises P&L into the balance.
void OrderManager::fill_perp(Holding& h, Side side, double usd, double price) {
    const double signed_usd = sign(side) * usd;
    if (h.qty == 0.0 || (h.qty > 0) == (signed_usd > 0)) {
        const double total = std::abs(h.qty) + usd;
        const double inv = (h.qty == 0.0 ? 0.0 : std::abs(h.qty) / h.entry) + usd / price;
        h.entry = total / inv;
        h.qty += signed_usd;
        return;
    }
    const double closed = std::min(usd, std::abs(h.qty));
    const double closed_signed = h.qty > 0 ? closed : -closed;
    coin_ += closed_signed * (1.0 / h.entry - 1.0 / price);
    h.qty -= closed_signed;
    const double rest = usd - closed;
    if (rest > 0.0) {  // flipped
        h.qty = sign(side) * rest;
        h.entry = price;
    }
    if (std::abs(h.qty) < 1e-9) h.qty = 0.0;
}

void OrderManager::apply(const ExecEvent& e) {
    const auto it = orders_.find(e.client_id);
    if (it == orders_.end()) return;
    Order& o = it->second;
    switch (e.kind) {
        case ExecEvent::Kind::Ack:
            if (o.state == OrderState::PendingNew) o.state = OrderState::Open;
            // Amend acks carry the price actually working (the old one if the amend was refused).
            if (e.price > 0.0 && o.live()) o.req.price = e.price;
            break;
        case ExecEvent::Kind::Rejected:
            if (o.live()) {
                o.state = OrderState::Rejected;
                o.reject_reason = e.reason;
            }
            break;
        case ExecEvent::Kind::Cancelled:
            if (o.live()) o.state = OrderState::Cancelled;
            break;
        case ExecEvent::Kind::Fill: {
            o.filled += e.qty;
            if (o.filled >= o.req.qty - 1e-12) o.state = OrderState::Filled;
            else if (o.state == OrderState::PendingNew) o.state = OrderState::Open;

            auto& h = holdings_[o.req.instrument];
            if (is_perpetual(o.req.instrument)) {
                h.perp = true;
                fill_perp(h, o.req.side, e.qty, e.price);
            } else {
                fill_option(h, o.req.side, e.qty, e.price);
                volume_ += e.qty;
            }
            coin_ -= e.fee;
            fees_ += e.fee;
            ++n_fills_;
            break;
        }
    }
}

Portfolio OrderManager::portfolio() const {
    Portfolio pf;
    pf.currency = currency_;
    pf.coin_balance = coin_;
    for (const auto& [name, h] : holdings_) {
        if (h.qty == 0.0) continue;
        pf.positions.push_back(
            {name, h.perp ? PositionKind::Perpetual : PositionKind::Option, h.qty, h.entry});
    }
    return pf;
}

}  // namespace od
