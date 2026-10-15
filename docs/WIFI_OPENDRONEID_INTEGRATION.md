# Wi-Fi OpenDroneID integration — 15 October 2026

## Delivered

The main application's Drones tab now includes **Wi-Fi Remote ID**, with its own
**Enable Wi-Fi OpenDroneID** control. It is off at startup and independent of the
DJI IQ decoder and security-event recording. The current USRP/scan pipeline remains
the sole receiver; this feature neither connects another radio nor retunes it.

Two immutable packet-observer calls cover the normal and security-only Wi-Fi
decode paths. Existing identity, packet display and security processing remain
unchanged. The observer only copies eligible management frames into a bounded
queue; native C++ parsing/tracking happens on a separate worker. Observer failures
are contained, and no model is trained or consulted.

## Supported envelopes and messages

- Complete FCS-valid Wi-Fi Beacon MPDUs with vendor element `fa:0b:bc / 0d`.
- Complete FCS-valid NAN public-action frames with service hash
  `88:69:19:9d:92:09`, using the reference follow-up service-descriptor layout.
- Protocol versions 0, 1 and 2. Future/mixed versions are not accepted.
- Basic ID, Location, Authentication pages, Self ID, System and Operator ID.
- Independently recalculated IEEE 802.11 FCS; both the source FCS flag and complete
  MPDU are required by the monitor. The standalone parser verifies FCS itself.
- Full element/attribute-chain bounds, exact message-pack lengths, version-specific
  pack limits, duplicate-page and conflicting same-type-ID checks.

Fragmented, protected, ordered or DS-addressed management frames are unsupported.
Other NAN descriptor layouts, Bluetooth, encrypted application data and Remote ID
signature verification are outside this delivery. Wi-Fi payload reception remains
limited by the existing PHY decoders and acquisition schedule.

## Information and interpretation

The inspector displays the transmitter address, transport, protocol version,
reported identifiers and aircraft category, aircraft position, course/speeds,
barometric/geodetic altitude and relative height, operator/system position, area
parameters, description, operator identifier and authentication-page bytes.

Every message retains its raw bytes and acquisition provenance: run/session,
capture sequence, sample coordinates, monitored channel/centre, sample rate/PHY,
device and host timestamps where present, host uncertainty, after-overflow flag
and the complete independently FCS-checked MPDU. Copy Remote ID evidence JSON
exports the selected source, message ages, raw values and provenance.

Reported identity is **unauthenticated** and is not an exact-model classification
or confirmation of a physical aircraft. Authentication pages are unverified data,
not a credential. Source addresses are not merged with DJI serials or ordinary
Wi-Fi identities. Operator position is not automatically a home/RTH location.
Vertical references remain separate. Accuracy fields describe transmitter-reported
categories, not measured receiver accuracy. Null/sentinel values are unavailable;
a new unavailable Location replaces old coordinates instead of retaining them.

Version-specific absent fields remain null: v0 lacks operator geodetic altitude;
v0/v1 lack System timestamp. Raw reserved fields and legacy authentication-page
semantics remain visible. Refer to the pinned test vectors for precise supported
wire layouts; this is not a claim of complete standards certification.

## Freshness, limits and isolation

- Each message has its own age and becomes stale after 10 seconds. Fresh Location
  cannot refresh an old Basic ID. Receive age does not prove the transmitter's
  reported position itself is current.
- Host acquisition estimates are used when supplied. Otherwise the view explicitly
  says only decoder arrival age is known. Protocol-carried timestamps remain separate.
- Sources are keyed by run, radio session, transport and transmitter address.
  Changed same-type Basic ID or protocol version resets previous telemetry.
  Earlier host-acquisition timestamps cannot roll a source back.
- Input MPDUs: at most 8192 bytes. Queue: 128 frames, nonblocking producer.
  Retention: 64 sources and 32 message slots/source. Oldest entries are evicted
  at capacity; different authentication pages consume separate slots. Source
  inactivity over five minutes is pruned when another supported pack arrives.
- Disabling stops processing and retains labelled history. Re-enabling resets it.
  GUI snapshots refresh at most four times per second while this view is visible.
- Examined/not-decoded frames, accepted packs, queue drops, errors and out-of-order
  input are distinct counters. Ordinary Beacons can increment not-decoded; this is
  not a false-drone-alarm counter. One valid packet does not establish healthy or
  continuous channel coverage.

## Build and use

The feature is included by the existing `RFMON_ENABLE_DJI_DRONEID` Drones build
option. The new Remote ID path itself uses native C++/nlohmann JSON and does not
start Python or turbo FEC. Turning that build option off removes the Drones UI.

After launching the rebuilt `build/rf_monitor_gui`, select/connect a Wi-Fi band
using Monitoring, then open Drones → Wi-Fi Remote ID and enable decoding.
Security-event recording is not necessary. Changing tabs leaves packet processing
active and does not retune the receiver. A currently running old binary needs a
restart to load this feature; this work does not interrupt an existing monitor.

`build/wifi_remote_id_inspect` also accepts one hexadecimal complete MPDU including
FCS per input line and returns decoded pack arrays as JSON. It has no radio or
tracker side effects; its output is offline parsing evidence. The Drones tab's
Offline IQ review remains the DJI decoder, not a new Wi-Fi IQ replay path.

## Validation

All 46 integration CTests passed, including existing DJI, LoRa, Wi-Fi security,
scanner and cyclostationary suites. The Drones-disabled GUI also builds.

The native parser matches 514 comparisons against the existing parser using
independently encoded Core C v0/v1/v2 Beacon/NAN vectors and local negative
controls. Cases cover fields, sentinels, full packs, legacy maximum NAN packs,
future/mixed versions, malformed lengths, fragmentation/protection, bad FCS,
truncation and conflicting identities. See `tests/fixtures/remote_id/PROVENANCE.md`
for source revisions, hashes and the distinction between reference and mutated
fixtures.

Monitor tests cover default-off behaviour, per-message freshness, unavailable
position replacement, identity resets, out-of-order rejection, radio-session
isolation, truncated-frame rejection and source-cap eviction. The production
inspector is exercised by the offscreen Drones GUI test with a labelled synthetic
fixture. The scanner regression injects failing IQ and packet observers and
compares existing packet, identity, registry/status and normalized security-log
outputs, including security-only tail frames.

Fresh over-the-air Remote ID reception, sensitivity, scan coverage and sustained
live load remain unqualified for this integrated build. No USRP, transmitter,
external detector process or dataset original was changed or operated for this work.
