#include "toshiba_climate.h"
#include "esphome/core/log.h"

namespace esphome {
namespace toshiba_suzumi {

static const uint8_t MONITOR_REGISTERS[] = {0xCA, 0xEE, 0xE5};
static const size_t MONITOR_REGISTER_COUNT = sizeof(MONITOR_REGISTERS) / sizeof(MONITOR_REGISTERS[0]);
static const uint32_t MONITOR_RESPONSE_TIMEOUT = 1000;
static const uint32_t MONITOR_QUIET_PERIOD = 250;
static const size_t MONITOR_LOG_CHUNK_BYTES = 32;

void ToshibaDiagnosticMonitorUart::set_scan_enabled(bool enabled) {
  if (enabled) {
    if (this->scan_active_) {
      this->monitor_stop_requested_ = false;
      ESP_LOGI(TAG, "Focused diagnostic monitor already running.");
      return;
    }

    this->scan_active_ = true;
    this->scan_started_ = false;
    this->scan_request_sent_ = false;
    this->scan_matched_response_ = false;
    this->scan_register_ = MONITOR_REGISTERS[0];
    this->scan_register_started_ = 0;
    this->scan_last_packet_timestamp_ = 0;
    this->monitor_stop_requested_ = false;
    this->monitor_register_index_ = 0;
    this->monitor_requests_ = 0;
    this->monitor_matched_ = 0;
    this->monitor_timeouts_ = 0;
    this->monitor_cycles_completed_ = 0;

    ESP_LOGI(TAG, "========== TOSHIBA FOCUSED MONITOR STARTED ==========");
    ESP_LOGI(TAG, "Read-only registers: 0xCA, 0xEE, 0xE5");
    ESP_LOGI(TAG, "Normal periodic polling paused until the monitor is disabled.");
    ESP_LOGI(TAG, "Waiting for the command queue to become idle...");
    return;
  }

  if (!this->scan_active_) {
    ESP_LOGI(TAG, "Focused diagnostic monitor is already stopped.");
    return;
  }

  this->monitor_stop_requested_ = true;
  ESP_LOGI(TAG, "Focused diagnostic monitor stop requested; finishing the current read.");

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

    this->scan_matched_response_ = false;
    this->scan_last_packet_timestamp_ = 0;
    this->scan_register_started_ = now;
    this->scan_started_ = true;
    this->scan_request_sent_ = true;
    this->scan_register_ = MONITOR_REGISTERS[this->monitor_register_index_];
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
  // Proven read-request envelope. Scan mode never constructs or sends write frames.
  std::vector<uint8_t> payload = {2, 0, 3, 16, 0, 0, 6, 1, 48, 1, 0, 1};
  payload.push_back(this->scan_register_);
  payload.push_back(checksum(payload, payload.size()));
  this->monitor_requests_++;
  this->send_to_uart(
      ToshibaCommand{.cmd = static_cast<ToshibaCommandType>(this->scan_register_), .payload = payload});
}

void ToshibaDiagnosticMonitorUart::complete_monitor_request_() {
  if (this->scan_matched_response_) {
    this->monitor_matched_++;
  } else {
    this->monitor_timeouts_++;
    ESP_LOGI(TAG, "MONITOR request=0x%02X timeout", static_cast<unsigned>(this->scan_register_));
  }

  this->scan_request_sent_ = false;

  if (this->monitor_stop_requested_) {
    this->finish_monitor_();
    return;
  }

  this->monitor_register_index_++;
  if (this->monitor_register_index_ >= MONITOR_REGISTER_COUNT) {
    this->monitor_register_index_ = 0;
    this->monitor_cycles_completed_++;
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

  ESP_LOGI(TAG, "========== TOSHIBA FOCUSED MONITOR STOPPED ==========");
  ESP_LOGI(TAG, "Requests: %u", static_cast<unsigned>(this->monitor_requests_));
  ESP_LOGI(TAG, "Matched responses: %u", static_cast<unsigned>(this->monitor_matched_));
  ESP_LOGI(TAG, "Timeouts: %u", static_cast<unsigned>(this->monitor_timeouts_));
  ESP_LOGI(TAG, "Completed cycles: %u", static_cast<unsigned>(this->monitor_cycles_completed_));
  ESP_LOGI(TAG, "Normal polling resumed.");
  ESP_LOGI(TAG, "=====================================================");

  this->getInitData();
}

void ToshibaDiagnosticMonitorUart::log_scan_packet_(const std::vector<uint8_t> &raw_data) {
  const int16_t response_register = this->extract_response_register_(raw_data);
  const bool matched = response_register == this->scan_register_;
  const size_t length = raw_data.size();

  this->scan_last_packet_timestamp_ = millis();
  if (matched) {
    this->scan_matched_response_ = true;
  }

  const uint8_t envelope = length > 3 ? raw_data[3] : 0;
  const uint8_t header_4 = length > 4 ? raw_data[4] : 0;
  const uint8_t header_5 = length > 5 ? raw_data[5] : 0;
  const uint8_t declared_length = length > 6 ? raw_data[6] : 0;
  const uint8_t route_7 = length > 7 ? raw_data[7] : 0;
  const uint8_t route_8 = length > 8 ? raw_data[8] : 0;
  const uint8_t route_9 = length > 9 ? raw_data[9] : 0;
  const uint8_t route_10 = length > 10 ? raw_data[10] : 0;
  const uint8_t operation = length > 11 ? raw_data[11] : 0;

  if (response_register >= 0) {
    ESP_LOGI(TAG,
             "MONITOR request=0x%02X response=0x%02X matched=%s length=%u checksum=OK "
             "header=[%02X %02X %02X len=%02X %02X %02X %02X %02X op=%02X]",
             static_cast<unsigned>(this->scan_register_), static_cast<unsigned>(response_register),
             matched ? "YES" : "NO", static_cast<unsigned>(length), envelope, header_4, header_5,
             declared_length, route_7, route_8, route_9, route_10, operation);
  } else {
    ESP_LOGI(TAG,
             "MONITOR request=0x%02X response=unknown matched=NO length=%u checksum=OK "
             "header=[%02X %02X %02X len=%02X %02X %02X %02X %02X op=%02X]",
             static_cast<unsigned>(this->scan_register_), static_cast<unsigned>(length), envelope, header_4,
             header_5, declared_length, route_7, route_8, route_9, route_10, operation);
  }

  this->log_monitor_bytes_(raw_data);
  this->log_monitor_decoded_(raw_data, response_register);
  this->log_scan_ascii_(raw_data);
}

void ToshibaDiagnosticMonitorUart::log_monitor_bytes_(const std::vector<uint8_t> &raw_data) const {
  for (size_t start = 0; start < raw_data.size(); start += MONITOR_LOG_CHUNK_BYTES) {
    const size_t end = std::min(start + MONITOR_LOG_CHUNK_BYTES, raw_data.size());
    std::string hex;
    hex.reserve((end - start) * 3);
    char byte_text[4];

    for (size_t index = start; index < end; index++) {
      snprintf(byte_text, sizeof(byte_text), "%02X", raw_data[index]);
      if (!hex.empty()) {
        hex.push_back('.');
      }
      hex.append(byte_text);
    }

    ESP_LOGI(TAG, "MONITOR DATA[%03u:%03u]=[%s]", static_cast<unsigned>(start),
             static_cast<unsigned>(end - 1), hex.c_str());
  }
}

void ToshibaDiagnosticMonitorUart::log_monitor_decoded_(const std::vector<uint8_t> &raw_data,
                                                         int16_t response_register) const {
  if (response_register == 0xCA) {
    const size_t offset = (raw_data.size() > 14 && raw_data[3] == 0x90) ? 15 : 13;
    if (raw_data.size() >= offset + 5) {
      const uint16_t word_1 = static_cast<uint16_t>(raw_data[offset + 1]) |
                              (static_cast<uint16_t>(raw_data[offset + 2]) << 8);
      const uint16_t word_2 = static_cast<uint16_t>(raw_data[offset + 3]) |
                              (static_cast<uint16_t>(raw_data[offset + 4]) << 8);
      ESP_LOGI(TAG, "MONITOR 0xCA payload=%02X %02X %02X %02X %02X le16=[%u,%u]",
               raw_data[offset], raw_data[offset + 1], raw_data[offset + 2], raw_data[offset + 3],
               raw_data[offset + 4], static_cast<unsigned>(word_1), static_cast<unsigned>(word_2));
    }
    return;
  }

  if (response_register == 0xEE) {
    const size_t offset = (raw_data.size() > 14 && raw_data[3] == 0x90) ? 15 : 13;
    if (raw_data.size() >= offset + 2) {
      const uint16_t word = static_cast<uint16_t>(raw_data[offset]) |
                            (static_cast<uint16_t>(raw_data[offset + 1]) << 8);
      ESP_LOGI(TAG, "MONITOR 0xEE payload=%02X %02X le16=%u", raw_data[offset], raw_data[offset + 1],
               static_cast<unsigned>(word));
    }
    return;
  }

  if (response_register == static_cast<int16_t>(ToshibaCommandType::ODU_STATUS)) {
    const size_t offset = raw_data.size() == 22 ? 13 : 15;
    if (raw_data.size() >= offset + 8) {
      const int8_t discharge = static_cast<int8_t>(raw_data[offset]);
      const int8_t suction = static_cast<int8_t>(raw_data[offset + 1]);
      const int8_t heat_exchanger = static_cast<int8_t>(raw_data[offset + 2]);
      const uint8_t load_raw = raw_data[offset + 3];
      const uint8_t unknown_4 = raw_data[offset + 4];
      const uint8_t unknown_5 = raw_data[offset + 5];
      const uint8_t current_raw = raw_data[offset + 6];
      const uint8_t unknown_7 = raw_data[offset + 7];
      ESP_LOGI(TAG,
               "MONITOR 0xE5 discharge=%dC suction=%dC exchanger=%dC load_raw=%u load=%.1f%% "
               "unknown=[%u,%u,%u] current_raw=%u current=%.1fA",
               discharge, suction, heat_exchanger, static_cast<unsigned>(load_raw), load_raw / 1.7f,
               static_cast<unsigned>(unknown_4), static_cast<unsigned>(unknown_5),
               static_cast<unsigned>(unknown_7), static_cast<unsigned>(current_raw), current_raw / 10.0f);
    }
  }
}

}  // namespace toshiba_suzumi
}  // namespace esphome
