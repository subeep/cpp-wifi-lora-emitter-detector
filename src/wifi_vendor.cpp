#include "wifi_vendor.hpp"
#include "../third_party/ieee/wifi_vendor_snapshot.hpp"
#include <algorithm>
#include <cctype>
#include <string_view>

namespace rfmon::wifi {
namespace {
struct Entry { const char* prefix; const char* name; };
const Entry entries[] = {
#include "../third_party/ieee/wifi_vendors.inc"
};
}
std::optional<std::string> canonical_mac(const std::string& mac) {
    if (mac.size() != 17) return std::nullopt;
    std::string out = mac;
    for (size_t i = 0; i < out.size(); ++i) {
        if (i % 3 == 2) { if (out[i] != ':') return std::nullopt; }
        else {
            if (!std::isxdigit(static_cast<unsigned char>(out[i]))) return std::nullopt;
            out[i] = char(std::tolower(static_cast<unsigned char>(out[i])));
        }
    }
    return out;
}
VendorInfo lookup_vendor(const std::string& mac) {
    auto normalized = canonical_mac(mac);
    if (!normalized) return {"Unknown", "No valid decoded MAC"};
    unsigned first = unsigned(std::stoul(normalized->substr(0, 2), nullptr, 16));
    if (first & 1) return {"Not applicable", "Group address"};
    if (first & 2) return {"Locally administered", "Vendor cannot be inferred from this MAC"};
    std::string hex;
    for (char c : *normalized) if (c != ':') hex += char(std::toupper(static_cast<unsigned char>(c)));
    for (size_t length : {size_t(9), size_t(7), size_t(6)}) {
        std::string prefix = hex.substr(0, length);
        auto it = std::lower_bound(std::begin(entries), std::end(entries), prefix,
                                  [](const Entry& a, const std::string& b) { return std::string_view(a.prefix) < b; });
        if (it != std::end(entries) && prefix == it->prefix)
            return {it->name, "IEEE " + std::string(length == 9 ? "MA-S" : length == 7 ? "MA-M" : "MA-L") +
                             " /" + std::to_string(length * 4) + " " + prefix + " (snapshot " WIFI_VENDOR_SNAPSHOT_DATE ")"};
    }
    return {"Unknown", "No match in bundled IEEE registry"};
}
}
