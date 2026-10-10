# Offline chirp span and boundary validation

9 October 2026. This extends the Leuven/Tampere development workflow. Production receiver, child process, GUI labels, named-family acceptance and running monitor behavior are unchanged. No model training, radio use, transmission or Remote ID work was performed.

## Implemented

- `marker_symbol.py` checks a reported approximately 66.67 µs chirp span, using the existing three fixed slope profiles. It searches at most 17 start positions in the difference between the original 64 µs marker support and the longer span, within one window of at most 65,536 samples.
- After nominal dechirping, 34 blocks must each have coherent-power fraction at least 0.8 and energy at least 0.1 of the median block energy. Two surrounding probes, 8–6 µs before and 2–4 µs after the proposed span, check for reduced coherence or energy. The earlier probe leaves room for a possible prefix; it does **not** verify that prefix.
- Results distinguish insufficient surrounding context, unsupported sample rate, unsupported full span, a span without boundary contrast, and a span with boundary contrast. Start coordinates map back to the original recording, including the copied interval offset and any FIR decimation.
- `qualify_replayed_marker_symbols.py` reads checksum-verified workspace copies and retained replay results. It reproduces the existing FIR geometry when evaluating a refined candidate. A real Mini 2 window cross-check agreed with the C++ frequency-fit mean and residual within 1 Hz. Raw/refined or overlapping views of one marker are grouped before distinct-marker counts are reported.
- `replay_contiguous_link_markers.py --symbol-context` extends overlap to cover 66.67 µs plus 8 µs before and 4 µs after. Default overlap and production behavior stay unchanged.
- `prepare_local_link_references.py --start-seconds 0.1` supports a second, bounded 100 ms interval. `--sources` accepts only the existing eight explicit Leuven/Tampere filenames. Short members are recorded as skips, without padding or joining files. The reader remains read-only with `O_NOATIME`; all outputs go outside the dataset root.

These thresholds are development policy. Selection and verification reuse the same IQ; they are not independent evidence. A longer sweep can occupy the span, and the surrounding probes are only local contrast checks. Exact symbol duration, cyclic-prefix content, uniqueness to a protocol and physical transmitter identity remain unestablished. `full_symbol_duration_established` and `family_accepted` remain false even on a passing diagnostic.

## Initial-interval results

| Publisher recording context | Distinct existing markers | Span plus surrounding contrast |
|---|---:|---:|
| Leuven Mini 2 video | 5 | 3 |
| Leuven Matrice video | 5 | 5 |
| Tampere Mavic Pro | 5 | 5 |
| Other five selected contexts | 0 | Not evaluated: no eligible marker |

The two failed Mini 2 checks had minimum block coherence around 0.786, below the fixed 0.8 threshold. Thresholds were not adjusted to pass these recordings. The check is evaluated only after the existing marker gate; these counts do not measure whole-dataset recall or false-positive rates.

[Initial interval diagnostics](../data/wifi_drone_analysis/local_link_symbol_validation_2026-10-09/summary.json).

## Additional intervals and combined results

The second 100 ms intervals were evaluated for Leuven Mini 2 video and Inspire 2 video, plus all four selected Tampere recordings. The selected Leuven controller and Matrice members contain only 10 million samples at 100 MSps; neither supplies a second interval. Their files were not joined to other members. The initial controller import attempt stopped on the insufficient interval; subsequent metadata inspection confirmed its length. The reader now records short intervals explicitly as skips.

Across **14 intervals / 1.4 seconds / 2,712 native windows**, there were **25 distinct eligible candidate markers**, of which **22** passed the span-and-contrast diagnostic:

| Publisher context | Evaluated candidate markers | Span plus contrast |
|---|---:|---:|
| Leuven Mini 2 video, two intervals | 10 | 7 |
| Leuven Matrice video, one interval | 5 | 5 |
| Tampere Mavic Pro, two intervals | 10 | 10 |

Other selected contexts still supplied no eligible marker. This is not a drone-absence decision. Each positive interval retained an apparent five-marker, approximately 20 ms sequence. All second intervals remain in the original recording/session group; none is an independent holdout.

[Mini 2 continuation diagnostics](../data/wifi_drone_analysis/local_link_continuation_symbol_validation_2026-10-09/summary.json), [other continuation diagnostics](../data/wifi_drone_analysis/local_link_continuations_more_symbol_validation_2026-10-09/summary.json), and [combined verification](../data/wifi_drone_analysis/local_link_symbol_validation_2026-10-09/combined_verification.json).

All derived IQ checksums and diagnostic policy checksums were verified at completion. Source imports used read-only/no-atime access and retained unchanged before/after source-stat checkpoints. The original eight paths under `/media/sudeep/Storage` were unavailable when attempting the final check after the continuation turn; **final source-stat preservation could not be reverified**. No alternative mount or other dataset was inspected. Analysis completed from the workspace copies. [Final source-check status](../data/wifi_drone_analysis/local_link_symbol_validation_2026-10-09/source_preservation.json).

## Validation and milestones

1. **Completed:** full-span diagnostic, boundary-context overlap, offset-aware source reader, explicit short-interval skips and provenance-preserving reports.
2. **Completed:** synthetic tests at 20/25/100/120/200 MSps, all three slopes, 64 µs short signals, 100 µs continuous sweeps, clipped context, wrong slope/carrier, tones, noise, zero energy, input bounds, unsupported bandwidth and extreme scaling. Reader tests cover offset correctness, incomplete intervals, source metadata preservation and prohibited output locations. All 23 standalone tests passed; affected tests were rerun after subsequent changes.
3. **Pending:** direct cyclic-prefix/repetition structure verification and wider testing of channel distortion, mixtures, CFO and clock mismatch. Passing block coherence alone is not sufficient.
4. **Pending:** independent recording sessions/units and representative router/phone/other non-drone confusables, with fixed thresholds and documented uncertainty. Additional intervals from the same file remain development data.
5. **Pending:** receiver passband/continuity qualification and live USRP cost/cadence tests before adding automatic refinement or recurrence/span results to the live GUI.
6. **Pending:** accepted named-family decisions and distinguishing proprietary link versions. No new Lightbridge or OcuSync-generation claim follows from these results.

Only the permitted Leuven/Tampere sources were accessed. Other datasets remain untouched.
