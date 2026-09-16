# Offline IEEE vendor assignments

`wifi_vendors.inc` contains assignment prefixes and organization names from IEEE's
public MA-L, MA-M and MA-S registries, downloaded on 2026-09-16:

- https://standards-oui.ieee.org/oui/oui.csv
- https://standards-oui.ieee.org/oui28/mam.csv
- https://standards-oui.ieee.org/oui36/oui36.csv

Only public prefix/name facts are included; organization addresses are omitted.
IEEE does not assert copyright in the OUI Public Listing or restrict its
redistribution. This status is also documented in Debian ieee-data's copyright
file: https://metadata.ftp-master.debian.org/changelogs/main/i/ieee-data/ieee-data_20210605.1_copyright
IEEE encourages obtaining and updating the data directly from IEEE.

To refresh, download those three CSVs and run:

    python3 tools/build_wifi_vendors.py oui.csv mam.csv oui36.csv

The table is compiled into the application: no network or working-directory
assumption at runtime. Lookup uses the longest matching /36, /28, or /24 prefix.
Locally administered and group addresses are not attributed to vendors.
An assignment identifies the registered organization, not necessarily the retail
brand, model, owner, or a unique physical device. Unknown assignments remain unknown.
