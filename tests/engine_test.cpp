// Standalone unit tests for the framework-agnostic dimmer engine.
//
// Builds and runs with plain g++ (C++17) -- NO ESPHome / ESP-IDF toolchain, no
// hardware. See tests/README.md. It exercises the ACTUAL shipped engine
// (components/shelly_wall_dimmer/dimmer_engine.h + dimmer_protocol.h), which is
// pure C++ by design, so a skeptic can check every kick / ramp / range-mapping
// claim in the README against the real code in a few seconds.
//
// The engine emits raw command bytes to a send callback; each byte is
// bit7 = output on/off, bits6:0 = brightness 0..100 (see PROTOCOL). We capture
// those bytes and simulate time by calling tick() once per millisecond.

#include "components/shelly_wall_dimmer/dimmer_engine.h"

#include <cstdint>
#include <cstdio>
#include <vector>

using namespace shelly_dimmer_core;

// ---- tiny test harness -----------------------------------------------------
static int g_pass = 0, g_fail = 0;
static const char *g_group = "";
static void group(const char *g) { g_group = g; }
#define CHECK(cond, msg) \
  do { \
    if (cond) { \
      g_pass++; \
    } else { \
      g_fail++; \
      std::printf("  FAIL [%s] %s (line %d)\n", g_group, (msg), __LINE__); \
    } \
  } while (0)

// ---- UART capture + a time-stepping rig -----------------------------------
static std::vector<uint8_t> g_tx;
static void capture(uint8_t b, void *) { g_tx.push_back(b); }
static bool byte_on(uint8_t b) { return (b & 0x80) != 0; }
static int byte_bri(uint8_t b) { return b & 0x7F; }

struct Rig {
  DimmerEngine e;
  uint32_t now = 0;
  Rig() {
    e.set_send_handler(capture, nullptr);
    // Test baseline: identity range map (min 0 / max 100) so device values equal
    // HA values unless a test overrides them. The non-identity map (incl. the
    // shipped default min=1) is covered explicitly by the range-map tests.
    e.params().min_brightness = 0;
    e.params().max_brightness = 100;
    g_tx.clear();
  }
  DimmerParams &p() { return e.params(); }
  void clear() { g_tx.clear(); }
  void req(bool on, uint8_t bri) { e.request(on, bri, now); }
  void req_transition(bool on, uint8_t bri, uint32_t transition_ms) {
    e.request_transition(on, bri, transition_ms, now);
  }
  void status(uint8_t real, bool on) { e.notify_status(real, on, now); }
  // Advance simulated time `ms` milliseconds, ticking the engine each ms.
  void advance(uint32_t ms) {
    uint32_t end = now + ms;
    for (; now <= end; now++)
      e.tick(now);
  }
  // Advance until the engine settles (mode back to IDLE) or a cap is hit.
  // Returns elapsed ms from `now` at call time, or UINT32_MAX if it never settles.
  uint32_t advance_until_idle(uint32_t cap_ms) {
    uint32_t start = now, end = now + cap_ms;
    for (; now <= end; now++) {
      e.tick(now);
      if (!e.busy())
        return now - start;
    }
    return UINT32_MAX;
  }
  int last_bri() { return g_tx.empty() ? -1 : byte_bri(g_tx.back()); }
  bool last_on() { return !g_tx.empty() && byte_on(g_tx.back()); }
};

// ---- range mapping (min/max stretch, and its inverse) ----------------------
static void test_range_mapping() {
  group("range-map");

  // Device -> HA (inverse map), via current_brightness_ha() after a report.
  {
    Rig r;
    r.p().min_brightness = 20;
    r.p().max_brightness = 80;
    r.status(20, true);
    CHECK(r.e.current_brightness_ha() == 0, "device=min -> HA 0");
    r.status(80, true);
    CHECK(r.e.current_brightness_ha() == 100, "device=max -> HA 100");
    r.status(50, true);
    CHECK(r.e.current_brightness_ha() == 50, "device=50 -> HA 50 (min20/max80)");
    r.status(10, true);
    CHECK(r.e.current_brightness_ha() == 0, "below-min touch saturates to HA 0");
    r.status(90, true);
    CHECK(r.e.current_brightness_ha() == 100, "above-max touch saturates to HA 100");
  }

  // HA -> device (forward map), via a plain (kick off / no ramp) turn-on.
  {
    Rig r;
    r.p().min_brightness = 20;
    r.p().max_brightness = 80;
    r.p().kick_enabled = false;
    r.p().ramp_on_off = false;
    r.req(true, 100);
    CHECK(r.last_bri() == 80, "HA 100 -> device max (80)");
    r.clear();
    r.status(0, false);  // reset to off/idle so the next req isn't a no-op
    Rig r2;
    r2.p().min_brightness = 20;
    r2.p().max_brightness = 80;
    r2.p().kick_enabled = false;
    r2.p().ramp_on_off = false;
    r2.req(true, 50);
    CHECK(r2.last_bri() == 50, "HA 50 -> device 50 (min20/max80)");
  }

  // Round-trip device -> HA -> device == device, across several windows. This is
  // the stability claim: the inverse map never drifts the level. (Skip
  // device==min, which maps to HA 0 == off, not an on-level.)
  {
    const int wins[][2] = {{0, 100}, {20, 80}, {1, 100}, {10, 90}, {30, 70}};
    for (auto &w : wins) {
      for (int real = w[0] + 1; real <= w[1]; real++) {
        Rig a;
        a.p().min_brightness = (uint8_t) w[0];
        a.p().max_brightness = (uint8_t) w[1];
        a.status((uint8_t) real, true);
        int ha = a.e.current_brightness_ha();

        Rig b;
        b.p().min_brightness = (uint8_t) w[0];
        b.p().max_brightness = (uint8_t) w[1];
        b.p().kick_enabled = false;
        b.p().ramp_on_off = false;
        b.req(true, (uint8_t) ha);
        CHECK(b.last_bri() == real, "round-trip device->HA->device is stable");
      }
    }
  }
}

// ---- single-pivot kick -----------------------------------------------------
static void mk_kick(Rig &r) {
  r.p().min_brightness = 0;
  r.p().max_brightness = 100;
  r.p().kick_enabled = true;
  r.p().kick_level = 20;
  r.p().kick_dwell_ms = 150;
  r.p().ramp_rate = 1000;
}

static void test_kick() {
  group("kick");

  // Below pivot: strike to kick_level first, then (after dwell) ramp DOWN.
  {
    Rig r;
    mk_kick(r);
    r.req(true, 10);
    CHECK(r.last_bri() == 20 && r.last_on(), "below-pivot: strikes at kick_level first");
    r.advance(400);
    CHECK(r.last_bri() == 10, "below-pivot: settles at target after dwell+ramp");
  }

  // Above pivot: start at kick_level, ramp UP to target (no dwell).
  {
    Rig r;
    mk_kick(r);
    r.req(true, 60);
    CHECK(r.last_bri() == 20, "above-pivot: starts at kick_level");
    r.advance(400);
    CHECK(r.last_bri() == 60, "above-pivot: ramps up to target");
  }

  // Equal to pivot: land at kick_level.
  {
    Rig r;
    mk_kick(r);
    r.req(true, 20);
    r.advance(50);
    CHECK(r.last_bri() == 20, "equal-pivot: lands at kick_level");
  }

  // Kick disabled: straight to target, one byte, no strike.
  {
    Rig r;
    mk_kick(r);
    r.p().kick_enabled = false;
    r.clear();
    r.req(true, 60);
    CHECK(r.last_bri() == 60 && g_tx.size() == 1, "kick off: jumps straight to target");
  }
}

// ---- ramps -----------------------------------------------------------------
static void test_ramp() {
  group("ramp");

  // Brightness change while on ramps (many steps), landing exactly on target.
  {
    Rig r;
    r.p().kick_enabled = false;
    r.p().ramp_on_change = true;
    r.p().ramp_rate = 100;  // 1%/10ms
    r.req(true, 50);        // jump on to 50 (ramp_on_off off)
    r.advance(5);
    r.clear();
    r.req(true, 60);
    r.advance(300);
    CHECK(r.last_bri() == 60, "ramp lands exactly on setpoint");
    CHECK(g_tx.size() >= 5, "ramp emits multiple steps (not a jump)");
  }

  // Partial last step goes to the setpoint, no overshoot, at a step>1 rate.
  {
    Rig r;
    r.p().kick_enabled = false;
    r.p().ramp_on_change = true;
    r.p().ramp_rate = 300;  // step 3 / 10ms
    r.req(true, 50);
    r.advance(5);
    r.clear();
    r.req(true, 60);
    r.advance(300);
    bool saw59 = false, overshoot = false;
    for (uint8_t b : g_tx) {
      int v = byte_bri(b);
      if (v == 59) saw59 = true;
      if (v > 60) overshoot = true;
    }
    CHECK(r.last_bri() == 60, "partial-step ramp ends on setpoint");
    CHECK(saw59 && !overshoot, "step=3 then clamps to setpoint (no overshoot/dither)");
  }

  // ramp_on_change off -> instantaneous jump.
  {
    Rig r;
    r.p().kick_enabled = false;
    r.p().ramp_on_change = false;
    r.req(true, 50);
    r.advance(5);
    r.clear();
    r.req(true, 60);
    CHECK(r.last_bri() == 60 && g_tx.size() == 1, "ramp_on_change off: jumps");
  }
}

// ---- off behavior ----------------------------------------------------------
static void test_off() {
  group("off");

  // Immediate off preserves the brightness bits (the falsifiable wire prediction).
  {
    Rig r;
    r.p().kick_enabled = false;
    r.p().ramp_on_off = false;
    r.req(true, 50);
    r.clear();
    r.req(false, 0);
    CHECK(!r.last_on() && r.last_bri() == 50, "off byte is output-off with brightness bits kept");
  }

  // Fade-out ramps down and ends in an off command at 0.
  {
    Rig r;
    r.p().kick_enabled = false;
    r.p().ramp_on_off = true;
    r.p().ramp_rate = 500;
    r.req(true, 60);    // fade in
    r.advance(600);     // settle at 60
    r.clear();
    r.req(false, 0);    // fade out
    r.advance(600);
    CHECK(!r.last_on(), "fade-out ends with output off");
    CHECK(r.last_bri() == 0, "fade-out reaches 0 before the off command");
  }
}

// ---- HA "transition: Ns" (request_transition) ------------------------------
// Tolerance for elapsed-time checks: the ramp is quantized to a 10ms floor and
// an integer step count, so total time is never exact -- just close. 25% (with
// a floor) comfortably covers that rounding without being a tautology.
static bool within(uint32_t actual, uint32_t expected) {
  uint32_t tol = expected / 4 + 15;
  return actual + tol >= expected && expected + tol >= actual;
}

static void test_transition() {
  group("transition");

  // Explicit transition ramps even though ramp_on_change is OFF, and takes
  // approximately the requested duration.
  {
    Rig r;
    r.p().kick_enabled = false;
    r.p().ramp_on_change = false;
    r.req(true, 20);
    r.advance(5);
    r.clear();
    r.req_transition(true, 80, 2000);
    CHECK(g_tx.size() <= 1, "transition doesn't jump instantly (no immediate final byte)");
    uint32_t el = r.advance_until_idle(4000);
    CHECK(r.last_bri() == 80, "transition lands exactly on the setpoint");
    CHECK(within(el, 2000), "transition takes ~the requested duration (change, toggle off)");
  }

  // Explicit transition on turn-off fades even though ramp_on_off is OFF.
  {
    Rig r;
    r.p().kick_enabled = false;
    r.p().ramp_on_off = false;
    r.req(true, 60);
    r.advance(5);
    r.clear();
    r.req_transition(false, 0, 1000);
    uint32_t el = r.advance_until_idle(3000);
    CHECK(!r.last_on() && r.last_bri() == 0, "transition-off ends off at 0");
    CHECK(within(el, 1000), "transition-off takes ~the requested duration");
  }

  // Kicked turn-on, target BELOW the pivot: strike is instantaneous, the dwell
  // is unaffected, and the requested duration covers the down-ramp segment
  // (deferred through tick() after the dwell -- the override must survive that).
  {
    Rig r;
    r.p().kick_enabled = true;
    r.p().kick_level = 20;
    r.p().kick_dwell_ms = 150;
    r.req_transition(true, 10, 1000);
    CHECK(r.last_bri() == 20 && r.last_on(), "kicked+transition: strikes at kick_level immediately");
    uint32_t el = r.advance_until_idle(3000);
    CHECK(r.last_bri() == 10, "kicked+transition: settles at the (mapped) target");
    CHECK(within(el, 150 + 1000), "kicked+transition: dwell + ~requested duration for the down-ramp");
  }

  // Kicked turn-on, target ABOVE the pivot: strike instantaneous, no dwell, the
  // requested duration covers the up-ramp.
  {
    Rig r;
    r.p().kick_enabled = true;
    r.p().kick_level = 20;
    r.req_transition(true, 70, 1000);
    CHECK(r.last_bri() == 20, "kicked+transition (above pivot): strikes at kick_level");
    uint32_t el = r.advance_until_idle(3000);
    CHECK(r.last_bri() == 70, "kicked+transition (above pivot): settles at target");
    CHECK(within(el, 1000), "kicked+transition (above pivot): ~requested duration, no dwell");
  }

  // transition_ms == 0 behaves exactly like a plain request() (respects the
  // ramp_on_change toggle, doesn't force a ramp).
  {
    Rig r;
    r.p().kick_enabled = false;
    r.p().ramp_on_change = false;
    r.req(true, 20);
    r.advance(5);
    r.clear();
    r.req_transition(true, 80, 0);
    CHECK(r.last_bri() == 80 && g_tx.size() == 1, "transition_ms=0 jumps like plain request()");
  }

  // The override never leaks into an unrelated later ramp (e.g. limit_correct)
  // after a transition call that didn't end up ramping (already at target).
  {
    Rig r;
    r.p().kick_enabled = false;
    r.p().min_brightness = 20;
    r.p().max_brightness = 80;
    r.p().limit_correct = true;
    r.p().ramp_rate = 1000;  // fast, so we can tell it's using ramp_rate not a leaked override
    r.req(true, 50);
    r.advance(5);
    r.clear();
    r.req_transition(true, 50, 5000);  // already at target -> no-op, no ramp started
    CHECK(g_tx.empty(), "transition to the current value is a no-op");
    r.status(10, true);  // touch report below min -> limit_correct ramps to min (20)
    uint32_t el = r.advance_until_idle(2000);
    CHECK(r.last_bri() == 20, "limit_correct still ramps to the limit");
    // At ramp_rate=1000%/s a 10-unit move finishes in one step, essentially
    // instantly. A leaked 5000ms transition override would make this take
    // seconds -- so a loose "well under a second" bound is exactly what
    // distinguishes "used ramp_rate" from "used the stale override".
    CHECK(el < 500, "limit_correct uses ramp_rate, not a leaked transition duration");
  }
}

// ---- no-op reflection ------------------------------------------------------
static void test_noop() {
  group("no-op");

  // A device report reflected back through the light layer must not re-command.
  {
    Rig r;
    r.p().min_brightness = 20;
    r.p().max_brightness = 80;
    r.p().kick_enabled = false;
    r.status(50, true);  // device holds 50, engine idle
    int ha = r.e.current_brightness_ha();
    r.clear();
    r.req(true, (uint8_t) ha);  // HA reflects that same value back
    CHECK(g_tx.empty(), "reflected device state is a no-op (no re-command)");
  }
}

int main() {
  test_range_mapping();
  test_kick();
  test_ramp();
  test_off();
  test_transition();
  test_noop();
  std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
