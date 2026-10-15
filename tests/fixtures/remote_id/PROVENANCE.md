# Wi-Fi OpenDroneID reference fixtures

Copied read-only from the local Drone Recorder project on 14 October 2026.
`remote_id_full_reference.txt` was generated using unmodified OpenDroneID Core C
revision ec44332f67be38035d1f9dace4ce8e0bdf3cc2c8. The historical-version JSON
records the pinned v0/v1/v2 encoder revisions, source hashes and normalizations.
These are synthetic protocol vectors, not radio recordings.

`reference_parser.py` is the existing local receiver parser, retained solely as a
port-comparison oracle; the production application does not load or run it.
`monitor_frames.json` wraps the independent full-vector messages in a Beacon and
adds Python zlib CRC-32; its changed-ID example is an intentional local test
mutation. The parser test includes independently encoded vectors plus local
malformation and ambiguity controls. Passing the differential test establishes
port agreement, not independent certification of every interpretation.

Source file SHA-256:
- `remote_id_full_reference.txt`: `d2e0077cf2cefb1d6a740297ebeb8e987aa39025fb1d42ed4bbd95a40aaa88f5`
- `remote_id_versions_reference.json`: `ad20190c348161800f2d300dc0fd45a81f90bb4957818e559939ea2f5fd09255`
- `reference_parser.py`: `0946f9f4f8711aa6260f4a93c0ed2cfa5ec319b5542cd20dbd59c658a0248d5c`
- `monitor_frames.json`: `7fc4431a10691aece222668ccc6807ad1495932faf4bec4e7cd9eae7d1a80bfb`
