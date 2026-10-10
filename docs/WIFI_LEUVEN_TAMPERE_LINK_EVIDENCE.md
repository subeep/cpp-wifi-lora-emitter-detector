# Leuven and Tampere: contiguous marker replay and refinement

Latest offline update: [full-span and boundary diagnostic](WIFI_MARKER_SYMBOL_VALIDATION_REPORT.md). The added checks remain development evidence; live behavior and named-family acceptance are unchanged.

9 October 2026. Dataset sources were limited to `/media/sudeep/Storage/datasets/KULeuven_DroneRF` and `/media/sudeep/Storage/datasets/Tampere_DroneRF`. No other dataset was opened. No radio, model, Remote ID decoder, live decision threshold or running monitor was changed.

## Source handling

`prepare_local_link_references.py` opens selected source IQ/ZIP files read-only with `O_NOATIME`, refuses outputs inside the dataset root, reads one lexicographically selected MATLAB member per selected Leuven archive and verifies that member's ZIP CRC. MATLAB v7.3 `uhd_samps` real/imaginary doubles are read with h5py and converted to finite complex-float working copies. Tampere signed interleaved little-endian int16 is copied without conversion. Publisher code is not executed.

Only the first 100 ms of each selected member/file is retained, at the publisher's 100 MSps (Leuven) or 120 MSps (selected Tampere 2.4 GHz files). All derivatives and analysis results are in the workspace. The 8 selected source files' device/inode/size/access-time/modification-time/change-time values matched before and after the work. Full archive/file checksums were not recomputed; this is read-only/stat-preservation and selected-member integrity verification, not a full dataset audit. The original ZIPs remain in place, unmodified.

The reader ran at low CPU/I/O priority. An isolated h5py reader was installed under `/tmp/cyclo-hdf5-reader`; the system Python and dataset directories were not changed. Invocation uses `/usr/bin/python3` with that directory on `PYTHONPATH` (NumPy is also required).

[Derived-source manifest](../data/wifi_drone_analysis/local_link_references_2026-10-09/manifest.json) and [source-preservation check](../data/wifi_drone_analysis/local_link_references_2026-10-09/source_preservation.json).

## Software improvement

`replay_contiguous_link_markers.py` covers each copied interval with overlapping windows of at most 65,536 native samples. Overlap covers the existing 64 µs marker support at the actual input rate, avoiding sparse-window and boundary omissions. It retains raw results and runs at most two bounded refinement trials per window when existing coherence, half-window, slope and alias checks suggest a marker.

Refinement centres the existing FIR/downsampling path on the measured frequency (rounded to 250 kHz), with nominal sweep width plus 2 MHz and a decimation of at most four. It retains all existing marker acceptance thresholds. The 90%-of-Fs digital span is an explicit offline software trial geometry, **not a calibrated receiver passband**. Filtering may change evidence and search multiplicity; it cannot establish a named family or physical source separation.

`marker_recurrence.py` deduplicates overlapping measurements and evaluates an apparent 20 ms up-chirp sequence within one recording, with 100 µs timing tolerance and 250 kHz frequency tolerance. At least three markers are needed. It does not fill missing periods, combine files, assign down-chirps that cadence or claim transmitter identity. These tolerances are development policy, not calibrated error rates. Continuity is not independently verified, so recurrence qualification and accepted family remain false.

## Observed results

Eight 100 ms intervals, 1,508 native analysis windows. All matches below are the reported 18 MHz up-chirp profile.

| Recording context | Raw matching markers | After bounded refinement and deduplication | Apparent spacing |
|---|---:|---:|---|
| Leuven Mini 2 video | 0 | 5 | Approximately 20 ms |
| Leuven Matrice video | 5 | 5 | Approximately 20 ms |
| Tampere Mavic Pro | 5 | 5 | Approximately 20 ms |
| Leuven Mini 2 controller | 0 | 0 | Unavailable |
| Leuven Inspire 2 video | 0 | 0 | Unavailable |
| Tampere Inspire 2 | 0 | 0 | Unavailable |
| Tampere Parrot Disco | 0 | 0 | Unavailable |
| Tampere Parrot Mambo control | 0 | 0 | Unavailable |

Mini 2's native signal had strong coherence but failed the existing frequency-fit residual check. Filtering recovered five candidates while retaining the original threshold. This is an observed development gain, not measured detection recall. Matrice and Mavic Pro repeat the marker in different publisher collections; that supports further testing of OcuSync-related compatibility but does not prove unique protocol identity or distinguish OcuSync generations. Lack of matches in other intervals does not mean lack of a drone, nor does it identify Lightbridge.

[Detailed replay results](../data/wifi_drone_analysis/local_link_marker_refined_2026-10-09/summary.json) retain every native window, refinement and recurrence sequence in per-record files.

## Verification and remaining work

New tests cover no-atime source reads, rejection of in-place output paths, recurrence deduplication, frequency conflicts, missing periods, down-chirp exclusion, invalid inputs and complete 64 µs window coverage at 20/100/120/200 MSps. All passed; Python syntax checks passed. Existing production binaries were reused unchanged; all derived source hashes remained unchanged during replay.

This is an offline development improvement. Automatic live filter refinement/history and a GUI recurrence field have not been enabled. Full-symbol/prefix qualification (a bounded span/contrast diagnostic is now available), calibrated background false positives, independent unit/session tests, source continuity/passband qualification and live USRP performance remain pending. No labelled ordinary router/phone negatives were added because this task was restricted to Leuven and Tampere. Device-role/file labels are context only. Mini 2 video/controller records stay grouped rather than counted as independent validation.
