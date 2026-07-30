#include "toshiba_climate.h"
#include "esphome/core/log.h"

namespace esphome {
namespace toshiba_suzumi {

static const uint8_t MONITOR_REGISTERS[] = {0xCA, 0xEE, 0xE5};
static const size_t MONITOR_REGISTER_COUNT = 3;
static const uint32_t MONITOR_RESPONSE_TIMEOUT = 1000;
static const uint32_t MONITOR_QUIET_PERIOD = 250;

static uint8_t monitor_checksum_(const std::vector<uint8_t> &data) {
  uint8_t sum = 0;
  for (size_t i = 1; i < data.size(); i++) sum += data[i];
  return 256 - sum;
}

void ToshibaDiagnosticMonitorUart::set_scan_enabled(bool enabled) {
  if (enabled) {
    if (this->scan_active_) {
      this->monitor_stop_requested_ = false;
      return;
    }
    this->scan_active_ = true;
    this->scan_started_ = false;
    this->scan_request_sent_ = false;
    this->scan_matched_response_ = false;
    this->monitor_stop_requested_ = false;
    this->monitor_register_index_ = 0;
    this->monitor_requests_ = 0;
    this->monitor_matched_ = 0;
    this->monitor_timeouts_ = 0;
    this->monitor_cycles_completed_ = 0;
    this->scan_register_ = MONITOR_REGISTERS[0];
    ESP_LOGI(TAG, "========== TOSHIBA FOCUSED MONITOR STARTED ==========");
    ESP_LOGI(TAG, "Read-only registers: 0xCA, 0xEE, 0xE5");
    ESP_LOGI(TAG, "Normal polling paused until disabled.");
    return;
  }

  if (!this->scan_active_) return;
  this->monitor_stop_requested_ = true;
  ESP_LOGI(TAG, "Focused monitor stop requested; finishing current read.");
  if (!this->scan_request_sent_) this->finish_monitor_();
}

void ToshibaDiagnosticMonitorUart::process_scan_() {
  if (!this->scan_active_) return;
  const uint32_t now = millis();

  if (this->monitor_stop_requested_ && !this->scan_request_sent_) {
    this->finish_monitor_();
    return;
  }

  if (!this->scan_request_sent_) {
    if ((!this->scan_started_ && !this->command_queue_.empty()) || !this->rx_message_.empty() ||
        now - this->last_command_timestamp_ < MONITOR_QUIET_PERIOD) return;
    this->scan_register_ = MONITOR_REGISTERS[this->monitor_register_index_];
    this->scan_matched_response_ = false;
    this->scan_last_packet_timestamp_ = 0;
    this->scan_register_started_ = now;
    this->scan_started_ = true;
    this->scan_request_sent_ = true;
    this->send_monitor_request_();
    return;
  }

  if (this->scan_matched_response_) {
    if (now - this->scan_last_packet_timestamp_ >= MONITOR_QUIET_PERIOD) this->complete_monitor_request_();
    return;
  }

  if (now - this->scan_register_started_ >= MONITOR_RESPONSE_TIMEOUT) this->complete_monitor_request_();
}

void ToshibaDiagnosticMonitorUart::send_monitor_request_() {
  // Proven read envelope only. Scan mode never constructs a write frame.
  std::vector<uint8_t> payload = {2, 0, 3, 16, 0, 0, 6, 1, 48, 1, 0, 1};
  payload.push_back(this->scan_register_);
  payload.push_back(monitor_checksum_(payload));
  this->monitor_requests_++;
  this->send_to_uart(ToshibaCommand{.cmd = static_cast<ToshibaCommandType>(this->scan_register_), .payload = payload});
}

void ToshibaDiagnosticMonitorUart::complete_monitor_request_() {
  if (this->scan_matched_response_) this->monitor_matched_++;
  else {
    this->monitor_timeouts_++;
    ESP_LOGI(TAG, "MONITOR request=0x%02X timeout", static_cast<unsigned>(this->scan_register_));
  }
  this->scan_request_sent_ = false;
  if (this->monitor_stop_requested_) {
    this->finish_monitor_();
    return;
  }
  this->monitor_register_index_ = (this->monitor_register_index_ + 1) % MONITOR_REGISTER_COUNT;
  if (this->monitor_register_index_ == 0) this->monitor_cycles_completed_++;
}

void ToshibaDiagnosticMonitorUart::finish_monitor_() {
  if (!this->scan_active_) return;
  this->scan_active_ = false;
  this->scan_started_ = false;
  this->scan_request_sent_ = false;
  this->scan_matched_response_ = false;
  this->monitor_stop_requested_ = false;
  ESP_LOGI(TAG, "========== TOSHIBA FOCUSED MONITOR STOPPED ==========");
  ESP_LOGI(TAG, "Requests=%u matched=%u timeouts=%u cycles=%u",
           static_cast<unsigned>(this->monitor_requests_), static_cast<unsigned>(this->monitor_matched_),
           static_cast<unsigned>(this->monitor_timeouts_), static_cast<unsigned>(this->monitor_cycles_completed_));
  ESP_LOGI(TAG, "Normal polling resumed.");
  this->getInitData();
}

void ToshibaDiagnosticMonitorUart::log_scan_packet_(const std::vector<uint8_t> &raw_data) {
  const int16_t response_register = this->extract_response_register_(raw_data);
  const bool matched = response_register == this->scan_register_;
  const size_t length = raw_data.size();
  this->scan_last_packet_timestamp_ = millis();
  if (matched) this->scan_matched_response_ = true;

  const uint8_t b3 = length > 3 ? raw_data[3] : 0;
  const uint8_t b4 = length > 4 ? raw_data[4] : 0;
  const uint8_t b5 = length > 5 ? raw_data[5] : 0;
  const uint8_t b6 = length > 6 ? raw_data[6] : 0;
  const uint8_t b7 = length > 7 ? raw_data[7] : 0;
  const uint8_t b8 = length > 8 ? raw_data[8] : 0;
  const uint8_t b9 = length > 9 ? raw_data[9] : 0;
  const uint8_t b10 = length > 10 ? raw_data[10] : 0;
  const uint8_t b11 = length > 11 ? raw_data[11] : 0;

  if (response_register >= 0) {
    ESP_LOGI(TAG, "MONITOR request=0x%02X response=0x%02X matched=%s length=%u checksum=OK header=[%02X %02X %02X %02X %02X %02X %02X %02X %02X] DATA=[%s]",
             static_cast<unsigned>(this->scan_register_), static_cast<unsigned>(response_register),
             matched ? "YES" : "NO", static_cast<unsigned>(length), b3,b4,b5,b6,b7,b8,b9,b10,b11,
             format_hex_pretty(raw_data).c_str());
  } else {
    ESP_LOGI(TAG, "MONITOR request=0x%02X response=unknown matched=NO length=%u checksum=OK header=[%02X %02X %02X %02X %02X %02X %02X %02X %02X] DATA=[%s]",
             static_cast<unsigned>(this->scan_register_), static_cast<unsigned>(length), b3,b4,b5,b6,b7,b8,b9,b10,b11,
             format_hex_pretty(raw_data).c_str());
  }

  if (response_register == 0xCA && length >= 20) {
    const size_t o = raw_data[3] == 0x90 ? 15 : 13;
    const uint16_t w1 = raw_data[o+1] | (raw_data[o+2] << 8);
    const uint16_t w2 = raw_data[o+3] | (raw_data[o+4] << 8);
    ESP_LOGI(TAG, "MONITOR 0xCA payload=%02X %02X %02X %02X %02X le16=[%u,%u]",
             raw_data[o],raw_data[o+1],raw_data[o+2],raw_data[o+3],raw_data[o+4],w1,w2);
  } else if (response_register == 0xEE && length >= 17) {
    const size_t o = raw_data[3] == 0x90 ? 15 : 13;
    const uint16_t w = raw_data[o] | (raw_data[o+1] << 8);
    ESP_LOGI(TAG, "MONITOR 0xEE payload=%02X %02X le16=%u", raw_data[o],raw_data[o+1],w);
  } else if (response_register == static_cast<int16_t>(ToshibaCommandType::ODU_STATUS)) {
    const size_t o = length == 22 ? 13 : 15;
    if (length >= o + 8) {
      ESP_LOGI(TAG, "MONITOR 0xE5 discharge=%dC suction=%dC exchanger=%dC load_raw=%u load=%.1f%% unknown=[%u,%u,%u] current_raw=%u current=%.1fA",
               static_cast<int8_t>(raw_data[o]), static_cast<int8_t>(raw_data[o+1]), static_cast<int8_t>(raw_data[o+2]),
               raw_data[o+3], raw_data[o+3] / 1.7f, raw_data[o+4], raw_data[o+5], raw_data[o+7],
               raw_data[o+6], raw_data[o+6] / 10.0f);
    }
  }
}

}  // namespace toshiba_suzumi
}  // namespace esphome
