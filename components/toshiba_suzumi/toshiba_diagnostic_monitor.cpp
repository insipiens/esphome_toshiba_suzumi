#include <algorithm>

#include "toshiba_climate.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome {
namespace toshiba_suzumi {

static const uint8_t MONITOR_REGISTERS[] = {
    0x81, 0x82, 0x86, 0x88, 0x89, 0x90, 0x92, 0x94,
    0x96, 0x97, 0x98, 0x99, 0x9A, 0xA4,
    0xB4, 0xB7, 0xB9, 0xBA,
    0xC0, 0xC6, 0xC7, 0xCA,
    0xD4, 0xD7,
    0xE2, 0xE3, 0xEE, 0xFE,
};
static const size_t MONITOR_REGISTER_COUNT = sizeof(MONITOR_REGISTERS) / sizeof(MONITOR_REGISTERS[0]);
static_assert(MONITOR_REGISTER_COUNT == 28, "diagnostic monitor register count");

static const uint32_t MONITOR_CYCLE_MS = 60000;
static const uint32_t MONITOR_RESPONSE_TIMEOUT_MS = 1000;
static const uint32_t MONITOR_B7_RETRY_TIMEOUT_MS = 2500;
static const uint32_t MONITOR_QUIET_MS = 250;
static const uint32_t F8_RESPONSE_TIMEOUT_MS = 1000;

static uint8_t diagnostic_checksum_(const std::vector<uint8_t> &bytes, size_t end) {
  uint8_t sum = 0;
  for (size_t i = 1; i < end; i++) sum += bytes[i];
  return 256 - sum;
}

static bool diagnostic_payload_(const std::vector<uint8_t> &raw, int16_t reg, size_t &offset, size_t &length) {
  const size_t size = raw.size();
  if (size < 2 || reg < 0) return false;

  size_t reg_offset = size;
  if ((size == 15 || size == 22) && raw[12] == reg)
    reg_offset = 12;
  else if (size > 14 && raw[3] == 0x90 && raw[14] == reg)
    reg_offset = 14;
  else if (size > 12 && raw[12] == reg)
    reg_offset = 12;

  if (reg_offset == size) return false;
  offset = reg_offset + 1;
  length = (size - 1) - offset;
  return offset <= size - 1;
}

bool ToshibaDiagnosticMonitorUart::has_f8_sensors_() const {
  return f8_byte_sensors_[0] != nullptr || f8_byte_sensors_[1] != nullptr ||
         f8_byte_sensors_[2] != nullptr || f8_byte_sensors_[3] != nullptr;
}

void ToshibaDiagnosticMonitorUart::update() {
  if (scan_active_) return;
  ToshibaClimateUart::update();
  if (has_f8_sensors_()) f8_poll_pending_ = true;
}

void ToshibaDiagnosticMonitorUart::begin_f8_request_() {
  std::vector<uint8_t> request = {2, 0, 3, 16, 0, 0, 6, 1, 48, 1, 0, 1, 0xF8};
  request.push_back(diagnostic_checksum_(request, request.size()));
  f8_poll_pending_ = false;
  f8_request_sent_ = true;
  f8_request_started_ = millis();
  f8_rx_message_.clear();
  send_to_uart(ToshibaCommand{ToshibaCommandType::IDU_COMPOSITE_STATUS, request, 0});
}

bool ToshibaDiagnosticMonitorUart::handle_f8_rx_byte_(uint8_t byte) {
  if (f8_rx_message_.empty() && byte != 0x02) return true;
  f8_rx_message_.push_back(byte);
  if (f8_rx_message_.size() < 7) return true;

  const size_t expected = 7 + f8_rx_message_[6];
  if (f8_rx_message_.size() < expected) return true;
  if (f8_rx_message_.size() > expected) {
    f8_rx_message_.clear();
    return true;
  }

  const uint8_t expected_checksum = diagnostic_checksum_(f8_rx_message_, expected - 1);
  if (f8_rx_message_.back() != expected_checksum) {
    ESP_LOGW(TAG, "F8 response checksum failed DATA=[%s]", format_hex_pretty(f8_rx_message_).c_str());
    f8_rx_message_.clear();
    return true;
  }

  const int16_t reg = extract_response_register_(f8_rx_message_);
  if (reg == 0xF8) {
    publish_f8_response_(f8_rx_message_);
    f8_request_sent_ = false;
  } else {
    parseResponse(f8_rx_message_);
  }
  f8_rx_message_.clear();
  return true;
}

void ToshibaDiagnosticMonitorUart::publish_f8_response_(const std::vector<uint8_t> &raw) {
  size_t offset = 0;
  size_t length = 0;
  if (!diagnostic_payload_(raw, 0xF8, offset, length) || length < 4) {
    ESP_LOGW(TAG, "F8 response has no four-byte payload DATA=[%s]", format_hex_pretty(raw).c_str());
    return;
  }

  for (size_t i = 0; i < 4; i++) {
    if (f8_byte_sensors_[i] != nullptr) f8_byte_sensors_[i]->publish_state(raw[offset + i]);
  }
  ESP_LOGD(TAG, "F8 raw bytes=[%u,%u,%u,%u]", (unsigned) raw[offset], (unsigned) raw[offset + 1],
           (unsigned) raw[offset + 2], (unsigned) raw[offset + 3]);
}

void ToshibaDiagnosticMonitorUart::loop() {
  while (available()) {
    uint8_t byte;
    read_byte(&byte);
    if (f8_request_sent_)
      handle_f8_rx_byte_(byte);
    else
      handle_rx_byte_(byte);
  }

  const uint32_t now = millis();
  if (f8_request_sent_ && now - f8_request_started_ >= F8_RESPONSE_TIMEOUT_MS) {
    ESP_LOGW(TAG, "F8 response timeout");
    f8_request_sent_ = false;
    f8_rx_message_.clear();
  }

  if (scan_active_) {
    if (!scan_request_sent_ && !command_queue_.empty()) {
      scan_started_ = false;
      process_command_queue_();
    }
  } else if (!f8_request_sent_) {
    process_command_queue_();
  }

  if (!scan_active_ && f8_poll_pending_ && !f8_request_sent_ && command_queue_.empty() && rx_message_.empty()) {
    const uint32_t last_bus_activity = std::max(last_command_timestamp_, last_rx_char_timestamp_);
    if (now - last_bus_activity >= MONITOR_QUIET_MS) begin_f8_request_();
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
    monitor_b7_partial_.clear();
    monitor_last_unrelated_packet_.clear();
    ESP_LOGI(TAG, "========== TOSHIBA REDUCED REGISTER MONITOR STARTED ==========");
    ESP_LOGI(TAG, "registers=%u cadence=60s read_only=YES", (unsigned) MONITOR_REGISTER_COUNT);
    start_monitor_cycle_(millis());
    return;
  }

  if (!scan_active_) return;
  monitor_stop_requested_ = true;
  if (!scan_request_sent_) finish_monitor_();
}

void ToshibaDiagnosticMonitorUart::reset_monitor_cycle_state_() {
  monitor_cycle_attempted_ = 0;
  monitor_cycle_matched_ = 0;
  monitor_cycle_timeouts_ = 0;
  monitor_cycle_requests_ = 0;
  monitor_cycle_unrelated_ = 0;
  monitor_cycle_unrelated_repeated_ = 0;
  monitor_cycle_unrelated_changed_ = 0;
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
  ESP_LOGI(TAG, "MONITOR cycle=%u started registers=%u", (unsigned) monitor_cycle_number_,
           (unsigned) MONITOR_REGISTER_COUNT);
}

void ToshibaDiagnosticMonitorUart::process_scan_() {
  if (!scan_active_) return;
  const uint32_t now = millis();

  if (monitor_stop_requested_ && !scan_request_sent_) {
    finish_monitor_();
    return;
  }

  if (monitor_waiting_for_cycle_) {
    if (now - monitor_cycle_started_ < MONITOR_CYCLE_MS) return;
    start_monitor_cycle_(now);
  }

  if (!command_queue_.empty() || !rx_message_.empty() || f8_request_sent_) return;

  if (monitor_b7_retry_pending_) {
    const uint32_t last_bus_activity = std::max(last_command_timestamp_, last_rx_char_timestamp_);
    if (now - scan_register_started_ < MONITOR_QUIET_MS || now - last_bus_activity < MONITOR_QUIET_MS) return;
    monitor_b7_retry_pending_ = false;
    monitor_b7_retry_active_ = true;
    scan_matched_response_ = false;
    scan_last_packet_timestamp_ = 0;
    scan_register_started_ = now;
    scan_request_sent_ = true;
    monitor_b7_retries_++;
    monitor_b7_partial_.clear();
    send_monitor_request_();
    return;
  }

  if (!scan_request_sent_) {
    const uint32_t last_bus_activity = std::max(last_command_timestamp_, last_rx_char_timestamp_);
    if (now - last_bus_activity < MONITOR_QUIET_MS) return;
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
    if (now - scan_last_packet_timestamp_ >= MONITOR_QUIET_MS) complete_monitor_request_();
    return;
  }

  uint32_t timeout = MONITOR_RESPONSE_TIMEOUT_MS;
  if (scan_register_ == 0xB7 && monitor_b7_retry_active_) timeout = MONITOR_B7_RETRY_TIMEOUT_MS;
  if (now - scan_register_started_ >= timeout) handle_monitor_timeout_();
}

void ToshibaDiagnosticMonitorUart::send_monitor_request_() {
  std::vector<uint8_t> request = {2, 0, 3, 16, 0, 0, 6, 1, 48, 1, 0, 1, scan_register_};
  request.push_back(diagnostic_checksum_(request, request.size()));
  monitor_requests_++;
  monitor_cycle_requests_++;
  send_to_uart(ToshibaCommand{static_cast<ToshibaCommandType>(scan_register_), request, 0});
}

void ToshibaDiagnosticMonitorUart::handle_monitor_timeout_() {
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

  if (scan_register_ == 0xB7 && monitor_b7_retry_active_) monitor_b7_retry_timeouts_++;
  complete_monitor_request_();
}

void ToshibaDiagnosticMonitorUart::complete_monitor_request_() {
  monitor_registers_attempted_++;
  monitor_cycle_attempted_++;

  if (scan_matched_response_) {
    monitor_matched_++;
    monitor_cycle_matched_++;
    if (scan_register_ == 0xB7 && monitor_b7_retry_active_) monitor_b7_retry_matches_++;
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
  if (!scan_active_) return;
  scan_active_ = false;
  scan_started_ = false;
  scan_request_sent_ = false;
  scan_matched_response_ = false;
  monitor_stop_requested_ = false;
  monitor_waiting_for_cycle_ = false;
  monitor_b7_retry_pending_ = false;
  monitor_b7_retry_active_ = false;
  rx_message_.clear();
  ESP_LOGI(TAG, "MONITOR stopped registers=%u requests=%u matched=%u timeouts=%u cycles=%u",
           (unsigned) monitor_registers_attempted_, (unsigned) monitor_requests_, (unsigned) monitor_matched_,
           (unsigned) monitor_timeouts_, (unsigned) monitor_cycles_completed_);
  getInitData();
}

void ToshibaDiagnosticMonitorUart::log_scan_packet_(const std::vector<uint8_t> &raw) {
  const int16_t reg = extract_response_register_(raw);
  scan_last_packet_timestamp_ = millis();

  if (reg != scan_register_) {
    monitor_unrelated_++;
    monitor_cycle_unrelated_++;
    if (raw == monitor_last_unrelated_packet_) {
      monitor_cycle_unrelated_repeated_++;
      return;
    }
    monitor_cycle_unrelated_changed_++;
    monitor_last_unrelated_packet_ = raw;
    ESP_LOGI(TAG, "MONITOR UNSOLICITED request=0x%02X response=%d DATA=[%s]", (unsigned) scan_register_,
             (int) reg, format_hex_pretty(raw).c_str());
    return;
  }

  scan_matched_response_ = true;
  size_t offset = 0;
  size_t length = 0;
  if (diagnostic_payload_(raw, reg, offset, length)) {
    ESP_LOGI(TAG, "MONITOR cycle=%u reg=0x%02X payload=[%s]", (unsigned) monitor_cycle_number_,
             (unsigned) reg, format_hex_pretty(raw.data() + offset, length).c_str());
  } else {
    ESP_LOGI(TAG, "MONITOR cycle=%u reg=0x%02X response=[%s]", (unsigned) monitor_cycle_number_,
             (unsigned) reg, format_hex_pretty(raw).c_str());
  }
}

void ToshibaDiagnosticMonitorUart::log_monitor_cycle_summary_() const {
  const uint32_t duration_ms = millis() - monitor_cycle_started_;
  const uint32_t wait_ms = duration_ms < MONITOR_CYCLE_MS ? MONITOR_CYCLE_MS - duration_ms : 0;
  ESP_LOGI(TAG,
           "MONITOR cycle=%u complete attempted=%u/%u requests=%u matched=%u timeouts=%u "
           "unsolicited=%u changed=%u repeated=%u duration=%.1fs next_in=%.1fs",
           (unsigned) monitor_cycle_number_, (unsigned) monitor_cycle_attempted_,
           (unsigned) MONITOR_REGISTER_COUNT, (unsigned) monitor_cycle_requests_,
           (unsigned) monitor_cycle_matched_, (unsigned) monitor_cycle_timeouts_,
           (unsigned) monitor_cycle_unrelated_, (unsigned) monitor_cycle_unrelated_changed_,
           (unsigned) monitor_cycle_unrelated_repeated_, duration_ms / 1000.0f, wait_ms / 1000.0f);
}

void ToshibaDiagnosticMonitorUart::log_monitor_b7_partial_() const {
  if (monitor_b7_partial_.empty()) return;
  ESP_LOGI(TAG, "MONITOR B7 partial attempt=%s length=%u bytes=[%s]",
           monitor_b7_retry_active_ ? "retry" : "first", (unsigned) monitor_b7_partial_.size(),
           format_hex_pretty(monitor_b7_partial_).c_str());
}

}  // namespace toshiba_suzumi
}  // namespace esphome