# Third-party code

This project is **GPL-3.0-or-later**. Vendored components:

## Native map
- stb_image: https://github.com/nothings/stb, MIT license option; vendored PNG
  decoder and hashes in `third_party/stb/`. License shipped with Windows.
- Map data/tiles: © OpenStreetMap contributors, https://www.openstreetmap.org/copyright.
  Visible attribution is included. Tiles are fetched for the current view and
  cached locally; no tiles are bundled or redistributed in the package.
- Linux uses the system libcurl (curl license); Windows uses system WinHTTP.
  See `docs/PACKET_MAP.md` for cache, network and build behavior.

## turbofec — `third_party/turbofec/`
- Source: https://github.com/ttsou/turbofec
- License: **GPL-3.0** (see `third_party/turbofec/LICENSE`)
- Use: LTE turbo encoder/decoder + rate matching (`src/fec/turbo.cpp` wraps it).
- Note: the turbo decoder core is only defined under `HAVE_SSE3`; our CMake enables SSE3/SSE4.1
  on x86. Without it the decoder silently returns zeros.

## CRC++ (CRCpp) — `third_party/crcpp/CRC.h`
- Source: https://github.com/d-bahr/CRCpp
- License: **BSD-3-Clause** (see `third_party/crcpp/LICENSE`)
- Use: CRC-24-LTEA computation (`src/fec/turbo.cpp`).

## Algorithm reference (not vendored, ported with attribution)
- https://github.com/proto17/dji_droneid — GPL. The DSP/descramble/turbo chain in
  `src/dji/` and `src/fec/` is ported from its `matlab/updated_scripts` and `cpp/*.cc`.
  Credits in that repo to RUB-SysSec / CISPA for the DroneID reverse engineering.

## Open Drone ID reference (not vendored)
- https://github.com/opendroneid/opendroneid-core-c — **Apache-2.0**. `libopendroneid/opendroneid.{c,h}`
  and `wifi.c` were consulted for ASTM F3411 field scaling, validity rules (`checkPackContent`, auth
  page limits) and the Wi-Fi Beacon / NAN frame layouts. `src/odid/` is an independent C++
  implementation; no code was copied. The 802.11 PHY in `src/odid/wifi_*.cpp` is written from
  IEEE 802.11 (DSSS and OFDM clauses).

## Real-IQ test fixtures — `tests/data/mini2_centered.sc16`, `mini2_wideband.cf32`
- Source: RUB-SysSec [DroneSecurity](https://github.com/RUB-SysSec/DroneSecurity),
  revision `9ff819843bee48fb140a0704ec78aff757896dea`, `samples/mini2_sm`.
- Upstream license: **AGPL-3.0**, retained in `tests/data/LICENSE.DroneSecurity`.
- Use: centered/resampled and raw 50-MSPS excerpts for decoder regression testing; provenance,
  transformation and checksums are in `tests/data/README.md`.
- The upstream packed record layout was also consulted to verify v2 field
  boundaries against the decoded bytes. Motion units still require validation.

## Windows build dependencies

- GLFW 3.4: https://github.com/glfw/glfw, zlib license; pinned archive and license
  accompany the Windows source/binary packages.
- FFTW 3.3.10: https://www.fftw.org, GPL-2.0-or-later; static float/SSE2 build,
  corresponding source archive and COPYING are supplied.
- Dear ImGui: `third_party/imgui/LICENSE.txt` (MIT).
- UHD C headers: `third_party/uhd_c/README.md`, derived from UHD 4.1.0.5-3,
  GPL-3.0-or-later. No UHD runtime DLL is redistributed.
- MinGW-w64/GCC runtime: linked statically; runtime copyright/exception notices
  are supplied under `licenses/` in the Windows package.
