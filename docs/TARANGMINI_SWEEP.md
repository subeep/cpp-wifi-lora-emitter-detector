# TarangMini sweep tool

Updated 2026-09-18 following repeated `2b038801` responses. That response is a rejected command, not a transmit acknowledgement.

## Current board limitation

The board assessed in this repository reported `TarangConnect_LW_S201_LoRaWAN_CA_v1_0_4`. The supplied API manual describes `TarangNet_TN_STD_WL_v0_0_4`, a different firmware family. This tool now identifies the mismatch and stops before writes. **This update does not provide a working LoRaWAN transmitter.** A matching LW-S201 API reference is still required. Do not assume that LoRaWAN data-rate settings can reproduce the TarangNet SF/BW sweep.

## Inspect without changing settings

Close other serial clients before using the tool. Prefer the adapter's stable `/dev/serial/by-id/...` path when available. The module must be using its binary API, not a transparent serial bridge.

```sh
python3 tools/tarangmini_sf_bw_sweep.py --port /dev/ttyUSB0 --inspect
```

This prints firmware identity. For the exact supported TarangNet version it also reads frequency, connection mode and current rate. For LoRaWAN/unknown firmware it only reports identity and the unsupported status. The LoRaWAN identity read is based on the response observed during the assessment, not a complete LW-S201 API implementation.

## Sweep supported TarangNet firmware

Only `TarangNet_TN_STD_WL_v0_0_4` is currently allowed to write or transmit:

```sh
python3 tools/tarangmini_sf_bw_sweep.py --port /dev/ttyUSB0 --read-freq
python3 tools/tarangmini_sf_bw_sweep.py --port /dev/ttyUSB0 --only 0x0f --reps 3
```

Set the GUI frequency lock to the frequency printed by the tool. The script reads root/router mode and chooses the documented send command accordingly; it does not change connection mode or carrier frequency. Router operation may depend on the appropriate network/root setup. A module uplink ACK is not proof of reception or payload decoding by the SDR.

Defaults exclude ambiguous index `0x02` and SF5/SF6 indices that appear only in the manual's read table. Explicit `--only` selection permits these as experimental entries with a warning. Rate labels are the manual's mappings, not measured PHY settings.

The sweep verifies configuration replies, restart replies, firmware after reboot and rate readback. It stops on Invalid Command, NACK, missing ACK, frequency/mode changes or malformed responses. `--tx-wait` defaults to five seconds before collecting a send response; it can be increased for slow configurations. The protocol has no transaction sequence number, so exclusive use of the UART is necessary.

After any attempted rate write, cleanup checks firmware again before restoring the original rate, saving, restarting and verifying readback. If restoration fails, the tool reports that settings are unverified and exits with an error. Unknown/LoRaWAN firmware never enters the write/restore path. Interrupting restoration itself cannot guarantee recovery. Read-only inspection and a sweep that never attempted a rate change do not issue restore writes.

The shared client also blocks direct write/flash/restart/transmit calls until supported firmware identification succeeds. This intentionally changes the previous client behavior. Existing Python processes retain their old code; editing files does not update an already-running sweep.

## Validation

```sh
python3 -m unittest discover -s tests -p test_tarangmini_tools.py -v
```

Seven mocked tests cover unsupported firmware (no writes), client write guards, both send modes, ACK/rejection/timeout handling, interrupted sweep restoration, response lengths/command matching, read-only inspection and invalid CLI input. No hardware transmission was performed for this revision. Tests validate host behavior, not the undocumented LW-S201 radio API.
