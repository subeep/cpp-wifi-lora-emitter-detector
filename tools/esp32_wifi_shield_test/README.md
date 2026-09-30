# Bounded ESP32 shield-box Wi-Fi test fixture

This is a separate, experimental build for the user's isolated own-AP test.
It does not use LoRa. It boots with Wi-Fi disabled and only starts its channel 11
AP by serial command. The AP allows one client. The test frame is addressed only
to the specific phone MAC observed in the prior owned-AP reception test,
`f4:30:8b:af:56:69`; a different or absent client blocks transmission.

Commands are `status`, `ap_start`, `probe_once`, `flood_once`,
`stop_test`, and `ap_stop`. The probe allows only one raw deauthentication
attempt. The separate bounded burst is one-use, capped at 60 attempts spaced by
25 ms, and stops on the first API error or `stop_test`/`ap_stop`.
There is no boot-triggered test, looping flood mode, arbitrary address or
channel input, or firmware claim that an API success means RF emission.
The vendor's documented raw-transmit API does not list deauthentication frames
as supported, so a USRP recording is required to establish whether either
operation actually transmits. The positive flood rule requires at least 20
distinct received units in two captures; serial attempt counts do not qualify.

The board was flashed and idle status verified on 2026-09-29. The first closed-box
passive recording decoded strong unrelated AP beacons, including
`Airtel_Zerotouch`. Therefore **no probe or burst was sent** pending correction
and verification of RF containment. The ESP32 AP was stopped afterward.
See `docs/ESP32_SHIELD_BOX_TEST_2026-09-29.md` for measured results.

Subsequent update: at the user's request, one targeted raw-frame probe was
attempted after the phone connected and the box was reconfirmed closed. The
ESP32 driver rejected subtype 0xc0 with `ESP_ERR_INVALID_ARG`; the X310
recorded zero disconnect frames. The burst was not invoked, and the AP is off.
