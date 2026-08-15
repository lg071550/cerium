#include "panels.h"

#include "../../ui/theme.h"

#include <cstdio>
#include <ctime>

// cached "HH:MM:SS" for a second-resolution timestamp — tape rows re-render
// the same entries every frame; the cache keeps localtime_r + snprintf off
// that path. Direct-mapped and bounded (64 slots).
const char* TapePanel::timeLabel(int64_t secs) {
  TimeLabels::Slot& s =
      timeLabels.slots[(size_t)(((uint64_t)secs * 0x9E3779B97F4A7C15ull) >> 58)];
  if (s.key != secs + 1) {
    time_t t = (time_t)secs;
    struct tm tmv;
    localtime_r(&t, &tmv);
    s.key = secs + 1;
    snprintf(s.text, sizeof(s.text), "%02d:%02d:%02d", tmv.tm_hour, tmv.tm_min,
             tmv.tm_sec);
  }
  return s.text;
}

void drawTape(Ui& u, Rect r, TapePanel& st, Feeds& feeds) {
  const Theme& t = theme();

  Rect header{r.x, r.y, r.w, 20};
  u.draw.textAligned(header, "Time", t.textDim, DrawList::Left, 10);
  u.draw.textAligned({r.x + 80, r.y, 60, 20}, "Venue", t.textDim, DrawList::Left);
  u.draw.textAligned({r.x + 150, r.y, 90, 20}, "Price", t.textDim, DrawList::Left);
  u.draw.textAligned(header, "Amount", t.textDim, DrawList::Right, 10);
  u.draw.rect({r.x, r.y + 20, r.w, 1}, t.border);

  Rect area{r.x, r.y + 21, r.w, r.h - 21};
  const Tape& tape = feeds.tape;
  if (tape.count == 0) {
    u.draw.textAligned(area, "waiting for trades…", t.textDim, DrawList::Center);
    return;
  }

  // newest first; listView adds wheel + scrollbar when the tape overflows
  listView(u, area, (int)tape.count, 16.0f, st.list,
           [&](DrawList& d, Rect row, int i) {
             const TapeEntry* e = tape.latest((size_t)i);
             if (!e) return;
             Color c = e->side == 0 ? t.green : t.red;

             const char* timeBuf = st.timeLabel((int64_t)(e->ts / 1000.0));
             char price[32], amount[32];
             snprintf(price, sizeof(price), "%.2f", e->price);
             snprintf(amount, sizeof(amount), "%.3f", e->qty);

             const char* vtag = "?";
             if (e->venue < feeds.venues.size())
               vtag = feeds.venues[e->venue].shortLabel.c_str();

             d.textAligned(row, timeBuf, t.textDim, DrawList::Left, 10);
             d.textAligned({row.x + 80, row.y, 60, row.h}, vtag, t.textDim,
                           DrawList::Left);
             d.textAligned({row.x + 150, row.y, 90, row.h}, price, c, DrawList::Left);
             d.textAligned(row, amount, t.textDim, DrawList::Right, 10);
           });
}
