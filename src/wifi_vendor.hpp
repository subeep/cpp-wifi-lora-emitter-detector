#pragma once
#include <optional>
#include <string>
namespace rfmon::wifi {
struct VendorInfo { std::string name; std::string source; };
std::optional<std::string> canonical_mac(const std::string& mac);
VendorInfo lookup_vendor(const std::string& mac);
}
