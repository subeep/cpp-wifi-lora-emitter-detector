#include "security/lora_rules.hpp"

#include "lorawan_inspect.hpp"

#include <algorithm>
#include <cstdio>

namespace rfmon::lora_rules {
namespace {
std::string hex(const std::vector<uint8_t>& b) {
    static const char* d = "0123456789ABCDEF";
    std::string s;
    s.reserve(b.size() * 2);
    for (uint8_t v : b) { s += d[v >> 4]; s += d[v & 15]; }
    return s;
}
std::string fmt(const char* f, double a, double b = 0, double c = 0) {
    char buf[160];
    std::snprintf(buf, sizeof buf, f, a, b, c);
    return buf;
}
std::string where(const Frame& f) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "%.4f MHz SF%d BW%.0f kHz", f.freq_hz / 1e6, f.sf, f.bw_hz / 1e3);
    return buf;
}
}  // namespace

template <class M, class F> void Engine::bound(M& m, F last_seen) {
    while (m.size() > MAX_TRACKED) {
        auto oldest = m.begin();
        for (auto it = m.begin(); it != m.end(); ++it)
            if (last_seen(it->second) < last_seen(oldest->second)) oldest = it;
        m.erase(oldest);
    }
}

void Engine::raise(const std::string& rule, const std::string& severity, const std::string& subject,
                   const std::string& summary, const std::string& evidence, double ts) {
    for (auto& i : incidents_) {
        if (i.open && i.rule == rule && i.subject == subject) {
            ++i.count;
            i.last_ts = ts;
            i.summary = summary;
            i.evidence = evidence;
            return;
        }
    }
    Incident i;
    i.id = next_id_++;
    i.first_ts = i.last_ts = ts;
    i.rule = rule;
    i.severity = severity;
    i.subject = subject;
    i.summary = summary;
    i.evidence = evidence;
    i.count = 1;
    incidents_.push_front(std::move(i));
    while (incidents_.size() > MAX_INCIDENTS) incidents_.pop_back();
}

void Engine::ingest(const Frame& f) {
    ++stats_.frames;
    const auto wan = inspect_lorawan(f.payload);
    if (!wan) { ++stats_.ignored; return; }
    const std::string bytes = hex(f.payload);

    // Identical bytes: retransmission inside the window, replay evidence after.
    bool repeat_in_window = false, replayed = false;
    {
        auto it = frames_.find(bytes);
        if (it != frames_.end()) {
            FrameSeen& seen = it->second;
            const double since = f.ts - seen.last;
            if (f.capture_seq == seen.capture || since <= RETRANSMIT_WINDOW_S) {
                repeat_in_window = true;
                ++stats_.retransmissions;
            } else if (wan->mtype != 0) {  // join requests are covered by the DevNonce rule
                raise(RULE_REPLAY, "High", wan->device_kind.empty() ? "Join accept" : wan->device_kind + " " + wan->device_id,
                      "Identical " + wan->type + " bytes heard again " + fmt("%.0f s", since) +
                          " after the previous copy (replay, or a device counter reset re-sending the same data).",
                      "First heard " + fmt("%.0f", seen.first) + ", previous " + fmt("%.0f", seen.last) +
                          ", now " + fmt("%.0f", f.ts) + " on " + where(f) + ". Bytes " + bytes,
                      f.ts);
                replayed = true;
            }
            seen.last = f.ts;
            seen.capture = f.capture_seq;
        } else {
            frames_[bytes] = {f.ts, f.ts, f.capture_seq};
            bound(frames_, [](const FrameSeen& v) { return v.last; });
        }
    }
    // Same transmission (counters already saw it), or already reported as a
    // replay (its old counter would only repeat the same finding).
    if (repeat_in_window || replayed) return;

    const std::string device = wan->device_kind.empty() ? "" : wan->device_kind + " " + wan->device_id;

    if (wan->fcnt16) {
        ++stats_.data_frames;
        const std::string key = device + (wan->uplink ? " uplink" : " downlink");
        auto [it, fresh] = counters_.try_emplace(key);
        CounterState& c = it->second;
        const uint16_t n = *wan->fcnt16;
        if (!fresh) {
            auto prior = c.bytes_by_fcnt.find(n);
            if (prior != c.bytes_by_fcnt.end() && prior->second != bytes) {
                raise(RULE_FCNT_REUSE, "High", device,
                      fmt("FCnt %.0f carried different bytes than before", n) +
                          " (forged frame, or the device restarted its counter).",
                      "Previous bytes " + prior->second + "; now " + bytes + " on " + where(f) + ".", f.ts);
            } else if (n != c.last) {
                const uint16_t back = uint16_t(c.last - n);
                // A 16-bit wrap moves forward by a small amount; a move
                // backwards by less than half the space is a regression.
                if (back != 0 && back < 0x8000) {
                    raise(RULE_FCNT_REGRESSION, "Medium", device,
                          fmt("FCnt dropped from %.0f to %.0f", c.last, n) + " (replay of an older frame, or a device reset).",
                          fmt("Previous FCnt %.0f at %.0f", c.last, c.last_ts) + fmt("; FCnt %.0f at %.0f", n, f.ts) +
                              " on " + where(f) + ". Missed frames cannot cause this; they only leave gaps.",
                          f.ts);
                }
            }
        }
        if (fresh || uint16_t(n - c.last) < 0x8000) { c.last = n; c.last_ts = f.ts; }
        if (!c.bytes_by_fcnt.count(n)) {
            c.bytes_by_fcnt[n] = bytes;
            c.order.push_back(n);
            while (c.order.size() > 256) { c.bytes_by_fcnt.erase(c.order.front()); c.order.pop_front(); }
        }
        bound(counters_, [](const CounterState& v) { return v.last_ts; });
    }

    if (wan->dev_nonce) {
        ++stats_.join_requests;
        auto& nonces = nonces_[wan->device_id];
        auto prior = nonces.find(*wan->dev_nonce);
        if (prior != nonces.end()) {
            raise(RULE_DEVNONCE_REUSE, "High", device,
                  fmt("Join request reused DevNonce %.0f", *wan->dev_nonce) +
                      " (join replay; a compliant device never repeats a nonce).",
                  fmt("Earlier use at %.0f, again at %.0f", prior->second, f.ts) + " on " + where(f) +
                      ". JoinEUI " + wan->join_eui + ".",
                  f.ts);
        }
        nonces[*wan->dev_nonce] = f.ts;
        if (nonces.size() > 1024)
            nonces.erase(std::min_element(nonces.begin(), nonces.end(),
                                          [](const auto& a, const auto& b) { return a.second < b.second; }));
        bound(nonces_, [](const std::map<uint16_t, double>& v) {
            double t = 0;
            for (const auto& [n, ts] : v) t = std::max(t, ts);
            return t;
        });

        joins_.push_back({f.ts, wan->device_id});
        while (!joins_.empty() && f.ts - joins_.front().first > JOIN_FLOOD_WINDOW_S) joins_.pop_front();
        if (joins_.size() >= JOIN_FLOOD_COUNT && f.ts >= join_flood_until_) {
            std::set<std::string> eui;
            for (const auto& j : joins_) eui.insert(j.second);
            raise(RULE_JOIN_FLOOD, "Medium", "All join requests",
                  fmt("At least %.0f join requests from %.0f DevEUIs within %.0f s", double(joins_.size()), double(eui.size()),
                      JOIN_FLOOD_WINDOW_S) +
                      " (sampled coverage: the real count can only be higher).",
                  "Latest on " + where(f) + ", DevEUI " + wan->device_id + ".", f.ts);
            join_flood_until_ = f.ts + JOIN_FLOOD_WINDOW_S;
        }
    }

    if (f.rf_cluster && !device.empty()) {
        RfState& r = rf_[device];
        r.last_ts = f.ts;
        size_t dominant_n = 0;
        int dominant = -1;
        for (const auto& [id, n] : r.clusters)
            if (n > dominant_n) { dominant_n = n; dominant = id; }
        if (r.total >= RF_BASELINE_MIN && dominant != *f.rf_cluster &&
            double(dominant_n) >= RF_BASELINE_SHARE * double(r.total)) {
            char a[16], b[16];
            std::snprintf(a, sizeof a, "LORA-%04d", dominant);
            std::snprintf(b, sizeof b, "LORA-%04d", *f.rf_cluster);
            raise(RULE_RF_MISMATCH, "Review", device,
                  std::string("Usually heard from RF cluster ") + a + " but this frame matched " + b +
                      " (spoofed address, or a fingerprint cluster split).",
                  fmt("%.0f of %.0f earlier cluster-tagged frames came from ", double(dominant_n), double(r.total)) + a +
                      ". Clusters are heuristic IRR/DC/IQ matches. On " + where(f) + ".",
                  f.ts);
        }
        ++r.clusters[*f.rf_cluster];
        ++r.total;
        bound(rf_, [](const RfState& v) { return v.last_ts; });
    }
}

void Engine::note_disallowed_emitter(int cluster, const std::string& label, double ts, double freq_hz) {
    char id[16];
    std::snprintf(id, sizeof id, "LORA-%04d", cluster);
    raise(RULE_DISALLOWED, "High", id, std::string("Fingerprint matched ") + id + (label.empty() ? "" : " (" + label + ")") +
          ", which is marked disallowed.",
          fmt("Heard on %.4f MHz at %.0f. Clusters are heuristic RF matches.", freq_hz / 1e6, ts), ts);
}

void Engine::remap_cluster(int from, int to) {
    for (auto& [key, r] : rf_) {
        auto it = r.clusters.find(from);
        if (it == r.clusters.end()) continue;
        r.clusters[to] += it->second;
        r.clusters.erase(from);
    }
}

bool Engine::acknowledge(uint64_t id) {
    for (auto& i : incidents_)
        if (i.id == id && i.open) { i.open = false; return true; }
    return false;
}

void Engine::clear_closed() {
    incidents_.erase(std::remove_if(incidents_.begin(), incidents_.end(), [](const Incident& i) { return !i.open; }),
                     incidents_.end());
}

Snapshot Engine::snapshot() const {
    Snapshot s;
    s.incidents.assign(incidents_.begin(), incidents_.end());
    s.stats = stats_;
    return s;
}

}  // namespace rfmon::lora_rules
