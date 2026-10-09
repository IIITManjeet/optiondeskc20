#include "od/gateway.hpp"

#include <cmath>

namespace od {

bool SimGateway::crosses(const OrderRequest& r) const {
    const auto it = books_.find(r.instrument);
    if (it == books_.end()) return false;
    const auto& b = it->second;
    return r.side == Side::Buy ? (b.ask > 0.0 && r.price >= b.ask) : (b.bid > 0.0 && r.price <= b.bid);
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
    resting_[req.client_id] = req;
    events_.push_back(ExecEvent::ack(req.client_id));
}

void SimGateway::amend(std::uint64_t id, double price, double qty) {
    const auto it = resting_.find(id);
    if (it == resting_.end()) {
        events_.push_back(ExecEvent::rejected(id, "unknown_order"));
        return;
    }
    OrderRequest next = it->second;
    next.price = price;
    next.qty = qty;
    if (next.post_only && crosses(next)) {
        // Like the exchange: a rejected amend leaves the original order working.
        events_.push_back(ExecEvent::ack(id, it->second.price));
        return;
    }
    it->second = next;
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

void SimGateway::on_market(const std::string& instrument, double best_bid, double best_ask) {
    books_[instrument] = {best_bid, best_ask};
    for (auto it = resting_.begin(); it != resting_.end();) {
        const auto& r = it->second;
        const bool hit = r.instrument == instrument &&
                         (r.side == Side::Buy ? (best_ask > 0.0 && best_ask <= r.price)
                                              : (best_bid > 0.0 && best_bid >= r.price));
        if (!hit) {
            ++it;
            continue;
        }
        events_.push_back(ExecEvent::fill(r.client_id, r.qty, r.price, option_fee_coin(r.price, r.qty)));
        it = resting_.erase(it);
    }
}

double hedge_notional_usd(double delta, double index, const HedgeParams& p) {
    if (std::abs(delta) <= p.threshold_coin || !(index > 0.0)) return 0.0;
    // Long delta -> sell perp. delta (coin) * index = USD notional to offset.
    const double usd = -delta * index;
    return std::round(usd / p.contract_usd) * p.contract_usd;
}

}  // namespace od
