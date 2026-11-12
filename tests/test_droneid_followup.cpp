// DroneID follow-up queue: when requests open and close, which visits are planned, and the cap
// on scanner time. Pure logic; no radio, decoder or threads.
#include "drones/droneid_followup.hpp"

#include <iostream>
#include <stdexcept>

using namespace rfmon::droneid_followup;
static void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }

int main() try {
    Config c;
    require(validate(c).empty(), "default configuration is valid");
    Config bad = c; bad.max_share = 0.8;
    require(!validate(bad).empty() && !Followup().set_config(bad), "an excessive share is refused");
    bad = c; bad.offset_hz = 6e6;
    require(!validate(bad).empty(), "an offset that hides the channel from the decoder is refused");
    Band band;
    require(band_of(2437e6, band) && band == Band::B24 && band_of(5745e6, band) && band == Band::B58 &&
            !band_of(868e6, band) && !band_of(5200e6, band), "band mapping");

    const double cycle = 7.44, overhead = 0.108;   // default schedule: 12 visits of 0.512 s
    {
        Followup f;
        require(f.plan(0, cycle, overhead).empty(), "nothing is planned without a sighting");
        f.link_seen({2437e6, 1, ""});              // Wi-Fi timing only: not a drone link
        f.link_seen({868e6, 1, "15kHz"});          // outside both bands
        require(f.plan(2, cycle, overhead).empty() && f.status().bands[0].requests == 0, "non-drone sightings open nothing");

        f.link_seen({2437e6, 10, "30kHz"});
        auto visits = f.plan(10, cycle, overhead);
        require(f.status().bands[0].open && f.status().bands[0].group == "30kHz" && f.status().bands[0].requests == 1, "request opened");
        require(visits.size() == 2, "two 1 s visits fit a quarter share of a 7.4 s cycle");
        require(visits[0].channel_hz == 2399.5e6 && visits[1].channel_hz == 2414.5e6 && visits[0].center_hz == 2401e6 &&
                visits[0].dwell_s == 1.0 && visits[0].band == Band::B24 && visits[0].label == "DroneID follow-up 2399.5 MHz",
                "visits rotate through the band's channels, offset from the capture centre");

        // The share holds over a long run, for long and short cycles alike.
        for (const double cyc : {7.44, 0.5, 30.0}) {
            Config lasting; lasting.max_request_s = 3600;   // keep one request open for the whole run
            Followup g(lasting);
            double extra = 0, ordinary = 0, now = 0;
            for (int i = 0; i < 400; ++i) {
                g.link_seen({5745e6, now, "15kHz"});
                const auto v = g.plan(now, cyc, overhead);
                extra += double(v.size()) * (1.0 + overhead);
                ordinary += cyc;
                now += cyc + double(v.size()) * (1.0 + overhead);
            }
            // Very long cycles are limited further by the unspent-allowance bound.
            require(extra > 0 && extra / (extra + ordinary) <= 0.25 + 1e-9 && (cyc > 20 || extra / (extra + ordinary) > 0.2),
                    "follow-up time is close to, and never above, its share");
        }

        // Every channel of the band is visited, in order, then again.
        std::vector<double> order;
        for (const auto& v : visits) order.push_back(v.channel_hz);
        for (double now = 12; order.size() < 7; now += 8) {
            f.link_seen({2437e6, now, "30kHz"});
            for (const auto& v : f.plan(now, cycle, overhead)) order.push_back(v.channel_hz);
        }
        require(order[2] == 2429.5e6 && order[3] == 2444.5e6 && order[4] == 2459.5e6 && order[5] == 2399.5e6, "channel rotation wraps");
    }
    {   // A decoded packet closes the request and suppresses reopening for the cooldown.
        Followup f;
        f.link_seen({2437e6, 10, "15kHz"});
        require(!f.plan(10, cycle, overhead).empty(), "open");
        f.packet_seen({2414.5e6, 12, false});      // auxiliary message: no identity, keep looking
        require(f.status().bands[0].open, "a packet without identity does not close the request");
        f.packet_seen({2414.5e6, 13, true});
        require(!f.status().bands[0].open && f.status().bands[0].last_outcome == "packet decoded" && f.status().bands[0].packets == 1, "closed by packet");
        f.link_seen({2437e6, 20, "15kHz"});
        require(f.plan(20, cycle, overhead).empty() && f.status().bands[0].suppressed == 1 && f.status().credit_s == 0,
                "no reopening, and no allowance kept, during the cooldown");
        f.link_seen({2437e6, 80, "15kHz"});
        require(!f.plan(80, cycle, overhead).empty() && f.status().bands[0].requests == 2, "reopens after the cooldown");
        // The other band is independent.
        f.packet_seen({5756.5e6, 81, true});
        require(f.status().bands[0].open && f.status().bands[1].packets == 1, "bands are independent");
    }
    {   // Link no longer seen, and timeout.
        Followup f;
        f.link_seen({5745e6, 0, "15kHz"});
        auto v = f.plan(1, cycle, overhead);
        require(!v.empty() && v[0].band == Band::B58 && v[0].channel_hz == 5756.5e6, "5.8 GHz request uses the 5.8 GHz channels");
        require(f.plan(31, cycle, overhead).empty() && f.status().bands[1].last_outcome == "link no longer seen" &&
                f.status().bands[1].link_lost == 1, "request ends when the link is not seen again");
        f.link_seen({5745e6, 40, "15kHz"});
        for (double now = 40; now <= 160; now += 10) { f.link_seen({5745e6, now, "15kHz"}); f.plan(now, cycle, overhead); }
        require(f.plan(161, cycle, overhead).empty() && f.status().bands[1].last_outcome == "timed out" && f.status().bands[1].timeouts == 1,
                "a request that finds nothing gives up");
        f.link_seen({5745e6, 170, "15kHz"});
        require(f.plan(170, cycle, overhead).empty(), "and stays closed for the cooldown");
    }
    {   // Both bands open: visits alternate.
        Followup f;
        f.link_seen({2437e6, 0, "15kHz"});
        f.link_seen({5745e6, 0, "15kHz"});
        const auto v = f.plan(0, cycle, overhead);
        require(v.size() == 2 && v[0].band != v[1].band, "open bands share the follow-up time");
    }
    {   // Pull sources: each sighting acts once; packets win over links from the same poll.
        Followup f;
        std::vector<LinkSighting> links{{2437e6, 5, "15kHz"}};
        std::vector<PacketSighting> packets;
        f.set_link_source([&] { return links; });
        f.set_packet_source([&] { return packets; });
        require(!f.plan(5, cycle, overhead).empty() && f.status().bands[0].requests == 1, "opened from the link source");
        packets.push_back({2444.5e6, 6, true});
        require(f.plan(7, cycle, overhead).empty() && f.status().bands[0].packets == 1, "closed from the packet source");
        require(f.plan(8, cycle, overhead).empty() && f.status().bands[0].packets == 1 && f.status().bands[0].requests == 1,
                "repeated history is not counted twice");
        f.set_link_source([]() -> std::vector<LinkSighting> { throw std::runtime_error("source failed"); });
        require(f.plan(9, cycle, overhead).empty(), "a failing source cannot stop the scanner");
    }
    {   // Capture marking and the enable switch.
        Followup f;
        f.link_seen({2437e6, 0, "15kHz"});
        const auto v = f.plan(0, cycle, overhead);
        f.visit_done(v[0], 41, true);
        f.visit_done(v[1], 0, false);
        require(f.is_followup_capture(41) && !f.is_followup_capture(42) && !f.is_followup_capture(0), "capture marking");
        require(f.status().bands[0].visits == 2 && f.status().bands[0].valid_visits == 1 && f.status().planned == 2, "visit counters");
        for (uint64_t seq = 100; seq < 140; ++seq) f.visit_done(v[0], seq, true);
        require(!f.is_followup_capture(41) && f.is_followup_capture(139), "capture marks are bounded");
        f.set_enabled(false);
        f.link_seen({2437e6, 1, "15kHz"});
        require(f.plan(1, cycle, overhead).empty() && !f.status().bands[0].open, "disabled: nothing opens or is planned");
    }
    std::cout << "droneid follow-up queue passed\n";
} catch (const std::exception& e) {
    std::cerr << "FAILED: " << e.what() << "\n";
    return 1;
}
