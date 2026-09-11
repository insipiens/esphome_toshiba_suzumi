#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace esphome {
namespace toshiba_suzumi {

/**
 * Indoor-unit families currently needed by the control-matrix redesign.
 *
 * These are derived from the IDU model field in the pushed 0xE0 equipment
 * identification message. Unknown models deliberately remain UNKNOWN so that
 * the component can fall back conservatively instead of inventing features.
 */
enum class ToshibaIndoorUnitFamily : uint8_t {
  UNKNOWN = 0,
  J2FVG,
  G3KVSG,
};

struct ToshibaEquipmentIdentification {
  bool valid{false};
  bool idu_model_available{false};
  bool odu_model_available{false};
  std::string idu_model;
  std::string odu_model;
  ToshibaIndoorUnitFamily idu_family{ToshibaIndoorUnitFamily::UNKNOWN};
};

/**
 * Decode the pushed class-0x11 register-0xE0 equipment-identification packet.
 *
 * Working packet layout established from captures:
 *   byte 12      register (0xE0)
 *   bytes 13-28  IDU model field (16 ASCII bytes)
 *   bytes 29-36  IDU identifier field (8 bytes, meaning unresolved)
 *   bytes 37-44  IDU identifier field (8 bytes, meaning unresolved)
 *   bytes 45-50  IDU identifier field (6 bytes, meaning unresolved)
 *   bytes 51-66  ODU model field (16 ASCII bytes)
 *
 * Some IDUs have been observed to return "NULL" in the IDU model field. That
 * is treated as model unavailable, not as evidence that 0xE0 has a different
 * meaning. The ODU model must never be promoted to the IDU model when the first
 * field is unavailable.
 */
ToshibaEquipmentIdentification decode_equipment_identification(const std::vector<uint8_t> &raw_data);

ToshibaIndoorUnitFamily indoor_unit_family_from_model(const std::string &model);
const char *indoor_unit_family_to_string(ToshibaIndoorUnitFamily family);

}  // namespace toshiba_suzumi
}  // namespace esphome
