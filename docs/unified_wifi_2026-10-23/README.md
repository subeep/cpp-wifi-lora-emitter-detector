# Unified Wi-Fi workspace

One visible Wi-Fi navigation item replaces the two band-specific items. The receive selector offers 2.4 GHz only, 5 GHz only, and Both bands. Selecting a mode changes acquisition and clears any fixed-channel lock. Merely opening the page does not change acquisition.

Both alternates full existing 2.4 and 5 GHz channel plans, then repeats. Each capture keeps its actual band, decoder path, registry and security provenance. Both ignores stale fixed-channel settings. Reception is sequential, with sweep gaps, not simultaneous dual-band reception. Single-band fixed-channel monitoring remains available in Monitoring setup.

The existing 5 GHz plan covers channels 36/40/44/48 and 149/153/157/161/165 (5.2 and 5.8 GHz portions); the selector is therefore labelled 5 GHz, not 5.2 GHz. No DFS or 6 GHz channels were added.

The master list is universal regardless of receive mode. Rows show coloured 2.4 GHz and/or 5 GHz labels based on observation history, plus latest monitored frequency. No production inventory files were changed. Packet views follow the selected mode; Both includes both bands. RF observations use separate band panels to preserve independent registry identities. Experimental analysis accepts the latest result from either band in Both mode.

Validation: application build passed. Simulated-radio scanner test verified complete channel order across both bands, repetition and stale-lock handling. Scanner lane, Wi-Fi GUI, enrolment, shell navigation and cyclostationary panel tests passed. The rendered enrolment table was visually inspected. No live USRP reception was tested for this change.
