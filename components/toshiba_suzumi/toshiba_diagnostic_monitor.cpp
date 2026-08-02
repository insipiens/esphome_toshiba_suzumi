#include <algorithm>
#include <array>
#include "toshiba_climate.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome {
namespace toshiba_suzumi {

static constexpr uint32_t SEND_DELAY_MS = 1000;
static constexpr uint32_t CAPTURE_AFTER_SEND_MS = 8000;
static constexpr size_t CHUNK = 24;

// Exact 247-byte programme frame captured at 14:05:22 on 2026-08-02.
// It contains the programme already stored in the IDU, so this is a same-data replay.
static const std::array<uint8_t, 247> REPLAY = {
  0x02,0x00,0x03,0x11,0x02,0x00,0xEF,0x01,0x30,0x01,0x00,0xEA,0x99,0x7E,0x07,0x02,0x0E,0x05,0x13,0x00,0x01,0x01,0x09,0x32,
  0x30,0x41,0x16,0x41,0x00,0xFF,0x0F,0x14,0x31,0xFF,0x16,0x41,0x00,0xFF,0x14,0x32,0x31,0xFF,0x16,0x41,0x00,0xFF,0x15,0x1E,
  0x31,0xFF,0x16,0x41,0x00,0xFF,0x09,0x32,0x30,0x41,0x16,0x41,0x00,0xFF,0x0F,0x14,0x31,0xFF,0x16,0x41,0x00,0xFF,0x14,0x32,
  0x31,0xFF,0x16,0x41,0x00,0xFF,0x15,0x1E,0x31,0xFF,0x16,0x41,0x00,0xFF,0x09,0x32,0x30,0x41,0x16,0x41,0x00,0xFF,0x0F,0x14,
  0x31,0xFF,0x16,0x41,0x00,0xFF,0x14,0x32,0x31,0xFF,0x16,0x41,0x00,0xFF,0x15,0x1E,0x31,0xFF,0x16,0x41,0x00,0xFF,0x09,0x32,
  0x30,0x41,0x16,0x41,0x00,0xFF,0x0F,0x14,0x31,0xFF,0x16,0x41,0x00,0xFF,0x14,0x32,0x31,0xFF,0x16,0x41,0x00,0xFF,0x15,0x1E,
  0x31,0xFF,0x16,0x41,0x00,0xFF,0x09,0x32,0x30,0x41,0x16,0x41,0x00,0xFF,0x0F,0x14,0x31,0xFF,0x16,0x41,0x00,0xFF,0x14,0x32,
  0x31,0xFF,0x16,0x41,0x00,0xFF,0x15,0x1E,0x31,0xFF,0x16,0x41,0x00,0xFF,0x09,0x32,0x30,0x41,0x16,0x41,0x00,0xFF,0x0F,0x14,
  0x31,0xFF,0x16,0x41,0x00,0xFF,0x14,0x32,0x31,0xFF,0x16,0x41,0x00,0xFF,0x15,0x1E,0x31,0xFF,0x16,0x41,0x00,0xFF,0x09,0x32,
  0x30,0x41,0x16,0x41,0x00,0xFF,0x0F,0x14,0x31,0xFF,0x16,0x41,0x00,0xFF,0x14,0x32,0x31,0xFF,0x16,0x41,0x00,0xFF,0x15,0x1E,
  0x31,0xFF,0x16,0x41,0x00,0xFF,0x47
};

static bool replay_sent_ = false;
static uint32_t replay_sent_at_ = 0;
static uint32_t captured_frames_ = 0;

void ToshibaDiagnosticMonitorUart::set_scan_enabled(bool enabled) {
  if (enabled) {
    if (this->scan_active_) return;
    this->scan_active_ = true;
    this->scan_started_ = true;
    this->scan_request_sent_ = true;  // route all valid RX frames to capture hook
    this->scan_matched_response_ = false;
    this->monitor_stop_requested_ = false;
    this->monitor_waiting_for_cycle_ = false;
    this->monitor_cycle_started_ = millis();
    replay_sent_ = false;
    replay_sent_at_ = 0;
    captured_frames_ = 0;
    ESP_LOGI(TAG, "========== TOSHIBA PROGRAMME REPLAY TEST ARMED ==========");
    ESP_LOGI(TAG, "exact_same_data_replay=YES send_delay=%ums capture_after_send=%ums",
             static_cast<unsigned>(SEND_DELAY_MS), static_cast<unsigned>(CAPTURE_AFTER_SEND_MS));
    return;
  }
  if (this->scan_active_) this->finish_monitor_();
}

void ToshibaDiagnosticMonitorUart::process_scan_() {
  if (!this->scan_active_) return;
  const uint32_t now = millis();

  if (!replay_sent_) {
    if (now - this->monitor_cycle_started_ < SEND_DELAY_MS || !this->rx_message_.empty()) return;
    std::vector<uint8_t> frame(REPLAY.begin(), REPLAY.end());
    replay_sent_ = true;
    replay_sent_at_ = now;
    ESP_LOGI(TAG, "REPLAY TX t=%ums length=%u checksum=0x%02X",
             static_cast<unsigned>(now - this->monitor_cycle_started_),
             static_cast<unsigned>(frame.size()), frame.back());
    this->send_to_uart(ToshibaCommand{.cmd = static_cast<ToshibaCommandType>(0x99), .payload = frame});
    return;
  }

  if (now - replay_sent_at_ >= CAPTURE_AFTER_SEND_MS) this->finish_monitor_();
}

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
  ESP_LOGI(TAG, "========== TOSHIBA PROGRAMME REPLAY TEST STOPPED ==========");
  ESP_LOGI(TAG, "elapsed=%ums replay_sent=%s rx_frames=%u",
           static_cast<unsigned>(elapsed), replay_sent_ ? "YES" : "NO",
           static_cast<unsigned>(captured_frames_));
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
  ESP_LOGI(TAG, "REPLAY RX seq=%u t=%ums class=%s reg=%s length=%u",
           static_cast<unsigned>(captured_frames_), static_cast<unsigned>(t),
           raw.size() > 3 ? str_sprintf("0x%02X", raw[3]).c_str() : "unknown",
           reg >= 0 ? str_sprintf("0x%02X", reg).c_str() : "unknown",
           static_cast<unsigned>(raw.size()));
  this->log_monitor_bytes_(raw, reg);
}

void ToshibaDiagnosticMonitorUart::log_monitor_bytes_(const std::vector<uint8_t> &raw, int16_t reg) const {
  const size_t n = (raw.size() + CHUNK - 1) / CHUNK;
  for (size_t c = 0; c < n; c++) {
    const size_t o = c * CHUNK;
    const size_t z = std::min(CHUNK, raw.size() - o);
    ESP_LOGI(TAG, "REPLAY RAW reg=%s chunk=%u/%u offset=%u bytes=[%s]",
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
