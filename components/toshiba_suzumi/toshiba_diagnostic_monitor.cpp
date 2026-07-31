#include <algorithm>

#include "toshiba_climate.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome {
namespace toshiba_suzumi {

static const uint8_t MONITOR_FIRST_REGISTER = 0x80;
static const uint16_t MONITOR_REGISTER_COUNT = 128;
static const uint32_t MONITOR_CYCLE_INTERVAL = 60000;
static const uint32_t MONITOR_RESPONSE_TIMEOUT = 350;
static const uint32_t MONITOR_QUIET_PERIOD = 20;
static const size_t MONITOR_LOG_CHUNK_BYTES = 24;

static uint8_t monitor_checksum_(const std::vector<uint8_t> &data) {
  uint8_t sum = 0;
  for (size_t i = 1; i < data.size(); i++)
    sum += data[i];
  return 256 - sum;
}

void ToshibaDiagnosticMonitorUart::set_scan_enabled(bool enabled) {
  if (enabled) {
    if (this->scan_active_) {
      this->monitor_stop_requested_ = false;
      ESP_LOGI(TAG, "Upper register monitor already running.");
      return;
    }

    this->scan_active_ = true;
    this->scan_started_ = false;
    this->scan_request_sent_ = false;
    this->scan_matched_response_ = false;
    this->monitor_stop_requested_ = false;
    this->monitor_waiting_for_cycle_ = false;
    this->monitor_register_index_ = 0;
    this->monitor_cycle_started_ = millis();
    this->monitor_requests_ = 0;
    this->monitor_matched_ = 0;
    this->monitor_timeouts_ = 0;
    this->monitor_unrelated_ = 0;
    this->monitor_cycles_completed_ = 0;
    this->scan_register_ = MONITOR_FIRST_REGISTER;
    this->monitor_payload_seen_.fill(false);
    for (auto &payload : this->monitor_last_payload_)
      payload.clear();

    ESP_LOGI(TAG, "========== TOSHIBA UPPER REGISTER MONITOR STARTED ==========");
    ESP_LOGI(TAG, "range=0x80-0xFF registers=128 cadence=60s read_only=YES timeout=350ms");
    ESP_LOGI(TAG, "All incoming packets are logged; matched payloads are retained and changes are identified.");
    ESP_LOGI(TAG, "Registers 0x90-0x9F are snapshotted at the end of every sweep.");
    ESP_LOGI(TAG, "Normal polling paused until monitor is disabled.");
    ESP_LOGI(TAG, "MONITOR cycle=1 started range=0x80-0xFF registers=128");
    return;
  }

  if (!this->scan_active_)
    return;

  this->monitor_stop_requested_ = true;
  ESP_LOGI(TAG, "Upper register monitor stop requested; finishing current read.");
  if (!this->scan_request_sent_)
    this->finish_monitor_();
}

void ToshibaDiagnosticMonitorUart::process_scan_() {
  if (!this->scan_active_)
    return;

  const uint32_t now = millis();

  if (this->monitor_stop_requested_ && !this->scan_request_sent_) {
    this->finish_monitor_();
    return;
  }

  if (this->monitor_waiting_for_cycle_) {
    if (now - this->monitor_cycle_started_ < MONITOR_CYCLE_INTERVAL)
      return;

    this->monitor_cycle_started_ = now;
    this->monitor_register_index_ = 0;
    this->monitor_waiting_for_cycle_ = false;
    ESP_LOGI(TAG, "MONITOR cycle=%u started range=0x80-0xFF registers=128",
             static_cast<unsigned>(this->monitor_cycles_completed_ + 1));
  }

  if (!this->scan_request_sent_) {
    if ((!this->scan_started_ && !this->command_queue_.empty()) || !this->rx_message_.empty() ||
        now - this->last_command_timestamp_ < MONITOR_QUIET_PERIOD)
      return;

    this->scan_register_ = static_cast<uint8_t>(MONITOR_FIRST_REGISTER + this->monitor_register_index_);
    this->scan_matched_response_ = false;
    this->scan_last_packet_timestamp_ = 0;
    this->scan_register_started_ = now;
    this->scan_started_ = true;
    this->scan_request_sent_ = true;
    this->send_monitor_request_();
    return;
  }

  if (this->scan_matched_response_) {
    if (now - this->scan_last_packet_timestamp_ >= MONITOR_QUIET_PERIOD)
      this->complete_monitor_request_();
    return;
  }

  if (now - this->scan_register_started_ >= MONITOR_RESPONSE_TIMEOUT)
    this->complete_monitor_request_();
}

void ToshibaDiagnosticMonitorUart::send_monitor_request_() {
  std::vector<uint8_t> payload = {2, 0, 3, 16, 0, 0, 6, 1, 48, 1, 0, 1};
  payload.push_back(this->scan_register_);
  payload.push_back(monitor_checksum_(payload));
  this->monitor_requests_++;
  this->send_to_uart(ToshibaCommand{.cmd = static_cast<ToshibaCommandType>(this->scan_register_), .payload = payload});
}

void ToshibaDiagnosticMonitorUart::complete_monitor_request_() {
  if (this->scan_matched_response_) {
    this->monitor_matched_++;
  } else {
    this->monitor_timeouts_++;
    ESP_LOGI(TAG, "MONITOR cycle=%u reg=0x%02X timeout",
             static_cast<unsigned>(this->monitor_cycles_completed_ + 1),
             static_cast<unsigned>(this->scan_register_));
  }

  this->scan_request_sent_ = false;

  if (this->monitor_stop_requested_) {
    this->finish_monitor_();
    return;
  }

  if (this->monitor_register_index_ >= MONITOR_REGISTER_COUNT - 1) {
    this->monitor_cycles_completed_++;
    this->log_timer_bank_snapshot_();
    this->monitor_waiting_for_cycle_ = true;
    ESP_LOGI(TAG, "MONITOR cycle=%u complete attempted=128 matched=%u timeouts=%u unsolicited=%u",
             static_cast<unsigned>(this->monitor_cycles_completed_),
             static_cast<unsigned>(this->monitor_matched_),
             static_cast<unsigned>(this->monitor_timeouts_),
             static_cast<unsigned>(this->monitor_unrelated_));
  } else {
    this->monitor_register_index_++;
  }
}

void ToshibaDiagnosticMonitorUart::finish_monitor_() {
  if (!this->scan_active_)
    return;

  this->scan_active_ = false;
  this->scan_started_ = false;
  this->scan_request_sent_ = false;
  this->scan_matched_response_ = false;
  this->monitor_stop_requested_ = false;
  this->monitor_waiting_for_cycle_ = false;

  ESP_LOGI(TAG, "========== TOSHIBA UPPER REGISTER MONITOR STOPPED ==========");
  ESP_LOGI(TAG, "Requests=%u matched=%u timeouts=%u unsolicited=%u cycles=%u",
           static_cast<unsigned>(this->monitor_requests_), static_cast<unsigned>(this->monitor_matched_),
           static_cast<unsigned>(this->monitor_timeouts_), static_cast<unsigned>(this->monitor_unrelated_),
           static_cast<unsigned>(this->monitor_cycles_completed_));
  ESP_LOGI(TAG, "Normal polling resumed.");
  this->getInitData();
}

bool ToshibaDiagnosticMonitorUart::extract_monitor_payload_(const std::vector<uint8_t> &raw_data,
                                                             int16_t response_register,
                                                             std::vector<uint8_t> &payload) const {
  const size_t length = raw_data.size();
  size_t register_offset;

  if ((length == 15 || length == 22) && length > 12 && raw_data[12] == response_register)
    register_offset = 12;
  else if (length > 14 && raw_data[3] == 0x90 && raw_data[14] == response_register)
    register_offset = 14;
  else if (length > 12 && raw_data[12] == response_register)
    register_offset = 12;
  else
    return false;

  const size_t payload_offset = register_offset + 1;
  const size_t checksum_offset = length - 1;
  if (payload_offset > checksum_offset)
    return false;

  payload.assign(raw_data.begin() + payload_offset, raw_data.begin() + checksum_offset);
  return true;
}

void ToshibaDiagnosticMonitorUart::remember_monitor_payload_(uint8_t response_register,
                                                              const std::vector<uint8_t> &payload) {
  if (response_register < MONITOR_FIRST_REGISTER)
    return;

  const size_t index = response_register - MONITOR_FIRST_REGISTER;
  if (index >= this->monitor_last_payload_.size())
    return;

  if (!this->monitor_payload_seen_[index]) {
    ESP_LOGI(TAG, "MONITOR FIRST reg=0x%02X bytes=[%s]", static_cast<unsigned>(response_register),
             format_hex_pretty(payload).c_str());
  } else if (this->monitor_last_payload_[index] != payload) {
    ESP_LOGI(TAG, "MONITOR CHANGE reg=0x%02X old=[%s] new=[%s]",
             static_cast<unsigned>(response_register),
             format_hex_pretty(this->monitor_last_payload_[index]).c_str(),
             format_hex_pretty(payload).c_str());
  }

  this->monitor_last_payload_[index] = payload;
  this->monitor_payload_seen_[index] = true;
}

void ToshibaDiagnosticMonitorUart::log_timer_bank_snapshot_() const {
  ESP_LOGI(TAG, "MONITOR TIMER-BANK cycle=%u snapshot-begin",
           static_cast<unsigned>(this->monitor_cycles_completed_));
  for (uint16_t reg = 0x90; reg <= 0x9F; reg++) {
    const size_t index = reg - MONITOR_FIRST_REGISTER;
    if (this->monitor_payload_seen_[index]) {
      ESP_LOGI(TAG, "MONITOR TIMER-BANK reg=0x%02X bytes=[%s]", static_cast<unsigned>(reg),
               format_hex_pretty(this->monitor_last_payload_[index]).c_str());
    } else {
      ESP_LOGI(TAG, "MONITOR TIMER-BANK reg=0x%02X unavailable", static_cast<unsigned>(reg));
    }
  }
  ESP_LOGI(TAG, "MONITOR TIMER-BANK cycle=%u snapshot-end",
           static_cast<unsigned>(this->monitor_cycles_completed_));
}

void ToshibaDiagnosticMonitorUart::log_scan_packet_(const std::vector<uint8_t> &raw_data) {
  const int16_t response_register = this->extract_response_register_(raw_data);
  const bool matched = response_register == this->scan_register_;
  this->scan_last_packet_timestamp_ = millis();

  if (matched) {
    this->scan_matched_response_ = true;
    ESP_LOGI(TAG, "MONITOR cycle=%u request=0x%02X response=0x%02X length=%u checksum=OK",
             static_cast<unsigned>(this->monitor_cycles_completed_ + 1),
             static_cast<unsigned>(this->scan_register_), static_cast<unsigned>(response_register),
             static_cast<unsigned>(raw_data.size()));
    this->log_monitor_decoded_(raw_data, response_register);
    this->log_monitor_bytes_(raw_data, response_register);
    return;
  }

  this->monitor_unrelated_++;
  if (response_register >= 0) {
    ESP_LOGI(TAG, "MONITOR UNSOLICITED request=0x%02X response=0x%02X length=%u",
             static_cast<unsigned>(this->scan_register_), static_cast<unsigned>(response_register),
             static_cast<unsigned>(raw_data.size()));
  } else {
    ESP_LOGI(TAG, "MONITOR UNSOLICITED request=0x%02X response=unknown length=%u",
             static_cast<unsigned>(this->scan_register_), static_cast<unsigned>(raw_data.size()));
  }
  this->log_monitor_bytes_(raw_data, response_register);
}

void ToshibaDiagnosticMonitorUart::log_monitor_bytes_(const std::vector<uint8_t> &raw_data,
                                                       int16_t response_register) const {
  const size_t total_chunks = (raw_data.size() + MONITOR_LOG_CHUNK_BYTES - 1) / MONITOR_LOG_CHUNK_BYTES;
  for (size_t chunk = 0; chunk < total_chunks; chunk++) {
    const size_t offset = chunk * MONITOR_LOG_CHUNK_BYTES;
    const size_t count = std::min(MONITOR_LOG_CHUNK_BYTES, raw_data.size() - offset);
    if (response_register >= 0) {
      ESP_LOGI(TAG, "MONITOR RAW reg=0x%02X chunk=%u/%u offset=%u bytes=[%s]",
               static_cast<unsigned>(response_register), static_cast<unsigned>(chunk + 1),
               static_cast<unsigned>(total_chunks), static_cast<unsigned>(offset),
               format_hex_pretty(raw_data.data() + offset, count).c_str());
    } else {
      ESP_LOGI(TAG, "MONITOR RAW reg=unknown chunk=%u/%u offset=%u bytes=[%s]",
               static_cast<unsigned>(chunk + 1), static_cast<unsigned>(total_chunks),
               static_cast<unsigned>(offset), format_hex_pretty(raw_data.data() + offset, count).c_str());
    }
  }
}

void ToshibaDiagnosticMonitorUart::log_monitor_decoded_(const std::vector<uint8_t> &raw_data,
                                                         int16_t response_register) {
  std::vector<uint8_t> payload;
  if (!this->extract_monitor_payload_(raw_data, response_register, payload))
    return;

  ESP_LOGI(TAG, "MONITOR PAYLOAD reg=0x%02X length=%u bytes=[%s]",
           static_cast<unsigned>(response_register), static_cast<unsigned>(payload.size()),
           format_hex_pretty(payload).c_str());
  this->remember_monitor_payload_(static_cast<uint8_t>(response_register), payload);

  if (payload.size() == 1) {
    const uint8_t value = payload[0];
    ESP_LOGI(TAG, "MONITOR VALUE reg=0x%02X unsigned=%u signed=%d hex=0x%02X",
             static_cast<unsigned>(response_register), static_cast<unsigned>(value),
             static_cast<int>(static_cast<int8_t>(value)), static_cast<unsigned>(value));
  }
}

}  // namespace toshiba_suzumi
}  // namespace esphome
