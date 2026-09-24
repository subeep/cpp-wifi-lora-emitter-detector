# 802.11 MAC parser vectors

`frames.hex` (name, tab, MPDU hex including the FCS) and `frames.pcap` (the same
40 MPDUs behind a radiotap header with the FCS-at-end flag) are consumed by
`test_wifi_security_frame`. They are regenerated with:

```bash
python3 tests/generate_wifi_security_frames.py
```

Provenance: frames are packed by hand from the IEEE 802.11-2020 clause 9 layouts
with Python `struct`, and their FCS is `zlib.crc32`. None of this shares code
with `src/security/wifi_mac_frame.cpp`. The test's expected field values were
read from tshark's dissection of `frames.pcap`:

```bash
tshark -r tests/fixtures/wifi_security/frames.pcap -o wlan.check_checksum:TRUE -V
```

On 2026-09-24, a field-by-field comparison of `wifi_frame_inspect --json`
against tshark agreed on 540 of 543 fields. The three differences are
deliberate:

- `too_short_3`: a 3-octet frame cannot hold a header and an FCS. Tshark reads
  a subtype from the FCS octets; the parser reports FCS too short and a
  truncated header.
- `header_truncated_addr2`: tshark leaves the FCS unverified on a malformed
  frame; the parser checks it, and it is valid.

The frames are synthetic, not received signals. They cover header layouts,
address roles, sequence and fragment numbers, retry and protection flags,
cipher headers (CCMP/GCMP, TKIP, WEP), management fixed fields, security and
DoS-relevant elements (RSN MFP, CSA, Quiet, BSS Load, MME), EAPOL-Key messages
and malformed or truncated cases.

`data_ccmp_ieee_vector_header` reuses the MAC and CCMP header of the IEEE CCMP
test vector (PN 0xB5039776E70C) with an abbreviated body. Only its header
fields are claimed.

Received-signal coverage comes from the three X310 beacons in
`../wifi_ofdm/`, which the same test also parses.
