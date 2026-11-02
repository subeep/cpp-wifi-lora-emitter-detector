# Imported Linux passive DSP subset

`SOURCE_MANIFEST.json` pins the imported files and their original SHA-256 values.
The source files are unmodified. `COPYING`, `THIRD_PARTY.md` and CRC++'s license
retain upstream notices; upstream `THIRD_PARTY.md` also describes components
outside this selected subset. No upstream GUI, SDR ownership, Windows package,
map, network fetcher or transmission executable is built here.

The adapter is `tools/drone_native_worker.cpp`, in a separate receive-only
process. It links the existing `rfmon_drone_turbo` library and system FFTW3f.
The existing Python decoder remains available because native telemetry/model
mapping has incomplete parity. This import does not claim complete OcuSync
control/video decoding, O4 identity, O4+ support or a qualified presence model.

Upstream O4 profile evaluations overlap reserved ML sources. See the integration
report and metadata-only exposure audit before using these features in training
or claiming independent final evaluation. Do not turn candidate outputs into
training labels.
