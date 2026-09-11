"""Minimal TarangNet binary API client (Melange Systems TarangMini ST22LR01),
per TarangNet(TM) API Documentation V2 (TarangNet_TN_STD_WL_v0_0_4).

Used by tools/tarangmini_sf_bw_sweep.py to reconfigure the module's
Default Data Rate (Spreading Factor + Bandwidth) and send test bursts.
Talks over the module's UART API window (see the doc's section 4 - the
module's MODE pin/setting must already be in API mode, not Transparent).
"""
import time

import serial

START = 0x2B


class TarangNet:
    def __init__(self, port, baud=9600, timeout=2.0):
        self.ser = serial.Serial(port, baud, bytesize=8, parity="N", stopbits=1, timeout=timeout)
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
        return self._parse_config_response(resp)

    def config_write(self, cmd_id, value_bytes):
        length = 5 + len(value_bytes)
        frame = bytes([START, length, 0x02, 0x01, (cmd_id >> 8) & 0xFF, cmd_id & 0xFF]) + bytes(
            value_bytes
        )
        resp = self._txrx(frame)
        return self._parse_config_response(resp)

    @staticmethod
    def _parse_config_response(resp):
        if len(resp) < 6 or resp[0] != START:
            return {"raw": resp, "ok": False, "error": "no/short response"}
        length = resp[1]
        ptype = resp[2]
        cmd_id = (resp[3] << 8) | resp[4]
        exec_status = resp[5]
        value = resp[6 : 6 + max(0, length - 5)]
        return {
            "raw": resp,
            "ok": exec_status == 0x00,
            "packet_type": ptype,
            "cmd_id": cmd_id,
            "exec_status": exec_status,
            "value": value,
        }

    def flash_write(self, wait_s=2.5):
        """Saves current config to flash. Required before restart() for a
        config_write to survive - and to take effect at all for params
        the doc marks '<*> A flash write and module restart is required'."""
        frame = bytes([START, 0x05, 0x02, 0x00, 0x00, 0x02])
        return self._parse_config_response(self._txrx(frame, wait_s=wait_s))

    def restart(self, wait_s=2.5):
        frame = bytes([START, 0x05, 0x02, 0x00, 0x00, 0x01])
        return self._parse_config_response(self._txrx(frame, wait_s=wait_s))

    def send_root(self, dest_address: bytes, payload: bytes, wait_s=0.6):
        """Root mode Data Send Command (0x05): broadcast is
        dest_address = bytes.fromhex('ffffffffffffffff')."""
        assert len(dest_address) == 8, "destination address must be 8 bytes"
        length = 2 + 8 + len(payload)
        frame = bytes([START, length, 0x05]) + dest_address + payload
        return self._txrx(frame, wait_s=wait_s)

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
