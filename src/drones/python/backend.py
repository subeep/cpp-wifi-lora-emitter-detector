#!/usr/bin/env python3
"""Receive-only IQ analysis. Same engine serves the UI and offline capture tests."""
import argparse
import contextlib
import ctypes
import json
import os
from pathlib import Path
import struct
import sys
import time
import warnings
import resource

# Child-only bounds; parent monitoring and its environment are untouched.
resource.setrlimit(resource.RLIMIT_AS, (1536*1024*1024, 1536*1024*1024))
try:
    os.nice(10)
except OSError:
    pass

for name in ("OPENBLAS_NUM_THREADS", "OMP_NUM_THREADS", "MKL_NUM_THREADS"):
    os.environ[name] = "1"
os.environ.setdefault("MPLBACKEND", "Agg")
os.environ.setdefault("MPLCONFIGDIR", "/tmp/drone-detector-mpl")
sys.path.insert(0, str(Path(__file__).parent / "reference"))
import numpy as np
from scipy import signal
from SpectrumCapture import SpectrumCapture
from Packet import Packet
from qpsk import Decoder
from goldgen import gold
from droneid_packet import DroneIDPacket
from packetizer import coarse_activity
from telemetry import packet_telemetry
HEADER = struct.Struct("<4sIdddQ")
MAX_SAMPLES = 2_000_000


def crc24(data):
    crc = 0
    for byte in data:
        crc ^= int(byte) << 16
        for _ in range(8):
            crc <<= 1
            if crc & 0x1000000:
                crc ^= 0x1864CFB
    return crc & 0xffffff


class Turbo:
    def __init__(self, path):
        self.lib = ctypes.CDLL(str(path))
        self.lib.drone_turbo_decode.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
        self.lib.drone_turbo_decode.restype = ctypes.c_int
        self.scrambler = gold(1600, 7200, 0x12345678).astype(bool)

    def decode(self, symbols):
        values = np.delete(np.asarray(symbols, dtype=np.uint8), 300, axis=1)
        bits = ((np.repeat(values, 2, axis=1) & np.tile([1, 2], 600)) != 0)
        bits = bits[-6:].reshape(-1)
        if len(bits) != 7200:
            raise ValueError("Invalid DJI symbol count")
        soft = np.where(bits ^ self.scrambler, 63, -63).astype(np.int8)
        output = np.zeros(176, dtype=np.uint8)
        if self.lib.drone_turbo_decode(soft.ctypes.data, output.ctypes.data):
            return None
        raw = output.tobytes()
        return raw if crc24(raw) == 0 else None


def observation(protocol, link, evidence, frequency, power, confirmed=False, **fields):
    return dict(protocol=protocol, link=link, evidence=evidence, frequency=frequency,
                power=power, confirmed=confirmed, **fields)


class Analyzer:
    def __init__(self, turbo_path):
        self.turbo = Turbo(turbo_path)
        self.tail = np.empty(0, np.complex64)
        self.key = None
        self.seen = {}

    def decode_frame(self, frame, legacy=False):
        # CP synchronization estimates carrier offset modulo the 15 kHz spacing.
        # Welch centering can land on a neighboring carrier, especially after
        # channelization. Retry only the adjacent bins, retaining both CRC checks.
        # Fast timing search first; retain the original exhaustive path on failure.
        for fast_timing in (True, False):
            for correction in (0, 15000, -15000):
                try:
                    shifted = frame.copy() if not correction else frame*np.exp(
                        2j*np.pi*correction*np.arange(len(frame))/15.36e6)
                    packet = Packet(shifted, enable_zc_detection=False, legacy=legacy, fast_timing=fast_timing)
                    decoder = Decoder(packet.get_symbol_data(skip_zc=True))
                    for rotation in range(4):
                        decoder.raw_data_to_symbol_bits(rotation)
                        decoded = self.turbo.decode(decoder.sym_bits)
                        if decoded is None:
                            continue
                        parsed = DroneIDPacket(decoded)
                        if parsed.check_crc() and parsed.droneid["pkt_len"] == 88 and parsed.droneid["version"] in (1, 2):
                            return dict(parsed.droneid), correction
                except (ValueError, IndexError, TypeError, UnicodeError, struct.error, FloatingPointError):
                    continue
        return None, None

    def analyze(self, samples, rate, frequency, timestamp=0., epoch=0):
        if not np.isfinite(rate) or not 15.26e6 <= rate <= 100e6 or not np.isfinite(frequency) or not np.isfinite(timestamp):
            raise ValueError("DJI decoder needs finite metadata and 15.26–100 MSps")
        if not (2.3e9 < frequency < 2.6e9 or 5.6e9 < frequency < 6e9):
            raise ValueError("Unsupported DJI receive band")
        if not 0 < len(samples) <= MAX_SAMPLES or not np.isfinite(samples).all():
            raise ValueError("Invalid IQ samples")
        key = (rate, frequency, epoch)
        if key != self.key:
            self.tail = np.empty(0, np.complex64)
            self.seen.clear()
            self.key = key
        carried = len(self.tail)
        raw = np.concatenate((self.tail, samples))
        self.tail = raw[-int(rate*.002):].copy()
        started = time.monotonic()
        events, candidates, rejected = [], 0, 0
        # Do not run the DJI demodulator on a band-limited DIY acquisition.
        if rate >= 15.26e6 and (2.3e9 < frequency < 2.6e9 or 5.6e9 < frequency < 6e9):
            with open(os.devnull, "w") as sink, contextlib.redirect_stdout(sink), warnings.catch_warnings():
                warnings.simplefilter("ignore")
                activity = coarse_activity(raw, rate)
                for legacy in (False, True):
                    capture = SpectrumCapture(raw, Fs=rate, legacy=legacy, activity=activity)
                    candidates += len(capture.packets)
                    for i in range(min(len(capture.packets), 24)):
                        try:
                            frame = capture.get_packet_samples(i)
                            payload, carrier_correction = self.decode_frame(frame, legacy)
                            if payload is None:
                                rejected += 1
                                continue
                            serial = payload["serial_number"]
                            if not serial or not serial.isprintable():
                                rejected += 1
                                continue
                            identity = (serial, payload["sequence_number"], payload["gps_time"])
                            if identity in self.seen:
                                continue
                            self.seen[identity] = timestamp
                            if len(self.seen) > 512:
                                del self.seen[next(iter(self.seen))]
                            power = float(10*np.log10(np.mean(np.abs(capture.packets[i])**2)+1e-20))
                            # Burst start on the receiver clock; raw[0] is `carried` samples before `timestamp`.
                            burst_time = timestamp + capture.packet_starts[i] - carried/rate
                            events.append(observation("DJI DroneID", "Identification broadcast",
                                "OFDM + descrambling + turbo FEC + CRC24A + payload CRC16 valid",
                                frequency, power, True, serial=serial, timestamp=burst_time,
                                model=payload["device_type"] or "Unknown DJI model",
                                sequence=payload["sequence_number"], latitude=payload["latitude"],
                                longitude=payload["longitude"], altitude=payload["altitude"],
                                legacy=legacy, carrier_correction_hz=carrier_correction,
                                telemetry=packet_telemetry(payload)))
                        except (ValueError, IndexError, TypeError, UnicodeError, struct.error, FloatingPointError):
                            rejected += 1
        for event in events:
            event.setdefault("timestamp", timestamp)
        return dict(events=events, candidates=candidates, rejected=rejected,
                    processing_ms=(time.monotonic()-started)*1000)

def read_exact(stream, size):
    buf = bytearray()
    while len(buf) < size:
        part = stream.read(size-len(buf))
        if not part:
            if not buf:
                return None
            raise EOFError("Truncated IQ message")
        buf.extend(part)
    return bytes(buf)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--turbo", required=True)
    parser.add_argument("--input")
    parser.add_argument("--rate", type=float, default=50e6)
    parser.add_argument("--frequency", type=float, default=2444.5e6)
    args = parser.parse_args()
    analyzer = Analyzer(args.turbo)
    if args.input:
        with open(args.input, "rb") as stream:
            position = 0
            while True:
                data = stream.read(min(MAX_SAMPLES, int(args.rate*.02))*8)
                if not data:
                    break
                samples = np.frombuffer(data, "<c8")
                print(json.dumps(analyzer.analyze(samples, args.rate, args.frequency, position/args.rate), allow_nan=False), flush=True)
                position += len(samples)
        return
    print(json.dumps({"ready": True, "fec": "LTE turbo + CRC24A + CRC16"}), flush=True)
    while True:
        header = read_exact(sys.stdin.buffer, HEADER.size)
        if header is None:
            break
        magic, count, rate, frequency, timestamp, epoch = HEADER.unpack(header)
        if magic != b"DDIQ" or count > MAX_SAMPLES:
            raise ValueError("Invalid IQ message")
        payload = read_exact(sys.stdin.buffer, count*8)
        if payload is None:
            raise EOFError("Missing IQ payload")
        try:
            result = analyzer.analyze(np.frombuffer(payload, "<c8"), rate, frequency, timestamp, epoch)
        except Exception as exc:
            result = {"error": f"{type(exc).__name__}: {exc}", "events": []}
        print(json.dumps(result, allow_nan=False), flush=True)


if __name__ == "__main__":
    main()
