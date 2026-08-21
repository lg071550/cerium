// Named layout snapshots: persistence (localStorage index + per-name keys)
// and the topbar "layouts" overlay menu (save / switch / favorite / rename /
// duplicate / delete). The working layout keeps autosaving to
// cerium.layout.v1; snapshots are frozen copies loaded on demand.
#include "terminal.h"

#include "../dock/dock_layout.h"
#include "../platform/shell.h"
#include "../ui/theme.h"

#include <algorithm>
#include <cstring>

static const char* kLayoutIndexKey = "cerium.layouts.index";
static const char* kLayoutPrefix = "cerium.layouts.";

// index JSON: [{"name":"alpha","fav":1},...] — we own the format, so a
// minimal scanner is enough (names escape only \ and ")

static void jsonEscape(std::string& out, const std::string& s) {
  for (char c : s) {
    if (c == '\\' || c == '"') out += '\\';
    out += c;
  }
}

void Terminal::loadLayoutIndex() {
  m_layouts.clear();
  char* json = shell_storage_get(kLayoutIndexKey);
  if (!json) return;
  const char* p = json;
  while ((p = std::strstr(p, "\"name\":\""))) {
    p += 8;
    SavedLayout l;
    while (*p && *p != '"') {
      if (*p == '\\' && p[1]) ++p; // unescape
      l.name += *p++;
    }
    if (!*p) break;
    const char* f = std::strstr(p, "\"fav\":");
    if (!f) break;
    l.favorite = f[6] == '1';
    m_layouts.push_back(std::move(l));
    p = f + 6;
  }
  free(json);
  sortLayouts();
}

void Terminal::saveLayoutIndex() {
  std::string json = "[";
  for (size_t i = 0; i < m_layouts.size(); ++i) {
    if (i) json += ',';
    json += "{\"name\":\"";
    jsonEscape(json, m_layouts[i].name);
    json += "\",\"fav\":";
    json += m_layouts[i].favorite ? '1' : '0';
    json += '}';
  }
  json += ']';
  shell_storage_set(kLayoutIndexKey, json.c_str());
}

void Terminal::sortLayouts() {
  std::stable_sort(m_layouts.begin(), m_layouts.end(),
                   [](const SavedLayout& a, const SavedLayout& b) {
                     return a.favorite > b.favorite;
                   });
}

bool Terminal::layoutNameTaken(const std::string& name, int skip) const {
  for (size_t i = 0; i < m_layouts.size(); ++i)
    if ((int)i != skip && m_layouts[i].name == name) return true;
  return false;
}

void Terminal::saveLayoutAs(const std::string& name) {
  std::string json = dockSerialize(dock, m_titles);
  if (json.empty()) return;
  shell_storage_set((kLayoutPrefix + name).c_str(), json.c_str());
  if (!layoutNameTaken(name)) {
    m_layouts.push_back({name, false});
    saveLayoutIndex();
  }
  m_activeLayout = name;
}

void Terminal::switchToLayout(const std::string& name) {
  char* json = shell_storage_get((kLayoutPrefix + name).c_str());
  if (!json) return;
  ensurePanelsForLayout(json);
  if (!dockDeserialize(dock, json, m_titles)) {
    free(json);
    return;
  }
  free(json);
  m_activeLayout = name;
  saveLayout(); // working copy follows the snapshot from here
}

void Terminal::renameLayout(int i, const std::string& name) {
  SavedLayout& l = m_layouts[(size_t)i];
  char* json = shell_storage_get((kLayoutPrefix + l.name).c_str());
  if (json) {
    shell_storage_set((kLayoutPrefix + name).c_str(), json);
    free(json);
  }
  shell_storage_remove((kLayoutPrefix + l.name).c_str());
  if (m_activeLayout == l.name) m_activeLayout = name;
  l.name = name;
  saveLayoutIndex();
}

void Terminal::duplicateLayout(int i) {
  const SavedLayout& l = m_layouts[(size_t)i];
  char* json = shell_storage_get((kLayoutPrefix + l.name).c_str());
  if (!json) return;
  std::string base = l.name + " copy";
  std::string name = base;
  for (int n = 2; layoutNameTaken(name); ++n) name = base + " " + std::to_string(n);
  shell_storage_set((kLayoutPrefix + name).c_str(), json);
  free(json);
  m_layouts.push_back({name, false});
  saveLayoutIndex();
}

void Terminal::deleteLayout(int i) {
  const SavedLayout& l = m_layouts[(size_t)i];
  shell_storage_remove((kLayoutPrefix + l.name).c_str());
  if (m_activeLayout == l.name) m_activeLayout.clear();
  m_layouts.erase(m_layouts.begin() + i);
  saveLayoutIndex();
}

// ---------------------------------------------------------------------------
// layouts menu (overlay)
// ---------------------------------------------------------------------------

static std::string trimCopy(const std::string& s) {
  size_t a = s.find_first_not_of(' ');
  if (a == std::string::npos) return "";
  return s.substr(a, s.find_last_not_of(' ') - a + 1);
}

void Terminal::drawLayoutsMenu() {
  if (!ui.overlayOpen(m_layoutsId)) {
    if (m_layoutName.focused) {
      m_layoutName.focused = false;
      shell_ime_blur();
    }
    if (m_renameInput.focused) {
      m_renameInput.focused = false;
      shell_ime_blur();
    }
    m_renaming = -1;
    return;
  }
  const Theme& t = theme();

  // rows: header + save row + one per snapshot (+ empty hint); height follows
  // the row count, so keep the live overlay rect in sync
  int rows = (int)m_layouts.size();
  float errH = m_layoutError.empty() ? 0.0f : 16.0f;
  float h = 4 + 26 + 4 + 26 + errH + 8 + (rows > 0 ? rows * 24.0f : 24.0f) + 8;
  Rect r{m_layoutsRect.x, m_layoutsRect.y, m_layoutsRect.w, h};
  m_layoutsRect = r;
  ui.updateOverlayRect(m_layoutsId, r);

  ui.draw.shadow(r, t.radius, 14.0f, 3.0f, hexColor(0x000000, 0.45f));
  ui.draw.rect(r, t.panel, t.radius);
  ui.draw.rectOutline(r, t.border, 1.0f, t.radius);

  // header: title + active snapshot name
  Rect hdr{r.x + 4, r.y + 4, r.w - 8, 22};
  ui.draw.textAligned(hdr, "LAYOUTS", t.textDim, DrawList::Left, 8);
  ui.draw.textAligned(
      hdr, m_activeLayout.empty() ? "working copy" : m_activeLayout.c_str(),
      m_activeLayout.empty() ? t.textDim : t.accent, DrawList::Right, 8);
  ui.draw.rect({hdr.x, hdr.y + hdr.h, hdr.w, 1}, t.border);

  // save row: name field + save button (Enter in the field also applies)
  Rect field{r.x + 8, hdr.y + hdr.h + 4, r.w - 16 - 66, 26};
  if (textField(ui, field, m_layoutName, "##layoutname", "snapshot name…"))
    m_layoutError.clear();
  bool submitted = m_layoutName.submitted;
  m_layoutName.submitted = false;

  Rect saveB{field.x + field.w + 6, field.y, 60, 26};
  bool apply = button(ui, saveB, "save") || submitted;

  if (apply) {
    std::string name = trimCopy(m_layoutName.text);
    if (name.empty()) m_layoutError = "name is empty";
    else if (layoutNameTaken(name)) m_layoutError = "name already saved";
    else {
      saveLayoutAs(name);
      m_layoutName.text.clear();
      shell_ime_set("");
      m_layoutError.clear();
    }
  }
  if (!m_layoutError.empty())
    ui.draw.textAligned({r.x + 8, field.y + field.h + 2, r.w - 16, 14},
                        m_layoutError.c_str(), t.red, DrawList::Left);

  // snapshot rows
  float y = field.y + field.h + errH + 8;
  if (rows == 0) {
    ui.draw.textAligned({r.x + 8, y, r.w - 16, 24}, "no saved layouts",
                        t.textDim, DrawList::Left, 8);
    return;
  }
  ui.pushId("layoutrows");
  for (int i = 0; i < rows; ++i) {
    SavedLayout& l = m_layouts[(size_t)i];
    Rect row{r.x + 4, y, r.w - 8, 24};
    bool hov = ui.hovered(row);
    if (hov) ui.hot = ui.id("row") + (uint64_t)i;

    if (m_renaming == i) { // inline rename field replaces the row text
      Rect rf{row.x + 24, row.y - 1, row.w - 30, 26};
      if (m_renameFresh) {
        shell_ime_set(m_renameInput.text.c_str());
        shell_ime_focus(rf.x, rf.y, rf.w, rf.h);
        m_renameInput.focused = true;
        m_renameFresh = false;
      }
      textField(ui, rf, m_renameInput, "##rename");
      if (m_renameInput.submitted) {
        m_renameInput.submitted = false;
        std::string name = trimCopy(m_renameInput.text);
        if (name.empty()) m_layoutError = "name is empty";
        else if (layoutNameTaken(name, i)) m_layoutError = "name already saved";
        else {
          renameLayout(i, name);
          m_layoutError.clear();
          m_renaming = -1;
        }
      } else if (!m_renameInput.focused) {
        m_renaming = -1; // Escape / click-away cancels
      }
      y += 24;
      continue;
    }

    if (hov) ui.draw.rect(row, t.bgHover, 1.0f);
    // favorite marker: accent when on, faint until hovered when off
    Rect favR{row.x + 2, row.y, 20, 24};
    uint64_t favId = ui.id("fav") + (uint64_t)i;
    bool favHov = ui.hovered(favR);
    if (favHov) ui.hot = favId;
    ui.draw.textAligned(favR, "*",
                        l.favorite ? t.accent
                                   : withAlpha(t.textDim, favHov ? 1.0f : 0.4f),
                        DrawList::Center);
    if (favHov && ui.input.released) {
      l.favorite = !l.favorite;
      sortLayouts();
      saveLayoutIndex();
      ui.input.released = false;
      ui.popId();
      return;
    }
    ui.tip(favId, favR, l.favorite ? "unfavorite" : "favorite");

    bool active = l.name == m_activeLayout;
    ui.draw.textAligned(row, l.name.c_str(), active ? t.accent : t.text,
                        DrawList::Left, 26);

    // row menu (hover-revealed): rename / duplicate / delete
    Rect moreR{row.x + row.w - 26, row.y, 22, 24};
    bool moreHov = ui.hovered(moreR);
    if (hov) {
      uint64_t moreId = ui.id("more") + (uint64_t)i;
      if (moreHov) {
        ui.hot = moreId;
        ui.draw.rect(moreR, t.bgRaised, 1.0f);
      }
      ui.draw.textAligned(moreR, "\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2",
                          moreHov ? t.text : t.textDim, DrawList::Center);
      if (moreHov && ui.input.released) {
        int idx = i;
        std::vector<MenuItem> items = {
            {l.favorite ? "Unfavorite" : "Favorite",
             [this, idx] {
               m_layouts[(size_t)idx].favorite = !m_layouts[(size_t)idx].favorite;
               sortLayouts();
               saveLayoutIndex();
             }},
            {"Rename",
             [this, idx] {
               m_renaming = idx;
               m_renameInput.text = m_layouts[(size_t)idx].name;
               m_layoutError.clear();
               m_renameFresh = true;
             }},
            {"Duplicate", [this, idx] { duplicateLayout(idx); }},
            {"Delete", [this, idx] { deleteLayout(idx); }},
        };
        openMenu(ui.input.mouseX, ui.input.mouseY, std::move(items), m_winW, m_winH);
        ui.input.released = false;
        ui.popId();
        return;
      }
    }

    if (hov && !moreHov && !favHov && ui.input.released) { // switch
      switchToLayout(l.name);
      ui.closeOverlay(m_layoutsId);
      ui.input.released = false;
      ui.popId();
      return;
    }
    y += 24;
  }
  ui.popId();
}
