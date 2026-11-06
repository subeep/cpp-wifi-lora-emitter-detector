#!/usr/bin/env bash
# Remaining over-the-air cases of the 2026-11-06 LoRa tab validation.
# Heltec V3 at 0 dBm, IN865 channels, BW125 only, ~2-3 m from the X310.
set -u
cd "$(dirname "$0")/../.."
R=tools/lora_validation/run_case.sh
CH=865.0625
if [ "${RUN_20MHZ_ONLY:-0}" != 1 ]; then

# 1. CRC-failure diagnosis: lock, telemetry, failed rows dumped.
$R t1crc T1CRC 300 "--lock $CH" "0:CH $CH" "0:INT 5000" "0:JIT 1000" "0.3:MODE TEL"

# 2. LoRaWAN uplinks + rule tests, lock mode for the best recall.
S=("0:CH $CH" "0:INT 5000" "0:JIT 1000" "0:FCNT 0" "0.3:MODE WAN")
for t in 60 90 120; do S+=("$t:@REPLAY_AGE 0"); done            # control: retransmission
for t in 200 230 260 290 320; do S+=("$t:@REPLAY_AGE 120"); done # replay
for t in 350 380 410 440 470; do S+=("$t:@REUSE"); done          # same FCnt, new bytes
S+=("500:JOIN 4660" "520:JOIN 4661" "540:JOIN 4662")              # spread joins (control)
S+=("600:JOIN 4660" "620:JOIN 4661" "640:JOIN 4662")              # DevNonce reuse
for i in $(seq 0 29); do S+=("$(python3 -c "print(680+1.5*$i)"):JOIN $((5000+i))"); done  # join burst
S+=("800:FCNT 10")                                                # counter reset / regression
$R t3rules T3RULES 960 "--lock $CH --disallow-after 850" "${S[@]}"

# 3. SF sweep, lock mode.
S=("0:CH $CH" "0:JIT 1000" "0:INT 6000" "0:SF 7" "0.3:MODE TEL")
t=100; for sf in 8 9 10; do S+=("$t:SF $sf"); t=$((t+100)); done
S+=("400:INT 9000" "400:SF 11" "520:SF 12")
$R t4sf T4SF 660 "--lock $CH" "${S[@]}"

# 4. Channel suggestion: 866.5 MHz is outside the plan, then added to it.
S=("0:CH 866.5" "0:INT 3000" "0:JIT 500" "0.3:MODE TEL")
$R t5off T5OFF 150 "" "${S[@]}"
$R t5add T5ADD 150 "--channels 865.0625,865.4025,865.985,866.5,866.9" "${S[@]}"
echo ALL_DONE
fi


# --- 20 MHz LoRa listening (run with RUN_20MHZ_ONLY=1 to skip the cases above) ---
# The Heltec stays on IN865 frequencies; the broad case only widens what the
# receiver decodes (13 IN865 + EU868 channels).
S=("0:INT 5000" "0:JIT 1000" "0.3:MODE TEL")
$R t7wide20 T7WIDE20 600 "--width 20" "${S[@]}"
BROAD=865.0625,865.4025,865.985,866.9,867.1,867.3,867.5,867.7,867.9,868.1,868.3,868.5,869.525
$R t7broad20 T7BROAD 300 "--width 20 --channels $BROAD" "${S[@]}"
echo ALL_20MHZ_DONE
