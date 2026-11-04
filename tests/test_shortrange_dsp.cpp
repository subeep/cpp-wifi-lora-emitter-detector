// Synthetic end-to-end checks for the Bluetooth LE advertising and
// IEEE 802.15.4 / Zigbee receivers: packets are modulated, placed in a
// 20 MSps wideband capture at a channel offset with carrier offset, noise
// and an interferer, then channel-extracted, demodulated and parsed.
// Synthetic only: real-radio interoperability needs live captures.
#include <cmath>
#include <complex>
#include <cstdio>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "shortrange/assigned_numbers.hpp"
#include "shortrange/ble.hpp"
#include "shortrange/ddc.hpp"
#include "shortrange/ieee802154.hpp"
#include "shortrange/monitor.hpp"
#include "shortrange_synth.hpp"

#include <chrono>
#include <filesystem>
#include <thread>

using namespace rfmon::shortrange;
using cf = std::complex<float>;
using namespace shortrange_synth;

static int failures = 0;
static void check(bool ok, const std::string& name, const std::string& detail = "") {
    std::printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", name.c_str(), detail.empty() ? "" : " | ", detail.c_str());
    if (!ok) ++failures;
}

int main() {
    std::mt19937 rng(7);
    std::normal_distribution<float> noise(0.f, 1.f);

    check(std::string(company_name(0x004C) ? company_name(0x004C) : "") == "Apple, Inc.", "assigned numbers: Apple company id");
    check(uuid16_name(0xFFFA) != nullptr, "assigned numbers: ASTM Remote ID UUID");
    const uint8_t kermit[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    check(ieee802154::fcs(kermit, 9) == 0x2189, "802.15.4 FCS check value (CRC-16/KERMIT)");

    // BLE: ideal baseband straight into the demodulator.
    {
        const auto pdu = ble_adv_pdu();
        const auto bb = ble::modulate(pdu, 38);
        std::vector<cf> padded(400, {0, 0});
        padded.insert(padded.end(), bb.begin(), bb.end());
        padded.insert(padded.end(), 400, {0, 0});
        const auto pk = ble::demodulate(padded, 38);
        check(pk.size() == 1 && pk[0].pdu == pdu, "BLE ideal baseband round trip", std::to_string(pk.size()) + " packets");
    }

    // Wideband: BLE ch 38 at +2.5 MHz and Zigbee ch 15 at +1.5 MHz would
    // overlap; use Wi-Fi ch 4 capture centre 2428.5 MHz: BLE 2426 (-2.5 MHz),
    // Zigbee ch 16 2430 (+1.5 MHz) at a different time, plus a CW interferer.
    const double centre = 2428.5e6, rate = 20e6;
    std::vector<cf> wide(size_t(rate * 0.05), {0, 0});
    for (auto& v : wide) v = cf(noise(rng), noise(rng)) * 0.01f;
    const auto pdu = ble_adv_pdu();
    place(wide, 50000, ble::modulate(pdu, 38), 2426e6 + 60e3 - centre, 0.2);          // +60 kHz CFO
    place(wide, 300000, ieee802154::modulate(zigbee_beacon()), 2430e6 - 70e3 - centre, 0.2);  // -70 kHz CFO
    place(wide, 600000, ieee802154::modulate(zigbee_data()), 2430e6 + 40e3 - centre, 0.05);   // weaker
    for (size_t n = 0; n < wide.size(); ++n) {  // CW 4 MHz away from BLE channel
        const double ph = 2 * M_PI * (2422e6 - centre) * double(n) / rate;
        wide[n] += 0.05f * cf(float(std::cos(ph)), float(std::sin(ph)));
    }

    {
        const auto ch = ddc(wide.data(), wide.size(), rate, 2426e6 - centre, design_fir(rate, 0.75e6, 3.25e6, 5));
        const auto narrow = ddc(ch.data(), ch.size(), 4e6, 0, design_fir(4e6, 0.75e6, 1.25e6, 1));
        ble::DemodStats st;
        const auto pk = ble::demodulate(narrow, 38, &st);
        check(pk.size() == 1 && pk[0].pdu == pdu, "BLE in 20 MSps capture with CFO, noise and CW", std::to_string(pk.size()) + " packets");
        if (!pk.empty()) {
            check(std::abs(pk[0].cfo_hz - 60e3) < 15e3, "BLE carrier offset estimate", std::to_string(pk[0].cfo_hz));
            check(std::abs(double(pk[0].sample) * 5 - 50000 - 32 * 5) < 200, "BLE packet position", std::to_string(pk[0].sample * 5));
            const auto a = ble::parse(pk[0].pdu);
            check(a.valid && a.type == 0 && a.advertiser && a.advertiser->text() == "C1:22:33:44:55:66" &&
                      a.advertiser->kind() == "Random static", "BLE address and type", a.advertiser ? a.advertiser->text() : "none");
            check(a.name == "RFMON-TEST", "BLE complete local name", a.name);
            check(a.company_id == 0x004C && a.company == "Apple, Inc.", "BLE manufacturer company", a.company);
            bool find_my = false;
            for (const auto& t : a.tags) find_my |= t == "Find My";
            check(find_my, "BLE Apple Find My recognised");
            check(a.appearance && *a.appearance == 961, "BLE appearance");
        }
    }
    {
        const auto ch = ddc(wide.data(), wide.size(), rate, 2430e6 - centre, design_fir(rate, 1.2e6, 2.8e6, 5));
        const auto narrow = ddc(ch.data(), ch.size(), 4e6, 0, design_fir(4e6, 1.2e6, 1.8e6, 1));
        ieee802154::DemodStats st;
        const auto fr = ieee802154::demodulate(narrow, 16, &st);
        check(fr.size() == 2, "802.15.4 two frames in 20 MSps capture", std::to_string(fr.size()) + " frames, " +
                                                                              std::to_string(st.syncs) + " syncs, " + std::to_string(st.fcs_failures) + " FCS failures");
        if (fr.size() == 2) {
            check(fr[0].psdu == zigbee_beacon() && fr[1].psdu == zigbee_data(), "802.15.4 PSDU bytes");
            check(std::abs(fr[0].cfo_hz + 70e3) < 25e3, "802.15.4 carrier offset estimate", std::to_string(fr[0].cfo_hz));
            const auto b = ieee802154::parse(fr[0].psdu);
            check(b.valid && b.is_beacon && b.pan_coordinator && b.association_permit && b.src_pan && *b.src_pan == 0x1A62 &&
                      b.src.mode == 2 && b.src.short_addr == 0, "802.15.4 beacon fields");
            check(b.network == "Zigbee" && b.zigbee_ext_pan && *b.zigbee_ext_pan == 0x1122334455667788ULL && b.stack_profile && *b.stack_profile == 2,
                  "Zigbee beacon payload", b.zigbee_ext_pan ? ieee802154::eui64_text(*b.zigbee_ext_pan) : "none");
            const auto d = ieee802154::parse(fr[1].psdu);
            check(d.valid && d.type == 1 && d.src.short_addr == 0x796F && d.dst.short_addr == 0x0000 && d.dst_pan && *d.dst_pan == 0x1A62,
                  "802.15.4 data frame addresses");
            check(d.network == "Zigbee" && d.nwk_src && *d.nwk_src == 0x796F && d.nwk_src_ext && *d.nwk_src_ext == 0x0002030405060708ULL &&
                      !d.nwk_security && d.role_hint == "Router (link status)", "Zigbee NWK header",
                  d.nwk_src_ext ? ieee802154::eui64_text(*d.nwk_src_ext) : "none");
        }
    }
    // Noise only: no packets.
    {
        std::vector<cf> n(size_t(4e6 * 0.2));
        for (auto& v : n) v = cf(noise(rng), noise(rng));
        check(ble::demodulate(n, 37).empty() && ieee802154::demodulate(n, 11).empty(), "noise-only input yields no packets");
    }
    // Remote ID service data (legacy advertising, one Basic ID message).
    {
        std::vector<uint8_t> msg(25, 0);
        msg[0] = 0x02;  // Basic ID, protocol v2
        msg[1] = 0x12;  // serial number, helicopter/multirotor
        const char* id = "TEST-BLE-RID-0001";
        for (size_t i = 0; id[i]; ++i) msg[2 + i] = uint8_t(id[i]);
        std::vector<uint8_t> payload = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 30, 0x16, 0xFA, 0xFF, 0x0D, 0x07};
        payload.insert(payload.end(), msg.begin(), msg.end());
        std::vector<uint8_t> pdu = {0x42, uint8_t(payload.size())};  // ADV_NONCONN_IND
        pdu.insert(pdu.end(), payload.begin(), payload.end());
        const auto a = ble::parse(pdu);
        check(a.valid && a.remote_id_messages.size() == 1 && a.remote_id_messages[0][1] == 0x12, "BLE Remote ID message extracted");
        const auto pk = ble::demodulate(ble::modulate(pdu, 37), 37);
        check(pk.size() == 1 && pk[0].pdu == pdu, "BLE Remote ID advert round trip");
    }
    // Stream centre from the UHD tune result (values read from the X310):
    // the samples are centred at actual_rf - actual_dsp, never rf + dsp.
    {
        struct Tune { double target, rf, dsp; };
        const Tune tunes[] = {{2401e6, 2401.001221e6, 1221.001}, {2423.5e6, 2423.504274e6, 4273.504}, {2428.5e6, 2428.498168e6, -1831.502}};
        bool ok = true;
        for (const auto& t : tunes) {
            rfmon::wifi_security::CaptureRecord r;
            r.capture_center_hz = t.target;
            r.actual_rf_hz = t.rf;
            r.actual_dsp_hz = t.dsp;
            ok &= std::abs(r.stream_center_hz() - t.target) < 1;
        }
        rfmon::wifi_security::CaptureRecord unknown;
        unknown.capture_center_hz = 2437e6;
        check(ok && unknown.stream_center_hz() == 2437e6, "capture centre: actual_rf - actual_dsp (X310 tune results)");
    }
    // Energy gate vs traffic beside the channel: the wide first stage passes
    // about +/-3 MHz, so near-continuous Wi-Fi next to BLE 37 used to set the
    // gate's "floor" and skip real packets (found on the X310 bench, 2026-11-04).
    {
        std::vector<cf> cap(size_t(rate * 0.02)), wifi(cap.size());
        for (auto& v : wifi) v = cf(noise(rng), noise(rng));
        const auto h = rfmon::blackman_lowpass(0.8e6 / rate, 0.2e6 / rate);  // ~1.6 MHz wide interferer
        for (size_t n = 0; n < cap.size(); ++n) {
            cf acc(0, 0);
            for (size_t k = 0; k < h.size() && k <= n; ++k) acc += h[k] * wifi[n - k];
            const double ph = 2 * M_PI * 3.2e6 * double(n) / rate;  // 2404.2 MHz: 2.2 MHz above BLE 37
            const bool on = (n / 20000) % 10 != 3;                    // busy 90% of the time
            cap[n] = cf(noise(rng), noise(rng)) * 0.002f + (on ? 0.6f : 0.f) * acc * cf(float(std::cos(ph)), float(std::sin(ph)));
        }
        place(cap, 100000, ble::modulate(ble_adv_pdu(), 37), 2402e6 - 2401e6, 0.05);
        Monitor m;
        m.process(cap.data(), cap.size(), rate, 2401e6, "TEST / adjacent Wi-Fi");
        const auto s = m.snapshot();
        check(s.ble.size() == 1 && s.ble_coverage[0].packets == 1, "gate: BLE 37 decoded beside busy adjacent traffic",
              std::to_string(s.ble_coverage[0].packets) + " packets");
    }
    // Coverage planning for the existing capture centres (Wi-Fi channel + 1.5 MHz, 20 MSps).
    {
        auto has = [](const std::vector<Monitor::Target>& t, bool b, int c) {
            for (const auto& x : t) if (x.ble == b && x.channel == c) return true;
            return false;
        };
        const auto ch1 = Monitor::targets(2413.5e6, 20e6), ch13 = Monitor::targets(2473.5e6, 20e6), drone = Monitor::targets(2401e6, 20e6);
        check(!has(ch1, true, 37) && has(ch13, true, 39) && has(drone, true, 37) && has(drone, false, 11),
              "coverage: BLE 37 only from the 2401 MHz visit, BLE 39 from Wi-Fi ch 13");
        check(Monitor::targets(5180e6, 20e6).empty() && Monitor::targets(2428.5e6, 25e6).empty(), "coverage: 5 GHz and non-integer rates skipped");
    }
    // Monitor: synchronous decode of the wideband capture into device lists.
    {
        Monitor m([](const uint8_t* msg) { return std::string("{\"parsed\":") + std::to_string(msg[0]) + "}"; });
        m.process(wide.data(), wide.size(), rate, centre, "TEST / synthetic", 1'000'000'000);
        auto s = m.snapshot();
        check(s.ble.size() == 1 && s.ble[0].name == "RFMON-TEST" && s.ble[0].company == "Apple, Inc." && s.ble[0].channel_counts.count(38),
              "monitor: BLE device listed", std::to_string(s.ble.size()) + " BLE devices");
        bool coordinator = false, router = false;
        for (const auto& d : s.zigbee) {
            coordinator |= d.key == "PAN 0x1A62 / 0x0000" && d.roles.count("PAN coordinator") && d.ext_pan.has_value();
            router |= d.key == "PAN 0x1A62 / 0x796F" && d.ext && *d.ext == 0x0002030405060708ULL && d.roles.count("Router (link status)");
        }
        check(s.zigbee.size() == 2 && coordinator && router, "monitor: Zigbee coordinator and router listed", std::to_string(s.zigbee.size()) + " Zigbee entries");
        const auto& cov38 = s.ble_coverage[1];
        check(cov38.captures == 1 && cov38.packets == 1 && s.zigbee_coverage[5].packets == 2 && s.ble_coverage[0].captures == 0,
              "monitor: per-channel coverage counters");
        check(s.recent_ble.size() == 1 && s.recent_zigbee.size() == 2, "monitor: recent packet rows");
        const auto dir = std::filesystem::temp_directory_path() / "rfmon-shortrange-test";
        std::filesystem::remove_all(dir);
        const auto path = m.export_json(dir.string());
        check(std::filesystem::file_size(path) > 500, "monitor: JSON export written", path);
        std::filesystem::remove_all(dir);
        // Remote ID via the parser hook.
        std::vector<uint8_t> msg(25, 0);
        msg[0] = 0x02; msg[1] = 0x12;
        for (size_t i = 0; i < 6; ++i) msg[2 + i] = uint8_t("RID-01"[i]);
        std::vector<uint8_t> payload = {0x10, 0x20, 0x30, 0x40, 0x50, 0xC6, 30, 0x16, 0xFA, 0xFF, 0x0D, 0x01};
        payload.insert(payload.end(), msg.begin(), msg.end());
        std::vector<uint8_t> pdu = {0x42, uint8_t(payload.size())};
        pdu.insert(pdu.end(), payload.begin(), payload.end());
        std::vector<cf> cap(size_t(rate * 0.01));
        for (auto& v : cap) v = cf(noise(rng), noise(rng)) * 0.01f;
        place(cap, 20000, ble::modulate(pdu, 37), 2402e6 - 2401e6, 0.2);
        m.process(cap.data(), cap.size(), rate, 2401e6, "TEST / drone visit");
        s = m.snapshot();
        bool rid = false;
        for (const auto& d : s.ble)
            for (const auto& [k, v] : d.remote_id) rid |= k == "Basic ID / id type 1" && v == "{\"parsed\":2}";
        check(rid, "monitor: Bluetooth Remote ID message parsed through the hook");
        m.clear();
        check(m.snapshot().ble.empty() && m.snapshot().ble_coverage[1].captures == 0, "monitor: clear");
    }
    // Stable list order: named first, otherwise first-seen; re-heard
    // addresses keep their row and only their counts change.
    {
        auto adv = [](uint8_t last, const std::string& name) {
            std::vector<uint8_t> payload = {last, 0x22, 0x33, 0x44, 0x55, 0xC0, 0x02, 0x01, 0x06};
            if (!name.empty()) {
                payload.push_back(uint8_t(name.size() + 1));
                payload.push_back(0x09);
                payload.insert(payload.end(), name.begin(), name.end());
            }
            std::vector<uint8_t> pdu = {0x40, uint8_t(payload.size())};
            pdu.insert(pdu.end(), payload.begin(), payload.end());
            return pdu;
        };
        auto capture_of = [&](const std::vector<std::vector<uint8_t>>& pdus) {
            std::vector<cf> cap(size_t(rate * 0.02));
            for (auto& v : cap) v = cf(noise(rng), noise(rng)) * 0.01f;
            for (size_t i = 0; i < pdus.size(); ++i) place(cap, 20000 + 60000 * i, ble::modulate(pdus[i], 38), 2426e6 - centre, 0.2);
            return cap;
        };
        auto order = [](const Snapshot& s) {
            std::string o;
            for (const auto& d : s.ble) o += d.address.substr(15) + (d.name.empty() ? "" : "*") + " ";
            return o;
        };
        Monitor m;
        auto c1 = capture_of({adv(0xA1, ""), adv(0xB2, "Named-B"), adv(0xC3, "")});
        m.process(c1.data(), c1.size(), rate, centre, "TEST / order 1");
        const auto first = order(m.snapshot());
        check(first == "B2* A1 C3 ", "order: named first, then first-seen", first);
        auto c2 = capture_of({adv(0xC3, ""), adv(0xA1, ""), adv(0xD4, "")});
        m.process(c2.data(), c2.size(), rate, centre, "TEST / order 2");
        auto s = m.snapshot();
        check(order(s) == "B2* A1 C3 D4 " && s.ble[1].packets == 2 && s.ble[2].packets == 2 && s.ble[0].packets == 1,
              "order: re-heard rows stay put, counts update", order(s));
        auto c3 = capture_of({adv(0xC3, "Named-C")});
        m.process(c3.data(), c3.size(), rate, centre, "TEST / order 3");
        check(order(m.snapshot()) == "B2* C3* A1 D4 ", "order: an address moves up once when its name arrives", order(m.snapshot()));
    }
    // Rotating private addresses: linked into one probable device only when
    // nothing contradicts it.
    {
        // Resolvable private address (top bits 01), Apple Nearby-info-like layout.
        auto rpa = [](uint8_t id, uint8_t continuity = 0x10) {
            std::vector<uint8_t> payload = {id, 0x11, 0x22, 0x33, 0x44, 0x52, 0x02, 0x01, 0x1A,
                                            0x07, 0xFF, 0x4C, 0x00, continuity, 0x02, 0x01, 0x00};
            std::vector<uint8_t> pdu = {0x40, uint8_t(payload.size())};
            pdu.insert(pdu.end(), payload.begin(), payload.end());
            return pdu;
        };
        struct Tx { std::vector<uint8_t> pdu; double cfo, amp; };
        auto run = [&](Monitor& m, const std::vector<Tx>& txs) {
            std::vector<cf> cap(size_t(rate * 0.02));
            for (auto& v : cap) v = cf(noise(rng), noise(rng)) * 0.01f;
            for (size_t i = 0; i < txs.size(); ++i)
                place(cap, 20000 + 60000 * i, ble::modulate(txs[i].pdu, 38), 2426e6 + txs[i].cfo - centre, txs[i].amp);
            m.process(cap.data(), cap.size(), rate, centre, "TEST / rotation");
            return m.snapshot();
        };
        {
            Monitor m;
            run(m, {{rpa(0x01), 30e3, 0.2}, {rpa(0x01), 30e3, 0.2}});
            auto s = run(m, {{rpa(0x02), 31e3, 0.2}, {rpa(0x02), 31e3, 0.2}, {rpa(0x02), 31e3, 0.2}});
            check(s.ble.size() == 2 && s.ble_grouped.size() == 1 && s.ble_grouped[0].group_addresses.size() == 2 &&
                      s.ble_grouped[0].packets == 5 && s.ble_grouped[0].address == "52:44:33:22:11:02" && s.ble_links == 1,
                  "rotation: new address joins the quiet one", std::to_string(s.ble_grouped.size()) + " rows");
            const auto row_key = s.ble_grouped[0].key;
            s = run(m, {{rpa(0x03), 29e3, 0.2}, {rpa(0x03), 29e3, 0.2}, {rpa(0x03), 29e3, 0.2}});
            check(s.ble_grouped.size() == 1 && s.ble_grouped[0].group_addresses.size() == 3 && s.ble_grouped[0].key == row_key,
                  "rotation: chain of three keeps one row and its key");
            s = run(m, {{rpa(0x01), 30e3, 0.2}});  // the first address again: it was not the same device
            check(s.ble_links_broken == 1 && s.ble_grouped.size() == 2, "rotation: link breaks when an old address is heard again",
                  std::to_string(s.ble_grouped.size()) + " rows");
        }
        {
            Monitor m;
            run(m, {{rpa(0x01), 30e3, 0.2}});
            auto s = run(m, {{rpa(0x02), 50e3, 0.2}, {rpa(0x02), 50e3, 0.2}, {rpa(0x02), 50e3, 0.2}});
            check(s.ble_grouped.size() == 2 && s.ble_links == 0, "rotation: different carrier offset is not linked");
            s = run(m, {{rpa(0x03, 0x0C), 50e3, 0.2}, {rpa(0x03, 0x0C), 50e3, 0.2}, {rpa(0x03, 0x0C), 50e3, 0.2}});
            check(s.ble_grouped.size() == 3 && s.ble_links == 0, "rotation: different advertising layout is not linked");
        }
        {
            Monitor m;
            run(m, {{rpa(0x01), 30e3, 0.2}, {rpa(0x02), 30e3, 0.2}});
            auto s = run(m, {{rpa(0x03), 30e3, 0.2}, {rpa(0x03), 30e3, 0.2}, {rpa(0x03), 30e3, 0.2}});
            check(s.ble_grouped.size() == 3 && s.ble_links == 0 && s.ble_links_ambiguous == 1,
                  "rotation: two equally good candidates link to neither");
        }
    }
    // Monitor observer path: disabled is inert; enabled decodes on its worker;
    // a burst within the queue limit is absorbed; beyond it, captures are dropped.
    {
        Limits none;
        none.queued_captures = 0;
        Monitor full({}, none);
        rfmon::wifi_security::CaptureRecord r0;
        r0.sample_rate_hz = rate;
        r0.capture_center_hz = centre;
        full.enable(true);
        full.observe(wide, r0);
        check(full.snapshot().dropped_busy == 1 && full.snapshot().decoded_captures == 0, "observer: full queue drops, never waits");
    }
    {
        Monitor m;
        rfmon::wifi_security::CaptureRecord rec;
        rec.sample_rate_hz = rate;
        rec.capture_center_hz = centre;
        rec.samples_received = wide.size();
        rec.run_id = "test";
        m.observe(wide, rec);
        check(m.snapshot().offered == 0, "observer: disabled monitor ignores captures");
        m.enable(true);
        m.observe(wide, rec);
        m.observe(wide, rec);  // arrives while the first is still decoding
        for (int i = 0; i < 500 && m.snapshot().decoded_captures < 2; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        auto s = m.snapshot();
        check(s.decoded_captures == 2 && s.dropped_busy == 0 && s.ble.size() == 1 && s.ble[0].packets == 2,
              "observer: burst queued and decoded on worker",
              std::to_string(s.decoded_captures) + " decoded, " + std::to_string(s.dropped_busy) + " dropped");
        rec.capture_center_hz = 5180e6;
        m.observe(wide, rec);
        check(m.snapshot().outside_band == 1, "observer: 5 GHz capture counted outside band");
    }
    std::printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
