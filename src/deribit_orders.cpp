#include "od/deribit_orders.hpp"

#include <charconv>

namespace od::deribit {

using nlohmann::json;

std::string DeribitOrders::label(std::uint64_t client_id) const {
    return run_ + "-" + std::to_string(client_id);
}

std::size_t DeribitOrders::live_orders() const {
    std::size_t n = 0;
    for (const auto& [_, l] : live_) n += !l.done;
    return n;
}

std::string DeribitOrders::rpc(const char* method, json params, Op op, std::uint64_t client_id,
                               double old_price) {
    const int id = next_rpc_id_++;
    pending_[id] = {op, client_id, old_price};
    return json{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", std::move(params)}}
        .dump();
}

std::optional<std::string> DeribitOrders::place(const OrderRequest& req) {
    Live l;
    l.instrument = req.instrument;
    l.price = req.price;
    l.qty = req.qty;
    l.perp = is_perpetual(req.instrument);
    live_[req.client_id] = l;

    json p{{"instrument_name", req.instrument}, {"amount", req.qty}, {"label", label(req.client_id)}};
    if (l.perp) {
        p["type"] = "market";  // the hedge: take liquidity, USD notional in $10 contracts
    } else {
        p["type"] = "limit";
        p["price"] = req.price;
        p["post_only"] = req.post_only;
        // Reject rather than let the exchange silently reprice a post-only order.
        p["reject_post_only"] = req.post_only;
    }
    return rpc(req.side == Side::Buy ? "private/buy" : "private/sell", std::move(p), Op::Place,
               req.client_id);
}

std::optional<std::string> DeribitOrders::amend(std::uint64_t client_id, double price, double qty) {
    const auto it = live_.find(client_id);
    if (it == live_.end() || it->second.done) return std::nullopt;
    auto& l = it->second;
    if (l.order_id.empty()) {  // not acknowledged yet: send once it is
        l.held_amend = {price, qty};
        return std::nullopt;
    }
    const double old = l.price;
    l.price = price;
    l.qty = qty;
    // Deribit's amount is the order's total size, so add what has already filled.
    return rpc("private/edit",
               {{"order_id", l.order_id}, {"amount", qty + l.filled}, {"price", price},
                {"post_only", true}, {"reject_post_only", true}},
               Op::Amend, client_id, old);
}

std::optional<std::string> DeribitOrders::cancel(std::uint64_t client_id) {
    const auto it = live_.find(client_id);
    if (it == live_.end() || it->second.done) return std::nullopt;
    auto& l = it->second;
    if (l.order_id.empty()) {
        l.held_cancel = true;
        return std::nullopt;
    }
    return rpc("private/cancel", {{"order_id", l.order_id}}, Op::Cancel, client_id);
}

std::string DeribitOrders::cancel_all(const std::string& currency) {
    return rpc("private/cancel_all_by_currency", {{"currency", currency}}, Op::CancelAll, 0);
}

std::optional<std::string> DeribitOrders::flush_held(std::uint64_t client_id) {
    auto& l = live_[client_id];
    if (l.held_cancel) {
        l.held_cancel = false;
        l.held_amend.reset();
        return cancel(client_id);
    }
    if (l.held_amend) {
        const auto [price, qty] = *l.held_amend;
        l.held_amend.reset();
        return amend(client_id, price, qty);
    }
    return std::nullopt;
}

std::optional<std::uint64_t> DeribitOrders::client_id_of(const json& obj) const {
    const auto it = obj.find("label");
    if (it == obj.end() || !it->is_string()) return std::nullopt;
    const auto& s = it->get_ref<const std::string&>();
    const std::string prefix = run_ + "-";
    if (s.size() <= prefix.size() || s.compare(0, prefix.size(), prefix) != 0) return std::nullopt;
    std::uint64_t id = 0;
    const auto [p, ec] = std::from_chars(s.data() + prefix.size(), s.data() + s.size(), id);
    if (ec != std::errc{} || p != s.data() + s.size()) return std::nullopt;
    return id;
}

void DeribitOrders::apply_order(const json& order, std::vector<ExecEvent>& events,
                                std::vector<std::string>& to_send) {
    const auto cid = client_id_of(order);
    if (!cid) return;
    const auto it = live_.find(*cid);
    if (it == live_.end()) return;
    auto& l = it->second;
    if (l.order_id.empty() && order.contains("order_id")) {
        l.order_id = order["order_id"].get<std::string>();
        if (auto msg = flush_held(*cid)) to_send.push_back(std::move(*msg));
    }
    const auto state = order.value("order_state", std::string{});
    if (l.done) return;
    if (state == "cancelled") {
        l.done = true;
        events.push_back(ExecEvent::cancelled(*cid));
    } else if (state == "rejected") {
        l.done = true;
        events.push_back(ExecEvent::rejected(*cid, order.value("cancel_reason", std::string{"rejected"})));
    } else if (state == "filled") {
        l.done = true;  // the fills themselves arrive as trades
    }
}

void DeribitOrders::apply_trade(const json& trade, std::vector<ExecEvent>& events) {
    const auto cid = client_id_of(trade);
    if (!cid) return;
    const auto tid = trade.value("trade_id", std::string{});
    if (tid.empty() || !seen_trades_.insert(tid).second) return;  // duplicate report
    const double qty = trade.value("amount", 0.0);
    const double price = trade.value("price", 0.0);
    const double fee = trade.value("fee", 0.0);
    if (qty <= 0.0) return;
    if (const auto it = live_.find(*cid); it != live_.end()) it->second.filled += qty;
    events.push_back(ExecEvent::fill(*cid, qty, price, fee));
}

void DeribitOrders::on_message(const json& msg, std::vector<ExecEvent>& events,
                               std::vector<std::string>& to_send) {
    // RPC response to one of our requests.
    if (const auto idit = msg.find("id"); idit != msg.end() && idit->is_number_integer()) {
        const auto pit = pending_.find(idit->get<int>());
        if (pit == pending_.end()) return;
        const Pending p = pit->second;
        pending_.erase(pit);

        if (const auto err = msg.find("error"); err != msg.end()) {
            const auto why = err->value("message", std::string{"error"});
            switch (p.op) {
                case Op::Place:
                    if (auto& l = live_[p.client_id]; !l.done) {
                        l.done = true;
                        events.push_back(ExecEvent::rejected(p.client_id, why));
                    }
                    break;
                case Op::Amend:
                    // The order keeps working at its old price; say so via the ack.
                    live_[p.client_id].price = p.old_price;
                    events.push_back(ExecEvent::ack(p.client_id, p.old_price));
                    break;
                case Op::Cancel:     // usually "already filled/cancelled": its own report follows
                case Op::CancelAll:
                    break;
            }
            return;
        }

        const json& res = msg.at("result");
        if (p.op == Op::CancelAll) return;  // result is a count; per-order reports follow
        const json& order = res.contains("order") ? res["order"] : res;
        if (p.op == Op::Place) {
            auto& l = live_[p.client_id];
            if (l.order_id.empty()) l.order_id = order.value("order_id", std::string{});
            events.push_back(ExecEvent::ack(p.client_id));
            if (auto m = flush_held(p.client_id)) to_send.push_back(std::move(*m));
        } else if (p.op == Op::Amend) {
            events.push_back(ExecEvent::ack(p.client_id, order.value("price", live_[p.client_id].price)));
        }
        if (const auto t = res.find("trades"); t != res.end() && t->is_array())
            for (const auto& tr : *t) apply_trade(tr, events);
        apply_order(order, events, to_send);
        return;
    }

    // Subscription notifications.
    if (msg.value("method", std::string{}) != "subscription") return;
    const auto& params = msg.at("params");
    const auto channel = params.value("channel", std::string{});
    const auto& data = params.at("data");
    if (channel.starts_with("user.orders.")) {
        if (data.is_array())
            for (const auto& o : data) apply_order(o, events, to_send);
        else
            apply_order(data, events, to_send);
    } else if (channel.starts_with("user.trades.")) {
        if (data.is_array())
            for (const auto& t : data) apply_trade(t, events);
        else
            apply_trade(data, events);
    }
}

void DeribitOrders::on_disconnect(std::vector<ExecEvent>& events) {
    for (auto& [cid, l] : live_) {
        if (l.done) continue;
        l.done = true;
        events.push_back(ExecEvent::cancelled(cid));
    }
    pending_.clear();
}

}  // namespace od::deribit
