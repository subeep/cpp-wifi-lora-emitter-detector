# Received OFDM beacon regression fixtures

Three 20 Msps CF32 little-endian crops from receive-only X310 recordings on
2026-09-19: one 2.4 GHz beacon, one 5 GHz beacon, and one 5 GHz beacon captured
with the production +1.5 MHz tuning offset. These contain public beacon metadata,
not user data frames. JSON files retain the source capture name, checksum, sample
range, receiver settings and exact expected MPDU. They are directly consumed by
`test_wifi_ofdm`; full-capture replay uses energy segmentation and needs the full
recording's background noise for its floor estimate.

Expected bytes were recovered with this project's decoder. CRCs were independently
checked with Python zlib; tshark verified BSSID/SSID and advertised channel. These
are received-signal regression vectors, not an independent PHY decoder oracle.
See `docs/WIFI_OFDM.md` for evidence and limitations. Synthetic all-rate vectors
are generated separately by `tests/generate_wifi_ofdm.py`.
