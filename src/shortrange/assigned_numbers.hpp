// Bluetooth SIG assigned numbers used to label decoded advertising fields.
// Lookups return nullptr for values the generated table does not know.
#pragma once
#include <cstdint>

namespace rfmon::shortrange {
struct NumberName { uint32_t value; const char* name; };
const char* company_name(uint16_t id);
const char* uuid16_name(uint16_t uuid);
const char* ad_type_name(uint8_t type);
const char* appearance_name(uint16_t value);
}  // namespace rfmon::shortrange
