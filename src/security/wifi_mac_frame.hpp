// General 802.11 MAC frame parser for the passive Wi-Fi security monitor
// (docs/WIFI_SECURITY_IMPLEMENTATION_PLAN.md, work package A step 2).
//
// Byte-level logic only - no DSP, no IQ, no detector policy. The receive
// chains (wifi_dsss_rx, wifi_ofdm_rx) recover MPDU bytes; this turns any
// such MPDU into typed fields a detector can reason about. It deliberately
// extracts more than today's rules use, so later rules do not need another
// decoder change: every field here is an OBSERVATION, never a verdict.
//
// What this does NOT establish, by design:
//  - FCS validity proves transmission integrity, not sender authenticity.
//    Every address is a CLAIMED address. Nothing here authenticates a frame.
//  - A protected body is reported as encrypted/integrity-protected with its
//    visible replay counter (PN/IPN) only. No keys, no MIC verification, so a
//    counter's meaning is limited to its (transmitter, key ID) context, and a
//    key ID is not a unique key epoch.
//  - Cipher identification from the header alone is ambiguous in some cases
//    (CCMP/GCMP versus TKIP). Ambiguity is reported, never guessed through.
//  - Parse state is independent of FCS state: a malformed body with a valid
//    FCS is a real transmission worth keeping (attack tools send malformed
//    frames), and a parsed header with an invalid FCS is diagnostic only.
//    Callers must gate acceptance on `fcs == FcsStatus::Valid`.
//
// Layout references: IEEE 802.11-2020 clause 9 (frame formats, Table 9-26
// address roles, 9.4.2 elements), 12.5 (CCMP/GCMP/TKIP headers), 12.7.2
// (EAPOL-Key). Field names cross-checked against Wireshark's dissector
// (https://www.wireshark.org/docs/dfref/w/wlan.html) - see the test file.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "wifi_frame.hpp"

namespace rfmon::wifi_security {

using MacAddress = std::array<uint8_t, 6>;

std::string format_mac(const MacAddress& a);
bool is_group_address(const MacAddress& a);            // I/G bit: broadcast or multicast
bool is_broadcast_address(const MacAddress& a);        // ff:ff:ff:ff:ff:ff
bool is_locally_administered(const MacAddress& a);     // U/L bit - context only; MAC
                                                       // randomisation is not malicious

enum class FrameType : uint8_t { Management = 0, Control = 1, Data = 2, Extension = 3 };

enum class FcsStatus {
    NotPresent,  // caller said the MPDU has no trailing FCS (e.g. a pcap without it)
    TooShort,    // fewer than 4 octets: nothing to check
    Valid,
    Invalid,
};

// How far the parser got. Header and body are reported separately so a
// truncated body never hides a perfectly readable header.
enum class ParseState {
    Ok,           // every field this frame type defines was read
    Truncated,    // the frame ended before a mandatory field
    Malformed,    // a length/structure rule was violated (e.g. element overrun)
    Unsupported,  // valid type/subtype this parser does not decode further
};

enum class BodyState {
    Absent,              // frame type defines no body, or it is empty
    Readable,            // plaintext body parsed
    Encrypted,           // Protected bit set: body is ciphertext after the cipher header
    IntegrityProtected,  // readable body carrying a Management MIC element (BIP)
    Truncated,
    Malformed,
    Unsupported,
};

// Duration/ID field interpretation (802.11-2020 9.2.4.2). A large value is
// not automatically a NAV reservation.
enum class DurationKind {
    Nav,             // bit 15 clear: duration in microseconds (0..32767)
    AssociationId,   // PS-Poll: bits 14 and 15 set, AID in bits 0..13
    CfpFixed,        // exactly 0x8000 outside PS-Poll: contention-free period
    Reserved,
};

struct FrameControl {
    uint8_t protocol_version = 0;
    FrameType type = FrameType::Management;
    uint8_t subtype = 0;
    bool to_ds = false, from_ds = false, more_fragments = false, retry = false;
    bool power_management = false, more_data = false, protected_frame = false;
    bool order_or_htc = false;  // +HTC/Order bit; meaning depends on frame type
    uint16_t raw = 0;
};

// Replay counter carried in the clear ahead of an encrypted body.
struct CipherHeader {
    enum class Kind {
        Wep,             // no ExtIV: 24-bit IV, no replay counter at all
        CcmpOrGcmp,      // ExtIV, reserved octet zero, not a TKIP pattern
        Tkip,            // ExtIV and the TKIP WEPSeed relation holds, reserved octet non-zero
        AmbiguousExtIv,  // ExtIV but both patterns fit: packet number not reported
    };
    Kind kind = Kind::Wep;
    uint8_t key_id = 0;
    bool ext_iv = false;
    std::optional<uint64_t> packet_number;  // 48-bit PN (CCMP/GCMP) or TSC (TKIP)
    std::optional<uint32_t> wep_iv;         // 24-bit IV for WEP
    size_t header_len = 0;
};

// Management MIC element (element ID 76): BIP integrity on group-addressed
// robust management frames and protected beacons. IPN is its replay counter.
struct ManagementMic {
    uint16_t key_id = 0;
    uint64_t ipn = 0;      // 48-bit
    size_t mic_len = 0;    // 8 (BIP-CMAC-128) or 16 (BIP-GMAC/CMAC-256)
};

// Compact RSN element summary: raw suite selectors so rules compare numbers,
// not display strings. Display strings stay in wifi::BeaconInfo.
struct RsnSummary {
    uint16_t version = 0;
    uint32_t group_cipher = 0;               // OUI<<8 | type
    std::vector<uint32_t> pairwise_ciphers;
    std::vector<uint32_t> akm_suites;
    std::optional<uint16_t> capabilities;
    bool mfp_required = false, mfp_capable = false;
    uint16_t pmkid_count = 0;
    std::optional<uint32_t> group_mgmt_cipher;
    bool well_formed = false;
};

struct ChannelSwitch {
    uint8_t mode = 0;          // 1: stations must stop transmitting until the switch
    uint8_t new_channel = 0;
    uint8_t count = 0;         // TBTTs until the switch
    std::optional<uint8_t> new_operating_class;  // extended CSA only
    bool extended = false;
};

struct QuietElement {
    uint8_t count = 0, period = 0;
    uint16_t duration_tu = 0, offset_tu = 0;
};

struct BssLoad {
    uint16_t station_count = 0;
    uint8_t channel_utilization = 0;  // AP-reported, x/255
    uint16_t admission_capacity = 0;
};

struct Tim {
    uint8_t dtim_count = 0, dtim_period = 0, bitmap_control = 0;
    bool group_traffic_buffered = false;  // bitmap control bit 0
};

// Everything readable from an information-element list.
struct ElementSummary {
    // Ordered element IDs as transmitted (extension elements as 255*256+ext).
    // The order/set is a passive client/AP behaviour signature, not identity.
    std::vector<uint16_t> ids;
    std::vector<uint32_t> vendor_ouis;  // OUI<<8 | vendor type, in order
    bool complete = true;               // false: overrun, bad length, or trailing bytes

    std::optional<std::string> ssid;  // raw octets (may be non-UTF-8)
    bool ssid_present = false, ssid_wildcard = false;  // wildcard: zero length
    bool ssid_all_zero = false;                         // hidden network padding
    std::vector<uint8_t> supported_rates;               // raw rate octets (0x80 = basic)
    std::vector<uint8_t> extended_rates;
    std::optional<uint8_t> ds_channel;
    std::optional<uint8_t> ht_primary_channel;
    std::optional<std::string> country;  // two-character code as transmitted
    std::optional<Tim> tim;
    std::optional<BssLoad> bss_load;
    std::optional<RsnSummary> rsn;
    bool wpa_vendor_ie = false;
    bool wps_present = false;
    std::optional<ChannelSwitch> channel_switch;  // CSA (37) or extended CSA (60)
    std::optional<QuietElement> quiet;
    std::optional<uint8_t> power_constraint_db;
    std::optional<ManagementMic> mme;
    bool multiple_bssid = false;       // element 71: several BSSIDs are normal here
    std::optional<uint16_t> ht_capabilities_info;
    bool vht_capabilities = false, he_capabilities = false, eht_capabilities = false;
    std::vector<uint8_t> extended_capabilities;  // raw octets
    std::optional<uint8_t> challenge_text_len;   // shared-key auth only; length recorded, text not kept
};

enum class EapolKeyMessage { Unknown, M1, M2, M3, M4, GroupM1, GroupM2 };

// EAPOL-Key frame visible in an unprotected data frame (4-way/group
// handshake). The replay counter and nonce are what replay/handshake-flood
// rules need; key data and MIC are not interpreted.
struct EapolKey {
    uint8_t protocol_version = 0;
    uint8_t descriptor_type = 0;  // 2: RSN, 254: WPA
    uint16_t key_info = 0;
    uint8_t descriptor_version = 0;  // key_info bits 0..2
    bool pairwise = false, install = false, ack = false, mic = false, secure = false;
    bool error = false, request = false, encrypted_key_data = false;
    uint16_t key_length = 0;
    uint64_t replay_counter = 0;
    std::array<uint8_t, 32> nonce{};
    bool nonce_zero = true;
    std::optional<uint16_t> key_data_length;  // unset when the MIC length (AKM) cannot be resolved
    EapolKeyMessage message = EapolKeyMessage::Unknown;  // inferred from flags
};

struct DataPayload {
    bool null_function = false;  // subtype bit 2: no payload by definition
    bool amsdu = false;          // QoS A-MSDU present: LLC not directly at body start
    std::optional<uint16_t> ethertype;  // from LLC/SNAP (AA AA 03 00 00 00 / 00 00 F8)
    std::optional<EapolKey> eapol_key;
    std::optional<uint8_t> eapol_packet_type;  // 0 EAP, 1 Start, 2 Logoff, 3 Key
};

// Fixed fields per management subtype. Only the ones the subtype defines are
// set; body_state says whether the frame ended early.
struct ManagementBody {
    std::optional<uint64_t> timestamp_tsf;   // beacon / probe response
    std::optional<uint16_t> beacon_interval_tu;
    std::optional<uint16_t> capability;
    std::optional<uint16_t> listen_interval;  // (re)association request
    std::optional<MacAddress> current_ap;     // reassociation request
    std::optional<uint16_t> status_code;      // auth / (re)association response
    std::optional<uint16_t> association_id;   // (re)association response, bits 0..13
    std::optional<uint16_t> reason_code;      // deauthentication / disassociation
    std::optional<uint16_t> auth_algorithm;   // 0 open, 1 shared key, 2 FT, 3 SAE, ...
    std::optional<uint16_t> auth_transaction;
    std::optional<uint16_t> sae_group;        // SAE commit (transaction 1, status 0/126)
    std::optional<uint8_t> action_category;
    std::optional<uint8_t> action_code;
    bool robust_action = false;               // category is a robust management category
    std::optional<uint16_t> sa_query_transaction;  // SA Query request/response
    std::optional<ElementSummary> elements;
};

struct MacFrame {
    FcsStatus fcs = FcsStatus::NotPresent;
    uint32_t fcs_value = 0;
    ParseState header_state = ParseState::Truncated;
    BodyState body_state = BodyState::Absent;
    std::string problem;  // first reason for a non-Ok state, for diagnostics

    FrameControl fc;
    uint16_t duration_id = 0;
    DurationKind duration_kind = DurationKind::Nav;
    std::optional<uint16_t> nav_us, aid_from_duration;

    // Raw address fields as present in the header.
    std::optional<MacAddress> addr1, addr2, addr3, addr4;
    // Roles per Table 9-26 / control subtype definitions. Only set when the
    // standard assigns the role for this frame type and DS combination.
    std::optional<MacAddress> receiver, transmitter, destination, source, bssid;

    std::optional<uint16_t> sequence_control, sequence_number;
    std::optional<uint8_t> fragment_number;
    std::optional<uint16_t> qos_control;
    std::optional<uint8_t> tid;
    std::optional<uint32_t> ht_control;
    size_t header_len = 0;
    size_t body_len = 0;  // octets between header and FCS (whole frame if no FCS)

    std::optional<CipherHeader> cipher;
    std::optional<ManagementBody> management;
    std::optional<DataPayload> data;

    // Existing identity parser output for beacon / probe response, reused
    // rather than duplicated (security/cipher display strings, WPS, channel).
    std::optional<wifi::BeaconInfo> beacon_identity;

    // FNV-1a 64 over the MPDU without FCS. `content_hash_retry_invariant`
    // masks only the Retry bit: a retransmission with otherwise identical
    // bytes matches it. It is evidence for duplicate analysis, not proof -
    // legitimate retransmissions may also differ elsewhere (e.g. Duration).
    uint64_t content_hash = 0, content_hash_retry_invariant = 0;

    bool fcs_valid() const { return fcs == FcsStatus::Valid; }
    bool is_management(uint8_t st) const { return fc.type == FrameType::Management && fc.subtype == st; }
};

// Management subtypes (802.11-2020 Table 9-1).
namespace mgmt {
constexpr uint8_t AssocRequest = 0, AssocResponse = 1, ReassocRequest = 2, ReassocResponse = 3,
                  ProbeRequest = 4, ProbeResponse = 5, TimingAdvertisement = 6, Beacon = 8,
                  Atim = 9, Disassociation = 10, Authentication = 11, Deauthentication = 12,
                  Action = 13, ActionNoAck = 14;
}
namespace ctrl {
constexpr uint8_t Trigger = 2, BeamformingReportPoll = 4, VhtNdpAnnouncement = 5,
                  ControlFrameExtension = 6, ControlWrapper = 7, BlockAckRequest = 8, BlockAck = 9,
                  PsPoll = 10, Rts = 11, Cts = 12, Ack = 13, CfEnd = 14, CfEndCfAck = 15;
}

// Parses one MPDU. With `has_fcs` the last four octets are the FCS (as the
// project's DSSS/OFDM receivers deliver them) and are checked with the same
// CRC-32 as wifi::fcs32(). Never reads outside [mpdu, mpdu + len).
MacFrame parse_mac_frame(const uint8_t* mpdu, size_t len, bool has_fcs = true);

// Parses an information-element list on its own (exposed for tests/tools).
ElementSummary parse_elements(const uint8_t* p, size_t len);

// Short labels for logs, tools and the GUI.
const char* frame_type_name(FrameType t);
std::string subtype_name(FrameType t, uint8_t subtype);
// "Mgmt/Deauthentication" etc. A non-zero protocol version (802.11ah PV1)
// uses different type semantics, so it is labelled as such rather than
// reading its bits as a PV0 type/subtype.
std::string frame_type_label(const MacFrame& f);
const char* fcs_status_name(FcsStatus s);
const char* parse_state_name(ParseState s);
const char* body_state_name(BodyState s);
const char* eapol_message_name(EapolKeyMessage m);
std::string reason_code_name(uint16_t code);
std::string status_code_name(uint16_t code);
std::string auth_algorithm_name(uint16_t alg);

// One-line human summary: type, roles, sequence, key fields and states.
std::string describe(const MacFrame& f);

}  // namespace rfmon::wifi_security
