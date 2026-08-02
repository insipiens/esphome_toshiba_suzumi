#include <algorithm>
#include "toshiba_climate.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome {
namespace toshiba_suzumi {

// Passive capture: transmit nothing. Once a 247-byte 0x99 programme frame is
// observed, continue capturing for five seconds and then stop automatically.
static constexpr uint32_t POST_99_CAPTURE_MS = 5000;
static constexpr size_t CHUNK = 24;

static bool programme_99_seen_ = false;
static uint32_t programme_99_seen_at_ = 0;
static uint32_t captured_frames_ = 0;

void ToshibaDiagnosticMonitorUart::set_scan_enabled(bool enabled) {
  if (enabled) {
    if (this->scan_active_) return;
    this->scan_active_ = true;
    this->scan_started_ = true;
    this->scan_request_sent_ = false;
    this->scan_matched_response_ = false;
    this->monitor_stop_requested_ = false;
    this->monitor_waiting_for_cycle_ = false;
    this->monitor_cycle_started_ = millis();
    this->monitor_requests_ = 0;
    this->monitor_matched_ = 0;
    this->monitor_timeouts_ = 0;
    this->monitor_unrelated_ = 0;
    this->monitor_cycles_completed_ = 0;
    programme_99_seen_ = false;
    programme_99_seen_at_ = 0;
    captured_frames_ = 0;
    ESP_LOGI(TAG, "========== TOSHIBA PASSIVE PROGRAMME CAPTURE ARMED ==========");
    ESP_LOGI(TAG, "no_transmit=YES; press PROGRAMME SET on remote; auto-stop=5s after 247-byte 0x99");
    return;
  }

  if (!this->scan_active_) return;
  this->finish_monitor_();
}

void ToshibaDiagnosticMonitorUart::process_scan_() {
  if (!this->scan_active_) return;
  if (programme_99_seen_ && millis() - programme_99_seen_at_ >= POST_99_CAPTURE_MS)
    this->finish_monitor_();
}

// Passive monitor: these request-oriented methods are deliberately inert.
void ToshibaDiagnosticMonitorUart::send_monitor_request_() {}
void ToshibaDiagnosticMonitorUart::complete_monitor_request_() {}

void ToshibaDiagnosticMonitorUart::finish_monitor_() {
  if (!this->scan_active_) return;
  const uint32_t elapsed = millis() - this->monitor_cycle_started_;
  this->scan_active_ = false;
  this->scan_started_ = false;
  this->scan_request_sent_ = false;
  this->scan_matched_response_ = false;
  this->monitor_stop_requested_ = false;
  this->monitor_waiting_for_cycle_ = false;
  ESP_LOGI(TAG, "========== TOSHIBA PASSIVE PROGRAMME CAPTURE STOPPED ==========");
  ESP_LOGI(TAG, "elapsed=%ums frames=%u programme_0x99_seen=%s",
           static_cast<unsigned>(elapsed), static_cast<unsigned>(captured_frames_),
           programme_99_seen_ ? "YES" : "NO");
}

bool ToshibaDiagnosticMonitorUart::extract_monitor_payload_(const std::vector<uint8_t> &raw,
                                                             int16_t reg,
                                                             std::vector<uint8_t> &payload) const {
  size_t o;
  if ((raw.size() == 15 || raw.size() == 22) && raw.size() > 12 && raw[12] == reg) o = 12;
  else if (raw.size() > 14 && raw[3] == 0x90 && raw[14] == reg) o = 14;
  else if (raw.size() > 12 && raw[12] == reg) o = 12;
  else return false;
  if (o + 1 > raw.size() - 1) return false;
  payload.assign(raw.begin() + o + 1, raw.end() - 1);
  return true;
}

void ToshibaDiagnosticMonitorUart::remember_monitor_payload_(uint8_t reg,
                                                              const std::vector<uint8_t> &payload) {
  const size_t i = reg - 0x80;
  if (i < this->monitor_last_payload_.size()) {
    this->monitor_last_payload_[i] = payload;
    this->monitor_payload_seen_[i] = true;
  }
}

void ToshibaDiagnosticMonitorUart::log_timer_bank_snapshot_() const {}

void ToshibaDiagnosticMonitorUart::log_scan_packet_(const std::vector<uint8_t> &raw) {
  const int16_t reg = this->extract_response_register_(raw);
  const uint32_t t = millis() - this->monitor_cycle_started_;
  captured_frames_++;

  ESP_LOGI(TAG, "CAPTURE FRAME seq=%u t=%ums class=%s reg=%s length=%u",
           static_cast<unsigned>(captured_frames_), static_cast<unsigned>(t),
           raw.size() > 3 ? str_sprintf("0x%02X", raw[3]).c_str() : "unknown",
           reg >= 0 ? str_sprintf("0x%02X", reg).c_str() : "unknown",
           static_cast<unsigned>(raw.size()));
  this->log_monitor_bytes_(raw, reg);

  if (reg == 0x99 && raw.size() == 247) {
    programme_99_seen_ = true;
    programme_99_seen_at_ = millis();
    ESP_LOGI(TAG, "CAPTURE PROGRAMME 0x99 detected t=%ums; capturing another %ums",
             static_cast<unsigned>(t), static_cast<unsigned>(POST_99_CAPTURE_MS));
  }
}

void ToshibaDiagnosticMonitorUart::log_monitor_bytes_(const std::vector<uint8_t> &raw, int16_t reg) const {
  const size_t n = (raw.size() + CHUNK - 1) / CHUNK;
  for (size_t c = 0; c < n; c++) {
    const size_t o = c * CHUNK;
    const size_t z = std::min(CHUNK, raw.size() - o);
    ESP_LOGI(TAG, "CAPTURE RAW reg=%s chunk=%u/%u offset=%u bytes=[%s]",
             reg >= 0 ? str_sprintf("0x%02X", reg).c_str() : "unknown",
             static_cast<unsigned>(c + 1), static_cast<unsigned>(n),
             static_cast<unsigned>(o), format_hex_pretty(raw.data() + o, z).c_str());
  }
}

void ToshibaDiagnosticMonitorUart::log_monitor_decoded_(const std::vector<uint8_t> &raw, int16_t reg) {
  std::vector<uint8_t> payload;
  if (reg >= 0 && this->extract_monitor_payload_(raw, reg, payload))
    this->remember_monitor_payload_(static_cast<uint8_t>(reg), payload);
}

}  // namespace toshiba_suzumi
}  // namespace esphome
