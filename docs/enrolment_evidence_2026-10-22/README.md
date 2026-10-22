# Conservative Wi-Fi inventory display

The old view counted every stored claimed transmitter address immediately, including a single CRC-valid frame and legacy records. Addresses were already keyed by MAC; this was not evidence of duplicate rows for the same MAC. Historical observations, randomized addresses and multiple interfaces can inflate the apparent physical-device count. The available records do not establish which explanation accounts for every entry.

Read-only inspection on 22 October found 1,067 enrolment records, including 559 with one observation and 539 without a usable RF profile (overlapping groups). Only 95 satisfied the new repeated-evidence criterion; 972 remain candidates. These are historical counts, not an occupancy measurement.

The default GUI now requires three usable capture observations spanning at least ten seconds in a single receiver context. The existing enrolment pipeline independently checks frame FCS and transmitter address and takes at most one RF profile sample per identity/context/capture. Repeated packets in one capture cannot meet the criterion. Evidence from different contexts is not added together. Retired profile summaries retain historical corroboration. Operator approval cannot bypass this display criterion.

Unconfirmed records are retained and can be shown using **Include unconfirmed candidates**. They appear amber with an insufficient-evidence label. Repeatedly observed unapproved interfaces remain red. Anonymous RF clusters remain separately optional. No production records were deleted or rewritten by this change, and no SSID/vendor-based merge is performed.

This is a conservative evidence filter, not physical-device authentication. Repeated spoofed addresses can still pass; intermittent real interfaces may remain candidates. Three captures/ten seconds is an explicit operational threshold, not a measured false-positive bound. Archived evidence describes historical observation, not current presence. The GUI therefore says **repeatedly observed interfaces**, never a definite physical-device count.

Regression cases cover absent evidence, operator approval without evidence, short capture floods, separate contexts, sufficient repetition and archived corroboration. The GUI fixture includes explicit synthetic corroborating captures and expects the revised label.

Validation: the application and both test targets built successfully; `wifi_enrolment` and `wifi_gui` passed after rebuilding the final sources. `git diff --check` passed. Restart the rebuilt application to load the new view; an already-running process retains its previous code.
