#pragma once
// Minimal Nelder-Mead simplex minimiser. Derivative-free, which suits the SVI
// objective (it has kinks from the feasibility projection).

#include <algorithm>
#include <array>
#include <cstddef>

namespace od {

template <std::size_t N>
struct NmResult {
    std::array<double, N> x;
    double fx;
    int iterations;
};

template <std::size_t N, class F>
NmResult<N> nelder_mead(F&& f, std::array<double, N> x0, std::array<double, N> step,
                        int max_iter = 2000, double ftol = 1e-14) {
    using Pt = std::array<double, N>;
    std::array<Pt, N + 1> p;
    std::array<double, N + 1> fv;
    p[0] = x0;
    for (std::size_t i = 0; i < N; ++i) {
        p[i + 1] = x0;
        p[i + 1][i] += step[i];
    }
    for (std::size_t i = 0; i <= N; ++i) fv[i] = f(p[i]);

    auto lerp = [](const Pt& a, const Pt& b, double t) {  // a + t (b - a)
        Pt r;
        for (std::size_t j = 0; j < N; ++j) r[j] = a[j] + t * (b[j] - a[j]);
        return r;
    };

    int it = 0;
    for (; it < max_iter; ++it) {
        std::array<std::size_t, N + 1> idx;
        for (std::size_t i = 0; i <= N; ++i) idx[i] = i;
        std::sort(idx.begin(), idx.end(), [&](auto a, auto b) { return fv[a] < fv[b]; });
        const std::size_t best = idx[0], worst = idx[N], second = idx[N - 1];

        if (fv[worst] - fv[best] <= ftol * (1.0 + std::abs(fv[best]))) break;

        Pt c{};
        for (std::size_t i = 0; i <= N; ++i)
            if (i != worst)
                for (std::size_t j = 0; j < N; ++j) c[j] += p[i][j] / N;

        const Pt xr = lerp(c, p[worst], -1.0);
        const double fr = f(xr);
        if (fr < fv[best]) {
            const Pt xe = lerp(c, p[worst], -2.0);
            const double fe = f(xe);
            if (fe < fr) p[worst] = xe, fv[worst] = fe;
            else p[worst] = xr, fv[worst] = fr;
        } else if (fr < fv[second]) {
            p[worst] = xr, fv[worst] = fr;
        } else {
            const bool outside = fr < fv[worst];
            const Pt xc = lerp(c, outside ? xr : p[worst], 0.5);
            const double fc = f(xc);
            if (fc < (outside ? fr : fv[worst])) {
                p[worst] = xc, fv[worst] = fc;
            } else {
                for (std::size_t i = 0; i <= N; ++i)
                    if (i != best) p[i] = lerp(p[best], p[i], 0.5), fv[i] = f(p[i]);
            }
        }
    }
    const auto b = static_cast<std::size_t>(std::min_element(fv.begin(), fv.end()) - fv.begin());
    return {p[b], fv[b], it};
}

}  // namespace od
