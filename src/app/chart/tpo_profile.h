#pragma once
#include "../../data/candles.h"
#include <algorithm>
#include <bitset>
#include <cmath>
#include <cstdint>
#include <map>
#include <vector>

struct TpoPriceRow {
  int64_t tick = 0;
  std::bitset<672> periods;
  int count() const { return (int)periods.count(); }
};
struct TpoProfile {
  int64_t day = 0;
  int days = 1;
  double open = 0, close = 0, ibLow = INFINITY, ibHigh = -INFINITY;
  bool partial = false;
  std::bitset<672> periods;
  int lastPeriod = 0, total = 0, poc = 0, vaLow = 0, vaHigh = 0;
  std::vector<TpoPriceRow> rows;
};
enum class TpoZoneKind { Single, Tail, PoorHigh, PoorLow, Poc, Vah, Val };
struct TpoZone {
  size_t session=0;
  int first=0, last=0, period=0;
  TpoZoneKind kind=TpoZoneKind::Single;
  int retestBar=-1;
  int retestSession=-1, retestPeriod=0, retestColumn=0;
};
struct TpoStructure {
  int lowTail=0, highTail=0;
  bool poorHigh=false, poorLow=false;
  std::vector<bool> singles;
};
struct TpoProfiles {
  int bracketMinutes=30;
  double step = 1;
  bool coarseSource = false;
  std::vector<TpoProfile> sessions;
  std::vector<TpoZone> zones;
  std::vector<TpoStructure> structures;
};

// Counts are independent of rendering and are recomputed for composites.
inline void summarizeTpo(TpoProfile& s) {
  s.total = s.poc = s.vaLow = s.vaHigh = 0;
  if (s.rows.empty()) return;
    // Deterministic POC: most TPOs, then closest to the profile midpoint,
    // then the lower price. No hash-order-dependent levels.
    const double mid = (s.rows.front().tick + s.rows.back().tick) * .5;
    for (int i = 0; i < (int)s.rows.size(); ++i) {
      s.total += s.rows[i].count();
      const auto& best = s.rows[s.poc];
      if (s.rows[i].count() > best.count() ||
          (s.rows[i].count() == best.count() &&
           std::abs(s.rows[i].tick - mid) < std::abs(best.tick - mid))) s.poc = i;
    }
    s.vaLow = s.vaHigh = s.poc;
    int covered = s.rows[s.poc].count();
    const int target = (int)std::ceil(s.total * .70);
    while (covered < target && (s.vaLow > 0 || s.vaHigh + 1 < (int)s.rows.size())) {
      const int down = s.vaLow > 0 ? s.rows[s.vaLow - 1].count() : -1;
      const int up = s.vaHigh + 1 < (int)s.rows.size() ? s.rows[s.vaHigh + 1].count() : -1;
      // Expand both sides on ties instead of systematically favoring highs.
      if (down >= up && down >= 0) covered += s.rows[--s.vaLow].count();
      if (up >= down && up >= 0) covered += s.rows[++s.vaHigh].count();
    }
}

// Position a session on the candle timeline, including a partially loaded day.
// Neither profile width nor position depends on the number of visible sessions.
inline double tpoSessionBar(const CandleSeries& cs, int64_t day) {
  const double ts = day * 86400000.0;
  const auto it = std::lower_bound(cs.v.begin(), cs.v.end(), ts,
      [](const Candle& c, double t) { return c.ts < t; });
  if (it == cs.v.end()) return (double)cs.v.size();
  return (double)(it - cs.v.begin()) + (ts - it->ts) / (cs.tf.value * 60000.0);
}

inline double tpoRowStep(const CandleSeries& cs, int targetRows) {
  double lo = INFINITY, hi = -INFINITY;
  for (const auto& c : cs.v) {
    if (!std::isfinite(c.l) || !std::isfinite(c.h)) continue;
    lo = std::min(lo, c.l); hi = std::max(hi, c.h);
  }
  const double raw = (hi - lo) / std::clamp(targetRows, 16, 512);
  if (!(raw > 0) || !std::isfinite(raw)) return 1;
  const double unit = std::pow(10.0, std::floor(std::log10(raw)));
  const double q = raw / unit;
  return (q <= 1 ? 1 : q <= 2 ? 2 : q <= 2.5 ? 2.5 : q <= 5 ? 5 : 10) * unit;
}

inline TpoProfiles buildTpoProfiles(const CandleSeries& cs, int first, int last,
                                    int bracketMinutes, double step) {
  TpoProfiles result;
  result.step = step;
  if (!(step > 0) || !std::isfinite(step) || cs.v.empty()) return result;
  bracketMinutes = bracketMinutes == 15 || bracketMinutes == 60 ? bracketMinutes : 30;
  result.bracketMinutes=bracketMinutes;
  result.coarseSource = cs.tf.kind != Timeframe::Time || !(cs.tf.value > 0) || !std::isfinite(cs.tf.value) || cs.tf.value > bracketMinutes ||
                        std::fmod((double)bracketMinutes, cs.tf.value) != 0;
  if (result.coarseSource) return result;
  first = std::clamp(first, 0, (int)cs.v.size() - 1);
  last = std::clamp(last, first, (int)cs.v.size() - 1);
  std::map<int64_t, std::bitset<672>> rows;
  auto finish = [&] {
    if (result.sessions.empty()) return;
    auto& s = result.sessions.back();
    for (const auto& [tick, periods] : rows) s.rows.push_back({tick, periods});
    rows.clear();
    if (s.rows.empty()) return;
    summarizeTpo(s);
  };
  for (int i = first; i <= last; ++i) {
    const auto& c = cs.v[i];
    if (!std::isfinite(c.ts) || c.ts <= 0 || c.ts > 253402300799999.0 || !std::isfinite(c.l) ||
        !std::isfinite(c.h) || c.h < c.l || c.l <= 0) continue;
    const int64_t day = (int64_t)std::floor(c.ts / 86400000.0);
    if (result.sessions.empty() || result.sessions.back().day != day) {
      finish();
      TpoProfile s; s.day = day; s.open = c.o;
      s.partial = c.ts > day * 86400000.0;
      result.sessions.push_back(s);
    }
    auto& session = result.sessions.back();
    session.close = c.c;
    const int period = std::clamp((int)((c.ts - day * 86400000.0) / (bracketMinutes * 60000.0)), 0, 95);
    session.lastPeriod = std::max(session.lastPeriod, period);
    session.periods.set(period);
    if (c.ts - day * 86400000.0 < 3600000 && !session.partial) {
      session.ibLow = std::min(session.ibLow, c.l); session.ibHigh = std::max(session.ibHigh, c.h);
    }
    const double low = std::floor(c.l / step), high = std::floor(c.h / step);
    if (std::abs(low) > 9e15 || std::abs(high) > 9e15 || high - low > 8192) continue;
    for (int64_t tick = (int64_t)low; tick <= (int64_t)high; ++tick) rows[tick].set(period);
  }
  finish();
  return result;
}

// Join only adjacent UTC days. Each day's brackets retain distinct time slots.
inline TpoProfiles mergeTpoProfiles(const TpoProfiles& source, const std::vector<int>& joins,
                                    int minutes) {
  TpoProfiles out; out.bracketMinutes=minutes; out.step=source.step; out.coarseSource=source.coarseSource;
  for (const auto& next : source.sessions) {
    if (out.sessions.empty() || out.sessions.back().day+out.sessions.back().days != next.day ||
        out.sessions.back().days >= 7 ||
        std::find(joins.begin(),joins.end(),(int)next.day-1)==joins.end()) {
      out.sessions.push_back(next); continue;
    }
    auto& s=out.sessions.back();
    const int offset=s.days*(1440/minutes);
    std::map<int64_t,std::bitset<672>> rows;
    for(const auto& r:s.rows) rows[r.tick]=r.periods;
    for(const auto& r:next.rows) rows[r.tick] |= r.periods << offset;
    s.rows.clear();
    for(const auto& [tick,periods]:rows) s.rows.push_back({tick,periods});
    s.periods |= next.periods << offset;
    s.lastPeriod=offset+next.lastPeriod; ++s.days; s.close=next.close;
    s.partial |= next.partial;
    summarizeTpo(s);
  }
  return out;
}

inline TpoStructure tpoStructure(const TpoProfile& s) {
  TpoStructure a; const int n=(int)s.rows.size(); a.singles.resize(n);
  if (!n) return a;
  a.poorLow=s.rows.front().count()>=2; a.poorHigh=s.rows.back().count()>=2;
  while(a.lowTail<n && s.rows[a.lowTail].count()==1 &&
      (a.lowTail==0 || s.rows[a.lowTail].tick==s.rows[a.lowTail-1].tick+1)) ++a.lowTail;
  while(a.highTail<n && s.rows[n-1-a.highTail].count()==1 &&
      (a.highTail==0 || s.rows[n-1-a.highTail].tick+1==s.rows[n-a.highTail].tick)) ++a.highTail;
  // Interior one-bracket rows, excluding unfinished last-bracket prints and extremes.
  for(int i=a.lowTail;i<n-a.highTail;++i)
    a.singles[i]=s.rows[i].count()==1 && !s.rows[i].periods.test(s.lastPeriod);
  if(a.lowTail<2 || a.lowTail==n) a.lowTail=0;
  if(a.highTail<2 || a.highTail==n) a.highTail=0;
  return a;
}

// Find the earliest later completed candle overlapping the requested price row.
inline int tpoRetestBar(const CandleSeries& candles, double after, double low, double high, double completedThrough=INFINITY) {
  auto it=std::lower_bound(candles.v.begin(),candles.v.end(),after,
      [](const Candle& c,double ts){return c.ts<ts;});
  for(;it!=candles.v.end();++it)
    if(it->ts+ candles.tf.value*60000.0 <= completedThrough && std::isfinite(it->l) && std::isfinite(it->h) && it->h>=low && it->l<high)
      return (int)(it-candles.v.begin());
  return -1;
}
// Locate the actual intersecting TPO cell, not its candle-time coordinate.
// Compact rows pack occupied periods; split rows retain their period columns.
inline void locateTpoRetest(TpoZone& z,const TpoProfiles& model,const CandleSeries& candles) {
  if(z.retestBar<0) return;
  const auto& candle=candles.v[z.retestBar];
  const auto tick=model.sessions[z.session].rows[z.first].tick;
  for(size_t si=z.session+1;si<model.sessions.size();++si) {
    const auto& s=model.sessions[si];
    if(candle.ts<s.day*86400000.0 || candle.ts>=(s.day+s.days)*86400000.0) continue;
    const auto row=std::lower_bound(s.rows.begin(),s.rows.end(),tick,
        [](const TpoPriceRow& r,int64_t t){return r.tick<t;});
    if(row==s.rows.end() || row->tick!=tick) return;
    const int period=(int)((candle.ts-s.day*86400000.0)/(model.bracketMinutes*60000.0));
    if(period<0 || period>=(int)row->periods.size() || !row->periods.test(period)) return;
    int column=0;
    for(int p=0;p<period;++p) column+=row->periods.test(p);
    z.retestSession=(int)si; z.retestPeriod=period; z.retestColumn=column;
    return;
  }
}
inline void buildTpoZones(TpoProfiles& model,const CandleSeries& candles,
                          double completedThrough=INFINITY) {
  model.zones.clear();
  model.structures.clear();
  model.structures.reserve(model.sessions.size());
  for(const auto& s:model.sessions) model.structures.push_back(tpoStructure(s));
  for(size_t si=0;si<model.sessions.size();++si) {
    const auto& s=model.sessions[si]; if(s.rows.empty()) continue;
    const auto& structure=model.structures[si];
    auto add=[&](int row,int period,TpoZoneKind kind) {
      const int hit=tpoRetestBar(candles,(s.day+s.days)*86400000.0,
          s.rows[row].tick*model.step,(s.rows[row].tick+1)*model.step,completedThrough);
      TpoZone z{si,row,row,period,kind,hit};
      locateTpoRetest(z,model,candles);
      // Join only rows with the same source and exact right edge. A partial
      // revisit never truncates the untouched remainder of a larger zone.
      if(!model.zones.empty()) {
        auto& prev=model.zones.back();
        if(prev.session==si && prev.kind==kind && prev.period==period &&
            prev.last+1==row && s.rows[prev.last].tick+1==s.rows[row].tick &&
            prev.retestBar==z.retestBar && prev.retestSession==z.retestSession &&
            prev.retestColumn==z.retestColumn && prev.retestPeriod==z.retestPeriod) {
          prev.last=row; return;
        }
      }
      model.zones.push_back(z);
    };
    for(int i=0;i<(int)s.rows.size();++i) {
      const bool tail=i<structure.lowTail || i>=(int)s.rows.size()-structure.highTail;
      if(!tail && !structure.singles[i]) continue;
      int period=0; while(period<s.lastPeriod && !s.rows[i].periods.test(period)) ++period;
      add(i,period,tail?TpoZoneKind::Tail:TpoZoneKind::Single);
    }
    add(s.poc,0,TpoZoneKind::Poc);
    add(s.vaHigh,0,TpoZoneKind::Vah);
    add(s.vaLow,0,TpoZoneKind::Val);
    if(structure.poorHigh) add((int)s.rows.size()-1,0,TpoZoneKind::PoorHigh);
    if(structure.poorLow) add(0,0,TpoZoneKind::PoorLow);
  }
}

inline double tpoRetestRight(const TpoProfiles& model,const CandleSeries& candles,
                             const TpoZone& zone,bool split,double startBar,double barWidth) {
  if(zone.retestSession<0) return INFINITY;
  const double dayWidth=(1440.0/candles.tf.value)*barWidth;
  const double cellWidth=dayWidth/(1440/model.bracketMinutes+12.0);
  const auto& s=model.sessions[zone.retestSession];
  const int column=split?zone.retestPeriod:zone.retestColumn;
  const double base=(tpoSessionBar(candles,s.day)-startBar)*barWidth;
  // At very small compact scales rows are drawn as a continuous strip.
  return base + (column+1)*cellWidth - ((!split && cellWidth<1.8)?0:cellWidth-std::max(.7,cellWidth-1));
}
