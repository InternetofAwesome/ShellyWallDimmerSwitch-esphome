#pragma once
//
// SH0S boot-state read-modify-write for the Shelly custom bootloader.
// ---------------------------------------------------------------------------
// This lets our ESPHome app do what stock firmware does over its own OTA
// backend: mark its slot committed (so the bootloader stops counting down
// boot-attempts and reverting), and hand control back to stock (revert).
//
// It is built ENTIRELY on the reverse-engineered, code-confirmed model in
// CLAUDE.md ("SH0S BOOT-STATE RECORD"). The load-bearing facts that make
// this safe:
//   1. The bootloader validates a record via wrapper 0x4008091c: magic "SH0S"
//      at +8, seq(+0) != 0, seq == mirror(+12), AND TWO CRC-32s --
//      +0x1c = crc32_le(0xFFFFFFFF, rec[0..0x1c) + 4 zero bytes) (header CRC),
//      +0x1fc = crc32_le(0xFFFFFFFF, rec[0..0x1fc)) (whole-body CRC).
//      **These CRCs are mandatory** -- an earlier note that the record was
//      "structural only, no checksum" was WRONG and made every commit/revert a
//      silent no-op (the bootloader recomputed, saw our stale CRCs, rejected
//      the copy, and booted the untouched one). Spans + convention verified
//      offline against stock's known-good boot_state.bin. We call the SAME ROM
//      esp_rom_crc32_le the bootloader uses, so results are identical by
//      construction. seal_crcs_() reseals both after any field edit.
//   2. There are TWO copies (otadata 0x0 / 0x1000). The bootloader boots the
//      highest-seq copy that passes validation. So writing ONE copy at a time
//      is inherently safe: a bad/rejected write just falls back to the other,
//      untouched, still-valid copy. The only bricking path is corrupting BOTH.
//
// Cardinal safety rule enforced here: every mutation writes exactly ONE copy
// (the non-winning one) and leaves the winner intact as the fallback. We never
// touch both in a single operation.
//
// NOT wired to any trigger yet (no commit-on-boot, no button). This is the
// library + a read/log diagnostic; triggers get added after the live-unit
// dump confirms the model and the code is reviewed.
//
#include <cstdint>
#include <cstring>

#include "esp_partition.h"
#include "esp_log.h"
#include "esp_rom_crc.h"  // esp_rom_crc32_le -- the exact ROM crc the bootloader uses

namespace shelly_dimmer_core {

// ---- record geometry (offsets within the 512-byte SH0S record) ----
static constexpr uint32_t SHOS_COPY0 = 0x0000;  // otadata copy A
static constexpr uint32_t SHOS_COPY1 = 0x1000;  // otadata copy B
static constexpr uint32_t SHOS_COPY_LEN = 0x1000;  // 4 KB flash sector per copy
static constexpr uint32_t SHOS_MAGIC = 0x53304853u;  // "SH0S" little-endian

static constexpr uint32_t SHOS_OFF_SEQ = 0x00;    // u32 LE
static constexpr uint32_t SHOS_OFF_MAGIC = 0x08;  // u32 == "SH0S"
static constexpr uint32_t SHOS_OFF_MIRROR = 0x0c;  // u32 LE == seq
static constexpr uint32_t SHOS_OFF_CTRL0 = 0x1d0;  // as(hi) | bit3 | bit2 | mfs(1) | c(0)
static constexpr uint32_t SHOS_OFF_CTRL1 = 0x1d1;  // ba(hi) | rs(lo)
static constexpr uint32_t SHOS_OFF_CRC_HDR = 0x1c;   // u32 LE crc32 of header (see seal_crcs_)
static constexpr uint32_t SHOS_OFF_CRC_BODY = 0x1fc;  // u32 LE crc32 of rec[0..0x1fc)

// Boot-attempts granted to a freshly DFU'd (uncommitted) image before the
// bootloader auto-reverts to the fallback slot. Must fit the ba nibble (0-15).
// QEMU-validated at 3: image gets 3 boots to prove healthy + call commit().
static constexpr uint8_t SHOS_DFU_BOOT_ATTEMPTS = 3;

// ---- proactive partition-layout guard --------------------------------------
// Every SH0S mutation is written against a HARD-CODED flash geometry: otadata
// lives at 0xd000, and our DFU/commit logic addresses the two app slots at
// 0x10000 (slot 0) and 0x200000 (slot 1). These three offsets were verified
// identical across stock 1.3.3 and 2.0.0 partition tables (only app *sizes* and
// the fs partitions move between versions; these offsets do not, and the
// bootloader's `Updating PT from PT0` resize preserves them). If a future stock
// build -- or some other vendor image we don't know about -- ever moves them,
// writing our record would corrupt boot on a table we don't understand. So we
// check the LIVE partition table against these expectations before EVER writing,
// and hard-refuse all boot-state writes on mismatch. Fail safe, loudly.
static constexpr uint32_t SHOS_EXPECT_OTADATA_ADDR = 0x0000d000u;
static constexpr uint32_t SHOS_EXPECT_APP0_ADDR = 0x00010000u;
static constexpr uint32_t SHOS_EXPECT_APP1_ADDR = 0x00200000u;

// Decoded, human-readable view of one copy.
struct BootStateView {
  bool valid = false;   // passed structural validation
  uint32_t seq = 0;
  uint8_t ctrl0 = 0;    // raw +0x1d0
  uint8_t ctrl1 = 0;    // raw +0x1d1
  // decoded fields
  uint8_t as = 0;       // active slot  (ctrl0 bits 4-7)
  uint8_t rs = 0;       // revert slot  (ctrl1 bits 0-3)
  uint8_t ba = 0;       // boot attempts(ctrl1 bits 4-7)
  bool committed = false;  // ctrl0 bit0
};

// Result of reading the whole otadata partition (both copies).
struct BootStatePair {
  bool ok = false;         // partition found and read
  BootStateView copy[2];   // [0]=@0x0, [1]=@0x1000
  int winner = -1;         // index of the copy the bootloader would boot, or -1
};

class BootState {
 public:
  static constexpr const char *TAG = "shos";

  // Locate the otadata partition. Returns false if not present.
  bool begin() {
    this->part_ = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                           ESP_PARTITION_SUBTYPE_DATA_OTA, nullptr);
    return this->part_ != nullptr;
  }

  // Read + parse both copies and decide the winner (highest-seq valid copy).
  BootStatePair read() {
    BootStatePair out;
    if (this->part_ == nullptr && !this->begin())
      return out;  // ok stays false
    for (int i = 0; i < 2; i++) {
      uint8_t hdr[SHOS_OFF_CTRL1 + 1];
      uint32_t off = i == 0 ? SHOS_COPY0 : SHOS_COPY1;
      if (esp_partition_read(this->part_, off, hdr, sizeof(hdr)) != ESP_OK)
        return out;
      out.copy[i] = parse_(hdr);
    }
    out.ok = true;
    out.winner = pick_winner_(out.copy);
    return out;
  }

  // Proactive geometry guard. Verifies the LIVE partition table places otadata,
  // app slot 0, and app slot 1 at exactly the offsets every SH0S write assumes
  // (see SHOS_EXPECT_* above). Result is cached after the first call: the table
  // can't change under a running image, so we check flash once and log once.
  //
  // Returns true only if ALL THREE match. On any mismatch it returns false and
  // logs an error naming the offending partition -- and mutate_() then refuses
  // every write, so commit/revert/DFU-stage/auto-commit all no-op safely rather
  // than scribble an SH0S record onto a table whose slot geometry we don't know.
  bool layout_ok() {
    if (this->layout_checked_)
      return this->layout_ok_;
    this->layout_checked_ = true;
    this->layout_ok_ = check_layout_();
    return this->layout_ok_;
  }

  void log_state(const BootStatePair &p) {
    if (!p.ok) { ESP_LOGW(TAG, "otadata read failed"); return; }
    for (int i = 0; i < 2; i++) {
      const auto &v = p.copy[i];
      ESP_LOGI(TAG, "copy%d @0x%04x: %s seq=%u as=%u rs=%u ba=%u c=%u (ctrl0=0x%02x ctrl1=0x%02x)%s",
               i, (unsigned) (i == 0 ? SHOS_COPY0 : SHOS_COPY1),
               v.valid ? "VALID" : "invalid", (unsigned) v.seq, v.as, v.rs, v.ba,
               v.committed ? 1 : 0, v.ctrl0, v.ctrl1,
               i == p.winner ? "  <-- boots" : "");
    }
  }

  // ---- mutations (each writes exactly ONE copy, leaving the winner intact) --
  // Return true on a successful flash write. Both take the winner's full 4 KB
  // content as the base so every byte outside the two control fields + header
  // seq is preserved verbatim (descriptor, +0x1c/+0x1fc markers, etc.).

  // Mark the currently-booted (winning) slot committed: c=1, mfs(bit1)=0,
  // active slot unchanged, AND ba:=0.
  //
  // BENCH-CONFIRMED CORRECTION: writing only ctrl0=(as<<4)|1 (ba left at 2) does
  // NOT commit. The bootloader treats any record with ba>0 as a PENDING boot
  // regardless of c: it boots the slot, decrements ba, and clears c back to 0.
  // Observed live: commit -> reboot gave c=0/ba=1, and with ba left counting it
  // went 2->1->0 and reverted to stock. A committed record needs ba=0 -- which
  // is exactly stock's own resting boot_state.bin (ctrl0=0x01, ctrl1=0x01 =
  // c=1, ba=0, rs=1). Zeroing ba here stops the rollback countdown. rs is
  // preserved so a later revert still knows the fallback slot.
  bool commit() {
    return mutate_([](uint8_t &ctrl0, uint8_t &ctrl1, const BootStateView &w) {
      ctrl0 = (uint8_t) ((w.as << 4) | 0x01);  // as unchanged, c=1, mfs=0, bit2=0, bit3=0
      ctrl1 = (uint8_t) (w.rs & 0x0f);         // ba=0 (stop the countdown), rs preserved
    });
  }

  // Hand control back to stock: swap active<->revert and commit, mirroring the
  // bootloader's own revert (bl 0x40080f16): as:=rs, c:=1, keep bits2-3, and
  // old as becomes the new rs. ba preserved. Next boot runs the stock slot.
  bool revert_to_stock() {
    return mutate_([](uint8_t &ctrl0, uint8_t &ctrl1, const BootStateView &w) {
      ctrl0 = (uint8_t) ((w.rs << 4) | 0x01 | (w.ctrl0 & 0x0c));
      ctrl1 = (uint8_t) ((w.ba << 4) | (w.as & 0x0f));
    });
  }

  // Stage a DFU: point the boot record at a freshly-flashed TARGET slot,
  // UNcommitted, with the fallback (rs) = the currently-RUNNING slot. This is
  // the record QEMU-validated against the real bootloader (as=target, rs=running,
  // c=0, ba=3, mfs=1): the bootloader boots the target; if the new image
  // crash-loops it counts ba 3->0 and auto-reverts to `running_slot` (the
  // known-good firmware). A healthy new image makes itself permanent via
  // commit(). One-copy-at-a-time + CRC-sealed like every other mutation, so a
  // failed/rejected write leaves the running slot bootable.
  bool stage_ota(uint8_t target_slot, uint8_t running_slot) {
    return mutate_([target_slot, running_slot](uint8_t &ctrl0, uint8_t &ctrl1, const BootStateView &w) {
      (void) w;
      ctrl0 = (uint8_t) (((target_slot & 0x0f) << 4) | 0x02);  // as=target, c=0, mfs=1
      ctrl1 = (uint8_t) ((SHOS_DFU_BOOT_ATTEMPTS << 4) | (running_slot & 0x0f));  // ba, rs=running
    });
  }

 protected:
  static BootStateView parse_(const uint8_t *hdr) {
    BootStateView v;
    uint32_t seq, magic, mirror;
    memcpy(&seq, hdr + SHOS_OFF_SEQ, 4);
    memcpy(&magic, hdr + SHOS_OFF_MAGIC, 4);
    memcpy(&mirror, hdr + SHOS_OFF_MIRROR, 4);
    v.seq = seq;
    v.valid = (magic == SHOS_MAGIC) && (seq != 0) && (seq == mirror);
    v.ctrl0 = hdr[SHOS_OFF_CTRL0];
    v.ctrl1 = hdr[SHOS_OFF_CTRL1];
    v.as = (v.ctrl0 >> 4) & 0x0f;
    v.committed = v.ctrl0 & 0x01;
    v.rs = v.ctrl1 & 0x0f;
    v.ba = (v.ctrl1 >> 4) & 0x0f;
    return v;
  }

  // Recompute BOTH boot-state CRC-32s in place, exactly as the bootloader
  // validates them (wrapper 0x4008091c -> ROM crc32_le @0x4005cfec). MUST be
  // called after any field edit and before writing, or the bootloader rejects
  // the copy. Order matters: the body CRC (+0x1fc) covers the header CRC
  // (+0x1c), so seal the header first. Spans verified offline against stock.
  static void seal_crcs_(uint8_t *rec) {
    // Header CRC: crc32_le(0xFFFFFFFF, rec[0..0x1c)) then 4 zero bytes (the
    // bootloader's two-step: crc over the 28-byte header with its own 4-byte
    // CRC field treated as zero). Store LE at +0x1c.
    const uint8_t zero4[4] = {0, 0, 0, 0};
    uint32_t hdr = esp_rom_crc32_le(0xFFFFFFFFu, rec, SHOS_OFF_CRC_HDR);
    hdr = esp_rom_crc32_le(hdr, zero4, 4);
    memcpy(rec + SHOS_OFF_CRC_HDR, &hdr, 4);
    // Body CRC: crc32_le(0xFFFFFFFF, rec[0..0x1fc)) -- covers the header CRC
    // just written. Store LE at +0x1fc.
    uint32_t body = esp_rom_crc32_le(0xFFFFFFFFu, rec, SHOS_OFF_CRC_BODY);
    memcpy(rec + SHOS_OFF_CRC_BODY, &body, 4);
  }

  // Replicate the bootloader's FULL acceptance test on a 512-byte record:
  // magic/seq/mirror AND both CRC-32s. Used to verify a write would actually
  // boot (not just parse), before trusting it.
  static bool record_valid_(const uint8_t *rec) {
    uint32_t seq, magic, mirror, c_hdr, c_body;
    memcpy(&seq, rec + SHOS_OFF_SEQ, 4);
    memcpy(&magic, rec + SHOS_OFF_MAGIC, 4);
    memcpy(&mirror, rec + SHOS_OFF_MIRROR, 4);
    memcpy(&c_hdr, rec + SHOS_OFF_CRC_HDR, 4);
    memcpy(&c_body, rec + SHOS_OFF_CRC_BODY, 4);
    if (magic != SHOS_MAGIC || seq == 0 || seq != mirror) return false;
    const uint8_t zero4[4] = {0, 0, 0, 0};
    uint32_t hdr = esp_rom_crc32_le(0xFFFFFFFFu, rec, SHOS_OFF_CRC_HDR);
    hdr = esp_rom_crc32_le(hdr, zero4, 4);
    if (hdr != c_hdr) return false;
    uint32_t body = esp_rom_crc32_le(0xFFFFFFFFu, rec, SHOS_OFF_CRC_BODY);
    return body == c_body;
  }

  // Verify one app slot sits at its expected offset. Logs and returns false on
  // a missing partition or an address mismatch.
  static bool check_app_slot_(esp_partition_subtype_t sub, uint32_t want, const char *name) {
    const esp_partition_t *p =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP, sub, nullptr);
    if (p == nullptr) {
      ESP_LOGE(TAG, "layout guard: %s partition not found", name);
      return false;
    }
    if (p->address != want) {
      ESP_LOGE(TAG, "layout guard: %s at 0x%06x, expected 0x%06x -- refusing boot-state writes",
               name, (unsigned) p->address, (unsigned) want);
      return false;
    }
    return true;
  }

  // The actual three-way check behind layout_ok() (cached by that wrapper).
  bool check_layout_() {
    if (this->part_ == nullptr && !this->begin()) {
      ESP_LOGE(TAG, "layout guard: no otadata partition -- refusing boot-state writes");
      return false;
    }
    bool ok = true;
    if (this->part_->address != SHOS_EXPECT_OTADATA_ADDR) {
      ESP_LOGE(TAG, "layout guard: otadata at 0x%06x, expected 0x%06x -- refusing boot-state writes",
               (unsigned) this->part_->address, (unsigned) SHOS_EXPECT_OTADATA_ADDR);
      ok = false;
    }
    ok &= check_app_slot_(ESP_PARTITION_SUBTYPE_APP_OTA_0, SHOS_EXPECT_APP0_ADDR, "app slot 0 (ota_0)");
    ok &= check_app_slot_(ESP_PARTITION_SUBTYPE_APP_OTA_1, SHOS_EXPECT_APP1_ADDR, "app slot 1 (ota_1)");
    if (ok) {
      ESP_LOGI(TAG, "layout guard OK: otadata@0x%06x app0@0x%06x app1@0x%06x",
               (unsigned) SHOS_EXPECT_OTADATA_ADDR, (unsigned) SHOS_EXPECT_APP0_ADDR,
               (unsigned) SHOS_EXPECT_APP1_ADDR);
    } else {
      ESP_LOGE(TAG, "layout guard FAILED -- SH0S commit/revert/DFU-stage/auto-commit all DISABLED");
    }
    return ok;
  }

  // Bootloader boots the highest-seq VALID copy (selector 0x40080c74).
  static int pick_winner_(const BootStateView *copy) {
    int w = -1;
    for (int i = 0; i < 2; i++) {
      if (!copy[i].valid) continue;
      if (w < 0 || copy[i].seq > copy[w].seq) w = i;
    }
    return w;
  }

  // Read winner's full 4 KB copy, apply `fn` to the two control bytes, bump
  // seq (+mirror), and write to the NON-winning copy. One copy touched.
  template<typename F>
  bool mutate_(F fn) {
    if (this->part_ == nullptr && !this->begin()) {
      ESP_LOGW(TAG, "no otadata partition");
      return false;
    }
    // Proactive guard: never write an SH0S record unless the live flash geometry
    // is the one every offset here was reverse-engineered against. On any
    // mismatch this refuses -- commit/revert/stage_ota/auto-commit all no-op.
    if (!this->layout_ok()) {
      ESP_LOGE(TAG, "boot-state write refused: partition layout guard failed");
      return false;
    }
    BootStatePair p = this->read();
    if (!p.ok || p.winner < 0) {
      ESP_LOGW(TAG, "no valid boot-state copy to base on; refusing to write");
      return false;
    }
    const BootStateView &w = p.copy[p.winner];
    int target = p.winner ^ 1;  // the OTHER copy -- winner stays as fallback
    uint32_t win_off = p.winner == 0 ? SHOS_COPY0 : SHOS_COPY1;
    uint32_t tgt_off = target == 0 ? SHOS_COPY0 : SHOS_COPY1;

    // Base = winner's full sector (preserves every unmodelled byte).
    static uint8_t buf[SHOS_COPY_LEN];
    if (esp_partition_read(this->part_, win_off, buf, SHOS_COPY_LEN) != ESP_OK) {
      ESP_LOGW(TAG, "read winner failed");
      return false;
    }
    // Guard the (practically unreachable) seq wrap: if the winner's seq is at
    // u32 max, winner.seq+1 wraps to 0. Re-seeding it to 1 would make our copy
    // LOWER-seq than the still-valid winner -- the bootloader would keep booting
    // the winner while commit()/revert() falsely reported success. Refuse rather
    // than write an ineffective copy. (seq is +1/boot from 39; never reaches this.)
    if (w.seq == 0xFFFFFFFFu) {
      ESP_LOGW(TAG, "winner seq at u32 max; refusing to write (new copy could not win)");
      return false;
    }

    // Apply control-field change.
    uint8_t c0 = buf[SHOS_OFF_CTRL0], c1 = buf[SHOS_OFF_CTRL1];
    fn(c0, c1, w);
    buf[SHOS_OFF_CTRL0] = c0;
    buf[SHOS_OFF_CTRL1] = c1;
    // Bump seq (+mirror) so this copy wins (strictly > winner, guarded above).
    uint32_t new_seq = w.seq + 1;
    memcpy(buf + SHOS_OFF_SEQ, &new_seq, 4);
    memcpy(buf + SHOS_OFF_MIRROR, &new_seq, 4);
    // Magic is already correct in the base; assert defensively.
    uint32_t magic = SHOS_MAGIC;
    memcpy(buf + SHOS_OFF_MAGIC, &magic, 4);

    // Reseal BOTH CRC-32s over the edited record. Without this the bootloader
    // recomputes, sees stale CRCs, rejects this copy, and keeps booting the
    // winner -- which is exactly why pre-CRC commit/revert were silent no-ops.
    seal_crcs_(buf);

    ESP_LOGI(TAG, "writing copy%d @0x%04x: seq %u->%u ctrl0 0x%02x->0x%02x ctrl1 0x%02x->0x%02x",
             target, (unsigned) tgt_off, (unsigned) w.seq, (unsigned) new_seq, w.ctrl0, buf[SHOS_OFF_CTRL0],
             w.ctrl1, buf[SHOS_OFF_CTRL1]);

    // Erase-then-write the target sector only. If power fails mid-write the
    // target goes to 0xFF (invalid magic) and the untouched winner still boots.
    if (esp_partition_erase_range(this->part_, tgt_off, SHOS_COPY_LEN) != ESP_OK) {
      ESP_LOGW(TAG, "erase target failed");
      return false;
    }
    if (esp_partition_write(this->part_, tgt_off, buf, SHOS_COPY_LEN) != ESP_OK) {
      ESP_LOGW(TAG, "write target failed");
      return false;
    }
    // Read back the FULL sector and confirm it would pass the bootloader's own
    // acceptance test (magic/seq/mirror + both CRC-32s), plus the control bytes
    // landed as intended. Reading only the header (as before) could not catch a
    // bad CRC -- the exact failure that made prior writes inert. Any miss ->
    // return false without a second write, leaving the untouched winner valid.
    static uint8_t vbuf[SHOS_COPY_LEN];
    if (esp_partition_read(this->part_, tgt_off, vbuf, SHOS_COPY_LEN) != ESP_OK) {
      ESP_LOGW(TAG, "verify read failed"); return false;
    }
    BootStateView vv = parse_(vbuf);
    bool crc_ok = record_valid_(vbuf);
    if (!vv.valid || !crc_ok || vv.seq != new_seq ||
        vv.ctrl0 != buf[SHOS_OFF_CTRL0] || vv.ctrl1 != buf[SHOS_OFF_CTRL1]) {
      ESP_LOGW(TAG,
               "verify FAILED (valid=%d crc_ok=%d seq=%u want=%u ctrl0=0x%02x/want0x%02x "
               "ctrl1=0x%02x/want0x%02x) -- winner copy untouched, safe",
               vv.valid, crc_ok, (unsigned) vv.seq, (unsigned) new_seq, vv.ctrl0, buf[SHOS_OFF_CTRL0],
               vv.ctrl1, buf[SHOS_OFF_CTRL1]);
      return false;
    }
    ESP_LOGI(TAG, "boot-state write OK: copy%d now wins (as=%u c=%u rs=%u ba=%u)",
             target, vv.as, vv.committed ? 1 : 0, vv.rs, vv.ba);
    return true;
  }

  const esp_partition_t *part_{nullptr};

  // layout_ok() cache: the partition table is fixed for a running image, so the
  // geometry check runs once against flash and the result is reused thereafter.
  bool layout_checked_{false};
  bool layout_ok_{false};
};

}  // namespace shelly_dimmer_core
