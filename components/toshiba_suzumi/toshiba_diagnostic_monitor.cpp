#include <algorithm>
#include "toshiba_climate.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome {
namespace toshiba_suzumi {

static constexpr uint8_t MONITOR_FIRST_REGISTER = 0x90;
static constexpr uint8_t MONITOR_LAST_REGISTER = 0x9F;
static constexpr size_t CHUNK = 24;

void ToshibaDiagnosticMonitorUart::set_scan_enabled(bool enabled) {
  if (enabled) {
    if (this->scan_active_) return;

    this->scan_active_ = true;
    this->scan_started_ = true;

    // validate_message_ routes received frames through log_scan_packet_ only
    // while scan_request_sent_ is true. In passive mode this flag therefore
    // means "capture enabled"; no UART request is actually transmitted.
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

    ESP_LOGI(TAG, "========== TOSHIBA PASSIVE 0x9x CAPTURE STARTED ==========");
    ESP_LOGI(TAG, "capturing unsolicited registers 0x%02X-0x%02X; no requests will be sent",
             MONITOR_FIRST_REGISTER, MONITOR_LAST_REGISTER);
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

  ESP_LOGI(TAG, "========== TOSHIBA PASSIVE 0x9x CAPTURE STOPPED ==========");
  ESP_LOGI(TAG, "elapsed=%ums captured=%u ignored=%u",
           static_cast<unsigned>(elapsed),
           static_cast<unsigned>(this->monitor_matched_),
           static_cast<unsigned>(this->monitor_unrelated_));
}

bool ToshibaDiagnosticMonitorUart::extract_monitor_payload_(const std::vector<uint8_t> &raw,
                                                             int16_t reg,
                                                             std::vector<uint8_t> &payload) const {
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
  const size_t index = reg - 0x80;
  if (index < this->monitor_last_payload_.size()) {
    this->monitor_last_payload_[index] = payload;
    this->monitor_payload_seen_[index] = true;
  }
}

void ToshibaDiagnosticMonitorUart::log_timer_bank_snapshot_() const {
  ESP_LOGI(TAG, "========== TOSHIBA PASSIVE 0x9x SUMMARY ==========");
  for (uint8_t reg = MONITOR_FIRST_REGISTER; reg <= MONITOR_LAST_REGISTER; reg++) {
    const size_t index = reg - 0x80;
    if (!this->monitor_payload_seen_[index]) continue;

    const auto &payload = this->monitor_last_payload_[index];
    ESP_LOGI(TAG, "PROGRAMME SUMMARY reg=0x%02X payload_length=%u payload=[%s]",
             static_cast<unsigned>(reg), static_cast<unsigned>(payload.size()),
             format_hex_pretty(payload).c_str());
  }
}

void ToshibaDiagnosticMonitorUart::log_scan_packet_(const std::vector<uint8_t> &raw) {
  const int16_t reg = this->extract_response_register_(raw);

  if (reg < MONITOR_FIRST_REGISTER || reg > MONITOR_LAST_REGISTER) {
    this->monitor_unrelated_++;
    // Preserve normal climate parsing for unrelated traffic while passive
    // capture is armed; otherwise the monitor would swallow every response.
    this->parseResponse(raw);
    return;
  }

  const uint32_t elapsed = millis() - this->monitor_cycle_started_;
  this->monitor_matched_++;
  ESP_LOGI(TAG, "PROGRAMME PASSIVE RX seq=%u t=%ums class=0x%02X reg=0x%02X length=%u",
           static_cast<unsigned>(this->monitor_matched_),
           static_cast<unsigned>(elapsed),
           raw.size() > 3 ? static_cast<unsigned>(raw[3]) : 0U,
           static_cast<unsigned>(reg), static_cast<unsigned>(raw.size()));
  this->log_monitor_bytes_(raw, reg);
  this->log_monitor_decoded_(raw, reg);
}

void ToshibaDiagnosticMonitorUart::log_monitor_bytes_(const std::vector<uint8_t> &raw, int16_t reg) const {
  const size_t chunk_count = (raw.size() + CHUNK - 1) / CHUNK;
  for (size_t chunk = 0; chunk < chunk_count; chunk++) {
    const size_t offset = chunk * CHUNK;
    const size_t size = std::min(CHUNK, raw.size() - offset);
    ESP_LOGI(TAG, "PROGRAMME RAW reg=0x%02X chunk=%u/%u offset=%u bytes=[%s]",
             static_cast<unsigned>(reg), static_cast<unsigned>(chunk + 1),
             static_cast<unsigned>(chunk_count), static_cast<unsigned>(offset),
             format_hex_pretty(raw.data() + offset, size).c_str());
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
