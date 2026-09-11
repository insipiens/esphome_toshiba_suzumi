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
  FEATURE_COMFORT_SLEEP = 1UL << 3,
  FEATURE_POWER_SELECT = 1UL << 4,
  FEATURE_OUTDOOR_SILENT = 1UL << 5,
  FEATURE_FIREPLACE = 1UL << 6,
  FEATURE_EIGHT_DEG_HEAT = 1UL << 7,
  FEATURE_VERTICAL_AIRFLOW = 1UL << 8,
  FEATURE_HORIZONTAL_AIRFLOW = 1UL << 9,
  FEATURE_FLOOR = 1UL << 10,
  FEATURE_AIR_OUTLET_SELECT = 1UL << 11,
  FEATURE_HADA_CARE = 1UL << 12,
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

ToshibaEquipmentIdentification decode_equipment_identification(const std::vector<uint8_t> &raw_data);

ToshibaIndoorUnitFamily indoor_unit_family_from_model(const std::string &model);
const char *indoor_unit_family_to_string(ToshibaIndoorUnitFamily family);
ToshibaCapabilityProfile capability_profile_from_model(const std::string &model);

}  // namespace toshiba_suzumi
}  // namespace esphome
