#include "od/deribit.hpp"

#include <curl/curl.h>

#include <fstream>
#include <memory>
#include <stdexcept>
#include <unordered_map>

namespace od::deribit {

namespace {

std::size_t on_body(char* data, std::size_t size, std::size_t n, void* user) {
    static_cast<std::string*>(user)->append(data, size * n);
    return size * n;
}

std::string http_get(const std::string& url) {
    static const bool curl_ready = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
    if (!curl_ready) throw std::runtime_error("curl_global_init failed");

    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> h(curl_easy_init(), curl_easy_cleanup);
    if (!h) throw std::runtime_error("curl_easy_init failed");

    std::string body;
    curl_easy_setopt(h.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(h.get(), CURLOPT_WRITEFUNCTION, on_body);
    curl_easy_setopt(h.get(), CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(h.get(), CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(h.get(), CURLOPT_ACCEPT_ENCODING, "");  // allow gzip
    curl_easy_setopt(h.get(), CURLOPT_USERAGENT, "optionsdesk/0.1");

    if (const CURLcode rc = curl_easy_perform(h.get()); rc != CURLE_OK)
        throw std::runtime_error(std::string("GET ") + url + ": " + curl_easy_strerror(rc));
    long status = 0;
    curl_easy_getinfo(h.get(), CURLINFO_RESPONSE_CODE, &status);
    if (status != 200)
        throw std::runtime_error("GET " + url + ": HTTP " + std::to_string(status) + " " +
                                 body.substr(0, 200));
    return body;
}

}  // namespace

nlohmann::json public_get(const std::string& method_and_query, bool testnet) {
    const std::string base = testnet ? "https://test.deribit.com/api/v2/public/"
                                     : "https://www.deribit.com/api/v2/public/";
    auto j = nlohmann::json::parse(http_get(base + method_and_query));
    if (j.contains("error")) throw std::runtime_error("deribit error: " + j["error"].dump());
    return std::move(j["result"]);
}

Snapshot fetch_snapshot(const std::string& currency, bool testnet) {
    Snapshot snap;
    snap.currency = currency;
    auto instruments = public_get(
        "get_instruments?currency=" + currency + "&kind=option&expired=false", testnet);
    snap.summary =
        public_get("get_book_summary_by_currency?currency=" + currency + "&kind=option", testnet);

    snap.instruments = nlohmann::json::array();
    for (const auto& i : instruments) {
        snap.instruments.push_back({{"instrument_name", i["instrument_name"]},
                                    {"expiration_timestamp", i["expiration_timestamp"]},
                                    {"strike", i["strike"]},
                                    {"option_type", i["option_type"]}});
    }
    return snap;
}

Snapshot load_snapshot(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open " + path);
    const auto j = nlohmann::json::parse(in);
    return {j.at("currency").get<std::string>(), j.at("instruments"), j.at("summary")};
}

void save_snapshot(const Snapshot& snap, const std::string& path) {
    std::ofstream out(path);
    if (!out) throw std::runtime_error("cannot write " + path);
    out << nlohmann::json{{"currency", snap.currency},
                          {"instruments", snap.instruments},
                          {"summary", snap.summary}}
                .dump();
}

OptionChain to_chain(const Snapshot& snap) {
    struct Meta {
        std::int64_t expiry_ms;
        double strike;
        OptionType type;
    };
    std::unordered_map<std::string, Meta> meta;
    for (const auto& i : snap.instruments) {
        meta.emplace(i.at("instrument_name").get<std::string>(),
                     Meta{i.at("expiration_timestamp").get<std::int64_t>(),
                          i.at("strike").get<double>(),
                          i.at("option_type").get<std::string>() == "call" ? OptionType::Call
                                                                           : OptionType::Put});
    }

    // Missing/null numeric fields mean "no quote".
    auto num = [](const nlohmann::json& o, const char* key) {
        const auto it = o.find(key);
        return it != o.end() && it->is_number() ? it->get<double>() : 0.0;
    };

    std::int64_t asof = 0;
    std::vector<OptionQuote> quotes;
    quotes.reserve(snap.summary.size());
    for (const auto& s : snap.summary) {
        const auto name = s.at("instrument_name").get<std::string>();
        const auto it = meta.find(name);
        if (it == meta.end()) continue;
        asof = std::max(asof, s.value("creation_timestamp", std::int64_t{0}));

        OptionQuote q;
        q.name = name;
        q.type = it->second.type;
        q.strike = it->second.strike;
        q.expiry_ms = it->second.expiry_ms;
        q.bid = num(s, "bid_price");
        q.ask = num(s, "ask_price");
        q.mark = num(s, "mark_price");
        q.mark_iv = num(s, "mark_iv") / 100.0;
        q.underlying = num(s, "underlying_price");
        q.open_interest = num(s, "open_interest");
        quotes.push_back(std::move(q));
    }
    return build_chain(snap.currency, asof, std::move(quotes));
}

InstrumentTable fetch_instrument_table(const std::string& currency, bool testnet) {
    InstrumentTable table;
    for (const auto& i :
         public_get("get_instruments?currency=" + currency + "&kind=option&expired=false", testnet)) {
        table.add({i.at("instrument_name").get<std::string>(),
                   i.at("expiration_timestamp").get<std::int64_t>(), i.at("strike").get<double>(),
                   i.at("option_type").get<std::string>() == "call" ? OptionType::Call
                                                                    : OptionType::Put});
    }
    return table;
}

}  // namespace od::deribit
