// Which downstream uses a classified DSSS burst is eligible for, decided by
// its duration alone.
//
// Split out of scanner.cpp so the separation required by the security plan
// (docs/WIFI_SECURITY_IMPLEMENTATION_PLAN.md, package A step 3: "isolate
// security eligibility from beacon-cadence eligibility") is one testable
// function rather than an if-chain inside the scan loop.
//
// Beacon-length bursts keep exactly their previous treatment: cadence
// clustering, decode, identity recording and RF fingerprinting. Shorter
// bursts that can still hold a complete 1 Mbps MPDU are decoded for
// security analysis ONLY:
//  - never offered to find_beacon_sources(), whose phase space saturates
//    and invents phantom sources when fed ordinary short traffic;
//  - never fingerprinted or written to the identity store, so the persisted
//    Wi-Fi master list is byte-for-byte what it was before this change -
//    even if such a burst happens to decode as an FCS-valid beacon.
#pragma once

#include "config.hpp"

namespace rfmon::wifi {

struct DsssBurstPolicy {
    bool decode = false;                    // attempt decode_dsss_burst()
    bool beacon_cadence = false;            // offer to find_beacon_sources()
    bool identity_and_fingerprint = false;  // record identity / extract RF fingerprint
    bool security_only = false;             // decoded solely for security analysis
};

inline DsssBurstPolicy dsss_burst_policy(double duration_s,
                                         bool security_decode_enabled = WIFI_SECURITY_DSSS_DECODE_ENABLED) {
    DsssBurstPolicy p;
    if (duration_s >= WIFI_BEACON_MIN_DURATION_S) {
        p.decode = p.beacon_cadence = p.identity_and_fingerprint = true;
    } else if (security_decode_enabled && duration_s >= WIFI_SECURITY_DSSS_MIN_DURATION_S) {
        p.decode = p.security_only = true;
    }
    return p;
}

}  // namespace rfmon::wifi
