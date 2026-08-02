#include <algorithm>
#include <array>
#include "toshiba_climate.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome {
namespace toshiba_suzumi {

struct Probe {
  uint8_t frame_class;
  uint8_t direction;
  const char *name;
};

static constexpr std::array<Probe, 2> PROBES = {{
    {0x10, 0x00, "baseline-10-00"},
    {0x11, 0x01, "candidate-11-01"},
}};
static constexpr uint8_t TARGET_REG = 0x99;
static constexpr uint32_t TIMEOUT = 5000;
static constexpr uint32_t QUIET = 50;
static constexpr uint32_t BETWEEN_TESTS = 1500;
static constexpr size_t CHUNK = 24;

static size_t probe_index_ = 0;
static Probe active_probe_ = PROBES[0];

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
    probe_index_ = 0;
    active_probe_ = PROBES[0];
    this->scan_register_ = TARGET_REG;
    this->monitor_payload_seen_.fill(false);
    for (auto &p : this->monitor_last_payload_) p.clear();
    ESP_LOGI(TAG, "========== TOSHIBA 0x99 HEADER PROBE STARTED ==========");
    ESP_LOGI(TAG, "tests=2 target=0x99 timeout=%ums candidate=class11/direction01",
             static_cast<unsigned>(TIMEOUT));
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
    if (now - this->scan_register_started_ < BETWEEN_TESTS) return;
    this->monitor_waiting_for_cycle_ = false;
  }

  if (!this->scan_request_sent_) {
    if ((!this->scan_started_ && !this->command_queue_.empty()) || !this->rx_message_.empty() ||
        now - this->last_command_timestamp_ < QUIET) return;
    this->scan_register_ = TARGET_REG;
    this->scan_matched_response_ = false;
    this->scan_last_packet_timestamp_ = 0;
    this->scan_register_started_ = now;
    this->scan_started_ = true;
    this->scan_request_sent_ = true;
    active_probe_ = PROBES[probe_index_];
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
  std::vector<uint8_t> p = {2,0,3,active_probe_.frame_class,active_probe_.direction,0,6,1,48,1,0,1,TARGET_REG};
  p.push_back(checksum_(p));
  this->monitor_requests_++;
  ESP_LOGI(TAG, "0x99 PROBE SEND test=%u/2 name=%s class=0x%02X direction=0x%02X length=%u bytes=[%s]",
           static_cast<unsigned>(probe_index_ + 1), active_probe_.name,
           active_probe_.frame_class, active_probe_.direction,
           static_cast<unsigned>(p.size()), format_hex_pretty(p).c_str());
  this->send_to_uart(ToshibaCommand{.cmd = static_cast<ToshibaCommandType>(TARGET_REG), .payload = p});
}

void ToshibaDiagnosticMonitorUart::complete_monitor_request_() {
  if (this->scan_matched_response_) {
    this->monitor_matched_++;
    ESP_LOGI(TAG, "0x99 PROBE COMPLETE test=%u/2 name=%s result=0x99_RESPONSE",
             static_cast<unsigned>(probe_index_ + 1), active_probe_.name);
  } else {
    this->monitor_timeouts_++;
    ESP_LOGI(TAG, "0x99 PROBE COMPLETE test=%u/2 name=%s result=TIMEOUT",
             static_cast<unsigned>(probe_index_ + 1), active_probe_.name);
  }

  this->scan_request_sent_ = false;
  this->monitor_waiting_for_cycle_ = true;
  this->scan_register_started_ = millis();
  probe_index_++;
  if (probe_index_ >= PROBES.size()) {
    this->finish_monitor_();
    return;
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
  ESP_LOGI(TAG, "========== TOSHIBA 0x99 HEADER PROBE STOPPED ==========");
  ESP_LOGI(TAG, "elapsed=%ums requests=%u matched=%u timeouts=%u unrelated=%u",
           static_cast<unsigned>(millis() - this->monitor_cycle_started_),
           static_cast<unsigned>(this->monitor_requests_),
           static_cast<unsigned>(this->monitor_matched_),
           static_cast<unsigned>(this->monitor_timeouts_),
           static_cast<unsigned>(this->monitor_unrelated_));
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
  ESP_LOGI(TAG, "0x99 PAYLOAD name=%s length=%u bytes=[%s]",
           active_probe_.name, static_cast<unsigned>(payload.size()),
           format_hex_pretty(payload).c_str());
  this->monitor_last_payload_[i] = payload;
  this->monitor_payload_seen_[i] = true;
}

void ToshibaDiagnosticMonitorUart::log_timer_bank_snapshot_() const {}

void ToshibaDiagnosticMonitorUart::log_scan_packet_(const std::vector<uint8_t> &raw) {
  const int16_t reg = this->extract_response_register_(raw);
  this->scan_last_packet_timestamp_ = millis();
  ESP_LOGI(TAG, "0x99 RX during=%s response=%s length=%u",
           active_probe_.name,
           reg >= 0 ? str_sprintf("0x%02X", reg).c_str() : "unknown",
           static_cast<unsigned>(raw.size()));
  this->log_monitor_bytes_(raw, reg);

  if (reg == TARGET_REG) {
    this->scan_matched_response_ = true;
    this->log_monitor_decoded_(raw, reg);
    return;
  }
  this->monitor_unrelated_++;
}

void ToshibaDiagnosticMonitorUart::log_monitor_bytes_(const std::vector<uint8_t> &raw, int16_t reg) const {
  const size_t n = (raw.size() + CHUNK - 1) / CHUNK;
  for (size_t c = 0; c < n; c++) {
    const size_t o = c * CHUNK;
    const size_t z = std::min(CHUNK, raw.size() - o);
    ESP_LOGI(TAG, "0x99 RAW name=%s reg=%s chunk=%u/%u offset=%u bytes=[%s]",
             active_probe_.name,
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
