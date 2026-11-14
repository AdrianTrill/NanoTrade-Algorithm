#include "nanotrade/engine.hpp"

#include "nanotrade/error.hpp"
#include "nanotrade/indicators.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace nanotrade {

void validate(const BacktestConfig& config) {
    const auto require = [](bool valid, const char* field, const char* message) {
        if (!valid) {
            throw Error(message, std::string("config.") + field);
        }
    };
    // Written so that NaN fails every check.
    require(config.initial_capital > 0.0 && config.initial_capital <= 1e12,
            "initial_capital",
            "the initial capital must be positive");
    require(config.max_leverage >= 0.1 && config.max_leverage <= 10.0,
            "max_leverage",
            "the leverage cap must be from 0.1 to 10");
    require(config.position_size > 0.0 && config.position_size <= config.max_leverage,
            "position_size",
            "the position size must be above 0 and at most the leverage cap");
    require(config.target_volatility > 0.0 && config.target_volatility <= 2.0,
            "target_volatility",
            "the target volatility must be above 0 and at most 200%");
    require(config.volatility_window >= 2 && config.volatility_window <= 500,
            "volatility_window",
            "the volatility window must be from 2 to 500 bars");
    require(config.rebalance_threshold >= 0.0 && config.rebalance_threshold <= 1.0,
            "rebalance_threshold",
            "the rebalance threshold must be from 0 to 1");
    require(config.commission_bps >= 0.0 && config.commission_bps <= 1000.0,
            "commission_bps",
            "the commission must be from 0 to 1000 basis points");
    require(config.commission_fixed >= 0.0 && config.commission_fixed <= 1e6,
            "commission_fixed",
            "the fixed commission must not be negative");
    require(config.slippage_bps >= 0.0 && config.slippage_bps <= 1000.0,
            "slippage_bps",
            "the slippage must be from 0 to 1000 basis points");
    require(config.borrow_rate >= 0.0 && config.borrow_rate <= 1.0,
            "borrow_rate",
            "the borrow rate must be from 0 to 100% a year");
    require(config.stop_loss >= 0.0 && config.stop_loss < 1.0, "stop_loss", "the stop loss must be from 0 to 99%");
    require(config.take_profit >= 0.0 && config.take_profit <= 10.0,
            "take_profit",
            "the take profit must be from 0 to 1000%");
    require(config.trailing_stop >= 0.0 && config.trailing_stop < 1.0,
            "trailing_stop",
            "the trailing stop must be from 0 to 99%");
    require(config.risk_free_rate >= -0.1 && config.risk_free_rate <= 0.5,
            "risk_free_rate",
            "the risk-free rate must be from -10% to 50%");
    require(config.bars_per_year >= 0.0 && config.bars_per_year <= 1e6,
            "bars_per_year",
            "bars per year must be from 0 (measured) to 1,000,000");
}

namespace {

int sign(double value) noexcept {
    return value > 0.0 ? 1 : (value < 0.0 ? -1 : 0);
}

struct OpenTrade {
    bool active{false};
    Trade trade;
    double opened_shares{0.0};
    double opened_value{0.0};
    double closed_shares{0.0};
    double closed_value{0.0};
    double cash_flow{0.0};
};

class Simulator {
public:
    Simulator(const BarSeries& bars,
              std::span<const std::int8_t> direction,
              const BacktestConfig& config,
              std::size_t begin,
              std::size_t end)
        : bars_(bars), direction_(direction), config_(config), begin_(begin), end_(end) {
        result_.begin = begin;
        result_.end = end;
        result_.initial_capital = config.initial_capital;
        result_.bars_per_year = config.bars_per_year > 0.0 ? config.bars_per_year : bars.bars_per_year();
        cash_ = config.initial_capital;
        slip_ = config.slippage_bps / 10'000.0;
        commission_rate_ = config.commission_bps / 10'000.0;
        if (config.sizing == Sizing::Volatility) {
            std::vector<double> returns(bars.size(), std::numeric_limits<double>::quiet_NaN());
            for (std::size_t i = 1; i < bars.size(); ++i) {
                returns[i] = bars.close[i] / bars.close[i - 1] - 1.0;
            }
            volatility_ = indicators::stdev(returns, config.volatility_window);
            const double annualize = std::sqrt(result_.bars_per_year);
            for (double& v : volatility_) {
                v *= annualize;
            }
        }
    }

    BacktestResult run() {
        const std::size_t count = end_ - begin_;
        result_.equity.reserve(count);
        result_.position.reserve(count);
        result_.exposure.reserve(count);

        // An order decided at the close before the window fills at its first open.
        if (begin_ > 0) {
            decide(begin_ - 1, cash_);
        }
        for (std::size_t t = begin_; t < end_; ++t) {
            if (has_pending_) {
                fill_pending(t);
            }
            if (position_ != 0.0) {
                check_risk_exits(t);
            }
            if (position_ < 0.0 && config_.borrow_rate > 0.0) {
                const double cost = -position_ * bars_.close[t] * config_.borrow_rate / result_.bars_per_year;
                cash_ -= cost;
                open_.cash_flow -= cost;
                result_.borrow += cost;
            }
            if (position_ != 0.0) {
                note_excursion(bars_.low[t], bars_.high[t]);
                trail_ = position_ > 0.0 ? std::max(trail_, bars_.high[t]) : std::min(trail_, bars_.low[t]);
            }

            double equity = cash_ + position_ * bars_.close[t];
            if (equity <= 0.0 && !halted_) {
                // Ruin: whatever is left is closed at the close, and nothing more trades.
                execute(t, 0.0, bars_.close[t], "ruin");
                halted_ = true;
                result_.ruined = true;
                equity = cash_;
            }
            if (t + 1 < end_) {
                if (!halted_) {
                    decide(t, equity);
                }
            } else if (config_.close_at_end && position_ != 0.0) {
                execute(t, 0.0, bars_.close[t], "end");
                equity = cash_;
            }
            result_.equity.push_back(equity);
            result_.position.push_back(position_);
            result_.exposure.push_back(equity > 0.0 ? position_ * bars_.close[t] / equity : 0.0);
        }
        if (open_.active) {
            // Still open: valued at the last close, and marked as open.
            Trade trade = open_.trade;
            const double last = bars_.close[end_ - 1];
            trade.exit_bar = end_ - 1;
            trade.exit_price = last;
            trade.pnl = open_.cash_flow + position_ * last;
            trade.return_pct = trade.pnl / (trade.entry_price * trade.shares);
            trade.exit_reason = "open";
            trade.open = true;
            result_.trades.push_back(trade);
        }
        return std::move(result_);
    }

private:
    double vol_exposure(std::size_t t) const {
        const double vol = volatility_[t];
        if (!indicators::defined(vol) || vol <= 0.0) {
            return std::numeric_limits<double>::quiet_NaN();
        }
        return std::min(config_.max_leverage, config_.target_volatility / vol);
    }

    void decide(std::size_t t, double equity) {
        int want = direction_[t];
        if (!config_.allow_short && want < 0) {
            want = 0;
        }
        // After a stop, the strategy has to change its mind before it can enter the same way again.
        if (blocked_ != 0) {
            if (want == blocked_) {
                want = 0;
            } else {
                blocked_ = 0;
            }
        }
        const int current = sign(position_);
        double target = position_;
        const char* reason = "";
        if (want == 0) {
            if (current != 0) {
                target = 0.0;
                reason = "exit";
            }
        } else {
            const double exposure = config_.sizing == Sizing::Percent ? config_.position_size : vol_exposure(t);
            double desired = 0.0;
            if (indicators::defined(exposure) && equity > 0.0) {
                desired = want * exposure * equity / bars_.close[t];
                if (!config_.fractional_shares) {
                    desired = std::trunc(desired);
                }
            }
            if (want != current) {
                target = desired;
                reason = current == 0 ? "entry" : "reverse";
            } else if (config_.sizing == Sizing::Volatility && desired != 0.0 &&
                       std::fabs(desired - position_) > config_.rebalance_threshold * std::fabs(position_)) {
                target = desired;
                reason = "rebalance";
            }
        }
        if (target != position_) {
            has_pending_ = true;
            pending_target_ = target;
            pending_reason_ = reason;
        }
    }

    void fill_pending(std::size_t t) {
        has_pending_ = false;
        const double open = bars_.open[t];
        double target = pending_target_;
        // The order was sized at the last close; if the open gapped, it may no longer fit under the leverage cap.
        if (std::fabs(target) > std::fabs(position_) || sign(target) != sign(position_)) {
            const double equity = cash_ + position_ * open;
            if (equity <= 0.0) {
                target = 0.0;
            } else {
                const double price = open * (1.0 + slip_);
                double cap =
                    (config_.max_leverage * equity - config_.commission_fixed) / (price * (1.0 + commission_rate_));
                cap = std::max(0.0, config_.fractional_shares ? cap : std::floor(cap));
                if (std::fabs(target) > cap) {
                    target = sign(target) * cap;
                    ++result_.trimmed;
                }
            }
        }
        execute(t, target, open, pending_reason_);
    }

    void check_risk_exits(std::size_t t) {
        const bool any = config_.stop_loss > 0.0 || config_.take_profit > 0.0 || config_.trailing_stop > 0.0;
        if (!any) {
            return;
        }
        const double open = bars_.open[t];
        const double high = bars_.high[t];
        const double low = bars_.low[t];
        const double entry = open_.trade.entry_price;
        const bool long_side = position_ > 0.0;
        constexpr double infinity = std::numeric_limits<double>::infinity();

        // The protective level is the tighter of the stop loss and the trailing stop.
        double stop = long_side ? -infinity : infinity;
        const char* stop_reason = "stop_loss";
        if (config_.stop_loss > 0.0) {
            stop = long_side ? entry * (1.0 - config_.stop_loss) : entry * (1.0 + config_.stop_loss);
        }
        if (config_.trailing_stop > 0.0) {
            const double trail =
                long_side ? trail_ * (1.0 - config_.trailing_stop) : trail_ * (1.0 + config_.trailing_stop);
            if (long_side ? trail > stop : trail < stop) {
                stop = trail;
                stop_reason = "trailing_stop";
            }
        }
        double target = long_side ? infinity : -infinity;
        if (config_.take_profit > 0.0) {
            target = long_side ? entry * (1.0 + config_.take_profit) : entry * (1.0 - config_.take_profit);
        }

        // A gap through a level fills at the open. Within the bar the order of the high and low is unknown, so when
        // both levels were reached the stop is assumed to have come first.
        double price = std::numeric_limits<double>::quiet_NaN();
        const char* reason = "";
        if (long_side) {
            if (open <= stop) {
                price = open;
                reason = stop_reason;
            } else if (open >= target) {
                price = open;
                reason = "take_profit";
            } else if (low <= stop) {
                price = stop;
                reason = stop_reason;
            } else if (high >= target) {
                price = target;
                reason = "take_profit";
            }
        } else {
            if (open >= stop) {
                price = open;
                reason = stop_reason;
            } else if (open <= target) {
                price = open;
                reason = "take_profit";
            } else if (high >= stop) {
                price = stop;
                reason = stop_reason;
            } else if (low <= target) {
                price = target;
                reason = "take_profit";
            }
        }
        if (indicators::defined(price)) {
            note_excursion(price, price);
            blocked_ = sign(position_);
            execute(t, 0.0, price, reason);
        }
    }

    void note_excursion(double low, double high) {
        Trade& trade = open_.trade;
        const double entry = trade.entry_price;
        const double adverse = trade.direction > 0 ? (entry - low) / entry : (high - entry) / entry;
        const double favorable = trade.direction > 0 ? (high - entry) / entry : (entry - low) / entry;
        trade.mae = std::max(trade.mae, adverse);
        trade.mfe = std::max(trade.mfe, favorable);
    }

    void execute(std::size_t t, double target, double reference, const char* reason) {
        const double delta = target - position_;
        if (delta == 0.0) {
            return;
        }
        const int side = sign(delta);
        const double price = reference * (1.0 + side * slip_);
        const double shares = std::fabs(delta);
        const double commission = shares * price * commission_rate_ + config_.commission_fixed;
        const double slippage = shares * reference * slip_;
        cash_ -= delta * price + commission;
        result_.commissions += commission;
        result_.slippage += slippage;
        result_.traded_value += shares * price;
        result_.fills.push_back({t, delta, price, commission, slippage, reason});

        // Split the fill into the part that reduces the position and the part that opens or adds to one.
        const double reducing = sign(position_) == -side ? std::min(shares, std::fabs(position_)) : 0.0;
        const double adding = shares - reducing;
        if (reducing > 0.0) {
            const double share_of_costs = reducing / shares;
            open_.closed_shares += reducing;
            open_.closed_value += reducing * price;
            open_.cash_flow += -side * reducing * price - commission * share_of_costs;
            position_ += side * reducing;
            if (position_ == 0.0) {
                Trade trade = open_.trade;
                trade.exit_bar = t;
                trade.exit_price = open_.closed_value / open_.closed_shares;
                trade.pnl = open_.cash_flow;
                trade.return_pct = trade.pnl / (trade.entry_price * trade.shares);
                trade.exit_reason = reason;
                result_.trades.push_back(trade);
                open_ = OpenTrade{};
            }
        }
        if (adding > 0.0) {
            if (!open_.active) {
                open_.active = true;
                open_.trade.direction = side;
                open_.trade.entry_bar = t;
                trail_ = price;
            }
            open_.opened_shares += adding;
            open_.opened_value += adding * price;
            open_.cash_flow += -side * adding * price - commission * (adding / shares);
            position_ += side * adding;
            open_.trade.entry_price = open_.opened_value / open_.opened_shares;
            open_.trade.shares = std::max(open_.trade.shares, std::fabs(position_));
        }
    }

    const BarSeries& bars_;
    std::span<const std::int8_t> direction_;
    const BacktestConfig& config_;
    std::size_t begin_;
    std::size_t end_;
    BacktestResult result_;
    std::vector<double> volatility_;
    double cash_{0.0};
    double position_{0.0};
    double slip_{0.0};
    double commission_rate_{0.0};
    bool has_pending_{false};
    double pending_target_{0.0};
    const char* pending_reason_{""};
    int blocked_{0};
    bool halted_{false};
    double trail_{0.0};
    OpenTrade open_;
};

}  // namespace

BacktestResult run_backtest(const BarSeries& bars,
                            std::span<const std::int8_t> direction,
                            const BacktestConfig& config,
                            std::size_t begin,
                            std::size_t end) {
    validate(config);
    if (direction.size() != bars.size()) {
        throw Error("internal: one direction is needed per bar");
    }
    if (begin >= end || end > bars.size()) {
        throw Error("the backtest needs at least one bar in its date range", "range");
    }
    return Simulator(bars, direction, config, begin, end).run();
}

}  // namespace nanotrade
