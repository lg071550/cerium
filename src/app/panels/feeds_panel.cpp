#include "panels.h"
#include "panels_common.h"

#include "../../ui/theme.h"

void drawFeeds(Ui& u, Rect r, FeedsPanel& st, Feeds& feeds) {
  const Theme& t = theme();

  Rect header{r.x, r.y, r.w, 20};
  u.draw.rect(header, t.panelAlt);
  const bool showStatus = r.w >= 150.0f;
  if (showStatus) {
    panelHeader(u, header, "VENUE", "STATUS");
  } else {
    u.draw.textFit({header.x + 8, header.y, header.w - 16, header.h}, "VENUE",
                   t.textDim, DrawList::Left);
    u.draw.rect({header.x, header.y + header.h, header.w, 1}, t.border);
  }

  Rect area{r.x, r.y + 21, r.w, r.h - 21};
  listView(u, area, (int)feeds.venues.size(), 24.0f, st.list,
           [&](Ui& ru, DrawList& d, Rect row, int i) {
             VenueState& v = feeds.venues[(size_t)i];

             Behavior b = behavior(ru, row, ru.id(v.id.c_str()));
             if (b.hovered) d.rect(row, t.bgHover);
             if (b.clicked) feeds.setVenueEnabled((int)i, !v.enabled);

             Color dot = v.enabled ? wireStatusColor(v.status, t) : t.textDim;
             float cy = row.y + row.h * 0.5f;
             d.rect({row.x + 10, cy - 3, 6, 6}, dot, 3.0f);
             Color nameCol = v.enabled ? t.text : t.textDim;
             Rect nameCell{row.x + 24.0f, row.y,
                           std::max(0.0f, row.w - (showStatus ? 116.0f : 30.0f)),
                           row.h};
             d.textFit(nameCell, v.label.c_str(), nameCol, DrawList::Left);
             if (showStatus)
               d.textFit({row.x + row.w - 82.0f, row.y, 72.0f, row.h},
                         v.enabled ? wireStatusText(v.status) : "off",
                         v.enabled ? wireStatusColor(v.status, t) : t.textDim,
                         DrawList::Right);
           });
}
