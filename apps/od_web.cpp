// od_web: the live desk in a browser.
//
//   od_feedd --currency BTC
//   od_web --static ui/dist                 then open http://localhost:8090
//
// Reads the shared-memory buses published by od_feedd (tickers, and trades if
// present), refits the vol surface every --refresh-ms on its own thread, and
// pushes each result as one JSON snapshot to every WebSocket client on /ws.
//
//   GET /ws             WebSocket: a snapshot per refresh (see od/web_snapshot.hpp)
//   GET /api/snapshot   the latest snapshot, once
//   GET /*              files from --static (the built UI), index.html otherwise
//
// It can start before od_feedd and survives od_feedd restarts: until a bus is
// attached the snapshot says "feed": null, and a stale heartbeat triggers a reattach.
//
// One thread per HTTP connection: a dashboard has a handful of clients, and a
// blocking session that waits for the next snapshot and writes it is easy to follow.

#include <signal.h>

#include <algorithm>
#include <atomic>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>

#include "od/feed_bus.hpp"
#include "od/histogram.hpp"
#include "od/surface.hpp"
#include "od/thread_util.hpp"
#include "od/web_snapshot.hpp"

namespace {

namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace fs = std::filesystem;
using tcp = net::ip::tcp;

std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop.store(true); }

struct Args {
    std::string bus = "od_feed_BTC";
    std::string trades_bus;  // default od_trades_<CCY>
    std::string bind = "127.0.0.1";
    int port = 8090;
    int refresh_ms = 1000;
    std::string static_dir;
};

bool parse(int argc, char** argv, Args& a) {
    for (int i = 1; i < argc; ++i) {
        const std::string f = argv[i];
        const char* v = i + 1 < argc ? argv[++i] : nullptr;
        if (!v) return false;
        if (f == "--bus") a.bus = v;
        else if (f == "--trades-bus") a.trades_bus = v;
        else if (f == "--bind") a.bind = v;
        else if (f == "--port") a.port = std::atoi(v);
        else if (f == "--refresh-ms") a.refresh_ms = std::atoi(v);
        else if (f == "--static") a.static_dir = v;
        else return false;
    }
    return a.port > 0 && a.refresh_ms >= 100;
}

std::int64_t system_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// Latest snapshot, versioned, shared by every session thread. Leaked on purpose:
// detached session threads may still be waiting on it while main() returns.
class Hub {
public:
    void publish(std::string s) {
        {
            std::lock_guard lk(m_);
            latest_ = std::make_shared<const std::string>(std::move(s));
            ++version_;
        }
        cv_.notify_all();
    }
    // Waits up to `timeout` for a version newer than `seen`.
    std::shared_ptr<const std::string> wait(std::uint64_t& seen, std::chrono::milliseconds timeout) {
        std::unique_lock lk(m_);
        cv_.wait_for(lk, timeout, [&] { return version_ != seen || g_stop.load(); });
        if (version_ == seen) return nullptr;
        seen = version_;
        return latest_;
    }
    std::shared_ptr<const std::string> latest() {
        std::lock_guard lk(m_);
        return latest_;
    }
    void wake() { cv_.notify_all(); }
    std::atomic<int> clients{0};

private:
    std::mutex m_;
    std::condition_variable cv_;
    std::shared_ptr<const std::string> latest_;
    std::uint64_t version_ = 0;
};

Hub& hub() {
    static Hub* h = new Hub;
    return *h;
}

// --- Snapshot ---------------------------------------------------------------

using Job = od::web::SnapshotInput;
using Trade = od::web::Trade;

// Latest-wins worker: fits and serialises off the reader thread, so a slow fit
// never stalls the bus drain.
class SnapshotWorker {
public:
    SnapshotWorker() : thread_([this] { loop(); }) {}
    ~SnapshotWorker() {
        {
            std::lock_guard lk(m_);
            stop_ = true;
        }
        cv_.notify_one();
        thread_.join();
    }
    void post(Job job) {
        {
            std::lock_guard lk(m_);
            pending_ = std::move(job);
        }
        cv_.notify_one();
    }

private:
    void loop() {
        od::name_current_thread("od-web-snap");
        for (;;) {
            Job job;
            {
                std::unique_lock lk(m_);
                cv_.wait(lk, [&] { return stop_ || pending_; });
                if (stop_) return;
                job = std::move(*pending_);
                pending_.reset();
            }
            const auto t0 = od::now_ns();
            std::vector<od::SmileFit> fits;
            if (job.chain) fits = od::build_surface(*job.chain);
            const double fit_ms = (od::now_ns() - t0) / 1e6;
            hub().publish(od::web::snapshot_json(job, fits, fit_ms, system_ms()).dump());
        }
    }
    std::mutex m_;
    std::condition_variable cv_;
    std::optional<Job> pending_;
    bool stop_ = false;
    std::thread thread_;  // last
};

// --- HTTP / WebSocket --------------------------------------------------------

std::string mime(const fs::path& p) {
    const auto e = p.extension().string();
    if (e == ".html") return "text/html; charset=utf-8";
    if (e == ".js" || e == ".mjs") return "text/javascript";
    if (e == ".css") return "text/css";
    if (e == ".json") return "application/json";
    if (e == ".svg") return "image/svg+xml";
    if (e == ".png") return "image/png";
    if (e == ".ico") return "image/x-icon";
    if (e == ".woff2") return "font/woff2";
    return "application/octet-stream";
}

// Resolves a request target under the static root, refusing anything that escapes it.
std::optional<fs::path> static_file(const std::string& root, std::string_view target) {
    if (root.empty()) return std::nullopt;
    target = target.substr(0, target.find('?'));
    if (target.find("..") != std::string_view::npos) return std::nullopt;
    fs::path p = fs::path(root) / fs::path(std::string(target)).relative_path();
    std::error_code ec;
    if (fs::is_directory(p, ec)) p /= "index.html";
    if (!fs::is_regular_file(p, ec)) p = fs::path(root) / "index.html";  // client-side routes
    if (!fs::is_regular_file(p, ec)) return std::nullopt;
    return p;
}

void serve_websocket(tcp::socket sock, http::request<http::string_body>& req) {
    websocket::stream<tcp::socket> ws(std::move(sock));
    ws.set_option(websocket::stream_base::decorator(
        [](websocket::response_type& r) { r.set(http::field::server, "od_web"); }));
    ws.accept(req);
    ws.text(true);
    ++hub().clients;
    struct Count {
        ~Count() { --hub().clients; }
    } count;
    std::uint64_t seen = 0;
    if (auto s = hub().latest()) ws.write(net::buffer(*s));
    while (!g_stop.load()) {
        // A client that went away shows up as a failed write on the next snapshot.
        if (auto s = hub().wait(seen, std::chrono::milliseconds(1000))) ws.write(net::buffer(*s));
    }
    beast::error_code ec;
    ws.close(websocket::close_code::going_away, ec);
}

void serve(tcp::socket sock, const Args& args) {
    beast::error_code ec;
    beast::flat_buffer buf;
    for (;;) {
        http::request<http::string_body> req;
        http::read(sock, buf, req, ec);
        if (ec) return;
        if (websocket::is_upgrade(req)) {
            if (req.target() != "/ws") return;
            serve_websocket(std::move(sock), req);
            return;
        }
        http::response<http::string_body> res{http::status::ok, req.version()};
        res.set(http::field::server, "od_web");
        res.keep_alive(req.keep_alive());
        const std::string_view target(req.target().data(), req.target().size());
        if (req.method() != http::verb::get && req.method() != http::verb::head) {
            res.result(http::status::method_not_allowed);
        } else if (target == "/api/snapshot") {
            auto s = hub().latest();
            res.set(http::field::content_type, "application/json");
            res.set(http::field::cache_control, "no-store");
            res.body() = s ? *s : "null";
        } else if (const auto p = static_file(args.static_dir, target)) {
            std::ifstream in(*p, std::ios::binary);
            std::ostringstream ss;
            ss << in.rdbuf();
            res.set(http::field::content_type, mime(*p));
            res.body() = std::move(ss).str();
        } else {
            res.result(http::status::not_found);
            res.set(http::field::content_type, "text/plain");
            res.body() = args.static_dir.empty()
                             ? "od_web: no --static directory; the UI dev server proxies /ws here\n"
                             : "not found\n";
        }
        res.prepare_payload();
        http::write(sock, res, ec);
        if (ec || !res.keep_alive()) return;
    }
}

void accept_loop(tcp::acceptor& acceptor, const Args& args) {
    od::name_current_thread("od-web-accept");
    for (;;) {
        beast::error_code ec;
        tcp::socket sock(acceptor.get_executor());
        acceptor.accept(sock, ec);
        if (g_stop.load() || !acceptor.is_open()) return;
        if (ec) continue;
        std::thread([s = std::move(sock), &args]() mutable {
            try {
                serve(std::move(s), args);
            } catch (const std::exception&) {
                // client went away mid-write: nothing to clean up
            }
        }).detach();
    }
}

// --- Bus reader --------------------------------------------------------------

// Attaches to the ticker (and trade) buses, keeps a LiveBook, and reattaches
// when the producer restarts. Owned by the main thread.
class Desk {
public:
    explicit Desk(const Args& args) : args_(args) {}

    bool attached() const { return reader_.has_value(); }

    // Drains both buses; returns how many messages it handled.
    int drain() {
        if (!reader_) {
            try_attach();
            return 0;
        }
        int n = 0;
        od::TickerUpdate u;
        for (;;) {
            const auto p = reader_->poll(u);
            if (p == od::bus::FeedBusReader::Poll::Empty) break;
            if (p == od::bus::FeedBusReader::Poll::Lapped) {
                reset_from_snapshot();
                continue;
            }
            book_->apply(u);
            hop_.record(static_cast<std::uint64_t>(std::max<std::int64_t>(0, od::now_ns() - u.parsed_ns)));
            ++updates_;
            ++n;
        }
        if (trades_) {
            od::TradeUpdate t;
            for (;;) {
                const auto p = trades_->poll(t);
                if (p == od::bus::TradeBusReader::Poll::Empty) break;
                if (p == od::bus::TradeBusReader::Poll::Lapped) {
                    trades_->seek_to_end();
                    continue;
                }
                if (t.instrument < table_.size()) {
                    recent_.push_front({table_[t.instrument].name, t.exch_ts_ms, t.taker_buy != 0,
                                        t.price, t.amount, t.iv, t.index_price});
                    if (recent_.size() > 100) recent_.pop_back();
                }
                ++trade_count_;
                ++n;
            }
        }
        if (n == 0) check_producer();
        return n;
    }

    Job job(double window_s) {
        Job j;
        j.feed.bus = args_.bus;
        if (!reader_) return j;
        j.chain = book_->to_chain(reader_->currency());
        auto& f = j.feed;
        f.stats = reader_->stats();
        f.live = book_->live_count();
        f.n_instruments = table_.size();
        f.updates_per_s = updates_ / window_s;
        f.trades_per_s = trade_count_ / window_s;
        f.hop_p50_us = hop_.percentile(0.5) / 1e3;
        f.hop_p99_us = hop_.percentile(0.99) / 1e3;
        f.laps = laps_ + reader_->laps();
        f.stale = stale_;
        j.trades.assign(recent_.begin(), recent_.end());
        updates_ = trade_count_ = 0;
        hop_.reset();
        return j;
    }

private:
    void try_attach() {
        const auto now = od::now_ns();
        if (now < next_check_) return;
        next_check_ = now + 1'000'000'000;
        try {
            reader_.emplace(od::bus::FeedBusReader::open(args_.bus));
        } catch (const std::exception&) {
            return;  // od_feedd not up yet
        }
        table_ = reader_->instruments();
        book_.emplace(table_);
        reset_from_snapshot();
        open_trades();
        std::fprintf(stderr, "od_web: attached to %s (%zu instruments)%s\n", args_.bus.c_str(),
                     table_.size(), trades_ ? ", trades too" : "");
    }

    void open_trades() {
        trades_.reset();
        const auto name = args_.trades_bus.empty() ? "od_trades_" + reader_->currency() : args_.trades_bus;
        try {
            trades_.emplace(od::bus::TradeBusReader::open(name));
            trades_->seek_to_end();
        } catch (const std::exception&) {
            // no trade bus: the tape stays empty
        }
    }

    void reset_from_snapshot() {
        for (const auto& s : reader_->snapshot()) book_->apply(s);
    }

    // A heartbeat older than 2 s means od_feedd died or restarted: reattach to a
    // new segment of the same name if one with a different epoch has appeared.
    void check_producer() {
        const auto now = od::now_ns();
        if (now < next_check_) return;
        next_check_ = now + 200'000'000;
        stale_ = now - reader_->heartbeat_ns() > 2'000'000'000;
        if (!stale_) return;
        try {
            auto fresh = od::bus::FeedBusReader::open(args_.bus);
            if (fresh.epoch() == reader_->epoch()) return;
            laps_ += reader_->laps();
            reader_ = std::move(fresh);
            table_ = reader_->instruments();
            book_.emplace(table_);
            stale_ = false;
            reset_from_snapshot();
            open_trades();
            recent_.clear();
            std::fprintf(stderr, "od_web: reattached to restarted producer on %s\n", args_.bus.c_str());
        } catch (const std::exception&) {
            // producer not back yet: keep serving the last state, flagged stale
        }
    }

    const Args& args_;
    std::optional<od::bus::FeedBusReader> reader_;
    std::optional<od::bus::TradeBusReader> trades_;
    od::InstrumentTable table_;
    std::optional<od::LiveBook> book_;
    std::deque<Trade> recent_;
    od::LatencyHistogram hop_;
    std::uint64_t updates_ = 0, trade_count_ = 0, laps_ = 0;
    std::int64_t next_check_ = 0;
    bool stale_ = false;
};

}  // namespace

int main(int argc, char** argv) {
    Args args;
    if (!parse(argc, argv, args)) {
        std::puts("usage: od_web [--bus SHM_NAME] [--trades-bus SHM_NAME] [--bind ADDR] [--port N]\n"
                  "              [--refresh-ms N] [--static DIR]");
        return 2;
    }
    struct sigaction sa {};
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    signal(SIGPIPE, SIG_IGN);  // a client closing mid-write is an error code, not a signal

    if (!args.static_dir.empty() && !fs::is_regular_file(fs::path(args.static_dir) / "index.html"))
        std::fprintf(stderr, "warning: %s/index.html not found (build the UI: cd ui && npm run build)\n",
                     args.static_dir.c_str());

    net::io_context ioc;
    tcp::acceptor acceptor(ioc);
    try {
        const tcp::endpoint ep(net::ip::make_address(args.bind), static_cast<unsigned short>(args.port));
        acceptor.open(ep.protocol());
        acceptor.set_option(net::socket_base::reuse_address(true));
        acceptor.bind(ep);
        acceptor.listen();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: cannot listen on %s:%d: %s\n", args.bind.c_str(), args.port, e.what());
        return 1;
    }
    std::fprintf(stderr, "od_web: http://%s:%d/  (WebSocket /ws, reading %s)\n",
                 args.bind == "0.0.0.0" ? "localhost" : args.bind.c_str(), args.port, args.bus.c_str());
    std::thread acceptor_thread([&] { accept_loop(acceptor, args); });

    {
        SnapshotWorker worker;
        Desk desk(args);
        od::name_current_thread("od-web-bus");
        const auto every = std::chrono::milliseconds(args.refresh_ms);
        auto window_start = std::chrono::steady_clock::now();
        bool was_attached = false;
        while (!g_stop.load()) {
            if (desk.drain() == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            const auto now = std::chrono::steady_clock::now();
            // Publish on schedule, and at once on attach so clients see data immediately.
            if (now - window_start >= every || desk.attached() != was_attached) {
                const double window_s = std::chrono::duration<double>(now - window_start).count();
                worker.post(desk.job(std::max(window_s, 1e-3)));
                window_start = now;
                was_attached = desk.attached();
            }
        }
    }

    beast::error_code ec;
    acceptor.close(ec);  // wakes the blocked accept()
    hub().wake();
    acceptor_thread.join();
    return 0;
}
