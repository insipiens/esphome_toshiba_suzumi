#include <algorithm>

#include "toshiba_climate.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome {
namespace toshiba_suzumi {

static const uint8_t MONITOR_REGISTERS[] = {
    0x80, 0x81, 0x82, 0x86, 0x87, 0x88, 0x89, 0x90, 0x92,
    0x96, 0x97, 0x98, 0x99, 0x9A, 0xA4, 0xB4, 0xB7, 0xB9,
    0xBA, 0xBB, 0xBE, 0xC0, 0xC6, 0xC7, 0xCA, 0xD4, 0xD7,
    0xDA, 0xE2, 0xE3, 0xE4, 0xE5, 0xEE, 0xF8, 0xFE,
};

static const size_t MONITOR_REGISTER_COUNT = sizeof(MONITOR_REGISTERS) / sizeof(MONITOR_REGISTERS[0]);
static_assert(MONITOR_REGISTER_COUNT == 35, "monitor register count");

static const uint32_t MONITOR_CYCLE_MS = 60000;
static const uint32_t MONITOR_RESPONSE_TIMEOUT_MS = 1000;
static const uint32_t MONITOR_B7_RETRY_TIMEOUT_MS = 2500;
static const uint32_t MONITOR_QUIET_MS = 250;
static const size_t MONITOR_LOG_CHUNK_SIZE = 24;

static uint8_t monitor_checksum_(const std::vector<uint8_t> &bytes) {
  uint8_t sum = 0;
  for (size_t i = 1; i < bytes.size(); i++) {
    sum += bytes[i];
  }
  return 256 - sum;
}

static bool monitor_payload_(const std::vector<uint8_t> &raw_data, int16_t response_register, size_t &offset,
                             size_t &length) {
  const size_t size = raw_data.size();
  if (size < 2 || response_register < 0) {
    return false;
  }

  size_t register_offset = size;
  if ((size == 15 || size == 22) && raw_data[12] == response_register) {
    register_offset = 12;
  } else if (size > 14 && raw_data[3] == 0x90 && raw_data[14] == response_register) {
    register_offset = 14;
  } else if (size > 12 && raw_data[12] == response_register) {
    register_offset = 12;
  }

  if (register_offset == size) {
    return false;
  }

  offset = register_offset + 1;
  length = (size - 1) - offset;
  return offset <= size - 1;
}

static uint32_t monitor_le32_(const uint8_t *data) {
  return uint32_t(data[0]) | (uint32_t(data[1]) << 8) | (uint32_t(data[2]) << 16) |
         (uint32_t(data[3]) << 24);
}

void ToshibaDiagnosticMonitorUart::loop() {
  while (available()) {
    uint8_t byte;
    read_byte(&byte);
    handle_rx_byte_(byte);
  }

  if (!(scan_active_ && scan_request_sent_ && scan_register_ == 0xB7 && !rx_message_.empty())) {
    process_command_queue_();
  }

  process_scan_();
}

void ToshibaDiagnosticMonitorUart::set_scan_enabled(bool enabled) {
  if (enabled) {
    if (scan_active_) {
      monitor_stop_requested_ = false;
      return;
    }

    scan_active_ = true;
    scan_started_ = false;
    scan_request_sent_ = false;
    scan_matched_response_ = false;
    monitor_stop_requested_ = false;
    monitor_waiting_for_cycle_ = false;
    monitor_b7_retry_pending_ = false;
    monitor_b7_retry_active_ = false;
    monitor_register_index_ = 0;
    monitor_cycle_number_ = 0;
    monitor_requests_ = 0;
    monitor_registers_attempted_ = 0;
    monitor_matched_ = 0;
    monitor_timeouts_ = 0;
    monitor_unrelated_ = 0;
    monitor_cycles_completed_ = 0;
    monitor_b7_first_timeouts_ = 0;
    monitor_b7_retries_ = 0;
    monitor_b7_retry_matches_ = 0;
    monitor_b7_retry_timeouts_ = 0;
    monitor_da_previous_valid_ = false;
    monitor_b7_partial_.clear();
    monitor_last_unrelated_packet_.clear();

    ESP_LOGI(TAG, "========== TOSHIBA ONE-MINUTE REGISTER SWEEP STARTED ==========");
    ESP_LOGI(TAG,
             "registers=35 cadence=60s mode=continuous read_only=YES "
             "B7_first_timeout=1000ms B7_retry_timeout=2500ms");
    start_monitor_cycle_(millis());
    return;
  }

  if (!scan_active_) {
    return;
  }

  monitor_stop_requested_ = true;
  if (!scan_request_sent_) {
    finish_monitor_();
  }
}

void ToshibaDiagnosticMonitorUart::reset_monitor_cycle_state_() {
  monitor_cycle_attempted_ = 0;
  monitor_cycle_matched_ = 0;
  monitor_cycle_timeouts_ = 0;
  monitor_cycle_requests_ = 0;
  monitor_cycle_unrelated_ = 0;
  monitor_cycle_unrelated_repeated_ = 0;
  monitor_cycle_unrelated_changed_ = 0;

  monitor_power_valid_ = false;
  monitor_limit_valid_ = false;
  monitor_room_valid_ = false;
  monitor_outdoor_valid_ = false;
  monitor_f8_valid_ = false;
  monitor_e4_valid_ = false;
  monitor_e5_valid_ = false;
  monitor_last_unrelated_packet_.clear();
}

void ToshibaDiagnosticMonitorUart::start_monitor_cycle_(uint32_t now) {
  monitor_cycle_started_ = now;
  monitor_cycle_number_++;
  monitor_register_index_ = 0;
  monitor_waiting_for_cycle_ = false;
  monitor_b7_retry_pending_ = false;
  monitor_b7_retry_active_ = false;
  monitor_b7_partial_.clear();
  reset_monitor_cycle_state_();

  ESP_LOGI(TAG, "MONITOR cycle=%u started registers=35", (unsigned) monitor_cycle_number_);
}

void ToshibaDiagnosticMonitorUart::process_scan_() {
  if (!scan_active_) {
    return;
  }

  const uint32_t now = millis();

  if (monitor_stop_requested_ && !scan_request_sent_) {
    finish_monitor_();
    return;
  }

  if (monitor_waiting_for_cycle_) {
    if (now - monitor_cycle_started_ < MONITOR_CYCLE_MS) {
      return;
    }
    start_monitor_cycle_(now);
  }

  if (monitor_b7_retry_pending_) {
    const uint32_t last_bus_activity = std::max(last_command_timestamp_, last_rx_char_timestamp_);
    if (now - scan_register_started_ < MONITOR_QUIET_MS || now - last_bus_activity < MONITOR_QUIET_MS) {
      return;
    }

    monitor_b7_retry_pending_ = false;
    monitor_b7_retry_active_ = true;
    scan_matched_response_ = false;
    scan_last_packet_timestamp_ = 0;
    scan_register_started_ = now;
    scan_request_sent_ = true;
    monitor_b7_retries_++;
    monitor_b7_partial_.clear();

    ESP_LOGI(TAG, "MONITOR cycle=%u B7 retry started", (unsigned) monitor_cycle_number_);
    send_monitor_request_();
    return;
  }

  if (!scan_request_sent_) {
    const uint32_t last_bus_activity = std::max(last_command_timestamp_, last_rx_char_timestamp_);
    if ((!scan_started_ && !command_queue_.empty()) || !rx_message_.empty() ||
        now - last_bus_activity < MONITOR_QUIET_MS) {
      return;
    }

    scan_register_ = MONITOR_REGISTERS[monitor_register_index_];
    scan_matched_response_ = false;
    scan_last_packet_timestamp_ = 0;
    scan_register_started_ = now;
    scan_started_ = true;
    scan_request_sent_ = true;
    send_monitor_request_();
    return;
  }

  if (scan_matched_response_) {
    if (now - scan_last_packet_timestamp_ >= MONITOR_QUIET_MS) {
      complete_monitor_request_();
    }
    return;
  }

  uint32_t timeout = MONITOR_RESPONSE_TIMEOUT_MS;
  if (scan_register_ == 0xB7 && monitor_b7_retry_active_) {
    timeout = MONITOR_B7_RETRY_TIMEOUT_MS;
  }

  if (now - scan_register_started_ >= timeout) {
    handle_monitor_timeout_();
  }
}

void ToshibaDiagnosticMonitorUart::send_monitor_request_() {
  std::vector<uint8_t> request = {2, 0, 3, 16, 0, 0, 6, 1, 48, 1, 0, 1};
  request.push_back(scan_register_);
  request.push_back(monitor_checksum_(request));

  monitor_requests_++;
  monitor_cycle_requests_++;
  send_to_uart(ToshibaCommand{static_cast<ToshibaCommandType>(scan_register_), request, 0});
}

void ToshibaDiagnosticMonitorUart::handle_monitor_timeout_() {
  const bool b7_retry = scan_register_ == 0xB7 && monitor_b7_retry_active_;

  if (scan_register_ == 0xB7 && !rx_message_.empty()) {
    monitor_b7_partial_ = rx_message_;
    rx_message_.clear();
    log_monitor_b7_partial_();
  }

  if (scan_register_ == 0xB7 && !monitor_b7_retry_active_) {
    monitor_b7_first_timeouts_++;
    monitor_b7_retry_pending_ = true;
    scan_request_sent_ = false;
    scan_register_started_ = millis();
    ESP_LOGI(TAG, "MONITOR cycle=%u B7 first timeout; retry pending", (unsigned) monitor_cycle_number_);
    return;
  }

  if (b7_retry) {
    monitor_b7_retry_timeouts_++;
  }

  complete_monitor_request_();
}

void ToshibaDiagnosticMonitorUart::complete_monitor_request_() {
  monitor_registers_attempted_++;
  monitor_cycle_attempted_++;

  if (scan_matched_response_) {
    monitor_matched_++;
    monitor_cycle_matched_++;
    if (scan_register_ == 0xB7 && monitor_b7_retry_active_) {
      monitor_b7_retry_matches_++;
    }
  } else {
    monitor_timeouts_++;
    monitor_cycle_timeouts_++;
    ESP_LOGI(TAG, "MONITOR cycle=%u reg=0x%02X attempt=%s timeout", (unsigned) monitor_cycle_number_,
             (unsigned) scan_register_, monitor_b7_retry_active_ ? "retry" : "first");
  }

  scan_request_sent_ = false;
  scan_matched_response_ = false;
  monitor_b7_retry_active_ = false;
  rx_message_.clear();

  if (monitor_stop_requested_) {
    finish_monitor_();
    return;
  }

  monitor_register_index_++;
  if (monitor_register_index_ >= MONITOR_REGISTER_COUNT) {
    monitor_cycles_completed_++;
    monitor_waiting_for_cycle_ = true;
    log_monitor_cycle_summary_();
  }
}

void ToshibaDiagnosticMonitorUart::finish_monitor_() {
  if (!scan_active_) {
    return;
  }

  scan_active_ = false;
  scan_started_ = false;
  scan_request_sent_ = false;
  scan_matched_response_ = false;
  monitor_stop_requested_ = false;
  monitor_waiting_for_cycle_ = false;
  monitor_b7_retry_pending_ = false;
  monitor_b7_retry_active_ = false;
  rx_message_.clear();

  ESP_LOGI(TAG, "MONITOR stopped registers=%u requests=%u matched=%u timeouts=%u unrelated=%u cycles=%u",
           (unsigned) monitor_registers_attempted_, (unsigned) monitor_requests_, (unsigned) monitor_matched_,
           (unsigned) monitor_timeouts_, (unsigned) monitor_unrelated_, (unsigned) monitor_cycles_completed_);
  ESP_LOGI(TAG, "MONITOR B7 first_timeouts=%u retries=%u retry_matches=%u retry_timeouts=%u",
           (unsigned) monitor_b7_first_timeouts_, (unsigned) monitor_b7_retries_,
           (unsigned) monitor_b7_retry_matches_, (unsigned) monitor_b7_retry_timeouts_);

  getInitData();
}

void ToshibaDiagnosticMonitorUart::log_scan_packet_(const std::vector<uint8_t> &raw_data) {
  const int16_t response_register = extract_response_register_(raw_data);
  const bool matched = response_register == scan_register_;
  scan_last_packet_timestamp_ = millis();

  if (matched) {
    scan_matched_response_ = true;
  } else {
    monitor_unrelated_++;
    monitor_cycle_unrelated_++;

    if (raw_data == monitor_last_unrelated_packet_) {
      monitor_cycle_unrelated_repeated_++;
      return;
    }

    monitor_cycle_unrelated_changed_++;
    monitor_last_unrelated_packet_ = raw_data;
    ESP_LOGI(TAG, "MONITOR UNSOLICITED request=0x%02X response=%d length=%u DATA=[%s]",
             (unsigned) scan_register_, (int) response_register, (unsigned) raw_data.size(),
             format_hex_pretty(raw_data).c_str());
    return;
  }

  ESP_LOGI(TAG, "MONITOR cycle=%u reg=0x%02X attempt=%s length=%u checksum=OK",
           (unsigned) monitor_cycle_number_, (unsigned) response_register,
           (response_register == 0xB7 && monitor_b7_retry_active_) ? "retry" : "first",
           (unsigned) raw_data.size());

  capture_monitor_control_(raw_data, response_register);

  size_t offset;
  size_t length;
  if (response_register != 0xDA && monitor_payload_(raw_data, response_register, offset, length)) {
    ESP_LOGI(TAG, "MONITOR cycle=%u reg=0x%02X payload=[%s]", (unsigned) monitor_cycle_number_,
             (unsigned) response_register, format_hex_pretty(raw_data.data() + offset, length).c_str());
  }

  if (response_register == 0xDA) {
    log_monitor_bytes_(raw_data, response_register);
    log_monitor_da_(raw_data);
  } else if (response_register == 0xB7) {
    log_monitor_bytes_(raw_data, response_register);
  }
}

void ToshibaDiagnosticMonitorUart::capture_monitor_control_(const std::vector<uint8_t> &raw_data,
                                                            int16_t response_register) {
  size_t offset;
  size_t length;
  if (!monitor_payload_(raw_data, response_register, offset, length)) {
    return;
  }

  const uint8_t *payload = raw_data.data() + offset;

  if (length == 1) {
    ESP_LOGI(TAG, "MONITOR VALUE reg=0x%02X unsigned=%u signed=%d", (unsigned) response_register,
             (unsigned) payload[0], (int) (int8_t) payload[0]);

    if (response_register == 0x80) {
      monitor_power_ = payload[0];
      monitor_power_valid_ = true;
    } else if (response_register == 0x87) {
      monitor_limit_ = payload[0];
      monitor_limit_valid_ = true;
    } else if (response_register == 0xBB) {
      monitor_room_ = payload[0];
      monitor_room_valid_ = true;
    } else if (response_register == 0xBE) {
      monitor_outdoor_ = payload[0];
      monitor_outdoor_valid_ = true;
    }
  }

  if (response_register == 0xF8 && length >= 4) {
    std::copy(payload, payload + 4, monitor_f8_);
    monitor_f8_valid_ = true;
  } else if (response_register == 0xE4 && length >= 3) {
    std::copy(payload, payload + 3, monitor_e4_);
    monitor_e4_valid_ = true;
  } else if (response_register == 0xE5 && length >= 8) {
    std::copy(payload, payload + 8, monitor_e5_);
    monitor_e5_valid_ = true;
  }
}

void ToshibaDiagnosticMonitorUart::log_monitor_cycle_summary_() const {
  const uint32_t duration_ms = millis() - monitor_cycle_started_;
  const uint32_t wait_ms = duration_ms < MONITOR_CYCLE_MS ? MONITOR_CYCLE_MS - duration_ms : 0;

  ESP_LOGI(TAG,
           "MONITOR cycle=%u complete attempted=%u/35 requests=%u matched=%u timeouts=%u "
           "unsolicited=%u changed=%u repeated=%u duration=%.1fs next_in=%.1fs",
           (unsigned) monitor_cycle_number_, (unsigned) monitor_cycle_attempted_,
           (unsigned) monitor_cycle_requests_, (unsigned) monitor_cycle_matched_,
           (unsigned) monitor_cycle_timeouts_, (unsigned) monitor_cycle_unrelated_,
           (unsigned) monitor_cycle_unrelated_changed_, (unsigned) monitor_cycle_unrelated_repeated_,
           duration_ms / 1000.0f, wait_ms / 1000.0f);

  ESP_LOGI(TAG,
           "MONITOR CONTROL power=%s%u limit=%s%u room=%s%d outdoor=%s%d "
           "F8=%s[%u,%u,%u,%u] E4=%s[%u,%u,%u] E5=%s[%u,%u,%u,%u,%u,%u,%u,%u]",
           monitor_power_valid_ ? "" : "NA/", (unsigned) monitor_power_,
           monitor_limit_valid_ ? "" : "NA/", (unsigned) monitor_limit_,
           monitor_room_valid_ ? "" : "NA/", (int) (int8_t) monitor_room_,
           monitor_outdoor_valid_ ? "" : "NA/", (int) (int8_t) monitor_outdoor_,
           monitor_f8_valid_ ? "" : "NA/", (unsigned) monitor_f8_[0], (unsigned) monitor_f8_[1],
           (unsigned) monitor_f8_[2], (unsigned) monitor_f8_[3], monitor_e4_valid_ ? "" : "NA/",
           (unsigned) monitor_e4_[0], (unsigned) monitor_e4_[1], (unsigned) monitor_e4_[2],
           monitor_e5_valid_ ? "" : "NA/", (unsigned) monitor_e5_[0], (unsigned) monitor_e5_[1],
           (unsigned) monitor_e5_[2], (unsigned) monitor_e5_[3], (unsigned) monitor_e5_[4],
           (unsigned) monitor_e5_[5], (unsigned) monitor_e5_[6], (unsigned) monitor_e5_[7]);
}

void ToshibaDiagnosticMonitorUart::log_monitor_bytes_(const std::vector<uint8_t> &raw_data,
                                                       int16_t response_register) const {
  const size_t chunks = (raw_data.size() + MONITOR_LOG_CHUNK_SIZE - 1) / MONITOR_LOG_CHUNK_SIZE;
  for (size_t chunk = 0; chunk < chunks; chunk++) {
    const size_t offset = chunk * MONITOR_LOG_CHUNK_SIZE;
    const size_t length = std::min(MONITOR_LOG_CHUNK_SIZE, raw_data.size() - offset);
    ESP_LOGI(TAG, "MONITOR RAW reg=0x%02X chunk=%u/%u offset=%u bytes=[%s]", (unsigned) response_register,
             (unsigned) (chunk + 1), (unsigned) chunks, (unsigned) offset,
             format_hex_pretty(raw_data.data() + offset, length).c_str());
  }
}

void ToshibaDiagnosticMonitorUart::log_monitor_da_(const std::vector<uint8_t> &raw_data) {
  size_t offset;
  size_t length;
  if (!monitor_payload_(raw_data, 0xDA, offset, length) || length < 130) {
    ESP_LOGI(TAG, "MONITOR DA decode unavailable payload=%u", (unsigned) length);
    return;
  }

  const uint8_t *payload = raw_data.data() + offset;
  const unsigned day = payload[2];
  if (day < 1 || day > 31) {
    return;
  }

  const uint32_t watt_hours = monitor_le32_(payload + 6 + (day - 1) * 4);
  const uint32_t now = millis();

  if (monitor_da_previous_valid_ && watt_hours >= monitor_da_previous_wh_) {
    const uint32_t delta_wh = watt_hours - monitor_da_previous_wh_;
    const uint32_t elapsed_ms = now - monitor_da_previous_ms_;
    const float inferred_watts = elapsed_ms ? delta_wh * 3600000.0f / elapsed_ms : 0.0f;
    ESP_LOGI(TAG, "MONITOR DA accumulated=%uWh delta=%uWh elapsed=%.1fs inferred=%.1fW hypothesis=energy",
             (unsigned) watt_hours, (unsigned) delta_wh, elapsed_ms / 1000.0f, inferred_watts);
  } else {
    ESP_LOGI(TAG, "MONITOR DA accumulated=%uWh baseline hypothesis=energy", (unsigned) watt_hours);
  }

  monitor_da_previous_valid_ = true;
  monitor_da_previous_wh_ = watt_hours;
  monitor_da_previous_ms_ = now;
}

void ToshibaDiagnosticMonitorUart::log_monitor_b7_partial_() const {
  if (monitor_b7_partial_.empty()) {
    return;
  }

  ESP_LOGI(TAG, "MONITOR B7 partial attempt=%s length=%u bytes=[%s]",
           monitor_b7_retry_active_ ? "retry" : "first", (unsigned) monitor_b7_partial_.size(),
           format_hex_pretty(monitor_b7_partial_).c_str());
}

}  // namespace toshiba_suzumi
}  // namespace esphome
