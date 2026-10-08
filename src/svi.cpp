#include "od/svi.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#include "od/nelder_mead.hpp"

namespace od {

double SviParams::w(double k) const {
    const double x = k - m;
    return a + b * (rho * x + std::sqrt(x * x + s * s));
}

double SviParams::w1(double k) const {
    const double x = k - m;
    return b * (rho + x / std::sqrt(x * x + s * s));
}

double SviParams::w2(double k) const {
    const double x = k - m;
    const double r2 = x * x + s * s;
    return b * s * s / (r2 * std::sqrt(r2));
}

double SviParams::iv(double k, double T) const {
    const double tv = w(k);
    return tv > 0.0 ? std::sqrt(tv / T) : 0.0;
}

double SviParams::g(double k) const {
    const double tv = w(k), d1 = w1(k), d2 = w2(k);
    const double t = 1.0 - k * d1 / (2.0 * tv);
    return t * t - 0.25 * d1 * d1 * (1.0 / tv + 0.25) + 0.5 * d2;
}

bool SviParams::is_valid() const {
    return b >= 0.0 && std::abs(rho) < 1.0 && s > 0.0 &&
           a + b * s * std::sqrt(1.0 - rho * rho) >= 0.0 && b * (1.0 + std::abs(rho)) <= 2.0;
}

double min_durrleman_g(const SviParams& p, double k_lo, double k_hi, int n) {
    double best = std::numeric_limits<double>::infinity();
    for (int i = 0; i < n; ++i) {
        const double k = k_lo + (k_hi - k_lo) * i / (n - 1);
        best = std::min(best, p.g(k));
    }
    return best;
}

namespace {

// Solve the 3x3 symmetric system A x = rhs by Cramer's rule. Returns false if singular.
bool solve3(const std::array<std::array<double, 3>, 3>& A, const std::array<double, 3>& rhs,
            std::array<double, 3>& x) {
    auto det3 = [](const std::array<std::array<double, 3>, 3>& M) {
        return M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1]) -
               M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0]) +
               M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0]);
    };
    const double d = det3(A);
    if (std::abs(d) < 1e-300) return false;
    for (int c = 0; c < 3; ++c) {
        auto M = A;
        for (int r = 0; r < 3; ++r) M[r][c] = rhs[r];
        x[c] = det3(M) / d;
    }
    return true;
}

struct Inner {
    SviParams p;
    double sse;
};

// For fixed (m, s): y = (k - m)/s, z = sqrt(y^2 + 1), and w = a + d*y + c*z with
// c = b*s, d = rho*b*s. Fit (a, d, c) by weighted least squares, then project onto
// the no-arbitrage domain: c in [0, 2s], |d| <= min(c, 2s - c), a >= -sqrt(c^2 - d^2).
Inner solve_inner(std::span<const SviQuote> q, double m, double s) {
    std::array<std::array<double, 3>, 3> A{};
    std::array<double, 3> rhs{};
    for (const auto& e : q) {
        const double y = (e.k - m) / s;
        const double z = std::sqrt(y * y + 1.0);
        const std::array<double, 3> row{1.0, y, z};
        for (int i = 0; i < 3; ++i) {
            rhs[i] += e.weight * row[i] * e.w;
            for (int j = 0; j < 3; ++j) A[i][j] += e.weight * row[i] * row[j];
        }
    }
    std::array<double, 3> x{0.0, 0.0, 0.0};
    if (!solve3(A, rhs, x)) return {{}, std::numeric_limits<double>::infinity()};
    double a = x[0], d = x[1], c = x[2];

    const double c_proj = std::clamp(c, 0.0, 2.0 * s);
    const double d_lim = std::min(c_proj, 2.0 * s - c_proj);
    const double d_proj = std::clamp(d, -d_lim, d_lim);
    if (c_proj != c || d_proj != d) {
        // Re-fit the level with the slopes pinned to the boundary.
        c = c_proj, d = d_proj;
        double sw = 0.0, swr = 0.0;
        for (const auto& e : q) {
            const double y = (e.k - m) / s;
            sw += e.weight;
            swr += e.weight * (e.w - d * y - c * std::sqrt(y * y + 1.0));
        }
        a = swr / sw;
    }
    a = std::max(a, -std::sqrt(std::max(c * c - d * d, 0.0)));

    SviParams p;
    p.a = a;
    p.b = c / s;
    p.rho = c > 0.0 ? std::clamp(d / c, -0.999999, 0.999999) : 0.0;
    p.m = m;
    p.s = s;

    double sse = 0.0;
    for (const auto& e : q) {
        const double r = p.w(e.k) - e.w;
        sse += e.weight * r * r;
    }
    return {p, sse};
}

}  // namespace

SviFit fit_svi(std::span<const SviQuote> quotes) {
    SviFit fit;
    fit.n = static_cast<int>(quotes.size());
    if (fit.n < 5) return fit;

    const auto lowest = std::min_element(quotes.begin(), quotes.end(),
                                         [](const auto& x, const auto& y) { return x.w < y.w; });
    double k_min = quotes.front().k, k_max = quotes.front().k;
    for (const auto& e : quotes) k_min = std::min(k_min, e.k), k_max = std::max(k_max, e.k);
    const double span = std::max(k_max - k_min, 1e-3);

    auto objective = [&](const std::array<double, 2>& x) {
        const double m = x[0], s = std::exp(x[1]);
        if (s < 1e-4 || s > 5.0 || m < k_min - span || m > k_max + span)
            return std::numeric_limits<double>::max();
        return solve_inner(quotes, m, s).sse;
    };

    // The (m, s) landscape can have several local minima; a few starts are cheap.
    NmResult<2> best{{0.0, 0.0}, std::numeric_limits<double>::infinity(), 0};
    for (double m0 : {lowest->k, 0.0}) {
        for (double s0 : {0.05, 0.15, 0.4}) {
            auto r = nelder_mead<2>(objective, {m0, std::log(s0)}, {0.1 * span, 0.5});
            if (r.fx < best.fx) best = r;
        }
    }

    const Inner in = solve_inner(quotes, best.x[0], std::exp(best.x[1]));
    double sw = 0.0;
    for (const auto& e : quotes) sw += e.weight;
    fit.params = in.p;
    fit.rmse_w = std::sqrt(in.sse / sw);
    fit.ok = std::isfinite(in.sse) && in.p.is_valid();
    return fit;
}

}  // namespace od
