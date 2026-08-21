#pragma once

#include <cstdint>

// Binary event layout shared with feeds/wire.ts — keep in sync.
// 32 bytes, fields ordered for natural alignment.
namespace wire {

enum Type : uint8_t {
  SnapshotBegin = 1,
  SnapshotLevel = 2,
  SnapshotEnd = 3,
  BookUpdate = 4, // qty == 0 ⇒ delete level
  Trade = 5,
  FeedStatus = 6,
};

// book sides; trade aggressor: 0 = buy, 1 = sell
enum Side : uint8_t { BidOrBuy = 0, AskOrSell = 1 };

enum Status : uint8_t {
  StatusNone = 0,
  Connecting = 1,
  Syncing = 2,
  Live = 3,
  Reconnecting = 4,
  Error = 5,
};

enum Cmd : uint32_t {
  CmdNone = 0,
  CmdResyncVenue = 1,
  CmdSetSymbol = 2,
  CmdSetVenueEnabled = 3,
  CmdSetCandles = 4, // venue = Timeframe::Kind, arg = value (minutes/trades/vol)
  CmdRequestOrderFlow = 5,
};

struct Event {
  double price;
  double qty;
  double ts;
  uint32_t aux;
  uint8_t type, venue, side, flags;
};
static_assert(sizeof(Event) == 32, "wire::Event must stay 32 bytes");

} // namespace wire
