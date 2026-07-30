#include <algorithm>

#include "toshiba_climate.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome {
namespace toshiba_suzumi {

// Registers selected from the full scan and the already-decoded control/status registers
// needed to classify each experimental stage. All requests use the proven read envelope.
static const uint8_t MONITOR_REGISTERS[] = {
    0x80, 0x81, 0x82, 0x86, 0x87, 0x88, 0x89, 0x90, 0x92, 0x96, 0x97, 0x98,
    0x99, 0x9A, 0xA0, 0xA4, 0xB4, 0xB7, 0xB9, 0xBA, 0xBB, 0xBE, 0xC0, 0xC6,
    0xC7, 0xCA, 0xD4, 0xD7, 0xDA, 0xE2, 0xE3, 0xE4, 0xE5, 0xEE, 0xF8,
};
static const size_t MONITOR_REGISTER_COUNT = sizeof(MONITOR_REGISTERS) / sizeof(MONITOR_REGISTERS[0]);
static const uint32_t MONITOR_CYCLE_INTERVAL = 60000;
static const uint32_t MONITOR_RESPONSE_TIMEOUT = 1000;
static const uint32_t MONITOR_B7_RESPONSE_TIMEOUT = 1600;
static const uint32_t MONITOR_QUIET_PERIOD = 250;
static const size_t MONITOR_LOG_CHUNK_BYTES = 24;

static uint8_t monitor_checksum_(const std::vector<uint8_t> &data) {
  uint8_t sum = 0;
  for (size_t i = 1; i < data.size(); i++) {
    sum += data[i];
  }
  return 256 - sum;
}

static size_t monitor_register_offset_(const std::vector<uint8_t> &raw_data, int16_t response_register) {
  const size_t length = raw_data.size();
  if ((length == 15 || length == 22) && length > 12 && raw_data[12] == response_register) {
    return 12;
  }
  if (length > 14 && raw_data[3] == 0x90 && raw_data[14] == response_register) {
    return 14;
  }
  if (length > 12 && raw_data[12] == response_register) {
    return 12;
  }
  return length;
}

static uint16_t monitor_le16_(const uint8_t *data) {
  return static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
}

void ToshibaDiagnosticMonitorUart::loop() {
  while (available()) {
    uint8_t c;
    this->read_byte(&c);
    this->handle_rx_byte_(c);
  }

  // The base command processor deliberately clears unknown partial messages after 200 ms.
  // During a B7 diagnostic read, preserve that buffer until the monitor timeout so the
  // first attempt and retry can report any incomplete frame rather than silently losing it.
  if (!(this->scan_active_ && this->scan_request_sent_ && this->scan_register_ == 0xB7 &&
        !this->rx_message_.empty())) {
    this->process_command_queue_();
  }
  this->process_scan_();
}

void ToshibaDiagnosticMonitorUart::set_scan_enabled(bool enabled) {
  if (enabled) {
    if (this->scan_active_) {
      this->monitor_stop_requested_ = false;
      ESP_LOGI(TAG, "Diagnostic monitor already running.");
      return;
    }

    this->scan_active_ = true;
    this->scan_started_ = false;
    this->scan_request_sent_ = false;
    this->scan_matched_response_ = false;
    this->monitor_stop_requested_ = false;
    this->monitor_waiting_for_cycle_ = false;
    this->monitor_b7_retry_pending_ = false;
    this->monitor_b7_retry_active_ = false;
    this->monitor_register_index_ = 0;
    this->monitor_cycle_started_ = millis();
    this->monitor_cycle_number_ = 0;
    this->monitor_requests_ = 0;
    this->monitor_registers_attempted_ = 0;
    this->monitor_matched_ = 0;
    this->monitor_timeouts_ = 0;
    this->monitor_unrelated_ = 0;
    this->monitor_cycles_completed_ = 0;
    this->monitor_b7_first_timeouts_ = 0;
    this->monitor_b7_retries_ = 0;
    this->monitor_b7_retry_matches_ = 0;
    this->monitor_b7_retry_timeouts_ = 0;
    this->monitor_b7_partial_.clear();
    this->monitor_last_unrelated_packet_.clear();
    this->scan_register_ = MONITOR_REGISTERS[0];
    this->reset_monitor_cycle_state_();

    ESP_LOGI(TAG, "========== TOSHIBA ONE-MINUTE DIAGNOSTIC MONITOR STARTED ==========");
    ESP_LOGI(TAG, "Registers=%u; one complete sweep every 60 seconds.",
             static_cast<unsigned>(MONITOR_REGISTER_COUNT));
    ESP_LOGI(TAG, "0xB7 receives one quiet retry and preserves partial frames.");
    ESP_LOGI(TAG, "Long responses are logged in %u-byte chunks.",
             static_cast<unsigned>(MONITOR_LOG_CHUNK_BYTES));
    ESP_LOGI(TAG, "Normal polling paused until disabled.");
    this->start_monitor_cycle_(this->monitor_cycle_started_);
    return;
  }

  if (!this->scan_active_) {
    return;
  }

  this->monitor_stop_requested_ = true;
  ESP_LOGI(TAG, "Diagnostic monitor stop requested; finishing current read.");
  if (!this->scan_request_sent_) {
    this->finish_monitor_();
  }
}

void ToshibaDiagnosticMonitorUart::reset_monitor_cycle_state_() {
  this->monitor_cycle_attempted_ = 0;
  this->monitor_cycle_matched_ = 0;
  this->monitor_cycle_timeouts_ = 0;
  this->monitor_cycle_requests_ = 0;
  this->monitor_cycle_unrelated_ = 0;
  this->monitor_cycle_unrelated_repeated_ = 0;
  this->monitor_cycle_unrelated_changed_ = 0;
  this->monitor_last_unrelated_packet_.clear();
  this->monitor_power_valid_ = false;
  this->monitor_limit_valid_ = false;
  this->monitor_room_valid_ = false;
  this->monitor_outdoor_valid_ = false;
  this->monitor_f8_valid_ = false;
  this->monitor_e4_valid_ = false;
  this->monitor_e5_valid_ = false;
}

void ToshibaDiagnosticMonitorUart::start_monitor_cycle_(uint32_t now) {
  this->monitor_cycle_number_++;
  this->monitor_cycle_started_ = now;
  this->monitor_register_index_ = 0;
  this->monitor_waiting_for_cycle_ = false;
  this->reset_monitor_cycle_state_();
  ESP_LOGI(TAG, "MONITOR cycle=%u started", static_cast<unsigned>(this->monitor_cycle_number_));
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

  if (this->monitor_waiting_for_cycle_) {
    if (now - this->monitor_cycle_started_ < MONITOR_CYCLE_INTERVAL) {
      return;
    }
    this->start_monitor_cycle_(now);
  }

  if (!this->scan_request_sent_) {
    if ((!this->scan_started_ && !this->command_queue_.empty()) || !this->rx_message_.empty() ||
        now - this->last_command_timestamp_ < MONITOR_QUIET_PERIOD) {
      return;
    }

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
    if (now - this->scan_last_packet_timestamp_ >= MONITOR_QUIET_PERIOD) {
      this->complete_monitor_request_();
    }
    return;
  }

  const uint32_t timeout = this->scan_register_ == 0xB7 ? MONITOR_B7_RESPONSE_TIMEOUT : MONITOR_RESPONSE_TIMEOUT;
  if (now - this->scan_register_started_ >= timeout) {
    this->handle_monitor_timeout_();
  }
}

void ToshibaDiagnosticMonitorUart::send_monitor_request_() {
  // Proven read envelope only. Diagnostic mode never constructs a write frame.
  std::vector<uint8_t> payload = {2, 0, 3, 16, 0, 0, 6, 1, 48, 1, 0, 1};
  payload.push_back(this->scan_register_);
  payload.push_back(monitor_checksum_(payload));
  this->monitor_requests_++;
  this->monitor_cycle_requests_++;
  this->send_to_uart(ToshibaCommand{static_cast<ToshibaCommandType>(this->scan_register_), payload, 0});
}

void ToshibaDiagnosticMonitorUart::handle_monitor_timeout_() {
  if (this->scan_register_ == 0xB7 && !this->rx_message_.empty()) {
    this->monitor_b7_partial_ = this->rx_message_;
    this->rx_message_.clear();
    this->log_monitor_b7_partial_();
  }

  if (this->scan_register_ == 0xB7 && !this->monitor_b7_retry_active_) {
    this->monitor_b7_first_timeouts_++;
    this->monitor_b7_retry_pending_ = true;
    this->scan_request_sent_ = false;
    ESP_LOGI(TAG, "MONITOR request=0xB7 first_attempt=timeout retry=scheduled");
    return;
  }

  this->complete_monitor_request_();
}

void ToshibaDiagnosticMonitorUart::complete_monitor_request_() {
  if (this->monitor_b7_retry_pending_) {
    this->monitor_b7_retry_pending_ = false;
    this->monitor_b7_retry_active_ = true;
    this->scan_matched_response_ = false;
    this->scan_last_packet_timestamp_ = 0;
    this->scan_register_started_ = millis();
    this->scan_request_sent_ = true;
    this->monitor_b7_retries_++;
    ESP_LOGI(TAG, "MONITOR request=0xB7 retry=started");
    this->send_monitor_request_();
    return;
  }

  this->monitor_registers_attempted_++;
  this->monitor_cycle_attempted_++;
  if (this->scan_matched_response_) {
    this->monitor_matched_++;
    this->monitor_cycle_matched_++;
    if (this->scan_register_ == 0xB7 && this->monitor_b7_retry_active_) {
      this->monitor_b7_retry_matches_++;
    }
  } else {
    this->monitor_timeouts_++;
    this->monitor_cycle_timeouts_++;
    if (this->scan_register_ == 0xB7 && this->monitor_b7_retry_active_) {
      this->monitor_b7_retry_timeouts_++;
      ESP_LOGI(TAG, "MONITOR request=0xB7 retry=timeout");
    } else {
      ESP_LOGI(TAG, "MONITOR request=0x%02X timeout", static_cast<unsigned>(this->scan_register_));
    }
  }

  this->scan_request_sent_ = false;
  this->monitor_b7_retry_active_ = false;

  if (this->monitor_stop_requested_) {
    this->finish_monitor_();
    return;
  }

  this->monitor_register_index_++;
  if (this->monitor_register_index_ >= MONITOR_REGISTER_COUNT) {
    this->monitor_cycles_completed_++;
    this->monitor_waiting_for_cycle_ = true;
    this->log_monitor_cycle_summary_();
  }
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
  this->monitor_b7_retry_pending_ = false;
  this->monitor_b7_retry_active_ = false;
  this->rx_message_.clear();

  ESP_LOGI(TAG, "========== TOSHIBA DIAGNOSTIC MONITOR STOPPED ==========");
  ESP_LOGI(TAG, "registers=%u requests=%u matched=%u timeouts=%u unrelated=%u cycles=%u",
           static_cast<unsigned>(this->monitor_registers_attempted_), static_cast<unsigned>(this->monitor_requests_),
           static_cast<unsigned>(this->monitor_matched_), static_cast<unsigned>(this->monitor_timeouts_),
           static_cast<unsigned>(this->monitor_unrelated_), static_cast<unsigned>(this->monitor_cycles_completed_));
  ESP_LOGI(TAG, "B7 first_timeouts=%u retries=%u retry_matches=%u retry_timeouts=%u",
           static_cast<unsigned>(this->monitor_b7_first_timeouts_), static_cast<unsigned>(this->monitor_b7_retries_),
           static_cast<unsigned>(this->monitor_b7_retry_matches_), static_cast<unsigned>(this->monitor_b7_retry_timeouts_));
  ESP_LOGI(TAG, "Normal polling resumed.");

  this->getInitData();
}

void ToshibaDiagnosticMonitorUart::log_scan_packet_(const std::vector<uint8_t> &raw_data) {
  const int16_t response_register = this->extract_response_register_(raw_data);
  const bool matched = response_register == this->scan_register_;
  const size_t length = raw_data.size();

  this->scan_last_packet_timestamp_ = millis();
  if (matched) {
    this->scan_matched_response_ = true;
  } else {
    this->monitor_unrelated_++;
    this->monitor_cycle_unrelated_++;
  }

  if (!matched) {
    const bool repeated = raw_data == this->monitor_last_unrelated_packet_;
    if (repeated) {
      this->monitor_cycle_unrelated_repeated_++;
      return;
    }
    this->monitor_cycle_unrelated_changed_++;
    this->monitor_last_unrelated_packet_ = raw_data;
    if (response_register >= 0) {
      ESP_LOGI(TAG, "MONITOR UNSOLICITED request=0x%02X response=0x%02X length=%u DATA=[%s]",
               static_cast<unsigned>(this->scan_register_), static_cast<unsigned>(response_register),
               static_cast<unsigned>(length), format_hex_pretty(raw_data).c_str());
    } else {
      ESP_LOGI(TAG, "MONITOR UNSOLICITED request=0x%02X response=unknown length=%u DATA=[%s]",
               static_cast<unsigned>(this->scan_register_), static_cast<unsigned>(length),
               format_hex_pretty(raw_data).c_str());
    }
    return;
  }

  ESP_LOGI(TAG, "MONITOR reg=0x%02X attempt=%s length=%u checksum=OK",
           static_cast<unsigned>(response_register),
           (response_register == 0xB7 && this->monitor_b7_retry_active_) ? "retry" : "first",
           static_cast<unsigned>(length));
  this->capture_monitor_control_(raw_data, response_register);
  this->log_monitor_bytes_(raw_data, response_register);
  if (response_register == 0xDA) {
    this->log_monitor_da_(raw_data);
  }
}

void ToshibaDiagnosticMonitorUart::capture_monitor_control_(const std::vector<uint8_t> &raw_data,
                                                             int16_t response_register) {
  const size_t register_offset = monitor_register_offset_(raw_data, response_register);
  if (register_offset >= raw_data.size() || register_offset + 1 >= raw_data.size() - 1) {
    return;
  }
  const uint8_t *payload = raw_data.data() + register_offset + 1;
  const size_t payload_length = raw_data.size() - register_offset - 2;

  if (payload_length == 1) {
    const uint8_t value = payload[0];
    ESP_LOGI(TAG, "MONITOR VALUE reg=0x%02X unsigned=%u signed=%d hex=0x%02X",
             static_cast<unsigned>(response_register), static_cast<unsigned>(value),
             static_cast<int>(static_cast<int8_t>(value)), static_cast<unsigned>(value));
    if (response_register == 0x80) {
      this->monitor_power_valid_ = true;
      this->monitor_power_ = value;
    } else if (response_register == 0x87) {
      this->monitor_limit_valid_ = true;
      this->monitor_limit_ = value;
    } else if (response_register == 0xBB) {
      this->monitor_room_valid_ = true;
      this->monitor_room_ = value;
    } else if (response_register == 0xBE) {
      this->monitor_outdoor_valid_ = true;
      this->monitor_outdoor_ = value;
    }
  }

  if (response_register == 0xF8 && payload_length >= 4) {
    std::copy(payload, payload + 4, this->monitor_f8_);
    this->monitor_f8_valid_ = true;
  } else if (response_register == 0xE4 && payload_length >= 3) {
    std::copy(payload, payload + 3, this->monitor_e4_);
    this->monitor_e4_valid_ = true;
  } else if (response_register == 0xE5 && payload_length >= 8) {
    std::copy(payload, payload + 8, this->monitor_e5_);
    this->monitor_e5_valid_ = true;
  }
}

void ToshibaDiagnosticMonitorUart::log_monitor_bytes_(const std::vector<uint8_t> &raw_data,
                                                       int16_t response_register) const {
  if (response_register != 0xDA && response_register != 0xB7) {
    return;
  }

  const size_t total_chunks = (raw_data.size() + MONITOR_LOG_CHUNK_BYTES - 1) / MONITOR_LOG_CHUNK_BYTES;
  for (size_t chunk = 0; chunk < total_chunks; chunk++) {
    const size_t offset = chunk * MONITOR_LOG_CHUNK_BYTES;
    const size_t count = std::min(MONITOR_LOG_CHUNK_BYTES, raw_data.size() - offset);
    ESP_LOGI(TAG, "MONITOR RAW reg=0x%02X chunk=%u/%u offset=%u bytes=[%s]",
             static_cast<unsigned>(response_register), static_cast<unsigned>(chunk + 1),
             static_cast<unsigned>(total_chunks), static_cast<unsigned>(offset),
             format_hex_pretty(raw_data.data() + offset, count).c_str());
  }
}

void ToshibaDiagnosticMonitorUart::log_monitor_da_(const std::vector<uint8_t> &raw_data) {
  const size_t register_offset = monitor_register_offset_(raw_data, 0xDA);
  if (register_offset >= raw_data.size()) {
    return;
  }
  const size_t payload_offset = register_offset + 1;
  const size_t payload_length = raw_data.size() - payload_offset - 1;
  if (payload_length < 130) {
    ESP_LOGI(TAG, "MONITOR DA payload_length=%u expected_at_least=130",
             static_cast<unsigned>(payload_length));
    return;
  }

  // The monthly packet contains 31 little-endian daily values after a six-byte date/time prefix.
  // This structural interpretation remains a hypothesis; log both the raw frame and derived sum.
  uint32_t total_wh = 0;
  for (size_t day = 0; day < 31; day++) {
    total_wh += monitor_le16_(raw_data.data() + payload_offset + 6 + day * 2);
  }

  const uint32_t now = millis();
  if (this->monitor_da_previous_valid_ && total_wh >= this->monitor_da_previous_wh_) {
    const uint32_t delta_wh = total_wh - this->monitor_da_previous_wh_;
    const uint32_t elapsed_ms = now - this->monitor_da_previous_ms_;
    if (elapsed_ms > 0) {
      const float inferred_w = delta_wh * 3600000.0f / elapsed_ms;
      ESP_LOGI(TAG, "MONITOR DA sum=%uWh delta=%uWh elapsed=%.1fs inferred=%.1fW hypothesis=monthly_energy",
               static_cast<unsigned>(total_wh), static_cast<unsigned>(delta_wh), elapsed_ms / 1000.0f, inferred_w);
    }
  } else {
    ESP_LOGI(TAG, "MONITOR DA sum=%uWh baseline=stored hypothesis=monthly_energy",
             static_cast<unsigned>(total_wh));
  }
  this->monitor_da_previous_valid_ = true;
  this->monitor_da_previous_wh_ = total_wh;
  this->monitor_da_previous_ms_ = now;
}

void ToshibaDiagnosticMonitorUart::log_monitor_b7_partial_() const {
  if (this->monitor_b7_partial_.empty()) {
    return;
  }
  ESP_LOGI(TAG, "MONITOR B7 partial attempt=%s length=%u bytes=[%s]",
           this->monitor_b7_retry_active_ ? "retry" : "first",
           static_cast<unsigned>(this->monitor_b7_partial_.size()),
           format_hex_pretty(this->monitor_b7_partial_).c_str());
}

void ToshibaDiagnosticMonitorUart::log_monitor_cycle_summary_() const {
  ESP_LOGI(TAG,
           "MONITOR cycle=%u complete attempted=%u/%u requests=%u matched=%u timeouts=%u unsolicited=%u changed=%u repeated=%u",
           static_cast<unsigned>(this->monitor_cycle_number_), static_cast<unsigned>(this->monitor_cycle_attempted_),
           static_cast<unsigned>(MONITOR_REGISTER_COUNT), static_cast<unsigned>(this->monitor_cycle_requests_),
           static_cast<unsigned>(this->monitor_cycle_matched_), static_cast<unsigned>(this->monitor_cycle_timeouts_),
           static_cast<unsigned>(this->monitor_cycle_unrelated_),
           static_cast<unsigned>(this->monitor_cycle_unrelated_changed_),
           static_cast<unsigned>(this->monitor_cycle_unrelated_repeated_));

  ESP_LOGI(TAG,
           "MONITOR CONTROL power=%s%u limit=%s%u room=%s%dC outdoor=%s%dC F8=%s[%u,%u,%u,%u] E4=%s[%u,%u,%u] E5=%s[%u,%u,%u,%u,%u,%u,%u,%u]",
           this->monitor_power_valid_ ? "" : "NA/", static_cast<unsigned>(this->monitor_power_),
           this->monitor_limit_valid_ ? "" : "NA/", static_cast<unsigned>(this->monitor_limit_),
           this->monitor_room_valid_ ? "" : "NA/", static_cast<int>(static_cast<int8_t>(this->monitor_room_)),
           this->monitor_outdoor_valid_ ? "" : "NA/", static_cast<int>(static_cast<int8_t>(this->monitor_outdoor_)),
           this->monitor_f8_valid_ ? "" : "NA/", static_cast<unsigned>(this->monitor_f8_[0]),
           static_cast<unsigned>(this->monitor_f8_[1]), static_cast<unsigned>(this->monitor_f8_[2]),
           static_cast<unsigned>(this->monitor_f8_[3]), this->monitor_e4_valid_ ? "" : "NA/",
           static_cast<unsigned>(this->monitor_e4_[0]), static_cast<unsigned>(this->monitor_e4_[1]),
           static_cast<unsigned>(this->monitor_e4_[2]), this->monitor_e5_valid_ ? "" : "NA/",
           static_cast<unsigned>(this->monitor_e5_[0]), static_cast<unsigned>(this->monitor_e5_[1]),
           static_cast<unsigned>(this->monitor_e5_[2]), static_cast<unsigned>(this->monitor_e5_[3]),
           static_cast<unsigned>(this->monitor_e5_[4]), static_cast<unsigned>(this->monitor_e5_[5]),
           static_cast<unsigned>(this->monitor_e5_[6]), static_cast<unsigned>(this->monitor_e5_[7]));
}

}  // namespace toshiba_suzumi
}  // namespace esphome
