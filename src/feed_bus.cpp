#include "od/feed_bus.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "od/spsc_ring.hpp"  // kCacheLine
#include "od/thread_util.hpp"

namespace od::bus {

using Word = std::atomic<std::uint64_t>;

template <class T>
constexpr std::size_t kWords = sizeof(T) / 8;

struct alignas(kCacheLine) Header {
    std::uint64_t magic;  // written last by the creator, read first by readers
    std::uint32_t version;
    std::uint32_t payload_words;
    std::uint64_t capacity;
    std::uint32_t n_instruments;
    std::uint32_t payload_tag;  // which message type this segment carries
    std::int64_t epoch_ns;
    char currency[16];

    alignas(kCacheLine) Word write_seq;  // hot: bumped on every publish
    alignas(kCacheLine) std::atomic<std::int64_t> heartbeat_ns;
    Word stats_connected, stats_frames, stats_bytes, stats_tickers, stats_dropped,
        stats_unknown, stats_heartbeats, stats_reconnects, stats_subscribed, stats_trades;
};

struct InstrumentRecord {
    char name[48];
    std::int64_t expiry_ms;
    double strike;
    std::uint64_t is_put;
};

// Ring slot. seq == n + 1 once message n is fully written; 0 while being written.
template <class T>
struct alignas(kCacheLine) Slot {
    static_assert(sizeof(T) % 8 == 0 && std::is_trivially_copyable_v<T>);
    Word seq;
    Word payload[kWords<T>];
};

// Last-value cache entry: classic seqlock, odd version while being written.
template <class T>
struct alignas(kCacheLine) LvcSlot {
    Word version;
    Word payload[kWords<T>];
};

static_assert(sizeof(Slot<TickerUpdate>) == 2 * kCacheLine);  // 8 + 96 bytes, padded to 128
static_assert(sizeof(Slot<TradeUpdate>) == 2 * kCacheLine);   // 8 + 72 bytes, padded to 128

namespace {

struct Layout {
    std::size_t instruments_off, lvc_off, slots_off, total;
};

constexpr std::size_t align_up(std::size_t x, std::size_t a) { return (x + a - 1) / a * a; }

template <class T>
Layout layout(std::size_t n_instruments, std::size_t capacity) {
    Layout l;
    l.instruments_off = align_up(sizeof(Header), kCacheLine);
    l.lvc_off = align_up(l.instruments_off + n_instruments * sizeof(InstrumentRecord), kCacheLine);
    l.slots_off = align_up(l.lvc_off + n_instruments * sizeof(LvcSlot<T>), kCacheLine);
    l.total = l.slots_off + capacity * sizeof(Slot<T>);
    return l;
}

std::string shm_path(const std::string& name) { return name.starts_with('/') ? name : "/" + name; }

[[noreturn]] void fail(const std::string& what) {
    throw std::runtime_error(what + ": " + std::strerror(errno));
}

template <class T>
void store_payload(Word* dst, const T& msg) {
    std::uint64_t w[kWords<T>];
    std::memcpy(w, &msg, sizeof msg);
    for (std::size_t i = 0; i < kWords<T>; ++i) dst[i].store(w[i], std::memory_order_relaxed);
}

template <class T>
T load_payload(const Word* src) {
    std::uint64_t w[kWords<T>];
    for (std::size_t i = 0; i < kWords<T>; ++i) w[i] = src[i].load(std::memory_order_relaxed);
    T msg;
    std::memcpy(&msg, w, sizeof msg);
    return msg;
}

const InstrumentRecord* records(const void* base, std::size_t instruments_off) {
    return reinterpret_cast<const InstrumentRecord*>(static_cast<const char*>(base) + instruments_off);
}

}  // namespace

// --- Mapping ------------------------------------------------------------------

Mapping::Mapping(Mapping&& o) noexcept
    : addr_(std::exchange(o.addr_, nullptr)), len_(std::exchange(o.len_, 0)) {}

Mapping& Mapping::operator=(Mapping&& o) noexcept {
    if (this != &o) {
        if (addr_) ::munmap(addr_, len_);
        addr_ = std::exchange(o.addr_, nullptr);
        len_ = std::exchange(o.len_, 0);
    }
    return *this;
}

Mapping::~Mapping() {
    if (addr_) ::munmap(addr_, len_);
}

// --- Writer -------------------------------------------------------------------

template <class T>
Writer<T> Writer<T>::create(const std::string& name, const std::string& currency,
                            const InstrumentTable& table, std::size_t capacity) {
    if (capacity < 2 || (capacity & (capacity - 1)) != 0)
        throw std::invalid_argument("bus capacity must be a power of two");
    const auto path = shm_path(name);
    const auto l = layout<T>(table.size(), capacity);

    // Replace a segment left behind by a crashed producer. Readers still mapping the
    // old one keep their mapping; they see its heartbeat stop and reattach.
    ::shm_unlink(path.c_str());
    const int fd = ::shm_open(path.c_str(), O_CREAT | O_EXCL | O_RDWR, 0644);
    if (fd < 0) fail("shm_open " + path);
    if (::ftruncate(fd, static_cast<off_t>(l.total)) != 0) {
        ::close(fd);
        fail("ftruncate " + path);
    }
    void* addr = ::mmap(nullptr, l.total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ::close(fd);  // the mapping keeps the segment alive
    if (addr == MAP_FAILED) fail("mmap " + path);

    Writer w;
    w.name_ = path;
    w.map_ = Mapping(addr, l.total);
    auto* base = static_cast<char*>(addr);
    // ftruncate zero-fills, so every atomic starts at 0. Construct them in place.
    w.hdr_ = new (base) Header{};
    auto* recs = reinterpret_cast<InstrumentRecord*>(base + l.instruments_off);
    w.lvc_ = reinterpret_cast<LvcSlot<T>*>(base + l.lvc_off);
    for (std::size_t i = 0; i < table.size(); ++i) new (&w.lvc_[i]) LvcSlot<T>{};
    w.slots_ = reinterpret_cast<Slot<T>*>(base + l.slots_off);
    for (std::size_t i = 0; i < capacity; ++i) new (&w.slots_[i]) Slot<T>{};
    w.mask_ = capacity - 1;

    for (std::size_t i = 0; i < table.size(); ++i) {
        const auto& inst = table[static_cast<std::uint32_t>(i)];
        auto& r = recs[i];
        std::strncpy(r.name, inst.name.c_str(), sizeof r.name - 1);
        r.expiry_ms = inst.expiry_ms;
        r.strike = inst.strike;
        r.is_put = inst.type == OptionType::Put;
    }
    auto* h = w.hdr_;
    h->version = kVersion;
    h->payload_words = kWords<T>;
    h->payload_tag = PayloadTag<T>::value;
    h->capacity = capacity;
    h->n_instruments = static_cast<std::uint32_t>(table.size());
    std::strncpy(h->currency, currency.c_str(), sizeof h->currency - 1);
    h->epoch_ns = now_ns();
    h->heartbeat_ns.store(h->epoch_ns, std::memory_order_relaxed);
    // Publish: a reader that sees the magic (acquire) sees everything above.
    std::atomic_ref<std::uint64_t>(h->magic).store(kMagic, std::memory_order_release);
    return w;
}

template <class T>
Writer<T>::Writer(Writer&& o) noexcept
    : name_(std::move(o.name_)),
      map_(std::move(o.map_)),
      hdr_(std::exchange(o.hdr_, nullptr)),
      slots_(o.slots_),
      lvc_(o.lvc_),
      mask_(o.mask_),
      seq_(o.seq_) {}

template <class T>
Writer<T>::~Writer() {
    if (hdr_) ::shm_unlink(name_.c_str());
}

template <class T>
void Writer<T>::publish(const T& msg) {
    // Ring slot (seqlock): mark busy, write payload, publish the sequence.
    Slot<T>& s = slots_[seq_ & mask_];
    s.seq.store(0, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    store_payload(s.payload, msg);
    s.seq.store(seq_ + 1, std::memory_order_release);

    // Last-value cache for this instrument.
    if (msg.instrument < hdr_->n_instruments) {
        LvcSlot<T>& c = lvc_[msg.instrument];
        const auto v = c.version.load(std::memory_order_relaxed);
        c.version.store(v + 1, std::memory_order_relaxed);  // odd: writing
        std::atomic_thread_fence(std::memory_order_release);
        store_payload(c.payload, msg);
        c.version.store(v + 2, std::memory_order_release);  // even: stable
    }

    hdr_->write_seq.store(++seq_, std::memory_order_release);
}

template <class T>
void Writer<T>::heartbeat(std::int64_t now) {
    hdr_->heartbeat_ns.store(now, std::memory_order_release);
}

template <class T>
void Writer<T>::set_stats(const deribit::FeedStats& s) {
    constexpr auto r = std::memory_order_relaxed;
    hdr_->stats_connected.store(s.connected, r);
    hdr_->stats_frames.store(s.frames, r);
    hdr_->stats_bytes.store(s.bytes, r);
    hdr_->stats_tickers.store(s.tickers, r);
    hdr_->stats_dropped.store(s.ring_full, r);
    hdr_->stats_unknown.store(s.unknown, r);
    hdr_->stats_heartbeats.store(s.heartbeats, r);
    hdr_->stats_reconnects.store(s.reconnects, r);
    hdr_->stats_subscribed.store(s.subscribed, r);
    hdr_->stats_trades.store(s.trades, r);
}

// --- Reader -------------------------------------------------------------------

template <class T>
Reader<T> Reader<T>::open(const std::string& name) {
    const auto path = shm_path(name);
    const int fd = ::shm_open(path.c_str(), O_RDONLY, 0);
    if (fd < 0) fail("shm_open " + path + " (is od_feedd running?)");
    struct stat st {};
    if (::fstat(fd, &st) != 0) {
        ::close(fd);
        fail("fstat " + path);
    }
    const auto len = static_cast<std::size_t>(st.st_size);
    if (len < sizeof(Header)) {
        ::close(fd);
        throw std::runtime_error(path + ": segment too small");
    }
    // Read-only mapping: a buggy reader can't corrupt the feed for everyone else.
    // MAP_POPULATE maps every page now, so the hot path never takes a page fault
    // the first time it reaches a new part of the ring.
    void* addr = ::mmap(nullptr, len, PROT_READ, MAP_SHARED | MAP_POPULATE, fd, 0);
    ::close(fd);
    if (addr == MAP_FAILED) fail("mmap " + path);

    Reader r;
    r.map_ = Mapping(addr, len);
    const auto* base = static_cast<const char*>(addr);
    r.hdr_ = reinterpret_cast<const Header*>(base);
    const auto magic = std::atomic_ref<std::uint64_t>(const_cast<std::uint64_t&>(r.hdr_->magic))
                           .load(std::memory_order_acquire);
    if (magic != kMagic) throw std::runtime_error(path + ": not initialised (bad magic)");
    if (r.hdr_->version != kVersion) throw std::runtime_error(path + ": incompatible bus version");
    if (r.hdr_->payload_tag != PayloadTag<T>::value || r.hdr_->payload_words != kWords<T>)
        throw std::runtime_error(path + ": segment carries a different message type");
    const auto l = layout<T>(r.hdr_->n_instruments, r.hdr_->capacity);
    if (l.total > len) throw std::runtime_error(path + ": segment truncated");
    r.lvc_ = reinterpret_cast<const LvcSlot<T>*>(base + l.lvc_off);
    r.slots_ = reinterpret_cast<const Slot<T>*>(base + l.slots_off);
    r.mask_ = r.hdr_->capacity - 1;
    r.pos_ = r.hdr_->write_seq.load(std::memory_order_acquire);
    return r;
}

template <class T>
std::string Reader<T>::currency() const {
    return std::string(hdr_->currency, strnlen(hdr_->currency, sizeof hdr_->currency));
}

template <class T>
InstrumentTable Reader<T>::instruments() const {
    const auto* recs =
        records(map_.addr(), layout<T>(hdr_->n_instruments, hdr_->capacity).instruments_off);
    InstrumentTable t;
    for (std::uint32_t i = 0; i < hdr_->n_instruments; ++i) {
        const auto& r = recs[i];
        t.add({std::string(r.name, strnlen(r.name, sizeof r.name)), r.expiry_ms, r.strike,
               r.is_put ? OptionType::Put : OptionType::Call});
    }
    return t;
}

template <class T>
std::int64_t Reader<T>::epoch() const {
    return hdr_->epoch_ns;
}

template <class T>
std::int64_t Reader<T>::heartbeat_ns() const {
    return hdr_->heartbeat_ns.load(std::memory_order_acquire);
}

template <class T>
deribit::FeedStats Reader<T>::stats() const {
    constexpr auto r = std::memory_order_relaxed;
    deribit::FeedStats s;
    s.connected = hdr_->stats_connected.load(r) != 0;
    s.frames = hdr_->stats_frames.load(r);
    s.bytes = hdr_->stats_bytes.load(r);
    s.tickers = hdr_->stats_tickers.load(r);
    s.ring_full = hdr_->stats_dropped.load(r);
    s.unknown = hdr_->stats_unknown.load(r);
    s.heartbeats = hdr_->stats_heartbeats.load(r);
    s.reconnects = hdr_->stats_reconnects.load(r);
    s.subscribed = hdr_->stats_subscribed.load(r);
    s.trades = hdr_->stats_trades.load(r);
    return s;
}

template <class T>
std::vector<T> Reader<T>::snapshot() {
    // Position first: anything published after this point will also come through
    // the ring, so nothing can fall in the gap between snapshot and stream.
    pos_ = hdr_->write_seq.load(std::memory_order_acquire);
    std::vector<T> out;
    out.reserve(hdr_->n_instruments);
    for (std::uint32_t i = 0; i < hdr_->n_instruments; ++i) {
        const LvcSlot<T>& c = lvc_[i];
        for (int attempt = 0; attempt < 1000; ++attempt) {
            const auto v1 = c.version.load(std::memory_order_acquire);
            if (v1 == 0) break;  // never published
            if (v1 & 1) continue;  // writer mid-update
            const T msg = load_payload<T>(c.payload);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (c.version.load(std::memory_order_relaxed) == v1) {
                out.push_back(msg);
                break;
            }
        }
    }
    return out;
}

template <class T>
void Reader<T>::seek_to_end() {
    pos_ = hdr_->write_seq.load(std::memory_order_acquire);
}

template <class T>
typename Reader<T>::Poll Reader<T>::poll(T& out) {
    const auto ws = hdr_->write_seq.load(std::memory_order_acquire);
    if (pos_ == ws) return Poll::Empty;
    if (ws - pos_ > mask_) {  // the writer is a full ring ahead: our next slot is gone
        ++laps_;
        return Poll::Lapped;
    }
    const Slot<T>& s = slots_[pos_ & mask_];
    const auto s1 = s.seq.load(std::memory_order_acquire);
    if (s1 != pos_ + 1) {  // overwritten (or being overwritten) since we checked
        ++laps_;
        return Poll::Lapped;
    }
    out = load_payload<T>(s.payload);
    std::atomic_thread_fence(std::memory_order_acquire);
    if (s.seq.load(std::memory_order_relaxed) != s1) {  // torn: writer lapped us mid-copy
        ++laps_;
        return Poll::Lapped;
    }
    ++pos_;
    return Poll::Update;
}

template class Writer<TickerUpdate>;
template class Reader<TickerUpdate>;
template class Writer<TradeUpdate>;
template class Reader<TradeUpdate>;

}  // namespace od::bus
