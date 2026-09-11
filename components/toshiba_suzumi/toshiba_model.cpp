#include "toshiba_model.h"

#include <algorithm>
#include <cctype>

namespace esphome {
namespace toshiba_suzumi {

namespace {

std::string decode_ascii_field(const std::vector<uint8_t> &raw_data, size_t offset, size_t width) {
  if (offset + width > raw_data.size()) {
    return {};
  }

  std::string value;
  value.reserve(width);
  for (size_t i = 0; i < width; i++) {
    const uint8_t byte = raw_data[offset + i];
    if (byte == 0x00 || byte == 0xFF) {
      break;
    }
    if (byte < 0x20 || byte > 0x7E) {
      break;
    }
    value.push_back(static_cast<char>(byte));
  }

  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
    value.pop_back();
  }
  return value;
}

bool is_model_field_available(const std::string &value) {
  return !value.empty() && value != "NULL" && value.rfind("RAS-", 0) == 0;
}

}  // namespace

ToshibaIndoorUnitFamily indoor_unit_family_from_model(const std::string &model) {
  if (model.find("J2FVG") != std::string::npos) {
    return ToshibaIndoorUnitFamily::J2FVG;
  }
  if (model.find("G3KVSG") != std::string::npos) {
    return ToshibaIndoorUnitFamily::G3KVSG;
  }
  return ToshibaIndoorUnitFamily::UNKNOWN;
}

const char *indoor_unit_family_to_string(ToshibaIndoorUnitFamily family) {
  switch (family) {
    case ToshibaIndoorUnitFamily::J2FVG:
      return "J2FVG";
    case ToshibaIndoorUnitFamily::G3KVSG:
      return "G3KVSG";
    default:
      return "UNKNOWN";
  }
}

ToshibaEquipmentIdentification decode_equipment_identification(const std::vector<uint8_t> &raw_data) {
  ToshibaEquipmentIdentification result;

  // Captured equipment-identification publication:
  //   02 00 03 11 .. .. 6A 01 30 01 00 65 E0 [100-byte payload] checksum
  // The 100-byte payload consists of two 50-byte equipment records:
  //   +0   model field, 21 bytes (ASCII then NUL padding)
  //   +21  identifier field, 13 bytes (8 ASCII then NUL padding)
  //   +34  identifier field, 9 bytes  (8 ASCII then NUL)
  //   +43  identifier field, 7 bytes  (6 ASCII then NUL)
  // First record = IDU, second record = ODU.
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
  } else {
    result.idu_model.clear();
  }

  if (!result.odu_model_available) {
    result.odu_model.clear();
  }

  return result;
}

}  // namespace toshiba_suzumi
}  // namespace esphome
