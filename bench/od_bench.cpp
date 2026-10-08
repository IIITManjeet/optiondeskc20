// Micro-benchmarks for the pricing hot path. Reports mean ns/op over many
// iterations; inputs vary per call so the compiler can't hoist the work.

#include <chrono>
#include <cstdio>
#include <vector>

#include "od/implied_vol.hpp"
#include "od/svi.hpp"

namespace {

volatile double g_sink;

template <class F>
void bench(const char* name, int iters, F&& f) {
    double acc = 0.0;
    for (int i = 0; i < iters / 10; ++i) acc += f(i);  // warm-up
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < iters; ++i) acc += f(i);
    const auto t1 = std::chrono::steady_clock::now();
    g_sink = acc;
    const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / iters;
    std::printf("%-28s %12.1f ns/op\n", name, ns);
}

}  // namespace

int main() {
    using od::OptionType;
    constexpr int N = 1 << 10;
    std::vector<double> strikes(N), prices(N);
    for (int i = 0; i < N; ++i) {
        strikes[i] = 60000.0 + 40000.0 * i / N;
        prices[i] = od::black76_price(OptionType::Call, 80000.0, strikes[i], 0.1, 0.5);
    }
    auto K = [&](int i) { return strikes[i & (N - 1)]; };

    bench("black76_price", 5'000'000,
          [&](int i) { return od::black76_price(OptionType::Call, 80000.0, K(i), 0.1, 0.5); });
    bench("black76_greeks", 5'000'000,
          [&](int i) { return od::black76_greeks(OptionType::Call, 80000.0, K(i), 0.1, 0.5).vega; });
    bench("implied_vol", 1'000'000, [&](int i) {
        return od::implied_vol(OptionType::Call, prices[i & (N - 1)], 80000.0, K(i), 0.1).sigma;
    });

    double iters = 0;
    for (int i = 0; i < N; ++i)
        iters += od::implied_vol(OptionType::Call, prices[i], 80000.0, strikes[i], 0.1).iterations;
    std::printf("%-28s %12.2f iterations\n", "  implied_vol mean", iters / N);

    const od::SviParams truth{0.01, 0.12, -0.35, 0.03, 0.15};
    std::vector<od::SviQuote> q;
    for (int i = 0; i <= 40; ++i) {
        const double k = -0.8 + 1.4 * i / 40;
        q.push_back({k, truth.w(k), 1.0});
    }
    bench("fit_svi (41 quotes)", 200, [&](int i) {
        q[i % q.size()].w *= 1.0 + 1e-9;  // perturb so each call does real work
        return od::fit_svi(q).params.a;
    });
}
