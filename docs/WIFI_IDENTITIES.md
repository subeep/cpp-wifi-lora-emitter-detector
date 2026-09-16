# Persistent Wi-Fi identities and identification details

The Wi-Fi master table now records **every decoded FCS-valid beacon or probe
response independently of RF fingerprint quality**. A network with no accepted
fingerprint still survives restart. The packet table links each observation to
its BSSID or provisional RF cluster key and shows decoded SSID, AP channel and
advertised security when available.

Click a master-table identity to view:

- BSSID, learned SSID, first/last seen and decoded observation count.
- The channel advertised by DS Parameter Set (or HT Operation fallback), separately
  from the monitored channel center. Missing advertised channels remain unknown.
- IEEE vendor assignment and matching prefix/source, with local/group/unknown
  MAC addresses handled explicitly. The compiled offline snapshot covers MA-L,
  MA-M and MA-S using longest-prefix matching; see
  [registry provenance](../third_party/ieee/README.md).
- Capability bits (including ESS/IBSS/Privacy), beacon interval, WPA/RSN AKM and
  cipher suites, PMF advertisement, and HT/VHT/HE capability IEs when present.
- WPS manufacturer, model name/number and device name when advertised. Fragmented
  WPS IEs are concatenated before bounded attribute parsing.
- Latest accepted RF reading, including its own timestamp and PHY. Missing RF
  readings are shown as unavailable, never as zero-valued measurements.

Names are retained when subsequent beacons hide/omit the SSID. WPS hints are
retained when later frames omit WPS. Both retain their original observation
source and time separately from the most recent frame. Present WPS IEs replace
previous WPS fields; absent fields in such an IE are not invented. Arbitrary
SSID/WPS bytes are preserved exactly, with unsafe display bytes escaped.

The Privacy bit alone is labeled **Privacy set (legacy/unknown)**, not assumed to
mean WPA or WEP. Unknown suite selectors remain unknown; incomplete/malformed IEs
are flagged. Capability IEs describe advertisements, not a verified generation,
certification or active security assessment. WPS strings are self-reported hints.
BSSIDs identify networks/interfaces, not guaranteed unique physical radios.
RF cluster counts remain provisional. Vendor assignment is not a verified retail
brand, model or owner.

## Persistence and compatibility

`data/wifi_master/*.ndjson` retains legacy meta and RF-reading records and adds
`type: "identity"` snapshots (schema 2 in new meta records). Existing files load
without an offline migration. Legacy MAC records gain details upon the next
successful decode; metadata never previously saved cannot be recovered from old
fingerprint values.

Identity snapshots store the latest metadata, observation count, source/timestamps
for last-known names and WPS hints, and monitored channel. Raw string bytes are
stored in hexadecimal alongside readable escaped fields so JSON remains valid for
arbitrary octets. Metadata and counts survive both identity and RF compaction.
Identity snapshots compact after 100 appended observations; RF history retains
1000 readings with the existing lazy-compaction slack up to 1100. Identity metadata
and first-seen times are retained across restarts, not every historical name/frame.

Malformed records are skipped with a visible storage warning. Partial final lines
are separated from subsequent appends so the next valid observation remains
recoverable. File-write/rename failures are shown in the GUI. Data remains in
memory on a save failure; a warning means persistence cannot be assumed.
No automatic RF-cluster-to-MAC merge is attempted.

## Build and verification

Wi-Fi persistence now requires nlohmann JSON headers (3.9 or later; Ubuntu package
`nlohmann-json3-dev`). The radio/application build otherwise uses existing deps.

    cmake -S . -B build
    cmake --build build -j4
    ./build/test_wifi_identity

`test_wifi_identity` exercises security/WPS/capability parsing, malformed inputs,
vendor prefixes, identity-only restarts, rejected fingerprints, hidden SSIDs,
compaction, legacy compatibility and storage failures. It uses an isolated
system-temp directory, never the application's saved data.

If EGL development files are installed, `test_wifi_gui` renders the actual Wi-Fi
UI using a surfaceless software OpenGL context, clicks an identity, and checks the
details popup without starting Scanner, a radio, or a desktop window:

    LIBGL_ALWAYS_SOFTWARE=1 ./build/test_wifi_gui /tmp/wifi-ui

The optional prefix writes `-table.ppm` and `-details.ppm` screenshots containing
explicitly labeled test fixtures. It does not capture or fabricate live devices.
Existing 12 regression binaries remain available. No LoRa-specific implementation
was changed by this work.

## Protocol references

- [Hostap WPA/RSN suite identifiers and PMF bits](https://raw.githubusercontent.com/vanhoefm/hostap-wpa3/master/src/common/wpa_common.h)
- [Hostap WPS attribute identifiers and lengths](https://w1.fi/wpa_supplicant/devel/wps__defs_8h_source.html)
- [IEEE public registry downloads](https://standards.ieee.org/products-programs/regauth/)

The identification parser uses these public field definitions; new tests assert
fixed wire layouts independently of the parsing code. Live RF yield is still
limited to the existing 1 Mbps DSSS receiver and its beacon-duration gate. This
work adds no OFDM MAC decoder or new 5 GHz validation.
