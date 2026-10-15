# DJI DroneID add-on — 14 October 2026

A subsequent native Wi-Fi OpenDroneID add-on is documented in
[WIFI_OPENDRONEID_INTEGRATION.md](WIFI_OPENDRONEID_INTEGRATION.md). The scope below
describes the DJI-specific integration.

## Delivered behaviour

The main C++ application has Monitoring and Drones tabs. Monitoring retains its
existing band controls, packet/identity views, LoRa functions, security rules and
cyclostationary panels. Selecting Drones does not change band, channel, recording
or radio ownership. The only scanner extension is an immutable optional read-only
observer after existing packet processing. Observer exceptions are contained.

DJI decoding is compiled with `RFMON_ENABLE_DJI_DRONEID=ON` (default) and disabled
at runtime until **Enable DJI add-on** is pressed. It owns no radio. It reuses
completed captures from either Wi-Fi band, independent of Wi-Fi decode success
and independent of the cyclostationary enable switch. A separate Python process
per active live/offline decoder performs demodulation, Gold descrambling, turbo
FEC, CRC24A and payload CRC16 validation. No transmission occurs.

The Drones page offers separate Live broadcasts and Offline IQ review views.
The table shows reported serial/model, receive centre, packet count and freshness.
The inspector preserves aircraft, controller/app and home positions, altitude and
height raw values/reference conversions, raw velocities, angle, sequence, GPS
time, UUID, state bits, validity annotations and complete packet JSON. Sample rate,
source-clock time, source path or capture/run/session/sample coordinates, symbol
format and carrier-bin correction are visible. JSON can be copied without display
rounding. A stopped worker retains historical results; a new start resets its state.

Checksum validity is not sender authentication. Tentative bit mappings, unknown
velocity units and unqualified altitude conversion/reference remain labelled.
Zero/invalid position values are not presented as verified locations. This is
DJI proprietary DroneID, not standards-based OpenDroneID and not exact-model ML.

## Bounds and operational limits

- Supported input metadata: 15.26–100 MSps, 2.3–2.6 or 5.6–6 GHz. This describes
  decoder input checks, not coverage of every frequency, aircraft or firmware.
- Live selection: up to four non-overlapping, distributed 20 ms windows per
  completed capture, capped at 2 million complex samples per window. Each gets a
  separate continuity epoch; unrelated intervals are never stitched together.
- Captures with overflow, timeout, exception or missing device-clock anchors are
  skipped. All capture samples, omitted samples, unusable captures and queue drops
  are counted separately. No-decode is not evidence of no drone.
- Four queued chunks maximum (64 MB sample storage at the maximum chunk size),
  plus the in-flight chunk and decoder working memory. The producer uses a
  try-lock and never waits for the child. Copy/allocation work still has bounded
  overhead; live non-interference is not assumed from architecture alone.
- Child numerical libraries use one thread; child nice level is lowered where
  supported, address space is bounded to 1536 MiB, and IPC operations have
  deadlines. Worker failures are displayed; re-enable explicitly to retry.
- At most 128 retained observation rows; history is bounded. This delivery does
  not infer hop schedules, continuous presence or a physical aircraft count.
- Offline GUI: regular little-endian complex-float32 files only, explicit actual
  rate/centre and offset, at most 40 ms/2 million samples per request. No automatic
  sidecar detection or whole-file playback. Decoding runs off the UI thread and
  never writes to the source. It does not publish into live observations.
- Live latency includes existing finite-capture and processing-lane delays.
  The existing scan schedule/tuning/passband can miss or truncate DJI broadcasts;
  there is no new DJI tuning or scan-and-hold controller in this integration.
- Runtime requires Python NumPy, SciPy, Matplotlib, bitarray and crcmod; versions
  are listed in `src/drones/python/requirements.txt`. Missing dependencies produce
  a decoder startup error, not a monitoring failure. Turbo FEC requires x86 SSSE3.
- No Remote ID, MAVLink, ELRS or analog-video decoder was added in this change.

## Build and use

Build normally with CMake and launch the rebuilt `build/rf_monitor_gui`. Select a
Wi-Fi band/channel using the existing Monitoring controls and connect normally.
In Drones, enable the add-on. It continues working while other tabs are visible.
Switching to LoRa leaves the add-on waiting for Wi-Fi captures. No auto-connect
or radio operation is performed at application startup.

For offline review, select a raw IQ file and its actual metadata, then Decode
interval. The bundled tests/fixtures/drones recordings use 50 MSps and the
inherited test centre of 2444.5 MHz. Results must stay labelled offline.

Set `RFMON_ENABLE_DJI_DRONEID=OFF` to build the original UI without the Drones
page, Python worker or turbo library. Existing cyclostationary options are
independent. Restarting a running application is necessary to load a new binary;
this implementation does not silently replace an active monitoring process.

## Validation and remaining work

The integration suite passed all 44 CTests, including existing LoRa, Wi-Fi
security, scanner-lane equivalence and cyclostationary suites. The scanner test
also injects a throwing observer and compares packet rows, identity state,
registry/status and normalized security logs against the original path.
The add-on-disabled GUI also builds.

DJI tests exercise the received Mavic Air 2 and Mini 2 fixtures, payload CRC
corruption, noise/tone/zero controls, invalid metadata/nonfinite samples,
continuity-epoch reset, the actual C++ subprocess and capture observer, offline
source failures, and live/offline separation. An offscreen ImGui test renders the
production inspector using decoded recording data. See the fixture manifest and
`src/drones/NOTICE.md` for source hashes and licensing.

This is software/recorded-input acceptance. No fresh USRP or drone test was run.
Live reception probability, end-to-end latency, passband qualification, sustained
off/on/off load tests and broader model/firmware coverage remain to be measured.
Mini 2 and Mavic Air 2 fixture successes do not validate every model name in the
payload lookup table. Scan-and-hold, continuous/full-file replay and durable
drone-track storage would be separate extensions.
