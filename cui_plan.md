# RF Monitor — GUI redesign plan (`cui_plan.md`)

Status: **plan + clickable mockup only. No application code changed.** Implementation
starts only on explicit approval. The interactive mockup lives as an Artifact
(dark instrument theme, IBM Plex, hand-drawn charts standing in for ImPlot).

This plan is display-layer work on the existing Dear ImGui app
([src/main.cpp](src/main.cpp), [src/lora_gui.cpp](src/lora_gui.cpp),
[src/wifi_gui.cpp](src/wifi_gui.cpp), [src/security_gui.cpp](src/security_gui.cpp))
plus **one** backend addition (the All-Devices band-hop mode). Receive-only
throughout; the LoRa/sub-GHz decode path is untouched.

---

## 1. Goals (from the user)

1. A **settings icon top-right** that opens device connect + gain + threshold +
   other settings in one place.
2. Three primary tabs — **LoRa**, **Wi-Fi**, **All Devices** — each a better
   presentation of what exists today, with real plots.
3. Wi-Fi tab: in-tab **2.4 GHz / 5 GHz** choice and a better device/capability view.
4. All Devices: **band-hopping** across LoRa + Wi-Fi to cover everything, showing
   every device "getting ready" (being discovered) with honest per-band duty.
5. A live **packet feed** answerable on every tab (per-tab sub-panel + a merged one).
6. **A generalized Security Monitor** — a dedicated view holding *everything* about
   safety, with plots — in addition to the per-tab security bits.
7. Every tab's modules have a **collapsing toggle**.
8. Every **table module is size-adjustable** (drag to resize).

Decisions already locked: All-Devices uses a **proper Scanner hop mode**; charts
use **ImPlot**; packet feeds are **per-tab sub-panels** plus a merged all-bands feed.

---

## 2. What exists today (grounding)

- One fullscreen ImGui window ([main.cpp:354](src/main.cpp:354)); flat vertical
  layout; a single **active band** at a time (`Scanner::set_active_band`).
- Scanner keeps one persistent `DeviceRegistry` **per band**; `snapshot(band)`
  works for any band even when inactive ([scanner.hpp](src/scanner.hpp)). There is
  **no cross-band hopping** — the radio is only ever tuned to the active band.
- Data already available: `DeviceRow` (freq/bw/protocol/power/hits/age),
  `WifiPacketRow` + `wifi::BeaconInfo` (SSID, security, ciphers, PMF, HT/VHT/HE/EHT
  standards, WPS), `wifi_master` identities, the security `SecuritySnapshot`
  (flood + beacon-replay incidents, baselines, detector diagnostics), `lora_packets`,
  `lora_master`.
- Setters that move behind the settings modal already exist: `set_device_type`,
  `set_gain`, `set_threshold_db`, `set_wifi_fixed_channel`, `set_wifi_security_recording`,
  LoRa lock/capture/lab-mode.

The one hard constraint: **one SDR = one frequency at a time.** "Cover everything"
means time-sharing the radio, i.e. reduced per-band duty. The UI states that openly.

---

## 3. Information architecture

```
Top bar:  [logo RF MONITOR]      ( ● Connected · X310 · step · cycles )      [⚙ Settings & Device]
Tabs:     LoRa | Wi-Fi | All Devices | Security
```

### 3.1 Settings & Device modal (the gear)
- **Receiver** — B210/X310 select, Connect/Reconnect/Disconnect, live state + error,
  "Record Wi-Fi security events" toggle (applies on next Connect).
- **RF controls** — Gain slider (clamped to `DeviceProfile::max_gain_db`), AGC
  (disabled+reason when unsupported), Threshold-above-noise slider.
- **All-Devices hop schedule** — per-band dwell sliders (Sub-GHz / 2.4 / 5) and
  which bands are in the hop.
- **Advanced** — LoRa capture window, legacy codecs, recording path.
- Rationale: connection + tuning are session-level, not per-view. Changing the radio
  still requires stop→set→start, so Connect/Reconnect stays an explicit action.

### 3.2 LoRa tab
- Sub-GHz frequency map (reuse `draw_frequency_track`), a **channel waterfall**
  (channel × time), and an **SF × BW distribution**.
- Live **LoRa PHY packets** feed (reuse `lora_packets`) and **Master emitters** with
  RSSI sparklines.
- LoRa capture controls (freq lock, capture seconds, lab mode, Save IQ, replay).

### 3.3 Wi-Fi tab
- **2.4 / 5 GHz segmented toggle** (`set_active_band`) + sweep/fixed-channel combo.
- **Channel occupancy** bar (`wifi_source_counts`).
- **AP capability cards** from `BeaconInfo`: SSID/BSSID/channel/signal + badges for
  security (Open/WPA2/WPA3), **PMF** state, Wi-Fi generation (4/5/6/7 from
  HT/VHT/HE/EHT), and WPS vendor/model.
- **Detected Wi-Fi packets** live burst sub-panel (reuse `wifi_packets`).
- A compact security summary that links into the Security tab.

### 3.4 All Devices tab
- Honesty callout: one radio, time-shared.
- **Spectrum coverage lanes** (Sub-GHz → 2.4 → 5) with a live "now" marker and the
  **hop schedule** timeline; per-band **duty %** and freshness (live / discovering /
  stale) — devices "getting ready" as the hop reaches each band.
- Merged **device list** across all bands (`snapshot()` for each band, concatenated).
- Merged **Live packets — all bands** feed.

### 3.5 Security Monitor tab (new — see §4)

---

## 4. Generalized Security Monitor (new)

A single place that consolidates *all* safety information, independent of which
protocol tab you're on. It reads the same `SecuritySnapshot` the per-tab panels use,
so nothing new is computed — it is a unified presentation with plots.

**Summary tiles:** open incidents · highest severity · detectors active · channels
under a learning hold · overall baseline coverage.

**Plots (ImPlot):**
- **Incident timeline** — incidents over time, marked by band and severity, on an
  analysed-time axis (not wall clock), so bursts of activity read at a glance.
- **Rule / severity distribution** — counts by rule (`management_disconnect_flood`,
  `historical_beacon_replay`, …) and severity.
- **Baseline vs live threshold** — measured deauth+disassoc rate against the learned
  `p95`-derived threshold per watched channel (the existing evidence-timeline idea,
  promoted to a first-class chart).
- **Coverage / duty gauges** — how much air is actually observed per band; low duty
  caps detection confidence, so it belongs beside the incidents.

**Detector status panel:** per rule — enabled, evaluated windows, excluded captures,
(beacon) history evictions, config version, active holders. Straight from the
snapshot's `flood`/`beacon_replay` fields.

**Incidents table (all bands):** time, band, channel, rule, claimed BSSID / target,
severity, confidence, status (open/closed/restored), key measures. Row click opens
the existing **evidence popup** (claimed identities, confidence basis, benign
alternatives, rate timeline, first/latest MPDU evidence).

**Honesty banner (kept):** observation-mode, confidence never exceeds medium, an
empty list is not proof of "no attack," sender identity and impact unverified.

Relationship to per-tab security: the Wi-Fi tab keeps a compact incidents summary for
context while monitoring that band; the Security tab is the full, cross-band console.
Same data, no duplication of logic.

---

## 5. Cross-cutting UI behaviors (new)

### 5.1 Collapsing toggle on every module
Every panel header carries a disclosure chevron; clicking the header collapses/expands
its body. Collapsed state is remembered per viewer. In ImGui this maps to
`CollapsingHeader` (already used in places today) applied consistently to every module,
or a custom header with a stored open/closed bool per panel id.

### 5.2 Size-adjustable table modules
Every module that contains a table is resizable — drag its bottom edge to change
height; the table scrolls within. In the mockup this is CSS `resize: vertical`; in the
ImGui app it maps to sizing each table's `BeginTable` outer size from a per-panel
stored height with a drag handle (or hosting tables in child regions with
`ImGuiChildFlags_ResizeY`). Remembered per panel. Widths already use ImGui table
column resizing; this adds height control to the module.

---

## 6. Look & feel

- One defined dark palette + spacing/rounding theme via `ImGui::GetStyle()` (ground
  `#0B0F14`, matching the app's clear color); consistent semantic colors
  (ok/warn/crit) separate from the accent.
- Reusable primitives: `status_pill`, `badge`, `stat_tile`, `sparkline`, collapsible
  `module()` wrapper with stored height.
- Typography: IBM Plex Sans (UI) + IBM Plex Mono (data) — engineering feel, not the
  generic default.
- Charts: **ImPlot** vendored into `third_party` for zoom/hover/axis-grade plots
  (waterfall, occupancy, incident timeline, baseline-vs-threshold, duty gauges).

---

## 7. Phasing (reviewable slices)

1. Theme + top bar + **Settings & Device** modal (relocate existing controls).
2. Tab shell (LoRa / Wi-Fi / All Devices / Security); move today's content into tabs
   unchanged (regression-safe).
3. Collapsible module wrapper + resizable table module wrapper applied everywhere.
4. Wi-Fi capability cards + occupancy; LoRa plots; per-tab + merged packet feeds.
5. **Security Monitor** tab with ImPlot charts, detector status, cross-band incidents,
   evidence popup.
6. **All-Devices Scanner hop mode** (backend) + coverage lanes / hop schedule / duty.
7. Vendor ImPlot; polish; extend `test_wifi_gui` to drive the new tabs, modal,
   collapsibles and the evidence popup.

Backend work is isolated to step 6 (a "cover all bands" rotation in the scanner with
dwell timing and per-band duty reporting). Everything else is display-layer.

---

## 8. Testing & constraints

- Extend the offscreen `test_wifi_gui` render smoke test to open each tab, the
  settings modal, a collapsed/expanded module, a resized table, and the incident
  evidence popup.
- Receive-only; nothing transmits. LoRa/sub-GHz decode path unchanged.
- Work left uncommitted for review.

---

## 9. Open items for the user

- Security tab: is the incident **timeline** the primary plot, or the
  **baseline-vs-threshold** chart? (Mockup shows both.)
- Should collapsed/resized module layout persist across restarts (write to the app's
  settings/state), or reset each launch?
- Any additional safety signals to surface beyond flood + beacon replay (e.g. coverage
  gaps, queue loss, RX stalls) as first-class Security-tab cards?
