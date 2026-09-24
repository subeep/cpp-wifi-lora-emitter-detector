#include "security/wifi_security_baseline.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace rfmon::wifi_security {

using nlohmann::json;

const char* baseline_metric_name(BaselineMetric m) {
    switch (m) {
        case BaselineMetric::Frames: return "frames";
        case BaselineMetric::Bursts: return "bursts";
        case BaselineMetric::Deauth: return "deauth";
        case BaselineMetric::DeauthBroadcast: return "deauth_broadcast";
        case BaselineMetric::Disassoc: return "disassoc";
        case BaselineMetric::Auth: return "auth";
        case BaselineMetric::AssocRequest: return "assoc_request";
        case BaselineMetric::ReassocRequest: return "reassoc_request";
        case BaselineMetric::ProbeRequest: return "probe_request";
        case BaselineMetric::Beacon: return "beacon";
        case BaselineMetric::Action: return "action";
        case BaselineMetric::Data: return "data";
        case BaselineMetric::Control: return "control";
        case BaselineMetric::Transmitters: return "transmitters";
        case BaselineMetric::kCount: break;
    }
    return "?";
}

namespace {
void bump(std::array<uint64_t, kBaselineMetrics>& a, BaselineMetric m, uint64_t n = 1) { a[size_t(m)] += n; }
}  // namespace

std::string BaselineStore::key_for(const CaptureRecord& c) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.0f", c.sample_rate_hz);
    std::string gain = c.gain_db ? std::to_string(int(std::lround(*c.gain_db * 10))) : "agc";
    return c.band + "|ch" + std::to_string(c.channel) + "|" + buf + "sps|gain" + gain + "|" + c.antenna + "|" + c.device;
}

void BaselineStore::note_frame(const FrameEvent& e, const MacFrame& f) {
    const auto key = std::make_pair(e.run_id, e.capture_seq);
    auto [it, inserted] = pending_.try_emplace(key);
    if (inserted) {
        pending_order_.push_back(key);
        while (pending_order_.size() > limits_.max_pending_captures) {
            pending_.erase(pending_order_.front());
            pending_order_.pop_front();
            ++pending_dropped_;
        }
        it = pending_.find(key);
        if (it == pending_.end()) return;
    }
    Pending& p = it->second;
    bump(p.counts, BaselineMetric::Frames);
    if (f.fc.type == FrameType::Control) bump(p.counts, BaselineMetric::Control);
    else if (f.fc.type == FrameType::Data) bump(p.counts, BaselineMetric::Data);
    else if (f.fc.type == FrameType::Management && f.fc.protocol_version == 0) {
        switch (f.fc.subtype) {
            case mgmt::Deauthentication:
                bump(p.counts, BaselineMetric::Deauth);
                if (f.receiver && is_broadcast_address(*f.receiver)) bump(p.counts, BaselineMetric::DeauthBroadcast);
                break;
            case mgmt::Disassociation: bump(p.counts, BaselineMetric::Disassoc); break;
            case mgmt::Authentication: bump(p.counts, BaselineMetric::Auth); break;
            case mgmt::AssocRequest: bump(p.counts, BaselineMetric::AssocRequest); break;
            case mgmt::ReassocRequest: bump(p.counts, BaselineMetric::ReassocRequest); break;
            case mgmt::ProbeRequest: bump(p.counts, BaselineMetric::ProbeRequest); break;
            case mgmt::Beacon: bump(p.counts, BaselineMetric::Beacon); break;
            case mgmt::Action: case mgmt::ActionNoAck: bump(p.counts, BaselineMetric::Action); break;
            default: break;
        }
    }
    if (f.transmitter && p.transmitters.size() < limits_.max_window_transmitters) p.transmitters.insert(*f.transmitter);
}

BaselineStore::Baseline& BaselineStore::baseline_for(const CaptureRecord& c) {
    const std::string key = key_for(c);
    auto it = baselines_.find(key);
    if (it != baselines_.end()) return it->second;
    if (baselines_.size() >= limits_.max_baselines) {
        // Evict the baseline with the least learned air time.
        auto victim = std::min_element(baselines_.begin(), baselines_.end(), [](const auto& a, const auto& b) {
            return a.second.meta.learned_analysed_s < b.second.meta.learned_analysed_s;
        });
        baselines_.erase(victim);
    }
    Baseline& b = baselines_[key];
    b.meta.key = key;
    b.meta.band = c.band;
    b.meta.channel = c.channel;
    b.meta.sample_rate_hz = c.sample_rate_hz;
    b.meta.gain_db = c.gain_db;
    b.meta.antenna = c.antenna;
    b.meta.device = c.device;
    b.meta.frozen = freeze_new_;
    b.current.version = b.meta.version;
    return b;
}

void BaselineStore::note_capture(const CaptureRecord& c) {
    const auto pkey = std::make_pair(c.run_id, c.capture_seq);
    Pending pend;
    if (auto it = pending_.find(pkey); it != pending_.end()) {
        pend = std::move(it->second);
        pending_.erase(it);
        pending_order_.erase(std::remove(pending_order_.begin(), pending_order_.end(), pkey), pending_order_.end());
    }
    Baseline& b = baseline_for(c);
    if (!c.processed || !c.contiguous() || c.analysed_seconds() <= 0) {
        ++b.meta.captures_excluded;
        return;  // degraded capture: its frames are not learned from
    }
    BaselineWindow& w = b.current;
    if (!w.captures) w.first_host_ns = c.host_before_ns;
    w.last_host_ns = c.host_before_ns;
    w.analysed_s += c.analysed_seconds();
    ++w.captures;
    for (size_t i = 0; i < kBaselineMetrics; ++i) w.counts[i] += pend.counts[i];
    bump(w.counts, BaselineMetric::Bursts, c.bursts_detected);
    for (const auto& t : pend.transmitters)
        if (b.current_transmitters.size() < limits_.max_window_transmitters) b.current_transmitters.insert(t);
    w.counts[size_t(BaselineMetric::Transmitters)] = b.current_transmitters.size();
    b.meta.current_analysed_s = w.analysed_s;
    if (w.analysed_s >= limits_.window_analysed_s) close_window(b);
}

void BaselineStore::close_window(Baseline& b) {
    BaselineWindow w = b.current;
    w.version = b.meta.version;
    w.excluded = b.meta.frozen;
    ++b.meta.windows_closed;
    if (w.excluded) ++b.meta.windows_excluded;
    b.windows.push_back(w);
    while (b.windows.size() > limits_.max_windows) b.windows.pop_front();
    b.current = BaselineWindow{};
    b.current.version = b.meta.version;
    b.current_transmitters.clear();
    b.meta.current_analysed_s = 0;
    recompute(b);
}

void BaselineStore::recompute(Baseline& b) {
    std::array<std::vector<double>, kBaselineMetrics> rates;
    double learned = 0;
    size_t included = 0;
    for (const auto& w : b.windows) {
        if (w.excluded || w.version != b.meta.version || w.analysed_s <= 0) continue;
        ++included;
        learned += w.analysed_s;
        for (size_t i = 0; i < kBaselineMetrics; ++i)
            rates[i].push_back(i == size_t(BaselineMetric::Transmitters) ? double(w.counts[i])
                                                                          : double(w.counts[i]) / w.analysed_s);
    }
    b.meta.windows_retained = b.windows.size();
    b.meta.windows_included = included;
    b.meta.learned_analysed_s = learned;
    for (size_t i = 0; i < kBaselineMetrics; ++i) {
        auto& v = rates[i];
        RateStats s;
        if (!v.empty()) {
            std::sort(v.begin(), v.end());
            double sum = 0;
            for (double x : v) sum += x;
            // Nearest-rank percentile: the smallest value with at least q of the windows at or below it.
            auto rank = [&](double q) {
                size_t idx = size_t(std::ceil(q * double(v.size())));
                return v[std::min(v.size() - 1, idx ? idx - 1 : 0)];
            };
            s.mean = sum / double(v.size());
            s.p50 = rank(0.5);
            s.p95 = rank(0.95);
            s.max = v.back();
        }
        b.meta.rates[i] = s;
    }
}

void BaselineStore::set_frozen(const std::string& key, bool frozen) {
    if (key.empty()) freeze_new_ = frozen;
    for (auto& [k, b] : baselines_)
        if (key.empty() || k == key) b.meta.frozen = frozen;
}

void BaselineStore::reset(const std::string& key) {
    for (auto& [k, b] : baselines_) {
        if (!key.empty() && k != key) continue;
        ++b.meta.version;
        b.windows.clear();
        b.current = BaselineWindow{};
        b.current.version = b.meta.version;
        b.current_transmitters.clear();
        b.meta.current_analysed_s = 0;
        b.meta.windows_closed = b.meta.windows_excluded = b.meta.captures_excluded = 0;
        recompute(b);
    }
}

std::vector<BaselineSummary> BaselineStore::summaries() const {
    std::vector<BaselineSummary> out;
    for (const auto& [k, b] : baselines_) out.push_back(b.meta);
    return out;
}

const std::deque<BaselineWindow>* BaselineStore::windows(const std::string& key) const {
    auto it = baselines_.find(key);
    return it == baselines_.end() ? nullptr : &it->second.windows;
}

json BaselineStore::export_json() const {
    json arr = json::array();
    for (const auto& [k, b] : baselines_) {
        json ws = json::array();
        for (const auto& w : b.windows) {
            json counts = json::object();
            for (size_t i = 0; i < kBaselineMetrics; ++i) counts[baseline_metric_name(BaselineMetric(i))] = w.counts[i];
            ws.push_back({{"version", w.version}, {"excluded", w.excluded}, {"first_host_ns", w.first_host_ns},
                          {"last_host_ns", w.last_host_ns}, {"analysed_s", w.analysed_s}, {"captures", w.captures},
                          {"counts", counts}});
        }
        json o = {{"key", k}, {"band", b.meta.band}, {"channel", b.meta.channel},
                  {"sample_rate_hz", b.meta.sample_rate_hz}, {"antenna", b.meta.antenna}, {"device", b.meta.device},
                  {"version", b.meta.version}, {"frozen", b.meta.frozen}, {"windows_closed", b.meta.windows_closed},
                  {"windows_excluded", b.meta.windows_excluded}, {"captures_excluded", b.meta.captures_excluded},
                  {"windows", ws}};
        if (b.meta.gain_db) o["gain_db"] = *b.meta.gain_db;
        arr.push_back(o);
    }
    return {{"schema", 1}, {"window_analysed_s", limits_.window_analysed_s}, {"baselines", arr}};
}

size_t BaselineStore::import_json(const json& j) {
    size_t restored = 0;
    if (!j.contains("baselines") || !j.at("baselines").is_array()) return 0;
    for (const auto& o : j.at("baselines")) {
        Baseline b;
        b.meta.key = o.at("key").get<std::string>();
        b.meta.band = o.value("band", "");
        b.meta.channel = o.value("channel", 0);
        b.meta.sample_rate_hz = o.value("sample_rate_hz", 0.0);
        if (o.contains("gain_db")) b.meta.gain_db = o.at("gain_db").get<double>();
        b.meta.antenna = o.value("antenna", "");
        b.meta.device = o.value("device", "");
        b.meta.version = o.value("version", 1u);
        b.meta.frozen = o.value("frozen", false);
        b.meta.windows_closed = o.value("windows_closed", uint64_t(0));
        b.meta.windows_excluded = o.value("windows_excluded", uint64_t(0));
        b.meta.captures_excluded = o.value("captures_excluded", uint64_t(0));
        b.meta.restored = true;
        for (const auto& wj : o.value("windows", json::array())) {
            BaselineWindow w;
            w.version = wj.value("version", 1u);
            w.excluded = wj.value("excluded", false);
            w.first_host_ns = wj.value("first_host_ns", int64_t(0));
            w.last_host_ns = wj.value("last_host_ns", int64_t(0));
            w.analysed_s = wj.value("analysed_s", 0.0);
            w.captures = wj.value("captures", uint64_t(0));
            const json counts = wj.value("counts", json::object());
            for (size_t i = 0; i < kBaselineMetrics; ++i)
                w.counts[i] = counts.value(baseline_metric_name(BaselineMetric(i)), uint64_t(0));
            b.windows.push_back(w);
            if (b.windows.size() > limits_.max_windows) b.windows.pop_front();
        }
        b.current.version = b.meta.version;  // partial windows never survive a restart
        recompute(b);
        baselines_[b.meta.key] = std::move(b);
        ++restored;
        if (baselines_.size() >= limits_.max_baselines) break;
    }
    return restored;
}

}  // namespace rfmon::wifi_security
