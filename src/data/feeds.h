#pragma once

#include "book.h"
#include "tape.h"
#include "wire.h"

#include <string>
#include <vector>

// Venue registry + per-venue books + global tape. Drains the bridge ring once
// per frame and applies events. Venue indices must match feeds/main.ts.
struct VenueState {
  std::string id;
  std::string label;
  uint8_t status = wire::StatusNone; // wire::Status*
  L2Book book;
};

struct Feeds {
  std::vector<VenueState> venues;
  Tape tape;

  void init();            // registers venues + spawns the worker bridge
  void frame();           // drain + apply pending events (call once per frame)
  void requestResync(int venue);

  VenueState* venue(size_t i) { return i < venues.size() ? &venues[i] : nullptr; }

private:
  void apply(const wire::Event& e);

  bool m_started = false;
  // snapshot assembly scratch
  bool m_collecting = false;
  uint8_t m_collectVenue = 0;
  std::vector<double> m_snapBidP, m_snapBidS, m_snapAskP, m_snapAskS;
};
