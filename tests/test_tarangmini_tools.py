"""Protocol and sweep control tests; no serial hardware or RF transmissions."""
import contextlib
import io
import pathlib
import sys
import unittest
from types import SimpleNamespace
from unittest.mock import patch
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / 'tools'))
from tarangnet_api import TarangNet
import tarangmini_sf_bw_sweep as sweep


class Board(TarangNet):
    def __init__(self, firmware='TarangNet_TN_STD_WL_v0_0_4', mode=0, reply=b'\x2b\x03\x81\x00'):
        self.verified_firmware = None
        self.firmware, self.mode, self.reply = firmware, mode, reply
        self.rate = 3
        self.frames = []

    def _txrx(self, frame, wait_s=0.6):
        self.frames.append(frame)
        if frame[2] in (4, 5):
            if self.reply is KeyboardInterrupt:
                raise KeyboardInterrupt
            return self.reply
        cmd = int.from_bytes(frame[4:6], 'big')
        status, value = 0, b''
        if frame[3] == 1:
            self.rate = frame[6]
        elif cmd == 6:
            if self.firmware.startswith('TarangNet'):
                value = self.firmware.encode()
            else:
                status = 1
        elif cmd == 11:
            value = self.firmware.encode()
        elif cmd == 8:
            value = bytes([self.rate])
        elif cmd == 15:
            value = (865100000).to_bytes(4, 'big')
        elif cmd == 21:
            value = bytes([self.mode])
        return bytes([43, 5 + len(value), 130, frame[4], frame[5], status]) + value


def args(**extra):
    a = dict(inspect=False, read_freq=False, combos=[(15, 7, 125000)], reps=1, gap=.5, tx_wait=.5)
    a.update(extra)
    return SimpleNamespace(**a)


class Tests(unittest.TestCase):
    def run_board(self, board, **options):
        with patch.object(sweep.time, 'sleep'), contextlib.redirect_stdout(io.StringIO()):
            return sweep.run(board, args(**options))

    def test_lorawan_never_writes_or_transmits(self):
        b = Board('TarangConnect_LW_S201_LoRaWAN_CA_v1_0_4')
        with self.assertRaisesRegex(RuntimeError, 'LW-S201'):
            self.run_board(b)
        self.assertEqual([f.hex() for f in b.frames], ['2b0502000006', '2b050200000b'])
        self.assertEqual(self.run_board(b, inspect=True), 0)
        self.assertTrue(all(f[2:4] == b'\x02\x00' for f in b.frames))

    def test_writes_require_identity(self):
        b = Board()
        for call in (lambda: b.config_write(8, [1]), b.flash_write, b.restart,
                     lambda: b.send_router(b'x'), lambda: b.send_root(b'\xff'*8, b'x')):
            with self.assertRaises(RuntimeError): call()
        self.assertEqual(b.frames, [])

    def test_router_and_root_sends_and_restore(self):
        for mode in (0, 1):
            b = Board(mode=mode)
            self.assertEqual(self.run_board(b), 0)
            tx = [f for f in b.frames if f[2] in (4, 5)]
            self.assertEqual(len(tx), 1)
            self.assertEqual(tx[0][2], 4 + mode)
            self.assertEqual(tx[0][1], len(tx[0])-1)
            self.assertEqual(b.rate, 3)
            writes = [f[6] for f in b.frames if f[2:4] == b'\x02\x01']
            self.assertEqual(writes, [15, 3])

    def test_reject_and_interrupt_restore(self):
        for reply, exception in ((bytes.fromhex('2b038801'), RuntimeError),
                                 (bytes.fromhex('2b038101'), RuntimeError),
                                 (b'', RuntimeError), (KeyboardInterrupt, KeyboardInterrupt)):
            b = Board(reply=reply)
            with self.assertRaises(exception): self.run_board(b)
            self.assertEqual(b.rate, 3)
            self.assertEqual(len([f for f in b.frames if f[2] in (4, 5)]), 1)

    def test_inspection_does_not_write(self):
        b = Board()
        self.run_board(b, inspect=True)
        self.assertTrue(all(f[2:4] == b'\x02\x00' for f in b.frames))

    def test_response_matching_and_events(self):
        parse = TarangNet._parse_config_response
        self.assertFalse(parse(bytes.fromhex('2b0682000800'), 8)['ok'])
        self.assertFalse(parse(bytes.fromhex('2b068200090001'), 8)['ok'])
        self.assertFalse(parse(bytes.fromhex('2b038801'), 8)['ok'])
        response = bytes.fromhex('2b0305782b0682000900012b068200080003')
        self.assertEqual(parse(response, 8)['value'], b'\x03')
        self.assertFalse(TarangNet.parse_send_response(bytes.fromhex('2b0388'))['ok'])
        self.assertTrue(TarangNet.parse_send_response(bytes.fromhex('2b0305782b038100'))['ok'])

    def test_invalid_cli_fails_before_open(self):
        with patch.object(sweep, 'TarangNet') as factory, contextlib.redirect_stderr(io.StringIO()):
            for argv in (['--only', '0xff'], ['--gap', 'nan'], ['--reps', '0']):
                with self.assertRaises(SystemExit): sweep.main(argv)
            factory.assert_not_called()


if __name__ == '__main__':
    unittest.main()
