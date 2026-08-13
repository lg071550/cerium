#pragma once

#include "wire.h"

#include <cstdint>

// JS↔WASM bridge to the feeds worker. The SAB ring lives in JS land; drain()
// bulk-copies pending events into wasm memory once per frame.
namespace bridge {

// Spawns the worker and creates the shared ring. Call once after startup.
void init();

// Copies up to maxEvents pending ring events into dest; returns the count.
int drain(wire::Event* dest, int maxEvents);

// Returns and resets the ring's dropped-event counter.
int takeDropped();

// Sends a command to the worker (wire::Cmd).
void sendCommand(uint32_t type, uint32_t venue, double arg);

} // namespace bridge
