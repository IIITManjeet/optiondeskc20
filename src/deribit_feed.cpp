#include "od/deribit_feed.hpp"

#include <sys/socket.h>

#include <algorithm>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <chrono>
#include <cstdio>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <thread>

#include "od/thread_util.hpp"
#include "od/ticker_parser.hpp"

namespace od::deribit {

namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
namespace beast = boost::beast;
namespace websocket = boost::beast::websocket;
using tcp = boost::asio::ip::tcp;
using nlohmann::json;

namespace {

constexpr int kIdHeartbeat = 1;
constexpr int kIdTest = 2;
constexpr int kIdSubscribeBase = 1000;


std::string rpc(int id, const char* method, json params) {
    return json{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", std::move(params)}}
        .dump();
}

}  // namespace

FeedStats snapshot(const FeedCounters& c) {
    constexpr auto r = std::memory_order_relaxed;
    return {c.connected.load(r), c.frames.load(r),     c.bytes.load(r),
            c.tickers.load(r),   c.ring_full.load(r),  c.unknown.load(r),
            c.heartbeats.load(r), c.reconnects.load(r), c.subscribed.load(r)};
}

Feed::Feed(FeedConfig cfg, const InstrumentTable& table, TickerSink sink, FeedCounters& counters)
    : cfg_(cfg), table_(table), sink_(std::move(sink)), counters_(counters) {
    channels_.reserve(table.size());
    for (const auto& inst : table.all()) channels_.push_back("ticker." + inst.name + ".100ms");
}

void Feed::interrupt() {
    const int fd = fd_.load();
    if (fd >= 0) ::shutdown(fd, SHUT_RDWR);  // async-signal-safe; makes the blocked read fail
}

void Feed::run(const std::atomic<bool>& stop) {
    int backoff_s = 1;
    while (!stop.load()) {
        try {
            session(stop);
            backoff_s = 1;
        } catch (const std::exception& e) {
            counters_.connected = false;
            if (stop.load()) break;
            counters_.reconnects.fetch_add(1, std::memory_order_relaxed);
            std::fprintf(stderr, "feed: %s; reconnecting in %ds\n", e.what(), backoff_s);
            for (int i = 0; i < backoff_s * 10 && !stop.load(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            backoff_s = std::min(backoff_s * 2, 30);
        }
    }
}

void Feed::session(const std::atomic<bool>& stop) {
    const std::string host = cfg_.testnet ? "test.deribit.com" : "www.deribit.com";

    net::io_context ioc;
    ssl::context ctx{ssl::context::tls_client};
    ctx.set_default_verify_paths();
    ctx.set_verify_mode(ssl::verify_peer);

    tcp::resolver resolver{ioc};
    websocket::stream<beast::ssl_stream<tcp::socket>> ws{ioc, ctx};

    net::connect(beast::get_lowest_layer(ws), resolver.resolve(host, "443"));
    auto& sock = beast::get_lowest_layer(ws);
    sock.set_option(tcp::no_delay(true));  // don't let Nagle batch our small requests
    fd_ = sock.native_handle();
    struct FdReset {
        std::atomic<int>& fd;
        ~FdReset() { fd = -1; }
    } fd_reset{fd_};

    if (!SSL_set_tlsext_host_name(ws.next_layer().native_handle(), host.c_str()))
        throw std::runtime_error("SNI setup failed");
    ws.next_layer().set_verify_callback(ssl::host_name_verification(host));
    ws.next_layer().handshake(ssl::stream_base::client);
    ws.handshake(host, "/ws/api/v2");
    ws.text(true);

    // Server sends test_request every heartbeat_s; if we don't answer, it disconnects us.
    // It also guarantees the read loop wakes up regularly even in a quiet market.
    ws.write(net::buffer(rpc(kIdHeartbeat, "public/set_heartbeat", {{"interval", cfg_.heartbeat_s}})));
    for (std::size_t i = 0, batch = 0; i < channels_.size(); i += cfg_.subscribe_batch, ++batch) {
        const auto end = std::min(channels_.size(), i + cfg_.subscribe_batch);
        json chans(std::vector<std::string>(channels_.begin() + i, channels_.begin() + end));
        ws.write(net::buffer(rpc(kIdSubscribeBase + static_cast<int>(batch), "public/subscribe",
                                 {{"channels", std::move(chans)}})));
    }
    counters_.subscribed = 0;
    counters_.connected = true;
    std::fprintf(stderr, "feed: connected to %s, subscribing %zu channels\n", host.c_str(),
                 channels_.size());

    TickerParser parser;
    TickerFields tick;
    beast::flat_buffer buf;
    while (!stop.load(std::memory_order_relaxed)) {
        buf.consume(buf.size());
        ws.read(buf);
        const std::int64_t recv_ns = now_ns();
        const auto data = buf.cdata();
        const std::string_view frame(static_cast<const char*>(data.data()), data.size());
        counters_.frames.fetch_add(1, std::memory_order_relaxed);
        counters_.bytes.fetch_add(frame.size(), std::memory_order_relaxed);

        // Hot path: ~all traffic is ticker notifications.
        if (parser.parse(frame, tick)) {
            const auto id = table_.find(tick.instrument_name);
            if (!id) {
                counters_.unknown.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            TickerUpdate u = tick.update;
            u.instrument = *id;
            u.recv_ns = recv_ns;
            u.parsed_ns = now_ns();
            counters_.tickers.fetch_add(1, std::memory_order_relaxed);
            // Never block the feed thread: if the consumer is behind, drop and count.
            // Tickers are full-state snapshots, so the next one supersedes a dropped one.
            if (!sink_(u)) counters_.ring_full.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        // Slow path: RPC responses and heartbeats.
        const json msg = json::parse(frame, nullptr, /*allow_exceptions=*/false);
        if (msg.is_discarded()) continue;

        if (const auto m = msg.find("method"); m != msg.end()) {
            if (*m == "heartbeat") {
                counters_.heartbeats.fetch_add(1, std::memory_order_relaxed);
                const auto& p = msg.at("params");
                if (p.value("type", std::string{}) == "test_request")
                    ws.write(net::buffer(rpc(kIdTest, "public/test", json::object())));
            }
        } else if (const auto id = msg.find("id"); id != msg.end()) {
            if (const auto err = msg.find("error"); err != msg.end())
                throw std::runtime_error("rpc error: " + err->dump());
            if (id->get<int>() >= kIdSubscribeBase && msg.at("result").is_array())
                counters_.subscribed.fetch_add(msg["result"].size(), std::memory_order_relaxed);
        }
    }
}

}  // namespace od::deribit
