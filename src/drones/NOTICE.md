# DJI DroneID add-on provenance

Integrated 14 October 2026 from the user's local `Drone_recorder.` working copy.
`SOURCE_MANIFEST.json` records hashes of the actual source inputs, including local
modifications; the recorded Git HEAD alone does not identify that working copy.

The reference-derived DSP and received test IQ originate in
RUB-SysSec/DroneSecurity, commit `9ff819843bee48fb140a0704ec78aff757896dea`,
under AGPL-3.0. Its full licence is retained in `python/reference/LICENSE`.
The local recorder had already adapted timing, coarse burst selection, NumPy
compatibility, instance-local parsing and telemetry presentation.

The turbo decoder originates in ttsou/turbofec, commit
`6de1f4604933d6c21a0ff0c75401cffa7debf3cd`, GPL-3.0-or-later. Licence,
authors and source notices are retained in `third_party/drone_turbofec`.
The C bridge and C++ worker are reused from the user's recorder project.

Integration adaptations: DJI-only Python backend; removal of BLE, OpenDroneID,
MAVLink and generic waveform execution; four-chunk queue; bounded input and child
resources; explicit source provenance; independent capture continuity epochs;
separate live/offline state; optional scanner observer; native ImGui Drones page.
No runtime dependency on the other project's directory remains.

These dependencies are copyleft; process isolation is a reliability boundary,
not a claim that distribution or source-availability obligations disappear.
Retain the original notices and licence texts with redistributed source/artifacts.

The separate native Wi-Fi Remote ID parser added on 15 October 2026 ports the
user's existing `remote_id.py` message/envelope interpretation. Its pinned
reference-vector provenance and comparison-oracle hashes are recorded in
`tests/fixtures/remote_id/PROVENANCE.md`. No OpenDroneID Core C runtime/library
source is embedded by this port. See `docs/WIFI_OPENDRONEID_INTEGRATION.md`.
