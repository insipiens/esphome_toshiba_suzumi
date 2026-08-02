#include <algorithm>
#include <array>
#include "toshiba_climate.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome {
namespace toshiba_suzumi {

// Contiguous structural sweep around the known 0x99 weekly programme table.
static constexpr std::array<uint8_t, 32> REGS = {
    0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97,
    0x98, 0x99, 0x9A, 0x9B, 0x9C, 0x9D, 0x9E, 0x9F,
    0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7,
    0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF,
};
static constexpr uint32_t SAMPLE_INTERVAL = 20;
static constexpr uint32_t TIMEOUT = 350;
static constexpr uint32_t QUIET = 20;
static constexpr uint8_t PASSES = 3;
static constexpr size_t CHUNK = 24;

static size_t reg_index_ = 0;
static uint8_t pass_ = 0;

static uint8_t checksum_(const std::vector<uint8_t> &d) {
  uint8_t s = 0;
  for (size_t i = 1; i < d.size(); i++) s += d[i];
  return 256 - s;
}

void ToshibaDiagnosticMonitorUart::set_scan_enabled(bool enabled) {
  if (enabled) {
    if (this->scan_active_) return;
    this->scan_active_ = true;
    this->scan_started_ = false;
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
    reg_index_ = 0;
    pass_ = 1;
    this->scan_register_ = REGS[0];
    this->monitor_payload_seen_.fill(false);
    for (auto &p : this->monitor_last_payload_) p.clear();
    ESP_LOGI(TAG, "========== TOSHIBA TIMER BANK SWEEP STARTED ==========");
    ESP_LOGI(TAG, "range=0x90-0xAF registers=%u passes=%u timeout=%ums read_only=YES",
             static_cast<unsigned>(REGS.size()), static_cast<unsigned>(PASSES),
             static_cast<unsigned>(TIMEOUT));
    ESP_LOGI(TAG, "BANK PASS pass=1/%u started", static_cast<unsigned>(PASSES));
    return;
  }
  if (!this->scan_active_) return;
  this->monitor_stop_requested_ = true;
  if (!this->scan_request_sent_) this->finish_monitor_();
}

void ToshibaDiagnosticMonitorUart::process_scan_() {
  if (!this->scan_active_) return;
  const uint32_t now = millis();

  if (this->monitor_stop_requested_ && !this->scan_request_sent_) {
    this->finish_monitor_();
    return;
  }

  if (this->monitor_waiting_for_cycle_) {
    if (now - this->scan_register_started_ < SAMPLE_INTERVAL) return;
    this->monitor_waiting_for_cycle_ = false;
  }

  if (!this->scan_request_sent_) {
    if ((!this->scan_started_ && !this->command_queue_.empty()) || !this->rx_message_.empty() ||
        now - this->last_command_timestamp_ < QUIET) return;
    this->scan_register_ = REGS[reg_index_];
    this->scan_matched_response_ = false;
    this->scan_last_packet_timestamp_ = 0;
    this->scan_register_started_ = now;
    this->scan_started_ = true;
    this->scan_request_sent_ = true;
    this->send_monitor_request_();
    return;
  }

  if (this->scan_matched_response_) {
    if (now - this->scan_last_packet_timestamp_ >= QUIET) this->complete_monitor_request_();
    return;
  }
  if (now - this->scan_register_started_ >= TIMEOUT) this->complete_monitor_request_();
}

void ToshibaDiagnosticMonitorUart::send_monitor_request_() {
  const uint8_t reg = this->scan_register_;
  std::vector<uint8_t> p = {2,0,3,16,0,0,6,1,48,1,0,1,reg};
  p.push_back(checksum_(p));
  this->monitor_requests_++;
  this->send_to_uart(ToshibaCommand{.cmd = static_cast<ToshibaCommandType>(reg), .payload = p});
}

void ToshibaDiagnosticMonitorUart::complete_monitor_request_() {
  const uint8_t completed_reg = this->scan_register_;
  if (this->scan_matched_response_) {
    this->monitor_matched_++;
  } else {
    this->monitor_timeouts_++;
    ESP_LOGI(TAG, "BANK TIMEOUT pass=%u reg=0x%02X t=%ums",
             static_cast<unsigned>(pass_), completed_reg,
             static_cast<unsigned>(millis() - this->monitor_cycle_started_));
  }

  this->scan_request_sent_ = false;
  this->monitor_waiting_for_cycle_ = true;
  reg_index_++;

  if (reg_index_ >= REGS.size()) {
    reg_index_ = 0;
    this->monitor_cycles_completed_++;
    ESP_LOGI(TAG, "BANK PASS pass=%u/%u complete t=%ums requests=%u matched=%u timeouts=%u unsolicited=%u",
             static_cast<unsigned>(pass_), static_cast<unsigned>(PASSES),
             static_cast<unsigned>(millis() - this->monitor_cycle_started_),
             static_cast<unsigned>(this->monitor_requests_),
             static_cast<unsigned>(this->monitor_matched_),
             static_cast<unsigned>(this->monitor_timeouts_),
             static_cast<unsigned>(this->monitor_unrelated_));
    if (pass_ >= PASSES) {
      this->finish_monitor_();
      return;
    }
    pass_++;
    ESP_LOGI(TAG, "BANK PASS pass=%u/%u started",
             static_cast<unsigned>(pass_), static_cast<unsigned>(PASSES));
  }

  if (this->monitor_stop_requested_) this->finish_monitor_();
}

void ToshibaDiagnosticMonitorUart::finish_monitor_() {
  if (!this->scan_active_) return;
  this->scan_active_ = false;
  this->scan_started_ = false;
  this->scan_request_sent_ = false;
  this->scan_matched_response_ = false;
  this->monitor_stop_requested_ = false;
  this->monitor_waiting_for_cycle_ = false;
  this->log_timer_bank_snapshot_();
  ESP_LOGI(TAG, "========== TOSHIBA TIMER BANK SWEEP STOPPED ==========");
  ESP_LOGI(TAG, "elapsed=%ums requests=%u matched=%u timeouts=%u unsolicited=%u complete_passes=%u",
           static_cast<unsigned>(millis() - this->monitor_cycle_started_),
           static_cast<unsigned>(this->monitor_requests_),
           static_cast<unsigned>(this->monitor_matched_),
           static_cast<unsigned>(this->monitor_timeouts_),
           static_cast<unsigned>(this->monitor_unrelated_),
           static_cast<unsigned>(this->monitor_cycles_completed_));
  this->getInitData();
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
  const uint32_t t = millis() - this->monitor_cycle_started_;
  if (!this->monitor_payload_seen_[i]) {
    ESP_LOGI(TAG, "BANK VALUE pass=%u reg=0x%02X t=%ums bytes=[%s]",
             static_cast<unsigned>(pass_), reg, static_cast<unsigned>(t),
             format_hex_pretty(payload).c_str());
  } else if (this->monitor_last_payload_[i] != payload) {
    ESP_LOGI(TAG, "BANK CHANGE pass=%u reg=0x%02X t=%ums old=[%s] new=[%s]",
             static_cast<unsigned>(pass_), reg, static_cast<unsigned>(t),
             format_hex_pretty(this->monitor_last_payload_[i]).c_str(),
             format_hex_pretty(payload).c_str());
  }
  this->monitor_last_payload_[i] = payload;
  this->monitor_payload_seen_[i] = true;
}

void ToshibaDiagnosticMonitorUart::log_timer_bank_snapshot_() const {
  ESP_LOGI(TAG, "BANK FINAL SNAPSHOT begin");
  for (const uint8_t reg : REGS) {
    const size_t i = reg - 0x80;
    if (this->monitor_payload_seen_[i])
      ESP_LOGI(TAG, "BANK FINAL reg=0x%02X bytes=[%s]", reg,
               format_hex_pretty(this->monitor_last_payload_[i]).c_str());
    else
      ESP_LOGI(TAG, "BANK FINAL reg=0x%02X unavailable", reg);
  }
  ESP_LOGI(TAG, "BANK FINAL SNAPSHOT end");
}

void ToshibaDiagnosticMonitorUart::log_scan_packet_(const std::vector<uint8_t> &raw) {
  const int16_t reg = this->extract_response_register_(raw);
  this->scan_last_packet_timestamp_ = millis();
  if (reg == this->scan_register_) {
    this->scan_matched_response_ = true;
    this->log_monitor_decoded_(raw, reg);
    return;
  }
  this->monitor_unrelated_++;
  ESP_LOGI(TAG, "BANK UNSOLICITED pass=%u t=%ums while=0x%02X response=%s length=%u",
           static_cast<unsigned>(pass_),
           static_cast<unsigned>(millis() - this->monitor_cycle_started_),
           this->scan_register_,
           reg >= 0 ? str_sprintf("0x%02X", reg).c_str() : "unknown",
           static_cast<unsigned>(raw.size()));
  this->log_monitor_bytes_(raw, reg);
}

void ToshibaDiagnosticMonitorUart::log_monitor_bytes_(const std::vector<uint8_t> &raw, int16_t reg) const {
  const size_t n = (raw.size() + CHUNK - 1) / CHUNK;
  for (size_t c = 0; c < n; c++) {
    const size_t o = c * CHUNK;
    const size_t z = std::min(CHUNK, raw.size() - o);
    ESP_LOGI(TAG, "BANK RAW reg=%s chunk=%u/%u offset=%u bytes=[%s]",
             reg >= 0 ? str_sprintf("0x%02X", reg).c_str() : "unknown",
             static_cast<unsigned>(c + 1), static_cast<unsigned>(n), static_cast<unsigned>(o),
             format_hex_pretty(raw.data() + o, z).c_str());
  }
}

void ToshibaDiagnosticMonitorUart::log_monitor_decoded_(const std::vector<uint8_t> &raw, int16_t reg) {
  std::vector<uint8_t> payload;
  if (this->extract_monitor_payload_(raw, reg, payload))
    this->remember_monitor_payload_(static_cast<uint8_t>(reg), payload);
}

}  // namespace toshiba_suzumi
}  // namespace esphome
