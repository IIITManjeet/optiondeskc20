#pragma once
// Portfolio files (JSON):
//
// {
//   "currency": "BTC",
//   "coin_balance": 2.0,
//   "positions": [
//     {"instrument": "BTC-27NOV26-88000-C", "qty": -5, "entry_price": 0.025},
//     {"instrument": "BTC-PERPETUAL", "qty": 20000, "entry_price": 81000}
//   ]
// }
//
// Options: qty in contracts (1 = 1 coin), entry_price in coin. Instruments ending
// in PERPETUAL are inverse perps: qty in USD notional, entry_price in USD.

#include <string>

#include "od/risk.hpp"

namespace od {

Portfolio load_portfolio(const std::string& path);

}  // namespace od
