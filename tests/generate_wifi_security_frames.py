#!/usr/bin/env python3
"""Independent 802.11 MAC test vectors for tests/test_wifi_security_frame.cpp.

Frames are packed by hand from the IEEE 802.11-2020 clause 9 layouts with
Python's struct module; the FCS is zlib.crc32. Nothing here shares code with
src/security/wifi_mac_frame.cpp. Writes:
  frames.hex  - name<TAB>MPDU hex including the trailing FCS
  frames.pcap - the same MPDUs behind a radiotap header (Flags: FCS at end),
                for re-checking fields with an independent dissector:
    tshark -r frames.pcap -o wlan.check_checksum:TRUE -V

Usage: python3 tests/generate_wifi_security_frames.py [output_dir]
"""
import struct, zlib, sys, os

out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "fixtures", "wifi_security")
AP = bytes.fromhex("001122334455")
STA = bytes.fromhex("66778899aabb")
STA2 = bytes.fromhex("da0102030405")   # locally administered (randomised-looking)
BC = b"\xff" * 6
DS = bytes.fromhex("0c0d0e0f1011")

def fc(type_, sub, flags=0):
    return bytes([(sub << 4) | (type_ << 2), flags])

def hdr(type_, sub, flags, dur, a1, a2, a3, seq, frag=0):
    return fc(type_, sub, flags) + struct.pack("<H", dur) + a1 + a2 + a3 + struct.pack("<H", (seq << 4) | frag)

def ie(i, body): return bytes([i, len(body)]) + body
def fcs(b): return b + struct.pack("<I", zlib.crc32(b) & 0xffffffff)

RATES = ie(1, bytes([0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24]))
RSN_MFPC = ie(48, struct.pack("<H", 1) + b"\x00\x0f\xac\x04" + struct.pack("<H", 1) + b"\x00\x0f\xac\x04"
              + struct.pack("<H", 1) + b"\x00\x0f\xac\x02" + struct.pack("<H", 0x0080))
RSN_MFPR = ie(48, struct.pack("<H", 1) + b"\x00\x0f\xac\x04" + struct.pack("<H", 1) + b"\x00\x0f\xac\x04"
              + struct.pack("<H", 1) + b"\x00\x0f\xac\x08" + struct.pack("<H", 0x00c0)
              + struct.pack("<H", 0) + b"\x00\x0f\xac\x06")

def mme(keyid, ipn, mic=b"\xa5" * 8):
    return ie(76, struct.pack("<H", keyid) + ipn.to_bytes(6, "little") + mic)

def eapol_key(key_info, replay, nonce, key_data=b"", mic=b"\x00" * 16):
    body = (bytes([2]) + struct.pack(">H", key_info) + struct.pack(">H", 16) + struct.pack(">Q", replay)
            + nonce + b"\x00" * 16 + b"\x00" * 8 + b"\x00" * 8 + mic + struct.pack(">H", len(key_data)) + key_data)
    return bytes([2, 3]) + struct.pack(">H", len(body)) + body

LLC = lambda et: b"\xaa\xaa\x03\x00\x00\x00" + struct.pack(">H", et)
ANONCE = bytes(range(1, 33))
SNONCE = bytes(range(0x40, 0x60))

frames = []
def add(name, raw, with_fcs=True):
    frames.append((name, fcs(raw) if with_fcs else raw))

# --- management -----------------------------------------------------------
add("deauth_broadcast_reason7", hdr(0, 12, 0, 314, BC, AP, AP, 291) + struct.pack("<H", 7))
add("deauth_unicast_retry_reason3", hdr(0, 12, 0x08, 314, STA, AP, AP, 292) + struct.pack("<H", 3))
add("disassoc_reason8", hdr(0, 10, 0, 0, AP, STA, AP, 17) + struct.pack("<H", 8))
add("auth_open_seq1", hdr(0, 11, 0, 314, AP, STA2, AP, 5) + struct.pack("<HHH", 0, 1, 0))
add("auth_sae_commit_group19", hdr(0, 11, 0, 314, AP, STA, AP, 6) + struct.pack("<HHH", 3, 1, 0)
    + struct.pack("<H", 19) + b"\x11" * 32 + b"\x22" * 64)
add("auth_sae_anticlogging", hdr(0, 11, 0, 314, STA, AP, AP, 7) + struct.pack("<HHH", 3, 1, 76)
    + struct.pack("<H", 19) + b"\x33" * 32)
add("assoc_req_rsn_mfpc", hdr(0, 0, 0, 314, AP, STA, AP, 8) + struct.pack("<HH", 0x0431, 10)
    + ie(0, b"LabNet") + RATES + RSN_MFPC)
add("assoc_resp_aid1", hdr(0, 1, 0, 314, STA, AP, AP, 9) + struct.pack("<HHH", 0x0431, 0, 0xC001) + RATES)
add("reassoc_req", hdr(0, 2, 0, 314, AP, STA, AP, 10) + struct.pack("<HH", 0x0431, 10) + DS
    + ie(0, b"LabNet") + RATES)
add("probe_req_wildcard", hdr(0, 4, 0, 0, BC, STA2, BC, 11) + ie(0, b"") + RATES
    + ie(221, bytes.fromhex("0050f208") + b"\x00\x10"))
add("beacon_csa_quiet_load_mme", hdr(0, 8, 0, 0, BC, AP, AP, 1000)
    + struct.pack("<QHH", 0x0000004BD017E036, 100, 0x0431)
    + ie(0, b"LabNet") + RATES + ie(3, b"\x06") + ie(5, bytes([0, 3, 1, 0]))
    + ie(7, b"IN\x20\x01\x0d\x14") + ie(11, struct.pack("<HBH", 12, 200, 0))
    + ie(37, bytes([1, 11, 5])) + ie(40, bytes([2, 10]) + struct.pack("<HH", 50, 3))
    + RSN_MFPR + mme(6, 0x010203040506))
add("sa_query_request", hdr(0, 13, 0, 314, STA, AP, AP, 12) + bytes([8, 0]) + struct.pack("<H", 0x1234))
add("action_csa_group_mme", hdr(0, 13, 0, 0, BC, AP, AP, 13) + bytes([0, 4]) + ie(37, bytes([1, 36, 3]))
    + mme(4, 0x0000000000FF, b"\x5a" * 16))
add("action_public_ext_csa", hdr(0, 13, 0, 0, BC, AP, AP, 30) + bytes([4, 4, 1, 115, 40, 2]))
add("deauth_broadcast_bip_mme", hdr(0, 12, 0, 0, BC, AP, AP, 14) + struct.pack("<H", 6) + mme(4, 777))
# unicast protected deauth: CCMP header PN=0x0000000100AB, key 0
add("deauth_protected_ccmp", hdr(0, 12, 0x40, 314, STA, AP, AP, 15)
    + bytes([0xAB, 0x00, 0x00, 0x20, 0x01, 0x00, 0x00, 0x00]) + b"\xde\xad" + b"\x99" * 8)
add("beacon_elem_overrun", hdr(0, 8, 0, 0, BC, AP, AP, 1001) + struct.pack("<QHH", 5, 100, 0x0001)
    + ie(0, b"Lab") + bytes([1, 40, 0x82, 0x84]))
add("deauth_truncated_reason", hdr(0, 12, 0, 0, BC, AP, AP, 16) + b"\x07")

# --- data -----------------------------------------------------------------
# MAC + CCMP header of the IEEE 802.11 CCMP test vector (PN 0xB5039776E70C,
# key 0). Ciphertext abbreviated, so this MPDU is not the published frame:
# only its header fields are claimed, and tshark decodes them identically.
add("data_ccmp_ieee_vector_header", bytes.fromhex(
    "0848c32c0fd2e128a57c5030f1844408abaea5b8fcba8033" "0ce7002076970 3b5".replace(" ", ""))
    + bytes.fromhex("f8ba1a55d02f85ae967bb62fb6cda8eb7e78a050"))
add("qos_data_fromds_eapol_m1", hdr(2, 8, 0x02, 44, STA, AP, AP, 20) + struct.pack("<H", 7)
    + LLC(0x888E) + eapol_key(0x008A, 1, ANONCE))
add("data_tods_eapol_m2", hdr(2, 0, 0x01, 44, AP, STA, AP, 21)
    + LLC(0x888E) + eapol_key(0x010A, 1, SNONCE, RSN_MFPC, b"\x77" * 16))
add("qos_data_fromds_eapol_m3", hdr(2, 8, 0x02, 44, STA, AP, AP, 22) + struct.pack("<H", 7)
    + LLC(0x888E) + eapol_key(0x13CA, 2, ANONCE, b"\x55" * 56, b"\x78" * 16))
add("data_tods_eapol_m4", hdr(2, 0, 0x01, 44, AP, STA, AP, 23)
    + LLC(0x888E) + eapol_key(0x030A, 2, b"\x00" * 32, b"", b"\x79" * 16))
add("null_tods_pwrmgt", hdr(2, 4, 0x11, 44, AP, STA, AP, 24))
add("data_wds_4addr", hdr(2, 0, 0x03, 44, AP, DS, STA, 25) + STA2 + LLC(0x0800) + b"\x45" + b"\x00" * 19)
add("data_tkip", hdr(2, 0, 0x41, 44, AP, STA, AP, 26) + bytes([0x12, 0x32, 0x34, 0x20, 0x01, 0, 0, 0]) + b"\x10" * 20)
add("data_wep_key1", hdr(2, 0, 0x41, 44, AP, STA, AP, 27) + bytes([1, 2, 3, 0x40]) + b"\x10" * 20)
add("qos_data_htc_ipv4", fc(2, 8, 0x82) + struct.pack("<H", 44) + STA + AP + AP + struct.pack("<H", 28 << 4)
    + struct.pack("<H", 5) + struct.pack("<I", 0x12345678) + LLC(0x0800) + b"\x45" + b"\x00" * 19)
add("qos_data_amsdu_arp", hdr(2, 8, 0x02, 44, STA, AP, AP, 29) + struct.pack("<H", 0x0080)
    + STA + DS + struct.pack(">H", 8 + 28) + LLC(0x0806) + b"\x00" * 28)

# --- control --------------------------------------------------------------
add("rts", fc(1, 11) + struct.pack("<H", 500) + AP + STA)
add("rts_bw_signalling_ta", fc(1, 11) + struct.pack("<H", 500) + AP + bytes([STA[0] | 1]) + STA[1:])
add("cts", fc(1, 12) + struct.pack("<H", 400) + STA)
add("ack", fc(1, 13) + struct.pack("<H", 0) + STA)
add("ps_poll_aid1", fc(1, 10, 0x10) + struct.pack("<H", 0xC001) + AP + STA)
add("block_ack_req", fc(1, 8) + struct.pack("<H", 60) + AP + STA + b"\x04\x00\x10\x00")
add("cf_end", fc(1, 14) + struct.pack("<H", 0) + BC + AP)

# --- negative -------------------------------------------------------------
bad = bytearray(fcs(hdr(0, 12, 0, 314, BC, AP, AP, 291) + struct.pack("<H", 7))); bad[-1] ^= 0xFF
frames.append(("deauth_bad_fcs", bytes(bad)))
frames.append(("too_short_3", b"\xc0\x00\x3a"))
add("protocol_version_1", bytes([0x01, 0x00]) + b"\x00" * 10)
add("header_truncated_addr2", fc(0, 12) + struct.pack("<H", 0) + BC + AP[:3])

with open(os.path.join(out, "frames.hex"), "w") as f:
    for n, b in frames:
        f.write(f"{n}\t{b.hex()}\n")

with open(os.path.join(out, "frames.pcap"), "wb") as f:
    f.write(struct.pack("<IHHiIII", 0xa1b2c3d4, 2, 4, 0, 0, 65535, 127))
    for i, (n, b) in enumerate(frames):
        rt = struct.pack("<BBHI", 0, 0, 9, 0x2) + bytes([0x10])  # Flags: FCS at end
        pkt = rt + b
        f.write(struct.pack("<IIII", i, 0, len(pkt), len(pkt)) + pkt)
print(len(frames), "frames")
