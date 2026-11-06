# Phantom 4 synchronization experiment

The scan produced a candidate OFDM symbol timing worth pursuing. Frame boundaries, transmitter attribution, carrier allocation and payload recovery remain open.

| Measurement | Result |
|---|---:|
| Phantom 4 recordings read | 50 (48 GB, 100 seconds nominal) |
| Spectral map coverage | 5,999,820,800 of 6,000,000,000 samples |
| Targeted 20 ms replays through stock native DJI worker | 200, zero DroneID packets |
| 1024+128 prefix matches in peak-selected excerpts | 31/200 excerpts, 23/50 files |
| Same test at a separate fixed time | 5/200 excerpts, 5/50 files |
| Same recording and frequency region passing both selections | 0 |
| Comparison recordings, both selections | 0/40 peak excerpts and 0/40 separate excerpts |

All original samples were read and checked for finite values. The final 3,584 samples per file fell outside a complete 8,192-sample map cell. The four contiguous map regions span 2424.375 to 2450.625 MHz. Cell duration was 136.53 microseconds. Each cell averaged 16 Hann-windowed 512-point spectra. We retained the strongest cell in each region and read its surrounding 20 ms of wideband IQ. Views can overlap. The stock decoder chooses its own channel within each parent excerpt. Its zero packet result applies only to those excerpts.

The strongest sample-lag candidates in channelized excerpts included roughly 1371 samples at 15 MSps. We tested a corresponding 1024-sample useful symbol at 11.2 MSps with prefixes of 32, 64, 128 and 256 samples. The 128-sample hypothesis gives 91.43 microseconds of useful symbol, an 11.43 microsecond prefix and a 102.86 microsecond symbol period. Each test chose prefix phase on half the excerpt and scored that phase on the other half. A deliberately incorrect period seven samples longer supplied a comparison. Exploratory selection required first- and second-half prefix contrast above 0.15 and a second-half advantage above 0.10 over the shifted period. These thresholds were chosen during exploration. The other prefix lengths passed in 0, 2 and 0 excerpts respectively.

The 31 peak-selected matches comprised 5 clean, 9 Bluetooth, 8 Wi-Fi and 9 combined-interference excerpts. A separate 20 ms window began at 250 ms in each file, or 750 ms if needed to avoid its discovery interval in the same region. Only one recording passed at both times, in different regions. The comparison set comprised five clean Parrot Disco and five clean DJI Air 2S switched-on recordings. It does not cover the full range of interferers or operating conditions. The lower yield at fixed times and zero same-band repeats leave burst scheduling, channel changes and competing emitters unresolved. Window counts cannot estimate aircraft detection accuracy.

The strongest clean example came from PHA_0000_03.dat at parent offset 2,971,712 samples. The exported grid contains 193 FFT rows after prefix alignment in a partial-band view centered at 2434.21875 MHz. The prefix phase yields a fractional frequency-offset estimate of +1.31 kHz modulo 10.9375 kHz. Rows may span gaps or different transmissions. The export preserves unequalized complex bins and processing metadata for pilot and framing analysis. No decoded payload bytes or independently verified telemetry fields were recovered.

## Reproduction

Run with the repository drone ML Python environment from the repository root. Supply a new output directory to prevent replacing an earlier run.

    python tools/lightbridge_full_scan.py docs/lightbridge_input_inventory.json build/drone_native_worker OUTPUT
    python tools/lightbridge_sync_probe.py OUTPUT
    python tools/lightbridge_prefix_check.py OUTPUT
    python -m tools.lightbridge_symbol_export OUTPUT

The completed run is in data/validation/lightbridge-full-2026-11-06-b. The control-prefix.jsonl file contains the control sources, intervals and test results. Controls used the same map_file, channel selection and evaluate functions. With that file present, run python -m tools.lightbridge_recheck OUTPUT to repeat the fixed-time comparison. Synthetic tests verified localization across chunk boundaries and recovery of an injected prefix at a phase selected on a separate half.

Next, use timing to locate individual contiguous symbol trains across neighboring frequency regions, test pilot recurrence across distinct bursts, and recover a frame with an independent integrity check before reporting telemetry. These observations are development data.
