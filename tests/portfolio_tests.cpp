// Portfolios: one market at full weight is its own backtest, sleeves without rebalancing add up, rebalancing restores
// the targets and pays for what it moves, inverse volatility favors the calm market, markets on different calendars
// trade on the dates they share, and the API reads and checks the request and answers it.

#include "check.hpp"

#include "nanotrade/api.hpp"
#include "nanotrade/engine.hpp"
#include "nanotrade/error.hpp"
#include "nanotrade/indicators.hpp"
#include "nanotrade/json.hpp"
#include "nanotrade/metrics.hpp"
#include "nanotrade/portfolio.hpp"
#include "nanotrade/research.hpp"
#include "nanotrade/series.hpp"
#include "nanotrade/strategy.hpp"
#include "nanotrade/synthetic.hpp"
#include "nanotrade/time.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

using nanotrade::BacktestConfig;
using nanotrade::BarSeries;
using nanotrade::Json;
using nanotrade::Metrics;
using nanotrade::PortfolioMarket;
using nanotrade::PortfolioResult;
using nanotrade::PortfolioSpec;
using nanotrade::Rebalance;
using nanotrade::StrategySpec;
using nanotrade::Weighting;
using Cache = nanotrade::indicators::Cache;

// Markets for a portfolio, each with the indicator cache its strategy reads.
struct Markets {
    std::vector<std::unique_ptr<BarSeries>> bars;
    std::vector<std::unique_ptr<Cache>> caches;

    void add(BarSeries series) {
        bars.push_back(std::make_unique<BarSeries>(std::move(series)));
        caches.push_back(std::make_unique<Cache>(*bars.back()));
    }
    std::vector<PortfolioMarket> list() const {
        std::vector<PortfolioMarket> markets;
        for (std::size_t i = 0; i < bars.size(); ++i) {
            markets.push_back({bars[i].get(), caches[i].get()});
        }
        return markets;
    }
};

BarSeries random_market(std::uint64_t seed, double volatility = 0.25, std::size_t bars = 1500) {
    nanotrade::SyntheticSpec spec;
    spec.seed = seed;
    spec.bars = bars;
    spec.drift = 0.06;
    spec.volatility = volatility;
    return nanotrade::synthesize(spec);
}

StrategySpec strategy(const std::string& id) {
    StrategySpec spec;
    spec.id = id;
    return spec;
}

BacktestConfig costly() {
    BacktestConfig config;
    config.commission_bps = 5.0;
    config.slippage_bps = 2.0;
    return config;
}

PortfolioResult run(const Markets& markets,
                    const StrategySpec& strategy,
                    const BacktestConfig& config,
                    const PortfolioSpec& spec,
                    std::optional<nanotrade::Timestamp> from = std::nullopt,
                    std::optional<nanotrade::Timestamp> to = std::nullopt) {
    const std::vector<PortfolioMarket> list = markets.list();
    return nanotrade::run_portfolio(list, strategy, config, spec, from, to);
}

void expect_relative(double actual, double expected, double tolerance, const std::string& message) {
    check::expect_near(actual, expected, tolerance * std::max(1.0, std::fabs(expected)), message);
}

// A month's last close in the calendar: the next bar is in another month.
bool month_end(const BarSeries& calendar, std::size_t bar) {
    if (bar + 1 >= calendar.size()) {
        return false;
    }
    const auto a = nanotrade::civil_from_timestamp(calendar.time[bar]);
    const auto b = nanotrade::civil_from_timestamp(calendar.time[bar + 1]);
    return a.year != b.year || a.month != b.month;
}

// ---- The engine ----

void one_market_at_full_weight_is_its_backtest() {
    Markets markets;
    markets.add(random_market(11));
    for (const Rebalance rebalance : {Rebalance::Never, Rebalance::Monthly}) {
        PortfolioSpec spec;
        spec.rebalance = rebalance;
        const PortfolioResult portfolio = run(markets, strategy("sma_cross"), costly(), spec);
        Cache cache(*markets.bars[0]);
        const nanotrade::BacktestReport alone =
            nanotrade::run_report(*markets.bars[0], cache, strategy("sma_cross"), costly(), 0, markets.bars[0]->size());
        check::expect(portfolio.result.equity == alone.result.equity, "the equity is the backtest's, to the bit");
        check::expect(portfolio.benchmark.equity == alone.benchmark.equity, "and buy and hold's too");
        const Metrics& a = portfolio.metrics;
        const Metrics& b = alone.metrics;
        check::expect_equal(a.sharpe, b.sharpe, "the Sharpe ratio");
        check::expect_equal(a.cagr, b.cagr, "the annual return");
        check::expect_equal(a.max_drawdown, b.max_drawdown, "the worst drawdown");
        check::expect_equal(a.trades, b.trades, "the trades");
        check::expect_equal(a.win_rate, b.win_rate, "the win rate");
        check::expect_equal(a.exposure, b.exposure, "the time in the market");
        check::expect_equal(a.turnover, b.turnover, "the turnover");
        check::expect_equal(a.costs, b.costs, "the costs");
        check::expect_equal(a.beta, b.beta, "the beta against buy and hold");
        check::expect_equal(portfolio.rebalance_cost, 0.0, "a single sleeve never moves capital");
        check::expect_equal(portfolio.sleeves[0].final_weight, 1.0, "and holds everything");
    }
}

void sleeves_without_rebalancing_add_up() {
    Markets markets;
    markets.add(random_market(1));
    markets.add(random_market(2, 0.4));
    markets.add(random_market(3, 0.15));
    PortfolioSpec spec;
    spec.weighting = Weighting::Fixed;
    spec.weights = {2.0, 1.0, 1.0};  // scaled to one half and two quarters
    spec.rebalance = Rebalance::Never;
    const PortfolioResult portfolio = run(markets, strategy("ema_cross"), costly(), spec);
    check::expect_equal(portfolio.rebalances, std::size_t{0}, "no rebalancing");
    const std::vector<double> weights = {0.5, 0.25, 0.25};
    for (std::size_t k = 0; k < portfolio.result.equity.size(); ++k) {
        double sum = 0.0;
        for (std::size_t i = 0; i < 3; ++i) {
            sum += portfolio.sleeves[i].report.result.equity[k] * weights[i];
        }
        expect_relative(portfolio.result.equity[k], sum, 1e-12, "the portfolio is its sleeves, weighted");
    }
    double drift = 0.0;
    for (std::size_t i = 0; i < 3; ++i) {
        drift += std::fabs(portfolio.sleeves[i].final_weight - weights[i]);
    }
    check::expect(drift > 0.01, "and its weights drift");
}

void rebalancing_restores_the_targets() {
    Markets markets;
    markets.add(random_market(4));
    markets.add(random_market(5, 0.35));
    PortfolioSpec spec;
    spec.weighting = Weighting::Fixed;
    spec.weights = {0.7, 0.3};
    spec.rebalance = Rebalance::Monthly;
    const PortfolioResult portfolio = run(markets, strategy("buy_hold"), BacktestConfig{}, spec);
    std::size_t month_ends = 0;
    for (std::size_t k = 0; k + 1 < portfolio.result.equity.size(); ++k) {
        if (month_end(portfolio.calendar, portfolio.result.begin + k)) {
            ++month_ends;
            check::expect_near(portfolio.sleeves[0].weight[k], 0.7, 1e-12, "a month's last close restores 70%");
            check::expect_near(portfolio.sleeves[1].weight[k], 0.3, 1e-12, "and 30%");
        }
    }
    check::expect_equal(portfolio.rebalances, month_ends, "it rebalances at every month's last close");
    check::expect(std::fabs(portfolio.sleeves[0].weight[10] - 0.7) > 1e-6, "and drifts between them");
}

void rebalancing_pays_for_what_it_moves() {
    Markets markets;
    markets.add(random_market(6));
    markets.add(random_market(7, 0.45));
    PortfolioSpec spec;
    spec.rebalance = Rebalance::Quarterly;
    BacktestConfig config;
    config.commission_bps = 10.0;
    config.slippage_bps = 5.0;
    const PortfolioResult portfolio = run(markets, strategy("buy_hold"), config, spec);
    check::expect(portfolio.rebalances > 10, "quarterly rebalancing over six years");
    expect_relative(portfolio.rebalance_cost,
                    portfolio.rebalance_traded * 15.0 / 10'000.0,
                    1e-12,
                    "it pays 15 basis points on what it moves");
    double contributions = 0.0;
    for (const auto& sleeve : portfolio.sleeves) {
        contributions += sleeve.contribution;
    }
    expect_relative(contributions - portfolio.rebalance_cost / config.initial_capital,
                    portfolio.metrics.total_return,
                    1e-12,
                    "the sleeves' gains, less rebalancing, are the portfolio's return");
    spec.rebalance = Rebalance::Never;
    const PortfolioResult unbalanced = run(markets, strategy("buy_hold"), config, spec);
    check::expect(unbalanced.metrics.costs < portfolio.metrics.costs, "rebalancing adds to the costs");
}

void a_flat_sleeve_moves_cash_for_free() {
    // A strategy that's never in the market holds cash only: moving it between sleeves trades nothing.
    Markets markets;
    markets.add(random_market(8));
    markets.add(random_market(9));
    StrategySpec never = strategy("custom");
    never.rules.long_entry = "close < 0";
    PortfolioSpec spec;
    spec.weighting = Weighting::Fixed;
    spec.weights = {0.9, 0.1};
    const PortfolioResult portfolio = run(markets, never, costly(), spec);
    check::expect_equal(portfolio.rebalance_traded, 0.0, "nothing is bought or sold");
    check::expect_equal(portfolio.metrics.final_equity, BacktestConfig{}.initial_capital, "and nothing is lost");
}

void inverse_volatility_favors_the_calm_market() {
    Markets markets;
    markets.add(random_market(12, 0.1));
    markets.add(random_market(13, 0.4));
    PortfolioSpec spec;
    spec.weighting = Weighting::InverseVolatility;
    const nanotrade::Timestamp start = markets.bars[0]->time[300];
    const PortfolioResult portfolio = run(markets, strategy("buy_hold"), BacktestConfig{}, spec, start);
    const double calm = portfolio.sleeves[0].target;
    const double wild = portfolio.sleeves[1].target;
    check::expect_near(calm + wild, 1.0, 1e-12, "the weights add up to one");
    check::expect(calm > 0.7 && calm < 0.9,
                  "a market a quarter as volatile gets about four fifths: " + std::to_string(calm));
    // Without history before the window, the first weights are equal.
    const PortfolioResult fresh = run(markets, strategy("buy_hold"), BacktestConfig{}, spec);
    check::expect_equal(fresh.sleeves[0].target, 0.5, "equal until there's history to measure");
    check::expect(fresh.sleeves[0].weight[100] > 0.7, "then inverse volatility from the first rebalance");
}

void markets_trade_on_the_dates_they_share() {
    Markets markets;
    const BarSeries full = random_market(14, 0.2, 400);
    BarSeries gappy;
    gappy.symbol = "gappy";
    for (std::size_t i = 0; i < full.size(); ++i) {
        if (i % 10 != 3) {  // a tenth of the days missing
            gappy.push_back(full.time[i], full.open[i], full.high[i], full.low[i], full.close[i], full.volume[i]);
        }
    }
    markets.add(full);
    markets.add(gappy);
    const PortfolioResult portfolio = run(markets, strategy("sma_cross"), BacktestConfig{}, PortfolioSpec{});
    check::expect_equal(portfolio.calendar.size(), gappy.size(), "the calendar is the dates they share");
    check::expect_equal(portfolio.result.equity.size(), gappy.size(), "every shared date has a close");
    check::expect_equal(portfolio.sleeves[0].end - portfolio.sleeves[0].begin,
                        full.size(),
                        "the full market still trades on all its own days");
}

void bad_portfolios_are_refused() {
    Markets markets;
    markets.add(random_market(15));
    markets.add(random_market(16));
    const auto blamed = [&](PortfolioSpec spec,
                            const std::string& field,
                            std::optional<nanotrade::Timestamp> from = {}) {
        try {
            run(markets, strategy("sma_cross"), BacktestConfig{}, spec, from);
        } catch (const nanotrade::Error& error) {
            check::expect_equal(error.field(), field, std::string("the error blames ") + field + ": " + error.what());
            return;
        }
        throw check::Failure("expected an error blaming " + field);
    };
    PortfolioSpec spec;
    spec.weighting = Weighting::Fixed;
    spec.weights = {1.0};
    blamed(spec, "portfolio.weights");
    spec.weights = {1.0, -1.0};
    blamed(spec, "portfolio.weights");
    spec.weights = {0.0, 0.0};
    blamed(spec, "portfolio.weights");
    spec = {};
    spec.volatility_window = 1;
    blamed(spec, "portfolio.volatility_window");
    blamed({}, "range", markets.bars[0]->time.back() + nanotrade::seconds_per_day);
    Markets apart;
    apart.add(random_market(17, 0.2, 100));
    BarSeries later = random_market(18, 0.2, 100);
    for (auto& t : later.time) {
        t += 400 * nanotrade::seconds_per_day;
    }
    apart.add(later);
    try {
        run(apart, strategy("sma_cross"), BacktestConfig{}, PortfolioSpec{});
        throw check::Failure("markets with no dates in common were accepted");
    } catch (const nanotrade::Error& error) {
        check::expect_equal(error.field(), std::string("portfolio.markets"), "markets with no dates in common");
    }
}

// ---- The API ----

Json answer(const std::string& request) {
    return Json::parse(nanotrade::api::handle(request));
}

void the_api_answers_a_portfolio() {
    const Json response = answer(R"({"command": "portfolio", "range": {"from": "2000-01-01"},
        "portfolio": {"markets": ["AAPL", "GE", {"dataset": "F"}], "weights": [2, 1, 1], "rebalance": "quarterly"},
        "strategy": {"id": "sma_cross"}, "config": {"commission_bps": 1, "slippage_bps": 2}})");
    check::expect(response.find("ok")->as_bool(), "the request succeeds: " + response.dump().substr(0, 300));
    const Json& result = *response.find("result");
    const Json& portfolio = *result.find("portfolio");
    check::expect_equal(portfolio.find("weighting")->as_string(), std::string("fixed"), "weights mean fixed weighting");
    check::expect_equal(portfolio.find("rebalance")->as_string(), std::string("quarterly"), "rebalanced quarterly");
    const Json& markets = *result.find("markets");
    check::expect_equal(markets.size(), std::size_t{3}, "three markets");
    check::expect_near(markets.items()[0].find("target")->as_number(), 0.5, 1e-15, "AAPL's weight, scaled");
    check::expect_equal(result.find("correlation")->size(), std::size_t{3}, "a correlation for each pair");
    const Json& series = *result.find("series");
    const std::size_t bars = static_cast<std::size_t>(result.find("range")->find("bars")->as_number());
    check::expect_equal(series.find("equity")->size(), bars, "the equity at each close");
    check::expect_equal(series.find("weights")->size(), std::size_t{3}, "and each market's weight");
    check::expect_equal(series.find("weights")->items()[1].size(), bars, "at each close");
    check::expect(result.find("calendar")->size() >= 14, "monthly returns by year");
}

void the_api_blames_the_field_at_fault() {
    const auto blamed = [](const std::string& request, const std::string& field, const std::string& words) {
        const Json response = answer(request);
        check::expect(!response.find("ok")->as_bool(), "expected an error for " + request);
        const Json& error = *response.find("error");
        check::expect_equal(error.find("field")->as_string(), field, "the error blames " + field);
        const std::string message = error.find("message")->as_string();
        check::expect(message.find(words) != std::string::npos, "the message says '" + words + "': " + message);
    };
    blamed(R"({"command": "portfolio", "portfolio": {"markets": ["AAPL", "GEE"]}})",
           "portfolio.markets[1]",
           "did you mean GE?");
    blamed(R"({"command": "portfolio", "portfolio": {"markets": ["AAPL", "F", "AAPL"]}})",
           "portfolio.markets[2]",
           "repeats AAPL");
    blamed(R"({"command": "portfolio", "data": "AAPL", "portfolio": {"markets": ["AAPL"]}})",
           "data",
           "portfolio.markets");
    blamed(R"({"command": "portfolio", "portfolio": {}})", "portfolio.markets", "list the markets");
    blamed(R"({"command": "portfolio", "portfolio": {"markets": ["AAPL", "F"], "weights": [1, 2],
        "weighting": "equal"}})",
           "portfolio.weights",
           "fixed weighting");
    blamed(R"({"command": "portfolio", "portfolio": {"markets": ["AAPL", "F"], "weighting": "inverse_volatilty"}})",
           "portfolio.weighting",
           "did you mean inverse_volatility?");
    blamed(R"({"command": "portfolio", "portfolio": {"markets": ["AAPL", "F"], "rebalance": "weekly"}})",
           "portfolio.rebalance",
           "never, monthly, quarterly, or yearly");
    blamed(R"({"command": "portfolio", "portfolio": {"markets": ["AAPL", "F"], "weights": [1, "a"]}})",
           "portfolio.weights[1]",
           "must be a number");
    blamed(R"({"command": "portfolio", "portfolio": {"markets": ["AAPL", "F"], "rebalanse": "never"}})",
           "portfolio.rebalanse",
           "did you mean rebalance?");
}

}  // namespace

int main() {
    nanotrade::api::set_data_directory(NANOTRADE_DATA_DIR);
    return check::run({
        {"one market at full weight is its own backtest, to the bit", one_market_at_full_weight_is_its_backtest},
        {"without rebalancing, the sleeves add up and drift", sleeves_without_rebalancing_add_up},
        {"rebalancing restores the target weights at each month's end", rebalancing_restores_the_targets},
        {"rebalancing pays for what it moves, and the books balance", rebalancing_pays_for_what_it_moves},
        {"moving a flat sleeve's cash costs nothing", a_flat_sleeve_moves_cash_for_free},
        {"inverse volatility favors the calm market", inverse_volatility_favors_the_calm_market},
        {"markets on different calendars trade on the dates they share", markets_trade_on_the_dates_they_share},
        {"bad portfolios are refused, blaming the field at fault", bad_portfolios_are_refused},
        {"the API answers a portfolio", the_api_answers_a_portfolio},
        {"the API blames the field at fault", the_api_blames_the_field_at_fault},
    });
}
