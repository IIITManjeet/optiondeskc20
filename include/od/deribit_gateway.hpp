#pragma once
// Order gateway to Deribit TESTNET.
//
// The host is a compile-time constant (test.deribit.com) with no way to change it:
// this code cannot send an order to the real exchange.
//
// Threads:
//   strategy thread : place/amend/cancel push a fixed-size command into an SPSC
//                     ring and post a wake-up to the gateway's io_context; poll()
//                     drains execution reports from a second SPSC ring. Neither
//                     side ever blocks on the network.
//   gateway thread  : one TLS WebSocket. Connects and sets up synchronously, then
//                     runs Boost.Asio async reads/writes; protocol logic lives in
//                     DeribitOrders (no I/O, unit-tested).
//
// On every connection, before reporting ready:
//   1. public/auth with client credentials (refreshed before expiry)
//   2. private/enable_cancel_on_disconnect: if this connection drops, the exchange
//      cancels all our orders, so nothing is left working unattended
//   3. private/get_positions: positions we didn't create are reported, so the
//      caller can refuse to trade on top of them
//   4. private/cancel_all_by_currency: start from a clean book
//   5. subscribe to user.orders / user.trades for the currency

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "od/gateway.hpp"
#include "od/spsc_ring.hpp"

namespace od::deribit {

inline constexpr const char* kTestnetHost = "test.deribit.com";

struct GatewayConfig {
    std::string currency = "BTC";
    std::string client_id, client_secret;
};

struct ExistingPosition {
    std::string instrument;
    double size;  // options: contracts; futures/perp: USD
};

class DeribitGateway : public Gateway {
public:
    explicit DeribitGateway(GatewayConfig cfg);
    ~DeribitGateway() override;

    void place(const OrderRequest& req) override;
    void amend(std::uint64_t client_id, double price, double qty) override;
    void cancel(std::uint64_t client_id) override;
    void cancel_all() override;
    void poll(std::vector<ExecEvent>& out) override;

    // Authenticated, cancel-on-disconnect on, book cleared, subscribed.
    bool ready() const { return ready_.load(); }
    // Set once per connection, after the position check. Empty = flat.
    bool positions_checked() const { return positions_checked_.load(); }
    std::vector<ExistingPosition> existing_positions() const;
    std::string last_error() const;
    std::uint64_t reconnects() const { return reconnects_.load(); }

private:
    struct Cmd {
        enum class Kind : std::uint8_t { Place, Amend, Cancel, CancelAll } kind;
        std::uint64_t client_id;
        char instrument[48];
        Side side;
        bool post_only;
        double price, qty;
    };
    struct Ev {
        ExecEvent::Kind kind;
        std::uint64_t client_id;
        double qty, price, fee;
        char reason[64];
    };
    struct Session;

    void run();
    void push_cmd(const Cmd& c);
    void push_event(const ExecEvent& e);
    void wake();

    GatewayConfig cfg_;
    SpscRing<Cmd> cmds_{4096};
    SpscRing<Ev> events_{4096};
    std::vector<ExecEvent> local_rejects_;  // strategy thread only: commands the ring refused

    std::atomic<bool> stop_{false}, ready_{false}, positions_checked_{false};
    std::atomic<std::uint64_t> reconnects_{0};
    mutable std::mutex mu_;  // guards session_, positions_, last_error_
    Session* session_ = nullptr;
    std::vector<ExistingPosition> positions_;
    std::string last_error_;
    std::thread thread_;  // last
};

}  // namespace od::deribit
