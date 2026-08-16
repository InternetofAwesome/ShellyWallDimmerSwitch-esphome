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
#include <string>
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

// ---- publish floor: never report "on at 0%" --------------------------------
// Regression test for a real defect. ESPHome's LightCall::validate_() rewrites
// ANY zero brightness into an explicit state=false; that turn-off round-trips
// through write_state() and puts an OFF byte on the wire. map_to_ha() saturates
// to 0 at or below min_brightness, so a lamp the co-processor reports as LIT
// was being switched off by our own state reflection. publish_brightness_ha()
// floors the published value at 1 while the output is on, and request() treats
// that floored value as a reflection rather than a new command.
static void test_publish_floor() {
  group("publish-floor");

  struct Case { uint8_t lo, hi, real; const char *what; };
  const Case cases[] = {
      {1, 100, 1, "shipped default min=1, touch at the bottom of the strip"},
      {20, 80, 10, "touch below a configured min"},
      {20, 80, 20, "device sitting exactly on min"},
      {20, 25, 20, "narrow window, at min"},
      {50, 50, 50, "degenerate window (min == max)"},
      {60, 40, 50, "inverted window (min > max)"},
  };

  for (const auto &c : cases) {
    Rig r;
    r.p().min_brightness = c.lo;
    r.p().max_brightness = c.hi;
    r.p().kick_enabled = false;
    r.p().limit_correct = false;
    r.status(c.real, true);  // co-processor reports the output ON at c.real

    CHECK(r.e.is_on(), c.what);
    CHECK(r.e.publish_brightness_ha() != 0,
          "published brightness is never 0 while on (0 would be read as OFF)");

    // Feed exactly what we would publish back in, the way the light layer does.
    r.clear();
    r.req(true, r.e.publish_brightness_ha());
    CHECK(g_tx.empty(), "publishing our own state does not command the device");
    CHECK(r.e.is_on(), "reflection leaves the output on");
  }

  // While OFF, no floor is applied -- the wrapper does not send a brightness at
  // all in that case, and clamping here would misreport the stored level.
  {
    Rig r;
    r.p().min_brightness = 20;
    r.p().max_brightness = 80;
    r.status(10, false);
    CHECK(!r.e.is_on(), "off state reported off");
    CHECK(r.e.publish_brightness_ha() == 0, "no floor applied while off");
  }

  // The floor must not mask a genuine command that differs from the reflection.
  {
    Rig r;
    r.p().min_brightness = 20;
    r.p().max_brightness = 80;
    r.p().kick_enabled = false;
    r.p().ramp_on_change = false;
    r.status(10, true);  // out of window
    r.clear();
    r.req(true, 50);  // a real HA command
    CHECK(!g_tx.empty(), "a genuine command still reaches the wire");
    CHECK(r.last_on() && r.last_bri() == 50, "and lands on the mapped level");
  }
}

// ---- status-frame parser ---------------------------------------------------
// The parser is the only ingress point for external data: everything the engine
// reconciles against and everything HA displays is derived from what it emits.
// Bounds are structurally safe (idx_ is capped by the state machine), so these
// cover decode correctness and RESYNC, which is the part that actually bites.
static std::vector<uint8_t> g_stray;
static void collect_stray(uint8_t b, void *) { g_stray.push_back(b); }

static void test_parser() {
  group("parser");

  auto fresh = [](FrameParser &p) {
    g_stray.clear();
    p.reset();
    p.set_stray_handler(collect_stray, nullptr);
  };

  // A clean frame decodes.
  {
    FrameParser p; StatusFrame f{}; fresh(p);
    const uint8_t frame[] = {0x24, 55, 0x01, 26, 0x23};
    int got = 0;
    for (uint8_t b : frame) if (p.feed(b, f)) got++;
    CHECK(got == 1, "one frame decoded from five bytes");
    CHECK(f.brightness == 55, "b0 -> brightness");
    CHECK(f.output_on, "b1 bit0 -> output on");
    CHECK(!f.flag_bit1, "b1 bit1 clear");
    CHECK(f.temp_c == 26, "b2 -> temperature");
    CHECK(g_stray.empty(), "no stray bytes from a clean frame");
  }

  // Payload bytes equal to SOF/EOF are data, not framing. 0x24 is a plausible
  // brightness (36) and 0x23 a plausible temperature (35 C), so this is a real
  // on-wire case, not a contrived one.
  {
    FrameParser p; StatusFrame f{}; fresh(p);
    const uint8_t frame[] = {0x24, 0x24, 0x01, 0x23, 0x23};
    int got = 0;
    for (uint8_t b : frame) if (p.feed(b, f)) got++;
    CHECK(got == 1, "frame with SOF/EOF-valued payload still decodes");
    CHECK(f.brightness == 0x24 && f.temp_c == 0x23, "payload passed through verbatim");
  }

  // Bad EOF is rejected, and the offending byte is offered to the stray sink.
  {
    FrameParser p; StatusFrame f{}; fresh(p);
    const uint8_t bad[] = {0x24, 10, 0x01, 20, 0x99};
    int got = 0;
    for (uint8_t b : bad) if (p.feed(b, f)) got++;
    CHECK(got == 0, "bad EOF yields no frame");
    CHECK(g_stray.size() == 1 && g_stray[0] == 0x99, "bad EOF byte goes to stray");
  }

  // RESYNC: a SOF arriving where EOF was expected restarts a frame from it
  // rather than being dropped. This is the branch that recovers a false lock.
  {
    FrameParser p; StatusFrame f{}; fresh(p);
    const uint8_t stream[] = {0x24, 1, 2, 3,      // truncated frame, no EOF...
                              0x24, 77, 0x01, 30, 0x23};  // ...real frame follows
    int got = 0;
    for (uint8_t b : stream) if (p.feed(b, f)) got++;
    CHECK(got == 1, "parser resyncs on SOF and decodes the following frame");
    CHECK(f.brightness == 77 && f.temp_c == 30, "resynced frame decodes correctly");
    CHECK(g_stray.empty(), "resync consumes the SOF rather than straying it");
  }

  // A false lock mid-stream self-heals within one frame.
  {
    FrameParser p; StatusFrame f{}; fresh(p);
    // Start mid-frame (as if the first bytes were lost), then two good frames.
    const uint8_t stream[] = {0x01, 30, 0x23,
                              0x24, 41, 0x01, 25, 0x23,
                              0x24, 42, 0x01, 25, 0x23};
    std::vector<int> bri;
    for (uint8_t b : stream) if (p.feed(b, f)) bri.push_back(f.brightness);
    CHECK(bri.size() == 2, "both real frames recovered after a mid-stream start");
    CHECK(bri.size() == 2 && bri[0] == 41 && bri[1] == 42, "recovered in order");
  }

  // Boot banner: unframed ASCII in IDLE reaches the stray sink intact, which is
  // how co-processor resets are detected and its version read.
  {
    FrameParser p; StatusFrame f{}; fresh(p);
    const char *banner = "reset!\nshelly_apt_003 mcu ver: v1.0.4\n";
    for (const char *s = banner; *s; s++) p.feed((uint8_t) *s, f);
    CHECK(g_stray.size() == std::char_traits<char>::length(banner),
          "every banner byte reaches the stray handler");
    CHECK(!g_stray.empty() && g_stray[0] == 'r', "banner starts intact");
  }

  // KNOWN LIMITATION, pinned so a change is deliberate: if the co-processor
  // resets MID-FRAME, the first banner bytes are consumed as payload and never
  // reach the stray sink, so the leading "reset!" line is truncated and reset
  // detection can miss it. Harmless (the next frame resyncs) but real.
  {
    FrameParser p; StatusFrame f{}; fresh(p);
    p.feed(0x24, f);  // SOF, now mid-frame
    const char *banner = "reset!\n";
    for (const char *s = banner; *s; s++) p.feed((uint8_t) *s, f);
    CHECK(g_stray.size() < std::char_traits<char>::length(banner),
          "mid-frame banner is truncated (documented limitation)");
  }

  // reset() drops partial state.
  {
    FrameParser p; StatusFrame f{}; fresh(p);
    p.feed(0x24, f); p.feed(9, f);
    p.reset();
    g_stray.clear();
    const uint8_t frame[] = {0x24, 12, 0x01, 22, 0x23};
    int got = 0;
    for (uint8_t b : frame) if (p.feed(b, f)) got++;
    CHECK(got == 1, "reset() lets the next frame parse cleanly");
    CHECK(f.brightness == 12, "post-reset frame decodes correctly");
  }
}

// ---- wire-format invariants ------------------------------------------------
// The codec is tiny, but a malformed command byte is indistinguishable on the
// wire from a legitimate one, and the co-processor will act on it. These pin
// the encoding itself plus two whole-sequence invariants that correspond to
// bugs this project actually shipped and had to chase on hardware.
static void test_protocol_invariants() {
  group("wire-format");

  // Encoding, from PROTOCOL: bit7 = on/off, bits6:0 = brightness 0..100.
  CHECK(encode_command(true, 0) == 0x80, "on at 0 -> 0x80");
  CHECK(encode_command(true, 1) == 0x81, "on at 1 -> 0x81");
  CHECK(encode_command(true, 100) == 0xE4, "on at 100 -> 0xE4");
  CHECK(encode_command(false, 0) == 0x00, "off at 0 -> 0x00");
  CHECK(encode_command(false, 100) == 0x64, "off preserves brightness bits -> 0x64");
  // Out-of-range must clamp, never wrap into the on/off bit. An unclamped 128
  // would encode as 0x80 == "on at 0".
  CHECK(encode_command(false, 200) == 0x64, "off clamps >100 to 100");
  CHECK(encode_command(true, 200) == 0xE4, "on clamps >100 to 100");
  CHECK(encode_command(false, 128) == 0x64, "off clamps 128 (would alias bit7)");
  CHECK(CMD_POLL == 0xFF, "poll byte is 0xFF");
  CHECK(FRAME_SOF == 0x24 && FRAME_EOF == 0x23, "frame delimiters are '$' and '#'");

  // Whole-sequence invariants, swept across the parameter space. Two failures
  // seen on hardware live here: brightness bytes escaping the 0..100 range, and
  // a SPURIOUS OFF byte emitted partway through a turn-on (originally caused by
  // a transition boundary being read as off, which made the lamp blink).
  const int levels[] = {1, 5, 20, 50, 99, 100};
  const int kicks[] = {0, 20, 60, 100};
  const int rates[] = {1, 150, 1000};
  for (int kick : kicks) {
    for (int rate : rates) {
      for (int target : levels) {
        for (int mode = 0; mode < 4; mode++) {
          Rig r;
          r.p().kick_enabled = (mode & 1) != 0;
          r.p().ramp_on_off = (mode & 2) != 0;
          r.p().ramp_on_change = true;
          r.p().kick_level = (uint8_t) kick;
          r.p().ramp_rate = (uint16_t) rate;
          r.clear();
          r.req(true, (uint8_t) target);       // turn on from off
          // Cap must cover the SLOWEST legal rate: at ramp_rate=1 %/s a
          // full-range move takes ~100 s, plus the kick dwell. A cap that only
          // suited the fast rates would assert mid-ramp and "fail" the engine
          // for still being busy.
          uint32_t el = r.advance_until_idle(150000);
          CHECK(el != UINT32_MAX, "turn-on settles within the rate's worst case");
          bool bad_range = false, spurious_off = false;
          for (uint8_t b : g_tx) {
            if (byte_bri(b) > 100) bad_range = true;
            if (!byte_on(b)) spurious_off = true;  // nothing here should turn it off
          }
          CHECK(!bad_range, "every emitted brightness stays within 0..100");
          CHECK(!spurious_off, "a turn-on sequence never emits an OFF byte");
          CHECK(r.last_on() && r.last_bri() == target,
                "turn-on settles exactly on the requested level");
        }
      }
    }
  }

  // A turn-off must end with exactly one OFF byte, and it must be last --
  // anything emitted after it would switch the lamp back on.
  {
    Rig r;
    r.p().kick_enabled = false;
    r.p().ramp_on_off = true;
    r.p().ramp_rate = 300;
    r.req(true, 80);
    r.advance_until_idle(5000);
    r.clear();
    r.req(false, 0);
    r.advance_until_idle(5000);
    CHECK(!g_tx.empty(), "turn-off emits something");
    CHECK(!g_tx.empty() && !byte_on(g_tx.back()), "the LAST byte of a fade-out is the OFF");
    int offs = 0;
    for (uint8_t b : g_tx) if (!byte_on(b)) offs++;
    CHECK(offs == 1, "exactly one OFF byte is emitted");
  }
}

int main() {
  test_range_mapping();
  test_kick();
  test_ramp();
  test_off();
  test_transition();
  test_noop();
  test_publish_floor();
  test_parser();
  test_protocol_invariants();
  std::printf("\n[engine+parser] %d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
