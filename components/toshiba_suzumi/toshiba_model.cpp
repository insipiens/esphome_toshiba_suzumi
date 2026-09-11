#include "toshiba_model.h"

#include <cctype>

namespace esphome {
namespace toshiba_suzumi {

namespace {

std::string decode_ascii_field(const std::vector<uint8_t> &raw_data, size_t offset, size_t width) {
  if (offset + width > raw_data.size()) return {};

  std::string value;
  value.reserve(width);
  for (size_t i = 0; i < width; i++) {
    const uint8_t byte = raw_data[offset + i];
    if (byte == 0x00 || byte == 0xFF) break;
    if (byte < 0x20 || byte > 0x7E) break;
    value.push_back(static_cast<char>(byte));
  }
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
  return value;
}

bool is_model_field_available(const std::string &value) {
  return !value.empty() && value != "NULL" && value.rfind("RAS-", 0) == 0;
}

constexpr uint32_t COMMON_RESIDENTIAL_FEATURES =
    FEATURE_COMMON_HVAC |
    FEATURE_ECO |
    FEATURE_HI_POWER |
    FEATURE_COMFORT_SLEEP |
    FEATURE_POWER_SELECT |
    FEATURE_OUTDOOR_SILENT |
    FEATURE_FIREPLACE |
    FEATURE_EIGHT_DEG_HEAT |
    FEATURE_VERTICAL_AIRFLOW |
    FEATURE_SLEEP |
    FEATURE_COMFORT;

}  // namespace

ToshibaIndoorUnitFamily indoor_unit_family_from_model(const std::string &model) {
  if (model.find("J2FVG") != std::string::npos) return ToshibaIndoorUnitFamily::J2FVG;
  if (model.find("G3KVSG") != std::string::npos) return ToshibaIndoorUnitFamily::G3KVSG;
  if (model.find("P2KVSG") != std::string::npos) return ToshibaIndoorUnitFamily::P2KVSG;
  return ToshibaIndoorUnitFamily::UNKNOWN;
}

const char *indoor_unit_family_to_string(ToshibaIndoorUnitFamily family) {
  switch (family) {
    case ToshibaIndoorUnitFamily::J2FVG: return "J2FVG";
    case ToshibaIndoorUnitFamily::G3KVSG: return "G3KVSG";
    case ToshibaIndoorUnitFamily::P2KVSG: return "P2KVSG";
    default: return "UNKNOWN";
  }
}

ToshibaCapabilityProfile capability_profile_from_model(const std::string &model) {
  ToshibaCapabilityProfile profile;
  const auto family = indoor_unit_family_from_model(model);

  // Shared controls are modelled once. Family mappings only add controls that
  // are physically specific to that indoor-unit construction/remote family.
  switch (family) {
    case ToshibaIndoorUnitFamily::J2FVG:
      profile.features = COMMON_RESIDENTIAL_FEATURES |
                         FEATURE_FLOOR |
                         FEATURE_AIR_OUTLET_SELECT;
      break;
    case ToshibaIndoorUnitFamily::G3KVSG:
      profile.features = COMMON_RESIDENTIAL_FEATURES |
                         FEATURE_HORIZONTAL_AIRFLOW |
                         FEATURE_HADA_CARE;
      break;
    case ToshibaIndoorUnitFamily::P2KVSG:
      profile.features = COMMON_RESIDENTIAL_FEATURES |
                         FEATURE_HORIZONTAL_AIRFLOW;
      break;
    default:
      profile.features = FEATURE_COMMON_HVAC;
      break;
  }

  return profile;
}

ToshibaEquipmentIdentification decode_equipment_identification(const std::vector<uint8_t> &raw_data) {
  ToshibaEquipmentIdentification result;

  // Captured equipment-identification publication:
  //   02 00 03 11 .. .. 6A 01 30 01 00 65 E0 [100-byte payload] checksum
  // Payload = two 50-byte equipment records. Each starts with a 21-byte model field.
  if (raw_data.size() < 114 || raw_data[0] != 0x02 || raw_data[2] != 0x03 || raw_data[3] != 0x11 ||
      raw_data[12] != 0xE0) {
    return result;
  }

  constexpr size_t IDU_RECORD_OFFSET = 13;
  constexpr size_t ODU_RECORD_OFFSET = IDU_RECORD_OFFSET + 50;
  constexpr size_t MODEL_FIELD_WIDTH = 21;

  result.valid = true;
  result.idu_model = decode_ascii_field(raw_data, IDU_RECORD_OFFSET, MODEL_FIELD_WIDTH);
  result.odu_model = decode_ascii_field(raw_data, ODU_RECORD_OFFSET, MODEL_FIELD_WIDTH);
  result.idu_model_available = is_model_field_available(result.idu_model);
  result.odu_model_available = is_model_field_available(result.odu_model);

  if (result.idu_model_available) {
    result.idu_family = indoor_unit_family_from_model(result.idu_model);
    result.capabilities = capability_profile_from_model(result.idu_model);
  } else {
    result.idu_model.clear();
    result.capabilities.features = FEATURE_COMMON_HVAC;
  }

  if (!result.odu_model_available) result.odu_model.clear();

  return result;
}

}  // namespace toshiba_suzumi
}  // namespace esphome
