// Correctness tests for src/security/wifi_mac_frame.cpp - the general 802.11
// MAC parser feeding the passive Wi-Fi security monitor.
//
// Same rule as test_wifi_frame.cpp: nothing here round-trips through code
// under test. Vectors come from tests/fixtures/wifi_security/frames.hex,
// packed independently by tests/generate_wifi_security_frames.py (Python
// struct + zlib CRC-32). Every expected field below was read from tshark's
// dissection of the matching frames.pcap (wlan.check_checksum enabled), not
// from this parser's output. Real-air coverage comes from the three
// receive-only X310 beacon MPDUs in tests/fixtures/wifi_ofdm/.
#include <cstdio>
#include <fstream>
#include <map>
#include <random>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "security/wifi_mac_frame.hpp"

using namespace rfmon::wifi_security;

namespace {

int failures = 0;

void check(bool cond, const std::string& name, const std::string& detail = "") {
    std::printf("%s [%s]%s%s\n", cond ? "PASS" : "FAIL", name.c_str(), detail.empty() ? "" : ": ", detail.c_str());
    if (!cond) ++failures;
}

std::vector<uint8_t> from_hex(const std::string& s) {
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 1 < s.size(); i += 2) out.push_back(uint8_t(std::stoi(s.substr(i, 2), nullptr, 16)));
    return out;
}

std::map<std::string, std::vector<uint8_t>> load_vectors() {
    std::map<std::string, std::vector<uint8_t>> v;
    std::ifstream in(std::string(PROJECT_ROOT_DIR) + "/tests/fixtures/wifi_security/frames.hex");
    std::string line;
    while (std::getline(in, line)) {
        auto tab = line.find('\t');
        if (tab != std::string::npos) v[line.substr(0, tab)] = from_hex(line.substr(tab + 1));
    }
    return v;
}

std::string mac(const std::optional<MacAddress>& a) { return a ? format_mac(*a) : "<unset>"; }

const std::string AP = "00:11:22:33:44:55", STA = "66:77:88:99:aa:bb", STA2 = "da:01:02:03:04:05",
                  BC = "ff:ff:ff:ff:ff:ff", DS = "0c:0d:0e:0f:10:11";

}  // namespace

int main() {
    auto V = load_vectors();
    check(V.size() == 40, "fixture_loaded", std::to_string(V.size()) + " vectors");
    auto P = [&](const std::string& n) { const auto& b = V.at(n); return parse_mac_frame(b.data(), b.size()); };

    // ---- FCS gating -------------------------------------------------------
    for (const auto& [name, bytes] : V) {
        MacFrame f = parse_mac_frame(bytes.data(), bytes.size());
        const bool expect_valid = name != "deauth_bad_fcs" && name != "too_short_3";
        check(f.fcs_valid() == expect_valid, "fcs_" + name, fcs_status_name(f.fcs));
    }
    check(P("too_short_3").fcs == FcsStatus::TooShort, "fcs_too_short_state");
    {
        MacFrame f = P("deauth_bad_fcs");
        check(f.fcs == FcsStatus::Invalid && f.management && f.management->reason_code == 7,
              "bad_fcs_still_parsed_for_diagnostics_but_not_valid");
    }

    // ---- management: deauth / disassoc -------------------------------------
    {
        MacFrame f = P("deauth_broadcast_reason7");
        check(f.is_management(mgmt::Deauthentication), "deauth_type");
        check(mac(f.receiver) == BC && mac(f.transmitter) == AP && mac(f.bssid) == AP, "deauth_roles");
        check(is_broadcast_address(*f.receiver) && is_group_address(*f.receiver), "deauth_broadcast_target");
        check(f.sequence_number == 291 && f.fragment_number == 0, "deauth_seq", std::to_string(*f.sequence_number));
        check(f.nav_us == 314 && f.duration_kind == DurationKind::Nav, "deauth_duration_314us");
        check(f.management->reason_code == 7, "deauth_reason_7");
        check(f.header_state == ParseState::Ok && f.body_state == BodyState::Readable, "deauth_states");
        check(!f.fc.retry, "deauth_no_retry");
    }
    {
        MacFrame f = P("deauth_unicast_retry_reason3");
        check(f.fc.retry && f.sequence_number == 292 && f.management->reason_code == 3, "deauth_retry_reason3");
        check(mac(f.receiver) == STA && !is_group_address(*f.receiver), "deauth_unicast_target");
    }
    {
        MacFrame f = P("disassoc_reason8");
        check(f.is_management(mgmt::Disassociation) && f.management->reason_code == 8 &&
              mac(f.transmitter) == STA && f.sequence_number == 17, "disassoc_reason8");
    }
    // ---- authentication ----------------------------------------------------
    {
        MacFrame f = P("auth_open_seq1");
        const auto& m = *f.management;
        check(m.auth_algorithm == 0 && m.auth_transaction == 1 && m.status_code == 0, "auth_open_fields");
        check(mac(f.transmitter) == STA2 && is_locally_administered(*f.transmitter), "auth_randomised_looking_ta");
    }
    {
        MacFrame f = P("auth_sae_commit_group19");
        const auto& m = *f.management;
        check(m.auth_algorithm == 3 && m.auth_transaction == 1 && m.status_code == 0 && m.sae_group == 19,
              "sae_commit_group19");
        check(!m.elements, "sae_body_not_treated_as_elements");
    }
    {
        MacFrame f = P("auth_sae_anticlogging");
        check(f.management->status_code == 76 && f.management->sae_group == 19, "sae_anticlogging_status76");
    }
    // ---- (re)association ---------------------------------------------------
    {
        MacFrame f = P("assoc_req_rsn_mfpc");
        const auto& m = *f.management;
        check(m.listen_interval == 10 && m.capability == 0x0431, "assoc_req_fixed");
        check(m.elements && m.elements->ssid == std::string("LabNet"), "assoc_req_ssid");
        check(m.elements->rsn && m.elements->rsn->well_formed && m.elements->rsn->mfp_capable &&
              !m.elements->rsn->mfp_required, "assoc_req_rsn_mfpc_only");
        check(m.elements->rsn->akm_suites.size() == 1 && m.elements->rsn->akm_suites[0] == 0x000FAC02,
              "assoc_req_akm_psk");
        check(m.elements->ids == std::vector<uint16_t>({0, 1, 48}), "assoc_req_element_order");
    }
    {
        MacFrame f = P("assoc_resp_aid1");
        check(f.management->status_code == 0 && f.management->association_id == 1, "assoc_resp_aid1");
    }
    {
        MacFrame f = P("reassoc_req");
        check(f.management->current_ap && format_mac(*f.management->current_ap) == DS &&
              f.management->listen_interval == 10, "reassoc_current_ap");
    }
    {
        MacFrame f = P("probe_req_wildcard");
        const auto& e = *f.management->elements;
        check(e.ssid_present && e.ssid_wildcard, "probe_req_wildcard_ssid");
        check(mac(f.bssid) == BC && e.ids == std::vector<uint16_t>({0, 1, 221}), "probe_req_bssid_and_ids");
        check(e.vendor_ouis.size() == 1 && e.vendor_ouis[0] == 0x0050F208, "probe_req_vendor_oui");
        check(e.supported_rates.size() == 8 && e.supported_rates[0] == 0x82, "probe_req_rates");
    }
    // ---- beacon with DoS-relevant elements ---------------------------------
    {
        MacFrame f = P("beacon_csa_quiet_load_mme");
        const auto& m = *f.management;
        const auto& e = *m.elements;
        check(m.timestamp_tsf == 325613772854ull, "beacon_tsf", std::to_string(*m.timestamp_tsf));
        check(m.beacon_interval_tu == 100, "beacon_interval");
        check(e.ids == std::vector<uint16_t>({0, 1, 3, 5, 7, 11, 37, 40, 48, 76}), "beacon_element_order");
        check(e.ds_channel == 6 && e.tim && e.tim->dtim_count == 0 && e.tim->dtim_period == 3, "beacon_ds_tim");
        check(e.country == std::string("IN"), "beacon_country");
        check(e.bss_load && e.bss_load->station_count == 12 && e.bss_load->channel_utilization == 200, "beacon_bss_load");
        check(e.channel_switch && e.channel_switch->mode == 1 && e.channel_switch->new_channel == 11 &&
              e.channel_switch->count == 5 && !e.channel_switch->extended, "beacon_csa");
        check(e.quiet && e.quiet->count == 2 && e.quiet->period == 10 && e.quiet->duration_tu == 50 &&
              e.quiet->offset_tu == 3, "beacon_quiet");
        check(e.rsn && e.rsn->mfp_required && e.rsn->mfp_capable && e.rsn->group_mgmt_cipher == 0x000FAC06,
              "beacon_rsn_mfpr_bip");
        check(e.mme && e.mme->key_id == 6 && e.mme->ipn == 0x010203040506ull && e.mme->mic_len == 8, "beacon_mme_ipn");
        check(f.body_state == BodyState::IntegrityProtected, "beacon_body_integrity_protected");
        check(f.beacon_identity && f.beacon_identity->bssid == AP && f.beacon_identity->ssid == "LabNet",
              "beacon_existing_identity_parser_reused");
    }
    // ---- action frames -----------------------------------------------------
    {
        MacFrame f = P("sa_query_request");
        const auto& m = *f.management;
        check(m.action_category == 8 && m.action_code == 0 && m.robust_action && m.sa_query_transaction == 0x1234,
              "sa_query_request");
    }
    {
        MacFrame f = P("action_csa_group_mme");
        const auto& m = *f.management;
        check(m.action_category == 0 && m.action_code == 4 && m.robust_action, "csa_action_category");
        check(m.elements && m.elements->channel_switch && m.elements->channel_switch->new_channel == 36 &&
              m.elements->channel_switch->count == 3, "csa_action_switch");
        check(m.elements->mme && m.elements->mme->key_id == 4 && m.elements->mme->ipn == 0xFF &&
              m.elements->mme->mic_len == 16, "csa_action_mme_bip256");
        check(f.body_state == BodyState::IntegrityProtected, "csa_action_integrity_protected");
    }
    {
        MacFrame f = P("action_public_ext_csa");
        const auto& m = *f.management;
        check(m.action_category == 4 && !m.robust_action, "public_action_not_robust");
        const auto& cs = m.elements->channel_switch;
        check(cs && cs->extended && cs->mode == 1 && cs->new_operating_class == 115 && cs->new_channel == 40 &&
              cs->count == 2, "public_ext_csa_fields");
    }
    {
        MacFrame f = P("deauth_broadcast_bip_mme");
        check(f.management->reason_code == 6 && f.management->elements->mme &&
              f.management->elements->mme->ipn == 777 && f.body_state == BodyState::IntegrityProtected,
              "broadcast_deauth_bip_ipn");
    }
    {
        MacFrame f = P("deauth_protected_ccmp");
        check(f.fc.protected_frame && f.body_state == BodyState::Encrypted, "protected_deauth_encrypted");
        check(f.management && !f.management->reason_code, "protected_deauth_reason_not_invented");
        check(f.cipher && f.cipher->kind == CipherHeader::Kind::CcmpOrGcmp &&
              f.cipher->packet_number == 0x0000000100ABull && f.cipher->key_id == 0, "protected_deauth_pn");
    }
    // ---- malformed / truncated with valid FCS --------------------------------
    {
        MacFrame f = P("beacon_elem_overrun");
        check(f.fcs_valid() && f.header_state == ParseState::Ok && f.body_state == BodyState::Malformed,
              "beacon_overrun_malformed_body", f.problem);
        check(f.management->timestamp_tsf == 5 && f.management->elements->ssid == std::string("Lab"),
              "beacon_overrun_keeps_readable_prefix");
        check(f.management->elements->ids == std::vector<uint16_t>({0, 1}) && !f.management->elements->complete,
              "beacon_overrun_records_offending_element_id");
    }
    {
        MacFrame f = P("deauth_truncated_reason");
        check(f.fcs_valid() && f.body_state == BodyState::Truncated && !f.management->reason_code,
              "deauth_truncated_reason");
    }
    check(P("protocol_version_1").header_state == ParseState::Unsupported, "protocol_version_1_unsupported");
    check(frame_type_label(P("protocol_version_1")) == "PV1/not decoded", "protocol_version_1_not_labelled_as_pv0_type",
          frame_type_label(P("protocol_version_1")));
    check(frame_type_label(P("deauth_broadcast_reason7")) == "Mgmt/Deauthentication", "pv0_type_label");
    {
        MacFrame f = P("header_truncated_addr2");
        check(f.header_state == ParseState::Truncated && f.addr1 && !f.addr2 && !f.transmitter &&
              mac(f.receiver) == BC, "header_truncated_at_addr2_keeps_receiver");
    }
    check(P("too_short_3").header_state == ParseState::Truncated, "too_short_header_truncated");

    // ---- data frames ---------------------------------------------------------
    {
        MacFrame f = P("data_ccmp_ieee_vector_header");
        check(f.fc.type == FrameType::Data && f.fc.retry && f.fc.protected_frame, "ccmp_vector_flags");
        check(f.nav_us == 11459 && f.sequence_number == 824, "ccmp_vector_duration_seq");
        check(mac(f.receiver) == "0f:d2:e1:28:a5:7c" && mac(f.transmitter) == "50:30:f1:84:44:08" &&
              mac(f.bssid) == "ab:ae:a5:b8:fc:ba", "ccmp_vector_roles");
        check(f.cipher && f.cipher->kind == CipherHeader::Kind::CcmpOrGcmp &&
              f.cipher->packet_number == 0xB5039776E70Cull && f.cipher->key_id == 0, "ccmp_vector_pn");
    }
    auto eapol = [&](const std::string& n, EapolKeyMessage msg, uint64_t replay, uint16_t kd) {
        MacFrame f = P(n);
        const auto& d = f.data;
        check(d && d->ethertype == 0x888E && d->eapol_key, n + "_eapol_key");
        if (!d || !d->eapol_key) return f;
        check(d->eapol_key->message == msg, n + "_message", eapol_message_name(d->eapol_key->message));
        check(d->eapol_key->replay_counter == replay, n + "_replay_counter");
        check(d->eapol_key->key_data_length == kd, n + "_key_data_length");
        return f;
    };
    {
        MacFrame f = eapol("qos_data_fromds_eapol_m1", EapolKeyMessage::M1, 1, 0);
        check(f.tid == 7 && mac(f.receiver) == STA && mac(f.bssid) == AP && mac(f.source) == AP, "m1_qos_fromds_roles");
        check(f.data->eapol_key->nonce[0] == 1 && !f.data->eapol_key->nonce_zero, "m1_anonce");
    }
    {
        MacFrame f = eapol("data_tods_eapol_m2", EapolKeyMessage::M2, 1, 22);
        check(mac(f.bssid) == AP && mac(f.source) == STA && mac(f.destination) == AP, "m2_tods_roles");
    }
    eapol("qos_data_fromds_eapol_m3", EapolKeyMessage::M3, 2, 56);
    {
        MacFrame f = eapol("data_tods_eapol_m4", EapolKeyMessage::M4, 2, 0);
        check(f.data->eapol_key->nonce_zero, "m4_zero_nonce");
    }
    {
        MacFrame f = P("null_tods_pwrmgt");
        check(f.data && f.data->null_function && f.fc.power_management && f.body_state == BodyState::Absent,
              "null_function_power_save");
    }
    {
        MacFrame f = P("data_wds_4addr");
        check(mac(f.receiver) == AP && mac(f.transmitter) == DS && mac(f.destination) == STA &&
              mac(f.source) == STA2 && !f.bssid, "four_address_roles_no_bssid");
        check(f.data->ethertype == 0x0800, "four_address_ipv4");
    }
    {
        MacFrame f = P("data_tkip");
        check(f.cipher && f.cipher->kind == CipherHeader::Kind::Tkip && f.cipher->packet_number == 0x011234ull,
              "tkip_tsc");
    }
    {
        MacFrame f = P("data_wep_key1");
        check(f.cipher && f.cipher->kind == CipherHeader::Kind::Wep && f.cipher->key_id == 1 &&
              f.cipher->wep_iv == 0x010203u && !f.cipher->packet_number, "wep_iv_no_replay_counter");
    }
    {
        MacFrame f = P("qos_data_htc_ipv4");
        check(f.tid == 5 && f.ht_control == 0x12345678u && f.header_len == 30 && f.data->ethertype == 0x0800,
              "qos_htc_header_length");
    }
    {
        MacFrame f = P("qos_data_amsdu_arp");
        check(f.data->amsdu && f.data->ethertype == 0x0806 && !f.source && mac(f.bssid) == AP, "amsdu_first_subframe_arp");
    }
    // ---- control frames ------------------------------------------------------
    {
        MacFrame f = P("rts");
        check(f.fc.type == FrameType::Control && mac(f.receiver) == AP && mac(f.transmitter) == STA &&
              f.nav_us == 500 && f.header_len == 16 && f.body_state == BodyState::Absent, "rts_fields");
    }
    {
        MacFrame f = P("rts_bw_signalling_ta");
        check(mac(f.addr2) == "67:77:88:99:aa:bb" && mac(f.transmitter) == STA, "rts_bandwidth_signalling_ta_cleared");
    }
    check(mac(P("cts").receiver) == STA && P("cts").nav_us == 400 && P("cts").header_len == 10 && !P("cts").transmitter,
          "cts_ra_only");
    check(mac(P("ack").receiver) == STA && P("ack").nav_us == 0, "ack_ra_only");
    {
        MacFrame f = P("ps_poll_aid1");
        check(f.duration_kind == DurationKind::AssociationId && f.aid_from_duration == 1 && !f.nav_us &&
              mac(f.bssid) == AP && mac(f.transmitter) == STA, "ps_poll_aid_not_nav");
    }
    check(P("block_ack_req").body_state == BodyState::Unsupported && mac(P("block_ack_req").transmitter) == STA,
          "bar_header_only");
    check(mac(P("cf_end").bssid) == AP && mac(P("cf_end").receiver) == BC, "cf_end_bssid");

    // ---- no-FCS mode ---------------------------------------------------------
    {
        const auto& b = V.at("deauth_broadcast_reason7");
        MacFrame f = parse_mac_frame(b.data(), b.size() - 4, false);
        check(f.fcs == FcsStatus::NotPresent && f.management->reason_code == 7 && f.body_len == 2, "no_fcs_mode");
    }

    // ---- content hashes ------------------------------------------------------
    {
        std::vector<uint8_t> a(V.at("deauth_unicast_retry_reason3"));
        a.resize(a.size() - 4);
        std::vector<uint8_t> b = a;
        b[1] &= uint8_t(~0x08);  // same frame, Retry cleared
        MacFrame fa = parse_mac_frame(a.data(), a.size(), false), fb = parse_mac_frame(b.data(), b.size(), false);
        check(fa.content_hash != fb.content_hash, "hash_distinguishes_retry");
        check(fa.content_hash_retry_invariant == fb.content_hash_retry_invariant, "hash_retry_invariant_matches");
        MacFrame fa2 = parse_mac_frame(a.data(), a.size(), false);
        check(fa.content_hash == fa2.content_hash, "hash_deterministic");
        b[a.size() - 1] ^= 1;  // reason code changed
        check(parse_mac_frame(b.data(), b.size(), false).content_hash_retry_invariant != fa.content_hash_retry_invariant,
              "retry_invariant_hash_still_sees_body_changes");
    }

    // ---- real received beacons (receive-only X310, tests/fixtures/wifi_ofdm) --
    {
        struct Want { const char* file; uint64_t tsf; uint16_t seq; const char* bssid; };
        const Want wants[] = {
            {"airtel-2g4.json", 0x00000016e4e3805eull, 2534, "92:f0:4c:40:52:99"},
            {"avgarde-5g.json", 0x0000004bb6481036ull, 573, "3c:52:a1:0b:bf:d6"},
            {"avgarde-5g-offset.json", 0x0000004bd017e036ull, 708, "3c:52:a1:0b:bf:d6"},
        };
        for (const auto& w : wants) {
            nlohmann::json j;
            std::ifstream(std::string(PROJECT_ROOT_DIR) + "/tests/fixtures/wifi_ofdm/" + w.file) >> j;
            auto b = from_hex(j.at("expected_mpdu_hex").get<std::string>());
            MacFrame f = parse_mac_frame(b.data(), b.size());
            std::string n = std::string("real_") + w.file;
            check(f.fcs_valid() && f.is_management(mgmt::Beacon), n + "_fcs_beacon");
            check(f.management && f.management->timestamp_tsf == w.tsf, n + "_tsf");
            check(f.sequence_number == w.seq, n + "_seq", f.sequence_number ? std::to_string(*f.sequence_number) : "");
            check(mac(f.bssid) == w.bssid && f.beacon_identity && f.beacon_identity->bssid == w.bssid,
                  n + "_bssid_agrees_with_existing_parser");
            check(f.body_state == BodyState::Readable && f.management->elements->complete, n + "_elements_complete");
        }
    }

    // ---- robustness: every prefix and random bytes never read out of bounds --
    // Run under -fsanitize=address,undefined to make this meaningful (see
    // docs/WIFI_SECURITY_IMPLEMENTATION_PLAN.md, package A acceptance).
    {
        size_t prefixes = 0;
        bool invariants = true;
        for (const auto& [name, bytes] : V) {
            for (size_t n = 0; n <= bytes.size(); ++n) {
                std::vector<uint8_t> p(bytes.begin(), bytes.begin() + long(n));  // exact-size heap copy
                for (bool fcs : {true, false}) {
                    MacFrame f = parse_mac_frame(p.data(), p.size(), fcs);
                    size_t payload = fcs ? (n >= 4 ? n - 4 : 0) : n;
                    if (f.header_len > payload || f.header_len + f.body_len > payload) invariants = false;
                    if (fcs && n < bytes.size() && n >= 4 && f.fcs_valid() && name != "deauth_bad_fcs") {
                        // A truncated copy carrying a valid FCS would be a 2^-32 coincidence.
                        invariants = false;
                    }
                    ++prefixes;
                }
            }
        }
        check(invariants, "prefix_invariants", std::to_string(prefixes) + " parses");
        std::mt19937 rng(20260924);
        bool ok = true;
        for (int i = 0; i < 20000; ++i) {
            std::vector<uint8_t> r(size_t(rng() % 400));
            for (auto& x : r) x = uint8_t(rng());
            if (!r.empty() && (i & 1)) r[0] = uint8_t(r[0] & 0xFC);  // bias toward protocol version 0
            MacFrame f = parse_mac_frame(r.data(), r.size(), i % 3 != 0);
            if (f.header_len + f.body_len > r.size()) ok = false;
            (void)describe(f);
        }
        check(ok, "random_bytes_invariants", "20000 buffers");
    }

    std::printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "ALL PASSED", failures);
    return failures ? 1 : 0;
}
