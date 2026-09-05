#include "period_levels.h"

#include <algorithm>
#include <chrono>

PeriodLevels computePeriodLevels(const CandleSeries& candles) {
  PeriodLevels result;
  if (candles.v.empty()) return result;
  const double tailMs = candles.v.back().ts;
  // chrono's civil calendar supports years [-32767, 32767]. Reject invalid
  // timestamps before converting to integral durations.
  if (!std::isfinite(tailMs) || tailMs < 0 || tailMs > 253402300799999.0)
    return result;
  using namespace std::chrono;
  const sys_days today = floor<days>(sys_time<milliseconds>{milliseconds{(int64_t)tailMs}});
  const sys_days monday = today - days{weekday{today}.iso_encoding() - 1};
  const year_month_day date{today};
  const year_month month = date.year() / date.month();
  const std::array<sys_days, PeriodLevels::Count> starts{
      today, monday, sys_days{month / day{1}}};
  const std::array<sys_days, PeriodLevels::Count> prior{
      today - days{1}, monday - days{7}, sys_days{(month - months{1}) / day{1}}};
  const auto at = [&](sys_days start) {
    PeriodOpen open;
    open.startMs = (double)duration_cast<milliseconds>(start.time_since_epoch()).count();
    const auto it = std::lower_bound(candles.v.begin(), candles.v.end(), open.startMs,
        [](const Candle& candle, double ts) { return candle.ts < ts; });
    if (it != candles.v.end() && it->ts == open.startMs &&
        std::isfinite(it->o) && it->o > 0) {
      open.price = it->o;
      open.bar = (int)(it - candles.v.begin());
    }
    return open;
  };
  for (size_t i = 0; i < PeriodLevels::Count; ++i) {
    result.current[i] = at(starts[i]);
    result.previous[i] = at(prior[i]);
  }
  return result;
}
