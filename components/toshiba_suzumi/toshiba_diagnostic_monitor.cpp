#include <algorithm>
#include <utility>
#include "toshiba_climate.h"
#include "toshiba_model.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome {
namespace toshiba_suzumi {

static constexpr size_t CHUNK = 24;

void ToshibaClimateUart::set_detected_equipment_(const ToshibaEquipmentIdentification &equipment) {
  if (equipment.idu_model_available) {
    this->idu_model_ = equipment.idu_model;
    this->idu_family_ = equipment.idu_family;
    this->capabilities_ = equipment.capabilities;
    ESP_LOGI(TAG, "E0 IDU model: %s", equipment.idu_model.c_str());
    ESP_LOGI(TAG, "E0 IDU family: %s", indoor_unit_family_to_string(equipment.idu_family));
    if (this->idu_model_sensor_ != nullptr) this->idu_model_sensor_->publish_state(equipment.idu_model);
  } else {
    this->idu_model_.clear();
    this->idu_family_ = ToshibaIndoorUnitFamily::UNKNOWN;
    this->capabilities_ = {};
    ESP_LOGI(TAG, "E0 IDU model: unavailable (NULL/blank in IDU model field)");
    if (this->idu_model_sensor_ != nullptr) this->idu_model_sensor_->publish_state("Unavailable");
  }

  if (equipment.odu_model_available) {
    this->odu_model_ = equipment.odu_model;
    ESP_LOGI(TAG, "E0 ODU model: %s", equipment.odu_model.c_str());
    if (this->odu_model_sensor_ != nullptr) this->odu_model_sensor_->publish_state(equipment.odu_model);
  } else {
    this->odu_model_.clear();
    ESP_LOGI(TAG, "E0 ODU model: unavailable");
    if (this->odu_model_sensor_ != nullptr) this->odu_model_sensor_->publish_state("Unavailable");
  }
}

void ToshibaClimateUart::publish_special_mode_entities_(SPECIAL_MODE mode) {
  // A received F7 value is positive evidence for that function. It is not, by
  // itself, evidence that every other Toshiba function has been cancelled.
  // Therefore only STANDARD clears the complete divided state; a non-standard
  // value updates the entity that it directly identifies and leaves unrelated
  // entities untouched until Toshiba gives us evidence about their interaction.
  if (mode == SPECIAL_MODE::STANDARD) {
    if (this->eco_switch_ != nullptr) this->eco_switch_->publish_state(false);
    if (this->hi_power_switch_ != nullptr) this->hi_power_switch_->publish_state(false);
    if (this->eight_degree_heat_switch_ != nullptr) this->eight_degree_heat_switch_->publish_state(false);
    if (this->sleep_switch_ != nullptr) this->sleep_switch_->publish_state(false);
    if (this->floor_switch_ != nullptr) this->floor_switch_->publish_state(false);
    if (this->comfort_switch_ != nullptr) this->comfort_switch_->publish_state(false);
    if (this->fireplace_select_ != nullptr) this->fireplace_select_->publish_state("Off");
    if (this->outdoor_silent_select_ != nullptr) this->outdoor_silent_select_->publish_state("Off");
    return;
  }

  switch (mode) {
    case SPECIAL_MODE::HI_POWER:
      if (this->hi_power_switch_ != nullptr) this->hi_power_switch_->publish_state(true);
      break;
    case SPECIAL_MODE::ECO:
      if (this->eco_switch_ != nullptr) this->eco_switch_->publish_state(true);
      break;
    case SPECIAL_MODE::EIGHT_DEG:
      if (this->eight_degree_heat_switch_ != nullptr) this->eight_degree_heat_switch_->publish_state(true);
      break;
    case SPECIAL_MODE::SLEEP:
      if (this->sleep_switch_ != nullptr) this->sleep_switch_->publish_state(true);
      break;
    case SPECIAL_MODE::FLOOR:
      if (this->floor_switch_ != nullptr) this->floor_switch_->publish_state(true);
      break;
    case SPECIAL_MODE::COMFORT:
      if (this->comfort_switch_ != nullptr) this->comfort_switch_->publish_state(true);
      break;
    case SPECIAL_MODE::FIREPLACE_1:
      if (this->fireplace_select_ != nullptr) this->fireplace_select_->publish_state("Fireplace 1");
      break;
    case SPECIAL_MODE::FIREPLACE_2:
      if (this->fireplace_select_ != nullptr) this->fireplace_select_->publish_state("Fireplace 2");
      break;
    case SPECIAL_MODE::SILENT_1:
      if (this->outdoor_silent_select_ != nullptr) this->outdoor_silent_select_->publish_state("Silent 1");
      break;
    case SPECIAL_MODE::SILENT_2:
      if (this->outdoor_silent_select_ != nullptr) this->outdoor_silent_select_->publish_state("Silent 2");
      break;
    default:
      break;
  }
}

void ToshibaClimateUart::on_set_special_mode_switch(SPECIAL_MODE mode, bool enabled) {
  ESP_LOGD(TAG, "Setting divided Toshiba function %s to %s", SpecialModeToPreset(mode), enabled ? "ON" : "OFF");
  this->sendCmd(ToshibaCommandType::SPECIAL_MODE,
                static_cast<uint8_t>(enabled ? mode : SPECIAL_MODE::STANDARD));
}

void ToshibaClimateUart::on_set_special_mode_level(SPECIAL_MODE level_one, SPECIAL_MODE level_two,
                                                    const std::string &option_one,
                                                    const std::string &option_two,
                                                    const std::string &value) {
  SPECIAL_MODE mode = SPECIAL_MODE::STANDARD;
  if (value == option_one) {
    mode = level_one;
  } else if (value == option_two) {
    mode = level_two;
  } else if (value != "Off") {
    ESP_LOGW(TAG, "Unknown divided Toshiba level option: %s", value.c_str());
    return;
  }
  ESP_LOGD(TAG, "Setting divided Toshiba level function to %s", value.c_str());
  this->sendCmd(ToshibaCommandType::SPECIAL_MODE, static_cast<uint8_t>(mode));
}

void ToshibaSpecialModeSwitch::write_state(bool state) {
  this->parent_->on_set_special_mode_switch(this->mode_, state);
}

void ToshibaSpecialModeLevelSelect::control(const std::string &value) {
  this->parent_->on_set_special_mode_level(this->level_one_, this->level_two_,
                                           this->option_one_, this->option_two_, value);
}

void ToshibaDiagnosticMonitorUart::parseResponse(std::vector<uint8_t> raw) {
  if (raw.size() > 12 && raw[3] == 0x11 &&
      raw[12] == static_cast<uint8_t>(ToshibaCommandType::EQUIPMENT_INFO)) {
    const auto equipment = decode_equipment_identification(raw);
    if (!equipment.valid) {
      ESP_LOGW(TAG, "E0 equipment-identification packet did not match the expected class-0x11 layout");
      return;
    }
    this->set_detected_equipment_(equipment);
    return;
  }

  // Feed all forms of F7 response/publication into the divided entity layer
  // before retaining the legacy climate-preset parser for compatibility.
  SPECIAL_MODE received_mode;
  bool have_special_mode = false;
  if (raw.size() == 15 && raw[12] == static_cast<uint8_t>(ToshibaCommandType::SPECIAL_MODE)) {
    received_mode = static_cast<SPECIAL_MODE>(raw[13]);
    have_special_mode = true;
  } else if (raw.size() == 17 && raw[14] == static_cast<uint8_t>(ToshibaCommandType::SPECIAL_MODE)) {
    received_mode = static_cast<SPECIAL_MODE>(raw[15]);
    have_special_mode = true;
  }
  if (have_special_mode) this->publish_special_mode_entities_(received_mode);

  ToshibaClimateUart::parseResponse(std::move(raw));
}

void ToshibaDiagnosticMonitorUart::set_scan_enabled(bool enabled) {
  if (enabled) {
    if (this->scan_active_) return;

    this->scan_active_ = true;
    this->scan_started_ = true;
    this->scan_request_sent_ = true;
    this->scan_matched_response_ = false;
    this->monitor_stop_requested_ = false;
    this->monitor_waiting_for_cycle_ = false;
    this->monitor_cycle_started_ = millis();
    this->monitor_requests_ = 0;
    this->monitor_matched_ = 0;
    this->monitor_timeouts_ = 0;
    this->monitor_unrelated_ = 0;
    this->monitor_cycles_completed_ = 0;
    this->monitor_payload_seen_.fill(false);

    ESP_LOGI(TAG, "========== TOSHIBA PASSIVE RAW UART CAPTURE STARTED ==========");
    ESP_LOGI(TAG, "capturing every checksum-valid assembled RX frame; no requests will be sent");
    return;
  }

  if (this->scan_active_) this->finish_monitor_();
}

void ToshibaDiagnosticMonitorUart::process_scan_() {
  // Passive monitor: deliberately send nothing and remain armed until the
  // Home Assistant switch is turned off.
}

void ToshibaDiagnosticMonitorUart::send_monitor_request_() {}
void ToshibaDiagnosticMonitorUart::complete_monitor_request_() {}

void ToshibaDiagnosticMonitorUart::finish_monitor_() {
  if (!this->scan_active_) return;

  const uint32_t elapsed = millis() - this->monitor_cycle_started_;
  this->log_timer_bank_snapshot_();
  this->scan_active_ = false;
  this->scan_started_ = false;
  this->scan_request_sent_ = false;
  this->scan_matched_response_ = false;
  this->monitor_stop_requested_ = false;
  this->monitor_waiting_for_cycle_ = false;

  ESP_LOGI(TAG, "========== TOSHIBA PASSIVE RAW UART CAPTURE STOPPED ==========");
  ESP_LOGI(TAG, "elapsed=%ums captured=%u", static_cast<unsigned>(elapsed),
           static_cast<unsigned>(this->monitor_matched_));
}

bool ToshibaDiagnosticMonitorUart::extract_monitor_payload_(const std::vector<uint8_t> &raw,
                                                             int16_t reg,
                                                             std::vector<uint8_t> &payload) const {
  if (reg < 0) return false;

  size_t offset;
  if ((raw.size() == 15 || raw.size() == 22) && raw.size() > 12 && raw[12] == reg) {
    offset = 12;
  } else if (raw.size() > 14 && raw[3] == 0x90 && raw[14] == reg) {
    offset = 14;
  } else if (raw.size() > 12 && raw[12] == reg) {
    offset = 12;
  } else {
    return false;
  }

  if (offset + 1 > raw.size() - 1) return false;
  payload.assign(raw.begin() + offset + 1, raw.end() - 1);
  return true;
}

void ToshibaDiagnosticMonitorUart::remember_monitor_payload_(uint8_t reg,
                                                              const std::vector<uint8_t> &payload) {
  if (reg < 0x80) return;
  const size_t index = reg - 0x80;
  if (index < this->monitor_last_payload_.size()) {
    this->monitor_last_payload_[index] = payload;
    this->monitor_payload_seen_[index] = true;
  }
}

void ToshibaDiagnosticMonitorUart::log_timer_bank_snapshot_() const {
  ESP_LOGI(TAG, "========== TOSHIBA PASSIVE RAW UART SUMMARY ==========");
  for (uint16_t reg = 0x80; reg <= 0xFF; reg++) {
    const size_t index = reg - 0x80;
    if (!this->monitor_payload_seen_[index]) continue;
    const auto &payload = this->monitor_last_payload_[index];
    ESP_LOGI(TAG, "UART SUMMARY reg=0x%02X payload_length=%u payload=[%s]",
             static_cast<unsigned>(reg), static_cast<unsigned>(payload.size()),
             format_hex_pretty(payload).c_str());
  }
}

void ToshibaDiagnosticMonitorUart::log_scan_packet_(const std::vector<uint8_t> &raw) {
  const int16_t reg = this->extract_response_register_(raw);
  const uint32_t elapsed = millis() - this->monitor_cycle_started_;
  this->monitor_matched_++;

  if (reg >= 0) {
    ESP_LOGI(TAG, "UART PASSIVE RX seq=%u t=%ums class=0x%02X reg=0x%02X length=%u checksum=OK",
             static_cast<unsigned>(this->monitor_matched_), static_cast<unsigned>(elapsed),
             raw.size() > 3 ? static_cast<unsigned>(raw[3]) : 0U,
             static_cast<unsigned>(reg), static_cast<unsigned>(raw.size()));
  } else {
    ESP_LOGI(TAG, "UART PASSIVE RX seq=%u t=%ums class=0x%02X reg=unknown length=%u checksum=OK",
             static_cast<unsigned>(this->monitor_matched_), static_cast<unsigned>(elapsed),
             raw.size() > 3 ? static_cast<unsigned>(raw[3]) : 0U,
             static_cast<unsigned>(raw.size()));
  }

  this->log_monitor_bytes_(raw, reg);
  this->log_monitor_decoded_(raw, reg);
  this->parseResponse(raw);
}

void ToshibaDiagnosticMonitorUart::log_monitor_bytes_(const std::vector<uint8_t> &raw, int16_t reg) const {
  const size_t chunk_count = (raw.size() + CHUNK - 1) / CHUNK;
  for (size_t chunk = 0; chunk < chunk_count; chunk++) {
    const size_t offset = chunk * CHUNK;
    const size_t size = std::min(CHUNK, raw.size() - offset);

    if (reg >= 0) {
      ESP_LOGI(TAG, "UART RAW RX reg=0x%02X chunk=%u/%u offset=%u bytes=[%s]",
               static_cast<unsigned>(reg), static_cast<unsigned>(chunk + 1),
               static_cast<unsigned>(chunk_count), static_cast<unsigned>(offset),
               format_hex_pretty(raw.data() + offset, size).c_str());
    } else {
      ESP_LOGI(TAG, "UART RAW RX reg=unknown chunk=%u/%u offset=%u bytes=[%s]",
               static_cast<unsigned>(chunk + 1), static_cast<unsigned>(chunk_count),
               static_cast<unsigned>(offset), format_hex_pretty(raw.data() + offset, size).c_str());
    }
  }
}

void ToshibaDiagnosticMonitorUart::log_monitor_decoded_(const std::vector<uint8_t> &raw, int16_t reg) {
  std::vector<uint8_t> payload;
  if (this->extract_monitor_payload_(raw, reg, payload)) {
    this->remember_monitor_payload_(static_cast<uint8_t>(reg), payload);
  }
}

}  // namespace toshiba_suzumi
}  // namespace esphome
