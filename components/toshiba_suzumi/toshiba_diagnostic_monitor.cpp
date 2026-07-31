#include <algorithm>

#include "toshiba_climate.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome {
namespace toshiba_suzumi {

static const uint8_t MONITOR_FIRST_REGISTER = 0x00;
static const uint16_t MONITOR_REGISTER_COUNT = 256;
static const uint32_t MONITOR_RESPONSE_TIMEOUT = 350;
static const uint32_t MONITOR_QUIET_PERIOD = 50;
static const size_t MONITOR_LOG_CHUNK_BYTES = 24;

static uint8_t monitor_checksum_(const std::vector<uint8_t> &data) {
  uint8_t sum = 0;
  for (size_t i = 1; i < data.size(); i++) {
    sum += data[i];
  }
  return 256 - sum;
}

void ToshibaDiagnosticMonitorUart::set_scan_enabled(bool enabled) {
  if (enabled) {
    if (this->scan_active_) {
      ESP_LOGI(TAG, "One-shot register sweep already running.");
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

    ESP_LOGI(TAG, "========== TOSHIBA ONE-SHOT REGISTER SWEEP STARTED ==========");
    ESP_LOGI(TAG, "range=0x00-0xFF registers=256 repeat=NO read_only=YES timeout=350ms quiet=50ms");
    ESP_LOGI(TAG, "Only the proven read envelope is transmitted; normal polling is paused.");
    ESP_LOGI(TAG, "Every matched response and every unsolicited packet is logged in full.");
    ESP_LOGI(TAG, "MONITOR cycle=1 started range=0x00-0xFF registers=256");
    return;
  }

  if (!this->scan_active_) {
    return;
  }

  this->monitor_stop_requested_ = true;
  ESP_LOGI(TAG, "One-shot register sweep stop requested; finishing current read.");
  if (!this->scan_request_sent_) {
    this->finish_monitor_();
  }
}

void ToshibaDiagnosticMonitorUart::process_scan_() {
  if (!this->scan_active_) {
    return;
  }

  const uint32_t now = millis();

  if (this->monitor_stop_requested_ && !this->scan_request_sent_) {
    this->finish_monitor_();
    return;
  }

  if (!this->scan_request_sent_) {
    if ((!this->scan_started_ && !this->command_queue_.empty()) || !this->rx_message_.empty() ||
        now - this->last_command_timestamp_ < MONITOR_QUIET_PERIOD) {
      return;
    }

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
    if (now - this->scan_last_packet_timestamp_ >= MONITOR_QUIET_PERIOD) {
      this->complete_monitor_request_();
    }
    return;
  }

  if (now - this->scan_register_started_ >= MONITOR_RESPONSE_TIMEOUT) {
    this->complete_monitor_request_();
  }
}

void ToshibaDiagnosticMonitorUart::send_monitor_request_() {
  // Proven read envelope only. This monitor never constructs a write frame.
  std::vector<uint8_t> payload = {2, 0, 3, 16, 0, 0, 6, 1, 48, 1, 0, 1};
  payload.push_back(this->scan_register_);
  payload.push_back(monitor_checksum_(payload));
  this->monitor_requests_++;
  this->send_to_uart(
      ToshibaCommand{.cmd = static_cast<ToshibaCommandType>(this->scan_register_), .payload = payload});
}

void ToshibaDiagnosticMonitorUart::complete_monitor_request_() {
  if (this->scan_matched_response_) {
    this->monitor_matched_++;
  } else {
    this->monitor_timeouts_++;
    ESP_LOGI(TAG, "MONITOR cycle=1 reg=0x%02X timeout", static_cast<unsigned>(this->scan_register_));
  }

  this->scan_request_sent_ = false;

  if (this->monitor_stop_requested_) {
    this->finish_monitor_();
    return;
  }

  // Explicitly stop at 0xFF before incrementing the uint8_t index.
  if (this->scan_register_ == 0xFF) {
    this->monitor_cycles_completed_ = 1;
    ESP_LOGI(TAG, "MONITOR cycle=1 complete attempted=256 matched=%u timeouts=%u unsolicited=%u",
             static_cast<unsigned>(this->monitor_matched_),
             static_cast<unsigned>(this->monitor_timeouts_),
             static_cast<unsigned>(this->monitor_unrelated_));
    this->finish_monitor_();
    return;
  }

  this->monitor_register_index_++;
}

void ToshibaDiagnosticMonitorUart::finish_monitor_() {
  if (!this->scan_active_) {
    return;
  }

  this->scan_active_ = false;
  this->scan_started_ = false;
  this->scan_request_sent_ = false;
  this->scan_matched_response_ = false;
  this->monitor_stop_requested_ = false;
  this->monitor_waiting_for_cycle_ = false;

  ESP_LOGI(TAG, "========== TOSHIBA ONE-SHOT REGISTER SWEEP STOPPED ==========");
  ESP_LOGI(TAG, "Requests=%u matched=%u timeouts=%u unsolicited=%u cycles=%u",
           static_cast<unsigned>(this->monitor_requests_),
           static_cast<unsigned>(this->monitor_matched_),
           static_cast<unsigned>(this->monitor_timeouts_),
           static_cast<unsigned>(this->monitor_unrelated_),
           static_cast<unsigned>(this->monitor_cycles_completed_));
  ESP_LOGI(TAG, "Normal polling resumed.");

  this->getInitData();
}

void ToshibaDiagnosticMonitorUart::log_scan_packet_(const std::vector<uint8_t> &raw_data) {
  const int16_t response_register = this->extract_response_register_(raw_data);
  const bool matched = response_register == this->scan_register_;

  this->scan_last_packet_timestamp_ = millis();

  if (matched) {
    this->scan_matched_response_ = true;
    ESP_LOGI(TAG, "MONITOR cycle=1 request=0x%02X response=0x%02X length=%u checksum=OK",
             static_cast<unsigned>(this->scan_register_),
             static_cast<unsigned>(response_register),
             static_cast<unsigned>(raw_data.size()));
    this->log_monitor_decoded_(raw_data, response_register);
    this->log_monitor_bytes_(raw_data, response_register);
    return;
  }

  this->monitor_unrelated_++;
  if (response_register >= 0) {
    ESP_LOGI(TAG, "MONITOR UNSOLICITED request=0x%02X response=0x%02X length=%u",
             static_cast<unsigned>(this->scan_register_),
             static_cast<unsigned>(response_register),
             static_cast<unsigned>(raw_data.size()));
  } else {
    ESP_LOGI(TAG, "MONITOR UNSOLICITED request=0x%02X response=unknown length=%u",
             static_cast<unsigned>(this->scan_register_),
             static_cast<unsigned>(raw_data.size()));
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
               static_cast<unsigned>(response_register),
               static_cast<unsigned>(chunk + 1),
               static_cast<unsigned>(total_chunks),
               static_cast<unsigned>(offset),
               format_hex_pretty(raw_data.data() + offset, count).c_str());
    } else {
      ESP_LOGI(TAG, "MONITOR RAW reg=unknown chunk=%u/%u offset=%u bytes=[%s]",
               static_cast<unsigned>(chunk + 1),
               static_cast<unsigned>(total_chunks),
               static_cast<unsigned>(offset),
               format_hex_pretty(raw_data.data() + offset, count).c_str());
    }
  }
}

void ToshibaDiagnosticMonitorUart::log_monitor_decoded_(const std::vector<uint8_t> &raw_data,
                                                         int16_t response_register) const {
  const size_t length = raw_data.size();
  size_t register_offset = 0;

  if ((length == 15 || length == 22) && length > 12 && raw_data[12] == response_register) {
    register_offset = 12;
  } else if (length > 14 && raw_data[3] == 0x90 && raw_data[14] == response_register) {
    register_offset = 14;
  } else if (length > 12 && raw_data[12] == response_register) {
    register_offset = 12;
  } else {
    return;
  }

  const size_t payload_offset = register_offset + 1;
  const size_t checksum_offset = length - 1;
  if (payload_offset >= checksum_offset) {
    ESP_LOGI(TAG, "MONITOR PAYLOAD reg=0x%02X length=0", static_cast<unsigned>(response_register));
    return;
  }

  const size_t payload_length = checksum_offset - payload_offset;
  ESP_LOGI(TAG, "MONITOR PAYLOAD reg=0x%02X length=%u bytes=[%s]",
           static_cast<unsigned>(response_register),
           static_cast<unsigned>(payload_length),
           format_hex_pretty(raw_data.data() + payload_offset, payload_length).c_str());

  if (payload_length == 1) {
    const uint8_t value = raw_data[payload_offset];
    ESP_LOGI(TAG, "MONITOR VALUE reg=0x%02X unsigned=%u signed=%d hex=0x%02X",
             static_cast<unsigned>(response_register),
             static_cast<unsigned>(value),
             static_cast<int>(static_cast<int8_t>(value)),
             static_cast<unsigned>(value));
  }
}

}  // namespace toshiba_suzumi
}  // namespace esphome
