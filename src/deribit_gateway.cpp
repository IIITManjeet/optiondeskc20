#include "od/deribit_gateway.hpp"

#include <sys/socket.h>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <optional>
#include <thread>
#include <nlohmann/json.hpp>
#include <stdexcept>

#include "od/deribit_orders.hpp"
#include "od/thread_util.hpp"

namespace od::deribit {

namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
namespace beast = boost::beast;
namespace websocket = boost::beast::websocket;
using tcp = boost::asio::ip::tcp;
using nlohmann::json;

namespace {

constexpr int kRefreshId = 7;

ssl::context make_tls_context() {
    ssl::context ctx{ssl::context::tls_client};
    ctx.set_default_verify_paths();
    ctx.set_verify_mode(ssl::verify_peer);
    return ctx;
}

std::string run_label() {
    // Distinguishes this run's orders from earlier runs and from manual orders.
    return "od" + std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count());
}

}  // namespace

struct DeribitGateway::Session {
    net::io_context ioc;
    ssl::context tls = make_tls_context();
    websocket::stream<beast::ssl_stream<tcp::socket>> ws{ioc, tls};
    beast::flat_buffer buf;
    std::deque<std::string> outbox;
    bool writing = false;
    net::steady_timer refresh_timer{ioc};
    std::string refresh_token;
    std::atomic<int> fd{-1};
    std::function<void()> drain;  // set once the async loop is running
};

DeribitGateway::DeribitGateway(GatewayConfig cfg)
    : cfg_(std::move(cfg)), thread_([this] { run(); }) {}

DeribitGateway::~DeribitGateway() {
    stop_ = true;
    {
        std::lock_guard lk(mu_);
        if (session_) {
            net::post(session_->ioc, [s = session_] { s->ioc.stop(); });
            if (const int fd = session_->fd.load(); fd >= 0) ::shutdown(fd, SHUT_RDWR);
        }
    }
    thread_.join();
}

// --- strategy thread ----------------------------------------------------------

void DeribitGateway::push_cmd(const Cmd& c) {
    if (!cmds_.try_push(c)) {
        if (c.kind == Cmd::Kind::Place) local_rejects_.push_back(ExecEvent::rejected(c.client_id, "gateway_queue_full"));
        return;
    }
    wake();
}

void DeribitGateway::wake() {
    std::lock_guard lk(mu_);
    if (session_ && session_->drain) net::post(session_->ioc, session_->drain);
}

void DeribitGateway::place(const OrderRequest& req) {
    Cmd c{Cmd::Kind::Place, req.client_id, {}, req.side, req.post_only, req.price, req.qty};
    std::strncpy(c.instrument, req.instrument.c_str(), sizeof c.instrument - 1);
    push_cmd(c);
}

void DeribitGateway::amend(std::uint64_t id, double price, double qty) {
    push_cmd({Cmd::Kind::Amend, id, {}, Side::Buy, true, price, qty});
}

void DeribitGateway::cancel(std::uint64_t id) { push_cmd({Cmd::Kind::Cancel, id, {}, Side::Buy, true, 0, 0}); }

void DeribitGateway::cancel_all() { push_cmd({Cmd::Kind::CancelAll, 0, {}, Side::Buy, true, 0, 0}); }

void DeribitGateway::poll(std::vector<ExecEvent>& out) {
    out.insert(out.end(), local_rejects_.begin(), local_rejects_.end());
    local_rejects_.clear();
    Ev e;
    while (events_.try_pop(e)) {
        ExecEvent x;
        x.kind = e.kind;
        x.client_id = e.client_id;
        x.qty = e.qty;
        x.price = e.price;
        x.fee = e.fee;
        x.reason = e.reason;
        out.push_back(std::move(x));
    }
}

std::vector<ExistingPosition> DeribitGateway::existing_positions() const {
    std::lock_guard lk(mu_);
    return positions_;
}

std::string DeribitGateway::last_error() const {
    std::lock_guard lk(mu_);
    return last_error_;
}

// --- gateway thread -----------------------------------------------------------

void DeribitGateway::push_event(const ExecEvent& x) {
    Ev e{x.kind, x.client_id, x.qty, x.price, x.fee, {}};
    std::strncpy(e.reason, x.reason.c_str(), sizeof e.reason - 1);
    // Execution reports must not be dropped (a lost fill is a wrong position), so
    // if the strategy isn't keeping up, wait for it.
    while (!events_.try_push(e) && !stop_.load()) std::this_thread::yield();
}

void DeribitGateway::run() {
    od::name_current_thread("od-gateway");
    DeribitOrders orders(run_label());
    int backoff_s = 1;
    bool first_session = true;
    while (!stop_.load()) {
        try {
            // session() only returns by throwing or on stop.
            Session s;
            {
                std::lock_guard lk(mu_);
                session_ = &s;
            }
            struct Clear {
                DeribitGateway& g;
                ~Clear() {
                    std::lock_guard lk(g.mu_);
                    g.session_ = nullptr;
                }
            } clear{*this};

            // ---- synchronous setup -------------------------------------------
            tcp::resolver resolver{s.ioc};
            net::connect(beast::get_lowest_layer(s.ws), resolver.resolve(kTestnetHost, "443"));
            beast::get_lowest_layer(s.ws).set_option(tcp::no_delay(true));
            s.fd = beast::get_lowest_layer(s.ws).native_handle();
            if (!SSL_set_tlsext_host_name(s.ws.next_layer().native_handle(), kTestnetHost))
                throw std::runtime_error("SNI setup failed");
            s.ws.next_layer().set_verify_callback(ssl::host_name_verification(kTestnetHost));
            s.ws.next_layer().handshake(ssl::stream_base::client);
            s.ws.handshake(kTestnetHost, "/ws/api/v2");
            s.ws.text(true);

            int sync_id = 1;
            auto call = [&](const char* method, json params) {
                const int id = sync_id++;
                s.ws.write(net::buffer(
                    json{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", std::move(params)}}.dump()));
                for (;;) {
                    s.buf.clear();
                    s.ws.read(s.buf);
                    auto j = json::parse(beast::buffers_to_string(s.buf.data()));
                    if (!j.contains("id") || j["id"] != id) continue;  // not ours (yet)
                    if (j.contains("error"))
                        throw std::runtime_error(std::string(method) + ": " +
                                                 j["error"].value("message", std::string{"error"}));
                    return j["result"];
                }
            };

            const auto auth = call("public/auth", {{"grant_type", "client_credentials"},
                                                   {"client_id", cfg_.client_id},
                                                   {"client_secret", cfg_.client_secret}});
            s.refresh_token = auth.value("refresh_token", std::string{});
            const int expires_in = auth.value("expires_in", 900);
            call("private/enable_cancel_on_disconnect", {{"scope", "connection"}});

            std::vector<ExistingPosition> pos;
            for (const auto& p : call("private/get_positions", {{"currency", cfg_.currency}}))
                if (const double size = p.value("size", 0.0); size != 0.0)
                    pos.push_back({p.value("instrument_name", std::string{}), size,
                                   p.value("average_price", 0.0)});
            {
                std::lock_guard lk(mu_);
                positions_ = std::move(pos);
            }
            positions_checked_ = true;

            // The account's limit on trading requests (place, amend, cancel), shared by
            // all of them. Exceeding it gets the connection closed ("over_limit"),
            // so pace below it.
            double rate = 5.0, burst = 20.0;
            const auto summary = call("private/get_account_summary", {{"currency", cfg_.currency}, {"extended", true}});
            if (const auto l = summary.find("limits"); l != summary.end()) {
                const auto t = l->value("/matching_engine/trading/total"_json_pointer, json::object());
                rate = t.value("rate", rate);
                burst = t.value("burst", burst);
            }
            rate *= 0.8;
            burst = std::max(1.0, std::floor(burst * 0.8));

            call("private/cancel_all_by_currency", {{"currency", cfg_.currency}});
            call("private/subscribe",
                 {{"channels", {"user.orders.any." + cfg_.currency + ".raw",
                                "user.trades.any." + cfg_.currency + ".raw"}}});
            call("public/set_heartbeat", {{"interval", 10}});

            // ---- asynchronous loop -------------------------------------------
            std::function<void()> write_next = [&] {
                if (s.writing || s.outbox.empty()) return;
                s.writing = true;
                s.ws.async_write(net::buffer(s.outbox.front()), [&](beast::error_code ec, std::size_t) {
                    s.writing = false;
                    if (ec) return s.ioc.stop();
                    s.outbox.pop_front();
                    write_next();
                });
            };
            auto send = [&](std::string m) {
                s.outbox.push_back(std::move(m));
                write_next();
            };

            // Trading requests go through a token bucket and wait in a queue rather than
            // being dropped. Cancels go to the front: pulling a quote is always more
            // urgent than placing one. (A cancel can't overtake the place of the same
            // order: DeribitOrders only issues it once the order is acknowledged.)
            // After a reconnect we start with an empty bucket, since the usual reason
            // for a disconnect is that the exchange's credits ran out.
            double tokens = first_session ? burst : 0.0;
            first_session = false;
            auto last_refill = std::chrono::steady_clock::now();
            std::deque<std::string> trading_q;
            net::steady_timer pace{s.ioc};
            bool pace_armed = false;
            std::function<void()> pump = [&] {
                const auto now = std::chrono::steady_clock::now();
                tokens = std::min(burst, tokens + std::chrono::duration<double>(now - last_refill).count() * rate);
                last_refill = now;
                while (!trading_q.empty() && tokens >= 1.0) {
                    tokens -= 1.0;
                    send(std::move(trading_q.front()));
                    trading_q.pop_front();
                }
                queued_ = trading_q.size();
                if (trading_q.empty() || pace_armed) return;
                pace_armed = true;
                pace.expires_after(std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::duration<double>((1.0 - tokens) / rate)));
                pace.async_wait([&](beast::error_code ec) {
                    pace_armed = false;
                    if (!ec) pump();
                });
            };
            auto send_trading = [&](std::string m) {
                if (m.find("\"private/cancel") != std::string::npos) trading_q.push_front(std::move(m));
                else trading_q.push_back(std::move(m));
                pump();
            };

            std::vector<ExecEvent> evs;
            std::vector<std::string> to_send;
            auto deliver = [&] {
                for (const auto& e : evs) push_event(e);
                for (auto& m : to_send) send_trading(std::move(m));
                evs.clear();
                to_send.clear();
            };

            auto drain = [&] {
                Cmd c;
                while (cmds_.try_pop(c)) {
                    std::optional<std::string> m;
                    switch (c.kind) {
                        case Cmd::Kind::Place:
                            m = orders.place({c.client_id, c.instrument, c.side, c.price, c.qty, c.post_only});
                            break;
                        case Cmd::Kind::Amend: m = orders.amend(c.client_id, c.price, c.qty); break;
                        case Cmd::Kind::Cancel: m = orders.cancel(c.client_id); break;
                        case Cmd::Kind::CancelAll: m = orders.cancel_all(cfg_.currency); break;
                    }
                    if (m) send_trading(std::move(*m));
                }
            };

            std::function<void(int)> schedule_refresh = [&](int seconds) {
                s.refresh_timer.expires_after(std::chrono::seconds(std::max(30, seconds * 4 / 5)));
                s.refresh_timer.async_wait([&](beast::error_code ec) {
                    if (ec) return;
                    send(json{{"jsonrpc", "2.0"}, {"id", kRefreshId}, {"method", "public/auth"},
                              {"params", {{"grant_type", "refresh_token"}, {"refresh_token", s.refresh_token}}}}
                             .dump());
                });
            };

            std::function<void()> read_next = [&] {
                s.buf.clear();
                s.ws.async_read(s.buf, [&](beast::error_code ec, std::size_t) {
                    if (ec) {
                        std::lock_guard lk(mu_);
                        last_error_ = "read: " + ec.message();
                        // The server's close frame says why it hung up.
                        if (ec == websocket::error::closed) {
                            const auto& r = s.ws.reason();
                            last_error_ += " (close code " + std::to_string(r.code) + " " +
                                           std::string(r.reason.data(), r.reason.size()) + ")";
                        }
                        return s.ioc.stop();
                    }
                    const auto j = json::parse(beast::buffers_to_string(s.buf.data()), nullptr, false);
                    if (!j.is_discarded()) {
                        if (j.value("method", std::string{}) == "heartbeat") {
                            if (j["params"].value("type", std::string{}) == "test_request")
                                send(json{{"jsonrpc", "2.0"}, {"id", 8}, {"method", "public/test"},
                                          {"params", json::object()}}.dump());
                        } else if (j.contains("id") && j["id"] == kRefreshId) {
                            if (j.contains("result")) {
                                s.refresh_token = j["result"].value("refresh_token", s.refresh_token);
                                schedule_refresh(j["result"].value("expires_in", 900));
                            } else {
                                std::lock_guard lk(mu_);
                                last_error_ = "token refresh failed: " + j["error"].dump();
                            }
                        } else {
                            orders.on_message(j, evs, to_send);
                            deliver();
                        }
                    }
                    read_next();
                });
            };

            {
                std::lock_guard lk(mu_);
                s.drain = [&] { drain(); };
            }
            schedule_refresh(expires_in);
            read_next();
            ready_ = true;
            backoff_s = 1;
            drain();  // anything queued while we were connecting
            std::fprintf(stderr,
                         "gateway: connected to %s (testnet), cancel-on-disconnect on, "
                         "pacing trading requests at %.1f/s (burst %.0f)\n",
                         kTestnetHost, rate, burst);
            s.ioc.run();

            ready_ = false;
            if (stop_.load()) {
                // Leave nothing working: cancel everything before closing.
                beast::error_code ec;
                s.ws.write(net::buffer(orders.cancel_all(cfg_.currency)), ec);
                s.ws.close(websocket::close_code::normal, ec);
                break;
            }
            throw std::runtime_error(last_error());
        } catch (const std::exception& e) {
            ready_ = false;
            {
                std::lock_guard lk(mu_);
                last_error_ = e.what();
            }
            // With cancel-on-disconnect the exchange has cancelled our orders.
            std::vector<ExecEvent> evs;
            orders.on_disconnect(evs);
            for (const auto& x : evs) push_event(x);
            if (stop_.load()) break;
            reconnects_.fetch_add(1);
            std::fprintf(stderr, "gateway: %s; reconnecting in %ds\n", e.what(), backoff_s);
            // While disconnected, new orders can't go anywhere: reject them.
            for (int i = 0; i < backoff_s * 10 && !stop_.load(); ++i) {
                Cmd c;
                while (cmds_.try_pop(c))
                    if (c.kind == Cmd::Kind::Place)
                        push_event(ExecEvent::rejected(c.client_id, "gateway_disconnected"));
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            backoff_s = std::min(backoff_s * 2, 30);
        }
    }
}

}  // namespace od::deribit
