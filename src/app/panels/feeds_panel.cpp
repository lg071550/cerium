#include "panels.h"

#include "../../ui/theme.h"

static const char* statusText(uint8_t s) {
  switch (s) {
    case wire::Connecting: return "connecting";
    case wire::Syncing: return "syncing";
    case wire::Live: return "live";
    case wire::Reconnecting: return "reconnecting";
    case wire::Error: return "error";
    default: return "offline";
  }
}

static Color statusColor(uint8_t s, const Theme& t) {
  switch (s) {
    case wire::Live: return t.green;
    case wire::Connecting:
    case wire::Syncing: return t.accent;
    case wire::Reconnecting:
    case wire::Error: return t.red;
    default: return t.textDim;
  }
}

void drawFeeds(Ui& u, Rect r, FeedsPanel& st, Feeds& feeds) {
  const Theme& t = theme();

  Rect header{r.x, r.y, r.w, 20};
  panelHeader(u, header, "Venue", "Status");

  Rect area{r.x, r.y + 21, r.w, r.h - 21};
  listView(u, area, (int)feeds.venues.size(), 24.0f, st.list,
           [&](Ui& ru, DrawList& d, Rect row, int i) {
             VenueState& v = feeds.venues[(size_t)i];

             Behavior b = behavior(ru, row, ru.id(v.id.c_str()));
             if (b.hovered) d.rect(row, t.bgHover);
             if (b.clicked) feeds.setVenueEnabled((int)i, !v.enabled);

             Color dot = v.enabled ? statusColor(v.status, t) : t.textDim;
             float cy = row.y + row.h * 0.5f;
             d.rect({row.x + 10, cy - 3, 6, 6}, dot, 3.0f);
             Color nameCol = v.enabled ? t.text : t.textDim;
             d.textAligned(row, v.label.c_str(), nameCol, DrawList::Left, 24);
             d.textAligned(row, v.enabled ? statusText(v.status) : "off",
                           v.enabled ? statusColor(v.status, t) : t.textDim,
                           DrawList::Right, 10);
           });
}
