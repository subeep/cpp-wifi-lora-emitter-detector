"""Minimal TarangNet binary API client (Melange Systems TarangMini ST22LR01),
per TarangNet(TM) API Documentation V2 (TarangNet_TN_STD_WL_v0_0_4).

Used by tools/tarangmini_sf_bw_sweep.py to reconfigure the module's
Default Data Rate (Spreading Factor + Bandwidth) and send test bursts.
Talks over the module's UART API window (see the doc's section 4 - the
module's MODE pin/setting must already be in API mode, not Transparent).

SUPPORTED_FIRMWARE: this board was reflashed on 2026-09-18 from the
LW-S201 LoRaWAN firmware to "TarangNet_TN_LW_STD_WL_v0_0_6" (see
TARANGMINI_ASSESSMENT.md and data/tarangmini_firmware_recovery/), a
different version string from the exact one the v0_0_4 API doc names.
Added to the accepted set on the assumption that it is the same
TarangNet STD/WL command family with a version bump (file name and the
command IDs read back in this session's own checks are consistent with
that) - NOT independently confirmed against a v0_0_6-specific manual,
since none was supplied. If a write/transmit ever behaves unexpectedly
on this firmware, treat the command MAP itself as unverified, not just
the specific call that misbehaved.
"""
import time

import serial

START = 0x2B

SUPPORTED_FIRMWARE = {
    "TarangNet_TN_STD_WL_v0_0_4",
    "TarangNet_TN_LW_STD_WL_v0_0_6",
}


class TarangNet:
    def __init__(self, port, baud=9600, timeout=2.0):
        self.verified_firmware = None
        self.ser = serial.Serial(port=None, baudrate=baud, bytesize=8, parity="N", stopbits=1,
                                 timeout=timeout, exclusive=True)
        self.ser.dtr = False
        self.ser.rts = False
        self.ser.port = port
        self.ser.open()
        time.sleep(0.2)
        self.ser.reset_input_buffer()

    def _txrx(self, frame, wait_s=0.6):
        self.ser.reset_input_buffer()
        self.ser.write(frame)
        time.sleep(wait_s)
        return self.ser.read(512)

    def config_read(self, cmd_id):
        frame = bytes([START, 0x05, 0x02, 0x00, (cmd_id >> 8) & 0xFF, cmd_id & 0xFF])
        resp = self._txrx(frame)
        return self._parse_config_response(resp, cmd_id)

    def config_write(self, cmd_id, value_bytes):
        self.require_supported_firmware()
        length = 5 + len(value_bytes)
        frame = bytes([START, length, 0x02, 0x01, (cmd_id >> 8) & 0xFF, cmd_id & 0xFF]) + bytes(
            value_bytes
        )
        resp = self._txrx(frame)
        return self._parse_config_response(resp, cmd_id)

    @staticmethod
    def _frames(raw):
        """Walk complete frames; never treat an event or partial frame as an ACK."""
        pos = 0
        while pos + 2 <= len(raw):
            if raw[pos] not in (0x2B, 0x2D):
                pos += 1
                continue
            end = pos + raw[pos + 1] + 1
            if raw[pos + 1] < 2:
                pos += 1
                continue
            if end > len(raw):
                break
            yield raw[pos:end]
            pos = end

    @staticmethod
    def _parse_config_response(resp, expected_cmd=None):
        for frame in TarangNet._frames(resp):
            if frame == bytes.fromhex("2b038801"):
                return {"raw": resp, "ok": False, "error": "Module rejected command (2b038801)"}
            if frame[0] != START or frame[2] != 0x82 or len(frame) < 6:
                continue
            cmd_id = int.from_bytes(frame[3:5], "big")
            if expected_cmd is not None and cmd_id != expected_cmd:
                continue
            return {"raw": resp, "ok": frame[5] == 0, "packet_type": frame[2],
                    "cmd_id": cmd_id, "exec_status": frame[5], "value": frame[6:]}
        return {"raw": resp, "ok": False, "error": "No complete matching configuration response"}

    @staticmethod
    def parse_send_response(resp):
        for frame in TarangNet._frames(resp):
            if frame == bytes.fromhex("2b038801"):
                return {"ok": False, "error": "Invalid command: module rejected transmit request", "raw": resp}
            if len(frame) == 4 and frame[:3] == bytes.fromhex("2b0381"):
                return {"ok": frame[3] == 0, "error": "Uplink NACK" if frame[3] == 1 else
                        "Unknown TX status" if frame[3] else "", "raw": resp}
        return {"ok": False, "error": "No complete uplink ACK (timeout or unrelated response)", "raw": resp}

    def identify(self):
        self.verified_firmware = None
        response = self.config_read(0x0006)
        value = response.get("value", b"").rstrip(b"\0").decode("ascii", errors="replace")
        if response.get("ok") and value in SUPPORTED_FIRMWARE:
            self.verified_firmware = value
            return value
        # This read-shaped ID returned the firmware string on the observed LW-S201
        # board. It is only a diagnostic; no LoRaWAN writes are implemented.
        alternate = self.config_read(0x000B)
        alt = alternate.get("value", b"").rstrip(b"\0").decode("ascii", errors="replace")
        if alternate.get("ok") and alt.startswith("TarangConnect_LW_"):
            return alt
        return "Unknown/unsupported firmware (read 0006: " + response.get("raw", b"").hex() + ")"

    def require_supported_firmware(self):
        if self.verified_firmware not in SUPPORTED_FIRMWARE:
            raise RuntimeError("Writes/transmission blocked: identify a supported TarangNet firmware first. "
                               "LW-S201 LoRaWAN requires its own API; the supplied TarangNet commands do not apply.")

    def flash_write(self, wait_s=2.5):
        """Saves current config to flash. Required before restart() for a
        config_write to survive - and to take effect at all for params
        the doc marks '<*> A flash write and module restart is required'."""
        self.require_supported_firmware()
        frame = bytes([START, 0x05, 0x02, 0x00, 0x00, 0x02])
        return self._parse_config_response(self._txrx(frame, wait_s=wait_s), 0x0002)

    def restart(self, wait_s=2.5):
        self.require_supported_firmware()
        frame = bytes([START, 0x05, 0x02, 0x00, 0x00, 0x01])
        return self._parse_config_response(self._txrx(frame, wait_s=wait_s), 0x0001)

    def send_root(self, dest_address: bytes, payload: bytes, wait_s=0.6):
        """Root mode Data Send Command (0x05): broadcast is
        dest_address = bytes.fromhex('ffffffffffffffff')."""
        self.require_supported_firmware()
        if len(dest_address) != 8 or len(payload) > 245:
            raise ValueError("Root send requires an 8-byte address and <=245 payload bytes")
        length = 2 + 8 + len(payload)
        frame = bytes([START, length, 0x05]) + dest_address + payload
        return self._txrx(frame, wait_s=wait_s)

    def send_router(self, payload: bytes, wait_s=0.6):
        self.require_supported_firmware()
        if len(payload) > 253:
            raise ValueError("Router payload exceeds UART frame size")
        return self._txrx(bytes([START, 2 + len(payload), 0x04]) + payload, wait_s=wait_s)

    def close(self):
        self.ser.close()


DATA_RATE_NAMES = {
    0x00: "SF12,BW125",
    0x01: "SF12,BW250",
    0x02: "SF12,BW250",
    0x03: "SF11,BW125",
    0x04: "SF11,BW250",
    0x05: "SF11,BW500",
    0x06: "SF10,BW125",
    0x07: "SF10,BW250",
    0x08: "SF10,BW500",
    0x09: "SF09,BW125",
    0x0A: "SF09,BW250",
    0x0B: "SF09,BW500",
    0x0C: "SF08,BW125",
    0x0D: "SF08,BW250",
    0x0E: "SF08,BW500",
    0x0F: "SF07,BW125",
    0x10: "SF07,BW250",
    0x11: "SF07,BW500",
    0x12: "SF06,BW125",
    0x13: "SF06,BW250",
    0x14: "SF06,BW500",
    0x15: "SF05,BW125",
    0x16: "SF05,BW250",
    0x17: "SF05,BW500",
}

FREQ_NAMES = {
    bytes.fromhex("339060E0"): "865.1MHz",
    bytes.fromhex("33936E20"): "865.3MHz",
    bytes.fromhex("33967B60"): "865.5MHz",
    bytes.fromhex("339988A0"): "865.7MHz",
    bytes.fromhex("339C95E0"): "865.9MHz",
    bytes.fromhex("339FA320"): "866.1MHz",
    bytes.fromhex("33A2B060"): "866.3MHz",
    bytes.fromhex("33A5BDA0"): "866.5MHz",
    bytes.fromhex("33A8CAE0"): "866.7MHz",
    bytes.fromhex("33ABD820"): "866.9MHz",
    bytes.fromhex("33AEE560"): "867.1MHz",
    bytes.fromhex("33B1F2A0"): "867.3MHz",
    bytes.fromhex("33B4FFE0"): "867.5MHz",
    bytes.fromhex("33B80D20"): "867.7MHz",
    bytes.fromhex("33BB1A60"): "867.9MHz",
}
