"""Interactive console restricted to the owned-AP fixture's four commands.
Opening a serial device may reset some boards: run only after fixture flashing.
"""
import argparse
import datetime
import select
import sys
import serial

parser = argparse.ArgumentParser()
parser.add_argument('--port', default='/dev/ttyUSB0')
parser.add_argument('--log', required=True)
args = parser.parse_args()
port = serial.Serial(port=None, baudrate=115200, timeout=0)
port.dtr = False
port.rts = False
port.port = args.port
port.open()
allowed = {'status', 'ap_start', 'disconnect_once', 'ap_stop'}
with open(args.log, 'a', buffering=1) as log:
    def emit(direction, data):
        line = datetime.datetime.now(datetime.timezone.utc).isoformat() + ' ' + direction + ' ' + data
        print(line, flush=True)
        log.write(line + '\n')
    emit('HOST', 'Console ready; commands: status, ap_start, disconnect_once, ap_stop, quit')
    pending = b''
    try:
        while True:
            ready, _, _ = select.select([port.fileno(), sys.stdin], [], [], .2)
            if port.fileno() in ready:
                pending += port.read(4096)
                while b'\n' in pending:
                    line, pending = pending.split(b'\n', 1)
                    emit('RX', line.decode(errors='replace').rstrip('\r'))
                if len(pending) > 8192:
                    emit('RX', pending.decode(errors='replace')); pending = b''
            if sys.stdin in ready:
                line = sys.stdin.readline()
                if not line or line.strip() == 'quit': break
                cmd = line.strip()
                if cmd not in allowed:
                    emit('HOST', 'Rejected unknown command'); continue
                emit('TX', cmd)
                port.write((cmd+'\n').encode()); port.flush()
    finally:
        port.close()
