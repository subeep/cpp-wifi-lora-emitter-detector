// CSV / JSON export of LoRa tab tables. Pure formatting plus one file write.
#pragma once
#include <string>
#include <vector>

#include "lora_master.hpp"
#include "lora_observation.hpp"
#include "security/lora_rules.hpp"

namespace rfmon::lora_export {

std::string packets_csv(const std::vector<LoraPacketRow>& rows);
std::string packets_json(const std::vector<LoraPacketRow>& rows);
std::string devices_csv(const std::vector<LoraDeviceRow>& rows);
std::string emitters_csv(const std::vector<lora_master::LoraMasterRow>& rows);
std::string history_csv(const std::vector<lora_master::LoraMasterReading>& readings);
std::string incidents_csv(const std::vector<lora_rules::Incident>& incidents);

// Default export folder: <project>/data/lora_exports when built with
// PROJECT_ROOT_DIR, else ./lora_exports.
std::string default_directory();

// Writes `content` to <directory>/<stem>-<YYYYmmdd-HHMMSS>.<ext>, creating
// the directory. Returns the path; throws std::runtime_error on failure.
std::string write(const std::string& directory, const std::string& stem, const std::string& ext,
                  const std::string& content);

}  // namespace rfmon::lora_export
