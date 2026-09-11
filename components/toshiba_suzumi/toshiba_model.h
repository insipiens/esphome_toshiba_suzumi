#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace esphome {
namespace toshiba_suzumi {

enum class ToshibaIndoorUnitFamily : uint8_t {
  UNKNOWN = 0,
  J2FVG,
  G3KVSG,
  P2KVSG,
};

enum ToshibaFeature : uint32_t {
  FEATURE_NONE = 0,
  FEATURE_COMMON_HVAC = 1UL << 0,
  FEATURE_ECO = 1UL << 1,
  FEATURE_HI_POWER = 1UL << 2,
  FEATURE_COMFORT_SLEEP = 1UL << 3,  // separate 0x94 function; relationship to F7 Comfort still to be tested
  FEATURE_POWER_SELECT = 1UL << 4,
  FEATURE_OUTDOOR_SILENT = 1UL << 5,
  FEATURE_FIREPLACE = 1UL << 6,
  FEATURE_EIGHT_DEG_HEAT = 1UL << 7,
  FEATURE_VERTICAL_AIRFLOW = 1UL << 8,
  FEATURE_HORIZONTAL_AIRFLOW = 1UL << 9,
  FEATURE_FLOOR = 1UL << 10,
  FEATURE_AIR_OUTLET_SELECT = 1UL << 11,
  FEATURE_HADA_CARE = 1UL << 12,
  FEATURE_SLEEP = 1UL << 13,         // F7 Sleep
  FEATURE_COMFORT = 1UL << 14,       // F7 Comfort
};

struct ToshibaCapabilityProfile {
  uint32_t features{FEATURE_NONE};

  bool has(ToshibaFeature feature) const {
    return (features & static_cast<uint32_t>(feature)) != 0;
  }
};

struct ToshibaEquipmentIdentification {
  bool valid{false};
  bool idu_model_available{false};
  bool odu_model_available{false};
  std::string idu_model;
  std::string odu_model;
  ToshibaIndoorUnitFamily idu_family{ToshibaIndoorUnitFamily::UNKNOWN};
  ToshibaCapabilityProfile capabilities;
};

/**
 * Decode pushed class-0x11 register-0xE0 equipment identification.
 *
 * Captured layout:
 *   byte 12      0xE0
 *   bytes 13-62  50-byte IDU record
 *   bytes 63-112 50-byte ODU record
 *   byte 113     checksum
 *
 * Each 50-byte record is:
 *   +0..20   model field (21 bytes, ASCII then NUL padding)
 *   +21..33  identifier field (13 bytes; meaning unresolved)
 *   +34..42  identifier field (9 bytes; meaning unresolved)
 *   +43..49  identifier field (7 bytes; meaning unresolved)
 *
 * A literal NULL/blank IDU model is retained as an unavailable-model anomaly;
 * the ODU model is never substituted for it.
 */
ToshibaEquipmentIdentification decode_equipment_identification(const std::vector<uint8_t> &raw_data);

ToshibaIndoorUnitFamily indoor_unit_family_from_model(const std::string &model);
const char *indoor_unit_family_to_string(ToshibaIndoorUnitFamily family);
ToshibaCapabilityProfile capability_profile_from_model(const std::string &model);

}  // namespace toshiba_suzumi
}  // namespace esphome
