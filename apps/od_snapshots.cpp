// od_snapshots: turn a recorded journal into compact replay files for the demo UI.
//
//   od_snapshots --journal /mnt/d/optionsdesk-rec/btc_2026-10-18_1200.odj --out ui/public/sessions
//   od_snapshots --journal a.odj --every-s 10 --minutes 30 --id btc-quiet-sunday --out ...
//
// Replays the journal through a LiveBook and, every --every-s of recorded time,
// refits the surface and builds the snapshot od_web would have pushed live
// (od::web::snapshot_json). To fit GitHub Pages, each one is then made compact:
//   - fields the browser can recompute exactly are dropped: the Greeks of every
//     quote, and each expiry's `curve` and `points` (ui/src/lib/demo.ts rebuilds
//     them from prices, mark IVs and the SVI parameters);
//   - numbers are rounded to --digits significant digits (6 keeps every price and
//     forward as quoted);
//   - each chunk starts with a full keyframe, then holds only the changes from the
//     previous snapshot (see `diff`).
//
// Output, per session:
//   <out>/<id>/index.json        {format, id, currency, journal, start_ms, end_ms, every_s, snapshots, bytes, chunks[]}
//   <out>/<id>/NNN.ndjson.gz     one message per line, --chunk-min of recorded time per file
// and <out>/index.json is rebuilt from every session's index.json, newest first.
// --fixture FILE writes one snapshot both in full and compact, for the UI's tests.
//
// The feed block is reconstructed from the recording: ticker and trade rates per
// snapshot interval, cumulative counts, and no hop latency (that was never recorded).

#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "od/journal.hpp"
#include "od/surface.hpp"
#include "od/web_snapshot.hpp"

namespace {

namespace fs = std::filesystem;
using nlohmann::json;

constexpr int kFormat = 1;  // bump when the compact form or the patch encoding changes

struct Args {
    std::string journal, out, id, fixture;
    double every_s = 5;
    double minutes = 0;   // 0: the whole journal
    double skip_min = 0;  // start this far into the journal
    double chunk_min = 10;
    double warmup_s = 5;  // let the book fill before the first snapshot
    int digits = 6;
};

bool parse(int argc, char** argv, Args& a) {
    for (int i = 1; i < argc; ++i) {
        const std::string f = argv[i];
        const char* v = i + 1 < argc ? argv[++i] : nullptr;
        if (!v) return false;
        if (f == "--journal") a.journal = v;
        else if (f == "--out") a.out = v;
        else if (f == "--id") a.id = v;
        else if (f == "--fixture") a.fixture = v;
        else if (f == "--every-s") a.every_s = std::atof(v);
        else if (f == "--minutes") a.minutes = std::atof(v);
        else if (f == "--skip-min") a.skip_min = std::atof(v);
        else if (f == "--chunk-min") a.chunk_min = std::atof(v);
        else if (f == "--warmup-s") a.warmup_s = std::atof(v);
        else if (f == "--digits") a.digits = std::atoi(v);
        else return false;
    }
    return !a.journal.empty() && !a.out.empty() && a.every_s >= 1 && a.chunk_min > 0 && a.digits >= 3 &&
           a.digits <= 15;
}

double round_to(double x, int digits) {
    if (x == 0 || !std::isfinite(x)) return x;
    const double scale = std::pow(10.0, digits - 1 - static_cast<int>(std::floor(std::log10(std::abs(x)))));
    return std::round(x * scale) / scale;
}

// Rounds every floating-point number in place to `digits` significant digits.
// nlohmann prints the shortest round-trip form, so this is what shrinks the text.
// Forwards keep 10: the browser prices off them, and for an expiry hours away the
// Greeks move by 1e-4 when the forward moves by 1e-6.
void round_numbers(json& j, int digits) {
    if (j.is_number_float()) {
        j = round_to(j.get<double>(), digits);
    } else if (j.is_object()) {
        for (auto it = j.begin(); it != j.end(); ++it)
            if (it.key() == "forward" && it->is_number_float()) *it = round_to(it->get<double>(), 10);
            else round_numbers(*it, digits);
    } else if (j.is_array()) {
        for (auto& v : j) round_numbers(v, digits);
    }
}

// Drops what ui/src/lib/demo.ts recomputes.
void make_compact(json& snap) {
    if (!snap.contains("expiries")) return;
    for (auto& e : snap["expiries"]) {
        e.erase("curve");
        e.erase("points");
        for (auto& row : e["chain"])
            for (const char* side : {"call", "put"})
                if (row[side].is_object())
                    for (const char* g : {"delta", "gamma", "vega", "theta"}) row[side].erase(g);
    }
}

// The change from `a` to `b` (a != b) as a patch node:
//   a scalar                        replace with that value
//   {"=": v}                        replace with v (an object or an array)
//   {"o": {key: node}, "d": [keys]} object: patch or add these keys, delete those
//   {"a": {"index": node}}          array of unchanged length: patch these elements
// Anything else (an array that changed length, a type change) is a replacement.
json diff(const json& a, const json& b) {
    if (a.is_object() && b.is_object()) {
        json o = json::object();
        for (auto it = b.begin(); it != b.end(); ++it) {
            const auto old = a.find(it.key());
            if (old == a.end()) o[it.key()] = it->is_structured() ? json{{"=", *it}} : *it;
            else if (*old != *it) o[it.key()] = diff(*old, *it);
        }
        json p{{"o", std::move(o)}};
        json d = json::array();
        for (auto it = a.begin(); it != a.end(); ++it)
            if (!b.contains(it.key())) d.push_back(it.key());
        if (!d.empty()) p["d"] = std::move(d);
        return p;
    }
    if (a.is_array() && b.is_array() && a.size() == b.size()) {
        json o = json::object();
        for (std::size_t i = 0; i < a.size(); ++i)
            if (a[i] != b[i]) o[std::to_string(i)] = diff(a[i], b[i]);
        return {{"a", std::move(o)}};
    }
    return b.is_structured() ? json{{"=", b}} : b;
}

class GzFile {
public:
    explicit GzFile(const fs::path& p) : f_(gzopen(p.c_str(), "wb9")) {
        if (!f_) throw std::runtime_error("cannot write " + p.string());
    }
    GzFile(const GzFile&) = delete;
    GzFile& operator=(const GzFile&) = delete;
    ~GzFile() { gzclose(f_); }
    void line(const std::string& s) {
        gzwrite(f_, s.data(), static_cast<unsigned>(s.size()));
        gzputc(f_, '\n');
    }

private:
    gzFile f_;
};

// Rebuilds <out>/index.json from every <out>/*/index.json, newest session first.
void write_top_index(const fs::path& out) {
    json sessions = json::array();
    for (const auto& d : fs::directory_iterator(out)) {
        const auto p = d.path() / "index.json";
        if (!d.is_directory() || !fs::is_regular_file(p)) continue;
        json s = json::parse(std::ifstream(p));
        s.erase("chunks");
        sessions.push_back(std::move(s));
    }
    std::sort(sessions.begin(), sessions.end(),
              [](const json& a, const json& b) { return a["start_ms"] > b["start_ms"]; });
    std::ofstream(out / "index.json") << json{{"format", kFormat}, {"sessions", sessions}}.dump(1) << '\n';
}

std::string default_id(const std::string& journal) {
    auto s = fs::path(journal).stem().string();
    std::replace(s.begin(), s.end(), '_', '-');
    return s;
}

}  // namespace

int main(int argc, char** argv) {
    Args args;
    if (!parse(argc, argv, args)) {
        std::puts("usage: od_snapshots --journal FILE.odj --out DIR [--id NAME] [--every-s 5] [--minutes N]\n"
                  "                    [--skip-min N] [--chunk-min 10] [--warmup-s 5] [--digits 6]\n"
                  "                    [--fixture FILE.json]");
        return 2;
    }
    if (args.id.empty()) args.id = default_id(args.journal);

    od::journal::Reader reader(args.journal);
    const auto& table = reader.instruments();
    od::LiveBook book(table);
    const fs::path dir = fs::path(args.out) / args.id;
    fs::remove_all(dir);
    fs::create_directories(dir);

    const auto every_ns = static_cast<std::int64_t>(args.every_s * 1e9);
    const auto chunk_ns = static_cast<std::int64_t>(args.chunk_min * 60e9);
    std::int64_t t0 = -1, begin_ns = 0, end_ns = 0, next_snap = 0, chunk_end = 0;

    od::web::SnapshotInput in;
    in.feed.bus = fs::path(args.journal).filename().string();
    in.feed.stats.connected = true;
    in.feed.n_instruments = table.size();
    auto& st = in.feed.stats;
    std::deque<od::web::Trade> recent;
    std::uint64_t win_tickers = 0, win_trades = 0;

    std::optional<GzFile> gz;
    json chunks = json::array(), prev;
    std::int64_t first_ms = 0, last_ms = 0;
    std::uint64_t n_snaps = 0, chunk_snaps = 0, raw_bytes = 0;
    double fit_total_ms = 0;
    auto close_chunk = [&] {
        if (!gz) return;
        gz.reset();
        chunks.back()["snapshots"] = chunk_snaps;
        chunks.back()["to_ms"] = last_ms;
        chunks.back()["bytes"] = fs::file_size(dir / chunks.back()["file"].get<std::string>());
    };

    const auto wall0 = std::chrono::steady_clock::now();
    od::journal::Event ev;
    while (reader.next(ev)) {
        if (t0 < 0) {
            t0 = ev.ts_ns;
            begin_ns = t0 + static_cast<std::int64_t>(args.skip_min * 60e9);
            end_ns = args.minutes > 0 ? begin_ns + static_cast<std::int64_t>(args.minutes * 60e9) : INT64_MAX;
            next_snap = std::max(begin_ns, t0 + static_cast<std::int64_t>(args.warmup_s * 1e9));
        }
        // Snapshots are due before this event is applied: they show the book as it was.
        while (ev.ts_ns >= next_snap && next_snap <= end_ns) {
            const std::int64_t server_ms = reader.start_unix_ms() + (next_snap - t0) / 1'000'000;
            const bool keyframe = !gz || next_snap >= chunk_end;
            if (keyframe) {
                close_chunk();
                char name[32];
                std::snprintf(name, sizeof name, "%03zu.ndjson.gz", chunks.size());
                gz.emplace(dir / name);
                chunks.push_back({{"file", name}, {"from_ms", server_ms}});
                chunk_snaps = 0;
                chunk_end = next_snap + chunk_ns;
            }
            in.chain = book.to_chain(reader.currency());
            in.feed.live = book.live_count();
            in.feed.updates_per_s = win_tickers / args.every_s;
            in.feed.trades_per_s = win_trades / args.every_s;
            in.trades.assign(recent.begin(), recent.end());
            const auto f0 = std::chrono::steady_clock::now();
            const auto fits = od::build_surface(*in.chain);
            const double fit_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - f0).count();
            fit_total_ms += fit_ms;

            json full = od::web::snapshot_json(in, fits, fit_ms, server_ms);
            json snap = full;
            make_compact(snap);
            round_numbers(snap, args.digits);
            if (!args.fixture.empty() && n_snaps == 0) {
                std::ofstream(args.fixture) << json{{"full", full}, {"compact", snap}}.dump() << '\n';
            }
            const auto line = keyframe ? snap.dump() : json{{"type", "patch"}, {"patch", diff(prev, snap)}}.dump();
            raw_bytes += line.size() + 1;
            gz->line(line);
            prev = std::move(snap);

            if (n_snaps == 0) first_ms = server_ms;
            last_ms = server_ms;
            ++n_snaps;
            ++chunk_snaps;
            win_tickers = win_trades = 0;
            next_snap += every_ns;
        }
        if (ev.ts_ns > end_ns) break;

        if (ev.type == od::journal::RecordType::Ticker) {
            book.apply(ev.ticker);
            ++st.tickers;
            ++win_tickers;
        } else if (ev.type == od::journal::RecordType::Trade) {
            const auto& t = ev.trade;
            if (t.instrument < table.size()) {
                recent.push_front({table[t.instrument].name, t.exch_ts_ms, t.taker_buy != 0, t.price, t.amount,
                                   t.iv, t.index_price});
                if (recent.size() > 100) recent.pop_back();
            }
            ++st.trades;
            ++win_trades;
        }
        ++st.frames;
    }
    close_chunk();
    if (n_snaps == 0) {
        std::fprintf(stderr, "error: no snapshots: the journal is shorter than --warmup-s / --skip-min\n");
        fs::remove_all(dir);
        return 1;
    }

    std::uint64_t gz_bytes = 0;
    for (const auto& c : chunks) gz_bytes += c["bytes"].get<std::uint64_t>();
    const json index{{"format", kFormat},      {"id", args.id},
                     {"currency", reader.currency()}, {"journal", in.feed.bus},
                     {"start_ms", first_ms},   {"end_ms", last_ms},
                     {"every_s", args.every_s}, {"snapshots", n_snaps},
                     {"bytes", gz_bytes},      {"chunks", chunks}};
    std::ofstream(dir / "index.json") << index.dump(1) << '\n';
    write_top_index(args.out);

    const double wall_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - wall0).count();
    std::fprintf(stderr,
                 "%s: %llu snapshots over %.1f min in %zu chunks, %.1f MB raw -> %.2f MB gzipped "
                 "(%.1f KB/snapshot); fit %.1f ms avg; %.1f s\n",
                 args.id.c_str(), static_cast<unsigned long long>(n_snaps), (last_ms - first_ms) / 60e3,
                 chunks.size(), raw_bytes / 1e6, gz_bytes / 1e6, gz_bytes / 1e3 / n_snaps, fit_total_ms / n_snaps,
                 wall_s);
    return 0;
}
