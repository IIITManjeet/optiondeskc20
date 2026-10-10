#pragma once
// Deribit order-entry protocol, without any I/O.
//
// Turns Gateway commands into JSON-RPC requests and exchange messages (RPC
// responses, user.orders and user.trades notifications) into ExecEvents. Kept
// free of sockets and threads so every awkward case can be unit-tested:
//
//   - amend or cancel arriving before the exchange has assigned an order_id:
//     held, then sent as soon as the place response arrives
//   - the same fill reported twice (RPC response and user.trades): deduplicated
//     by trade_id
//   - the same cancel reported twice (cancel response and user.orders): one event
//   - a rejected amend: the order keeps its old price, reported via the ack
//
// Our orders are labelled "<run>-<client_id>"; anything without our run prefix
// (orders placed by hand, or by an earlier run) is ignored.

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

#include "od/orders.hpp"

namespace od::deribit {

class DeribitOrders {
public:
    explicit DeribitOrders(std::string run_label) : run_(std::move(run_label)) {}

    // Each returns the JSON-RPC text to send now, or nothing if it must wait
    // (amend/cancel before the order_id is known) or is already pointless.
    std::optional<std::string> place(const OrderRequest& req);
    std::optional<std::string> amend(std::uint64_t client_id, double price, double qty);
    std::optional<std::string> cancel(std::uint64_t client_id);
    std::string cancel_all(const std::string& currency);

    // Feed every message received from the exchange. Execution reports are appended
    // to `events`; requests that became sendable (held amends/cancels) to `to_send`.
    void on_message(const nlohmann::json& msg, std::vector<ExecEvent>& events,
                    std::vector<std::string>& to_send);

    // Connection lost: with cancel-on-disconnect the exchange cancels everything,
    // so report every live order as cancelled and forget pending requests.
    void on_disconnect(std::vector<ExecEvent>& events);

    std::string label(std::uint64_t client_id) const;
    std::size_t live_orders() const;

private:
    struct Live {
        std::string instrument;
        std::string order_id;  // empty until the exchange acknowledges
        double price = 0.0, qty = 0.0;
        double filled = 0.0;
        bool done = false;     // terminal state reported
        std::optional<std::pair<double, double>> held_amend;  // price, qty
        bool held_cancel = false;
        bool perp = false;
    };
    enum class Op { Place, Amend, Cancel, CancelAll };
    struct Pending {
        Op op;
        std::uint64_t client_id;
        double old_price;  // for Amend: restore on failure
    };

    std::string rpc(const char* method, nlohmann::json params, Op op, std::uint64_t client_id,
                    double old_price = 0.0);
    std::optional<std::uint64_t> client_id_of(const nlohmann::json& order_or_trade) const;
    void apply_order(const nlohmann::json& order, std::vector<ExecEvent>& events,
                     std::vector<std::string>& to_send);
    void apply_trade(const nlohmann::json& trade, std::vector<ExecEvent>& events);
    std::optional<std::string> flush_held(std::uint64_t client_id);

    std::string run_;
    int next_rpc_id_ = 1000;
    std::unordered_map<int, Pending> pending_;
    std::unordered_map<std::uint64_t, Live> live_;
    std::unordered_set<std::string> seen_trades_;
};

}  // namespace od::deribit
