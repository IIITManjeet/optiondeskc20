#include "od/journal.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <stdexcept>

namespace od::journal {

namespace {
[[noreturn]] void fail(const std::string& what) {
    throw std::runtime_error(what + ": " + std::strerror(errno));
}
}  // namespace

Writer::Writer(const std::string& path, const std::string& currency, const InstrumentTable& table)
    : buf_(1 << 20) {
    f_ = std::fopen(path.c_str(), "wb");
    if (!f_) fail("open " + path);
    std::setvbuf(f_, buf_.data(), _IOFBF, buf_.size());

    FileHeader h{};
    std::memcpy(h.magic, kMagic, sizeof h.magic);
    h.version = kVersion;
    h.n_instruments = static_cast<std::uint32_t>(table.size());
    std::strncpy(h.currency, currency.c_str(), sizeof h.currency - 1);
    h.start_unix_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
    std::fwrite(&h, sizeof h, 1, f_);
    for (const auto& inst : table.all()) {
        JournalInstrument r{};
        std::strncpy(r.name, inst.name.c_str(), sizeof r.name - 1);
        r.expiry_ms = inst.expiry_ms;
        r.strike = inst.strike;
        r.is_put = inst.type == OptionType::Put;
        std::fwrite(&r, sizeof r, 1, f_);
    }
    bytes_ = sizeof h + table.size() * sizeof(JournalInstrument);
}

Writer::~Writer() {
    if (!f_) return;
    std::fflush(f_);
    ::fsync(::fileno(f_));
    std::fclose(f_);
}

void Writer::flush() { std::fflush(f_); }

void Writer::append(RecordType type, std::int64_t ts, const void* payload, std::size_t size) {
    const RecordHeader h{static_cast<std::uint16_t>(type), static_cast<std::uint16_t>(size), 0, ts};
    std::fwrite(&h, sizeof h, 1, f_);
    std::fwrite(payload, size, 1, f_);
    ++records_;
    bytes_ += sizeof h + size;
}

Reader::Reader(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) fail("open " + path);
    struct stat st {};
    if (::fstat(fd, &st) != 0) {
        ::close(fd);
        fail("fstat " + path);
    }
    len_ = static_cast<std::size_t>(st.st_size);
    if (len_ < sizeof(FileHeader)) {
        ::close(fd);
        throw std::runtime_error(path + ": not a journal (too small)");
    }
    void* addr = ::mmap(nullptr, len_, PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);
    if (addr == MAP_FAILED) fail("mmap " + path);
    map_ = bus::Mapping(addr, len_);
    ::madvise(addr, len_, MADV_SEQUENTIAL);  // replay reads front to back

    const auto* base = static_cast<const char*>(addr);
    FileHeader h;
    std::memcpy(&h, base, sizeof h);
    if (std::memcmp(h.magic, kMagic, sizeof h.magic) != 0) throw std::runtime_error(path + ": bad magic");
    if (h.version != kVersion) throw std::runtime_error(path + ": unsupported journal version");
    currency_ = std::string(h.currency, strnlen(h.currency, sizeof h.currency));
    start_unix_ms_ = h.start_unix_ms;

    const std::size_t table_end = sizeof h + h.n_instruments * sizeof(JournalInstrument);
    if (table_end > len_) throw std::runtime_error(path + ": truncated instrument table");
    for (std::uint32_t i = 0; i < h.n_instruments; ++i) {
        JournalInstrument r;
        std::memcpy(&r, base + sizeof h + i * sizeof r, sizeof r);
        table_.add({std::string(r.name, strnlen(r.name, sizeof r.name)), r.expiry_ms, r.strike,
                    r.is_put ? OptionType::Put : OptionType::Call});
    }
    first_record_ = pos_ = table_end;
}

bool Reader::next(Event& out) {
    const auto* base = static_cast<const char*>(map_.addr());
    while (pos_ + sizeof(RecordHeader) <= len_) {
        RecordHeader h;
        std::memcpy(&h, base + pos_, sizeof h);
        if (pos_ + sizeof h + h.size > len_) break;  // truncated tail
        const char* payload = base + pos_ + sizeof h;
        pos_ += sizeof h + h.size;
        out.ts_ns = h.ts_ns;
        out.type = static_cast<RecordType>(h.type);
        if (out.type == RecordType::Ticker && h.size == sizeof(TickerUpdate)) {
            std::memcpy(&out.ticker, payload, sizeof out.ticker);
            return true;
        }
        if (out.type == RecordType::Trade && h.size == sizeof(TradeUpdate)) {
            std::memcpy(&out.trade, payload, sizeof out.trade);
            return true;
        }
        // Unknown record type or size from a newer writer: skip it.
    }
    truncated_ = pos_ != len_;
    return false;
}

}  // namespace od::journal
