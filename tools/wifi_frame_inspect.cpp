// Offline 802.11 MPDU inspector for the security frame parser.
//
// Reads one MPDU per line as hex (optionally "name<TAB>hex"), from a file
// or stdin, and prints either a one-line summary or one JSON object per
// frame. No radio, no live records. Used to cross-check parse_mac_frame()
// field-by-field against an independent dissector (tshark).
//
//   wifi_frame_inspect [--json] [--no-fcs] [file]
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "security/wifi_mac_frame.hpp"

using namespace rfmon::wifi_security;

namespace {

bool parse_hex(const std::string& s, std::vector<uint8_t>& out) {
    out.clear();
    int hi = -1;
    for (char ch : s) {
        int v;
        if (ch >= '0' && ch <= '9') v = ch - '0';
        else if (ch >= 'a' && ch <= 'f') v = ch - 'a' + 10;
        else if (ch >= 'A' && ch <= 'F') v = ch - 'A' + 10;
        else if (ch == ' ' || ch == ':' || ch == '\r') continue;
        else return false;
        if (hi < 0) hi = v;
        else { out.push_back(uint8_t(hi << 4 | v)); hi = -1; }
    }
    return hi < 0;
}

template <class T> void put(nlohmann::json& j, const char* k, const std::optional<T>& v) {
    if (v) j[k] = *v;
}
void put_mac(nlohmann::json& j, const char* k, const std::optional<MacAddress>& v) {
    if (v) j[k] = format_mac(*v);
}

nlohmann::json to_json(const MacFrame& f) {
    nlohmann::json j;
    j["type"] = int(f.fc.type);
    j["subtype"] = f.fc.subtype;
    j["retry"] = f.fc.retry;
    j["protected"] = f.fc.protected_frame;
    j["fcs"] = fcs_status_name(f.fcs);
    j["header"] = parse_state_name(f.header_state);
    j["body"] = body_state_name(f.body_state);
    if (!f.problem.empty()) j["problem"] = f.problem;
    put(j, "nav_us", f.nav_us);
    put(j, "aid_from_duration", f.aid_from_duration);
    put_mac(j, "addr1", f.addr1); put_mac(j, "addr2", f.addr2);
    put_mac(j, "addr3", f.addr3); put_mac(j, "addr4", f.addr4);
    put_mac(j, "ra", f.receiver); put_mac(j, "ta", f.transmitter);
    put_mac(j, "da", f.destination); put_mac(j, "sa", f.source); put_mac(j, "bssid", f.bssid);
    put(j, "seq", f.sequence_number);
    put(j, "frag", f.fragment_number);
    put(j, "tid", f.tid);
    put(j, "ht_control", f.ht_control);
    if (f.cipher) {
        auto& c = j["cipher"];
        const char* kinds[] = {"WEP", "CCMP/GCMP", "TKIP", "ambiguous"};
        c["kind"] = kinds[int(f.cipher->kind)];
        c["key_id"] = f.cipher->key_id;
        put(c, "pn", f.cipher->packet_number);
        put(c, "wep_iv", f.cipher->wep_iv);
    }
    if (f.management) {
        const auto& m = *f.management;
        auto& o = j["mgmt"];
        o = nlohmann::json::object();
        put(o, "tsf", m.timestamp_tsf); put(o, "beacon_interval_tu", m.beacon_interval_tu);
        put(o, "capability", m.capability); put(o, "listen_interval", m.listen_interval);
        put_mac(o, "current_ap", m.current_ap);
        put(o, "status", m.status_code); put(o, "aid", m.association_id); put(o, "reason", m.reason_code);
        put(o, "auth_alg", m.auth_algorithm); put(o, "auth_seq", m.auth_transaction); put(o, "sae_group", m.sae_group);
        put(o, "category", m.action_category); put(o, "action", m.action_code);
        if (m.action_category) o["robust_action"] = m.robust_action;
        put(o, "sa_query_tid", m.sa_query_transaction);
        if (m.elements) {
            const auto& e = *m.elements;
            auto& el = o["elements"];
            el["ids"] = e.ids;
            el["complete"] = e.complete;
            if (e.ssid) el["ssid"] = rfmon::wifi::display_text(*e.ssid);
            if (e.ssid_present) el["ssid_wildcard"] = e.ssid_wildcard;
            put(el, "ds_channel", e.ds_channel);
            put(el, "country", e.country);
            if (e.tim) { el["dtim_count"] = e.tim->dtim_count; el["dtim_period"] = e.tim->dtim_period; }
            if (e.bss_load) { el["bss_stations"] = e.bss_load->station_count; el["bss_utilization"] = e.bss_load->channel_utilization; }
            if (e.rsn) { el["mfpr"] = e.rsn->mfp_required; el["mfpc"] = e.rsn->mfp_capable; el["rsn_well_formed"] = e.rsn->well_formed; }
            if (e.channel_switch) {
                el["csa_mode"] = e.channel_switch->mode; el["csa_channel"] = e.channel_switch->new_channel;
                el["csa_count"] = e.channel_switch->count; put(el, "csa_operating_class", e.channel_switch->new_operating_class);
            }
            if (e.quiet) { el["quiet_count"] = e.quiet->count; el["quiet_period"] = e.quiet->period;
                           el["quiet_duration"] = e.quiet->duration_tu; el["quiet_offset"] = e.quiet->offset_tu; }
            if (e.mme) { el["mme_key_id"] = e.mme->key_id; el["mme_ipn"] = e.mme->ipn; }
            if (!e.vendor_ouis.empty()) el["vendor_ouis"] = e.vendor_ouis;
        }
    }
    if (f.data) {
        auto& d = j["data"];
        d["null"] = f.data->null_function;
        d["amsdu"] = f.data->amsdu;
        put(d, "ethertype", f.data->ethertype);
        put(d, "eapol_type", f.data->eapol_packet_type);
        if (f.data->eapol_key) {
            const auto& k = *f.data->eapol_key;
            d["eapol_message"] = eapol_message_name(k.message);
            d["replay_counter"] = k.replay_counter;
            d["key_info"] = k.key_info;
            put(d, "key_data_length", k.key_data_length);
        }
    }
    if (f.beacon_identity) {
        j["beacon_identity"] = {{"bssid", f.beacon_identity->bssid},
                                {"ssid", rfmon::wifi::display_text(f.beacon_identity->ssid)},
                                {"channel", f.beacon_identity->channel},
                                {"security", f.beacon_identity->security}};
    }
    char h[24];
    std::snprintf(h, sizeof(h), "%016llx", static_cast<unsigned long long>(f.content_hash));
    j["content_hash"] = h;
    std::snprintf(h, sizeof(h), "%016llx", static_cast<unsigned long long>(f.content_hash_retry_invariant));
    j["content_hash_retry_invariant"] = h;
    return j;
}

}  // namespace

int main(int argc, char** argv) {
    bool json = false, has_fcs = true;
    std::string path;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--json") json = true;
        else if (a == "--no-fcs") has_fcs = false;
        else if (a == "-h" || a == "--help") {
            std::printf("usage: %s [--json] [--no-fcs] [file]\n"
                        "One MPDU per line as hex, optionally 'name<TAB>hex'. Default input: stdin.\n", argv[0]);
            return 0;
        } else path = a;
    }
    std::ifstream file;
    if (!path.empty()) {
        file.open(path);
        if (!file) { std::fprintf(stderr, "cannot open %s\n", path.c_str()); return 1; }
    }
    std::istream& in = path.empty() ? std::cin : file;
    std::string line;
    int bad_lines = 0;
    std::vector<uint8_t> bytes;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::string name, hex = line;
        if (auto tab = line.find('\t'); tab != std::string::npos) { name = line.substr(0, tab); hex = line.substr(tab + 1); }
        if (!parse_hex(hex, bytes)) { std::fprintf(stderr, "skipping non-hex line: %s\n", line.c_str()); ++bad_lines; continue; }
        MacFrame f = parse_mac_frame(bytes.data(), bytes.size(), has_fcs);
        if (json) {
            nlohmann::json j = to_json(f);
            if (!name.empty()) j["name"] = name;
            j["length"] = bytes.size();
            std::cout << j.dump() << "\n";
        } else {
            std::cout << (name.empty() ? "" : name + ": ") << describe(f) << "\n";
        }
    }
    return bad_lines ? 2 : 0;
}
