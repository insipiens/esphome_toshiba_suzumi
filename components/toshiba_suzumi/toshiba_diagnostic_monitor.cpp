#include <algorithm>
#include "toshiba_climate.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome {
namespace toshiba_suzumi {

static constexpr uint8_t MONITOR_FIRST_REGISTER = 0x90;
static constexpr uint8_t MONITOR_LAST_REGISTER = 0x9F;
static constexpr uint32_t MONITOR_RESPONSE_TIMEOUT_MS = 1500;
static constexpr uint32_t MONITOR_QUIET_PERIOD_MS = 250;
static constexpr size_t CHUNK = 24;

void ToshibaDiagnosticMonitorUart::set_scan_enabled(bool enabled) {
  if (enabled) {
    if (this->scan_active_) return;

    this->scan_active_ = true;
    this->scan_started_ = true;
    this->scan_request_sent_ = false;
    this->scan_matched_response_ = false;
    this->monitor_stop_requested_ = false;
    this->monitor_waiting_for_cycle_ = false;
    this->monitor_register_index_ = 0;
    this->monitor_cycle_started_ = millis();
    this->scan_last_packet_timestamp_ = millis();
    this->monitor_requests_ = 0;
    this->monitor_matched_ = 0;
    this->monitor_timeouts_ = 0;
    this->monitor_unrelated_ = 0;
    this->monitor_cycles_completed_ = 0;
    this->monitor_payload_seen_.fill(false);

    ESP_LOGI(TAG, "========== TOSHIBA 0x9x REGISTER CAPTURE STARTED ==========");
    ESP_LOGI(TAG, "registers=0x%02X-0x%02X response_timeout=%ums quiet_period=%ums",
             MONITOR_FIRST_REGISTER, MONITOR_LAST_REGISTER,
             static_cast<unsigned>(MONITOR_RESPONSE_TIMEOUT_MS),
             static_cast<unsigned>(MONITOR_QUIET_PERIOD_MS));
    return;
  }

  if (this->scan_active_) {
    this->monitor_stop_requested_ = true;
    if (!this->scan_request_sent_) this->finish_monitor_();
  }
}

void ToshibaDiagnosticMonitorUart::process_scan_() {
  if (!this->scan_active_) return;

  const uint32_t now = millis();
  if (this->monitor_stop_requested_ && !this->scan_request_sent_) {
    this->finish_monitor_();
    return;
  }

  if (!this->scan_request_sent_) {
    if (!this->rx_message_.empty()) return;
    if (now - this->scan_last_packet_timestamp_ < MONITOR_QUIET_PERIOD_MS) return;
    this->send_monitor_request_();
    return;
  }

  if (now - this->scan_register_started_ >= MONITOR_RESPONSE_TIMEOUT_MS) {
    ESP_LOGW(TAG, "PROGRAMME TIMEOUT reg=0x%02X after=%ums",
             static_cast<unsigned>(this->scan_register_),
             static_cast<unsigned>(now - this->scan_register_started_));
    this->monitor_timeouts_++;
    this->complete_monitor_request_();
  }
}

void ToshibaDiagnosticMonitorUart::send_monitor_request_() {
  this->scan_register_ = static_cast<uint8_t>(MONITOR_FIRST_REGISTER + this->monitor_register_index_);

  std::vector<uint8_t> frame = {0x02, 0x00, 0x03, 0x10, 0x00, 0x00, 0x06,
                                0x01, 0x30, 0x01, 0x00, 0x01, this->scan_register_};
  frame.push_back(checksum(frame, frame.size()));

  this->scan_request_sent_ = true;
  this->scan_matched_response_ = false;
  this->scan_register_started_ = millis();
  this->monitor_requests_++;

  ESP_LOGI(TAG, "PROGRAMME REQUEST seq=%u reg=0x%02X bytes=[%s]",
           static_cast<unsigned>(this->monitor_requests_),
           static_cast<unsigned>(this->scan_register_),
           format_hex_pretty(frame).c_str());
  this->send_to_uart(ToshibaCommand{
      .cmd = static_cast<ToshibaCommandType>(this->scan_register_),
      .payload = frame,
  });
}

void ToshibaDiagnosticMonitorUart::complete_monitor_request_() {
  this->scan_request_sent_ = false;
  this->scan_matched_response_ = false;
  this->scan_last_packet_timestamp_ = millis();

  if (this->monitor_stop_requested_) {
    this->finish_monitor_();
    return;
  }

  const uint8_t register_count = MONITOR_LAST_REGISTER - MONITOR_FIRST_REGISTER + 1;
  this->monitor_register_index_++;
  if (this->monitor_register_index_ >= register_count) {
    this->monitor_cycles_completed_++;
    this->log_timer_bank_snapshot_();
    this->finish_monitor_();
  }
}

void ToshibaDiagnosticMonitorUart::finish_monitor_() {
  if (!this->scan_active_) return;

  const uint32_t elapsed = millis() - this->monitor_cycle_started_;
  this->scan_active_ = false;
  this->scan_started_ = false;
  this->scan_request_sent_ = false;
  this->scan_matched_response_ = false;
  this->monitor_stop_requested_ = false;
  this->monitor_waiting_for_cycle_ = false;

  ESP_LOGI(TAG, "========== TOSHIBA 0x9x REGISTER CAPTURE STOPPED ==========");
  ESP_LOGI(TAG, "elapsed=%ums requests=%u matched=%u timeouts=%u unrelated=%u cycles=%u",
           static_cast<unsigned>(elapsed),
           static_cast<unsigned>(this->monitor_requests_),
           static_cast<unsigned>(this->monitor_matched_),
           static_cast<unsigned>(this->monitor_timeouts_),
           static_cast<unsigned>(this->monitor_unrelated_),
           static_cast<unsigned>(this->monitor_cycles_completed_));
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
  ESP_LOGI(TAG, "========== TOSHIBA 0x9x REGISTER SUMMARY ==========");
  for (uint8_t reg = MONITOR_FIRST_REGISTER; reg <= MONITOR_LAST_REGISTER; reg++) {
    const size_t index = reg - 0x80;
    if (!this->monitor_payload_seen_[index]) {
      ESP_LOGI(TAG, "PROGRAMME SUMMARY reg=0x%02X response=NONE", static_cast<unsigned>(reg));
      continue;
    }

    const auto &payload = this->monitor_last_payload_[index];
    ESP_LOGI(TAG, "PROGRAMME SUMMARY reg=0x%02X payload_length=%u payload=[%s]",
             static_cast<unsigned>(reg), static_cast<unsigned>(payload.size()),
             format_hex_pretty(payload).c_str());
  }
}

void ToshibaDiagnosticMonitorUart::log_scan_packet_(const std::vector<uint8_t> &raw) {
  const int16_t reg = this->extract_response_register_(raw);
  this->scan_last_packet_timestamp_ = millis();

  if (reg < MONITOR_FIRST_REGISTER || reg > MONITOR_LAST_REGISTER) {
    this->monitor_unrelated_++;
    ESP_LOGD(TAG, "PROGRAMME unrelated response reg=%s length=%u",
             reg >= 0 ? str_sprintf("0x%02X", reg).c_str() : "unknown",
             static_cast<unsigned>(raw.size()));
    return;
  }

  const uint32_t elapsed = millis() - this->monitor_cycle_started_;
  ESP_LOGI(TAG, "PROGRAMME RX t=%ums reg=0x%02X requested=0x%02X length=%u",
           static_cast<unsigned>(elapsed), static_cast<unsigned>(reg),
           static_cast<unsigned>(this->scan_register_), static_cast<unsigned>(raw.size()));
  this->log_monitor_bytes_(raw, reg);
  this->log_monitor_decoded_(raw, reg);

  if (this->scan_request_sent_ && reg == this->scan_register_) {
    this->scan_matched_response_ = true;
    this->monitor_matched_++;
    this->complete_monitor_request_();
  }
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
