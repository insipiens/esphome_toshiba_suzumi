#pragma once

#include "esphome/core/component.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/climate/climate.h"
#include "esphome/components/uart/uart.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/select/select.h"
#include "toshiba_climate_mode.h"

namespace esphome {
namespace time {
class RealTimeClock;
}  // namespace time
namespace toshiba_suzumi {

static const char *const TAG = "ToshibaClimateUart";
// default max temp for units
static const uint8_t MAX_TEMP = 30;
// default min temp for units without 8° heating mode
static const uint8_t MIN_TEMP_STANDARD = 17;
static const uint8_t SPECIAL_TEMP_OFFSET = 16;
static const uint8_t SPECIAL_MODE_EIGHT_DEG_MIN_TEMP = 5;
static const uint8_t SPECIAL_MODE_EIGHT_DEG_MAX_TEMP = 13;
static const uint8_t SPECIAL_MODE_EIGHT_DEG_DEF_TEMP = 8;
static const uint8_t NORMAL_MODE_DEF_TEMP = 20;

static const std::vector<uint8_t> HANDSHAKE[6] = {
    {2, 255, 255, 0, 0, 0, 0, 2},       {2, 255, 255, 1, 0, 0, 1, 2, 254}, {2, 0, 0, 0, 0, 0, 2, 2, 2, 250},
    {2, 0, 1, 129, 1, 0, 2, 0, 0, 123}, {2, 0, 1, 2, 0, 0, 2, 0, 0, 254},  {2, 0, 2, 0, 0, 0, 0, 254},
};

static const std::vector<uint8_t> AFTER_HANDSHAKE[2] = {
    {2, 0, 2, 1, 0, 0, 2, 0, 0, 251},
    {2, 0, 2, 2, 0, 0, 2, 0, 0, 250},
};

struct ToshibaCommand {
  ToshibaCommandType cmd;
  std::vector<uint8_t> payload;
  int delay;
};

class ToshibaClimateUart : public PollingComponent, public climate::Climate, public uart::UARTDevice {
 public:
  ToshibaClimateUart();

  void setup() override;
  void loop() override;
  void dump_config() override;
  void update() override;
  virtual void scan();
  virtual void set_scan_enabled(bool enabled) {
    if (enabled) {
      this->scan();
    }
  }
  virtual bool is_scan_enabled() const { return this->scan_active_; }
  void set_wifi_led(bool enabled);
  float get_setup_priority() const override { return setup_priority::LATE; }

  void set_indoor_temp_sensor(sensor::Sensor *indoor_temp_sensor) { indoor_temp_sensor_ = indoor_temp_sensor; }
  void set_outdoor_temp_sensor(sensor::Sensor *outdoor_temp_sensor) { outdoor_temp_sensor_ = outdoor_temp_sensor; }
  void set_odu_discharge_temp_sensor(sensor::Sensor *sensor) { odu_discharge_temp_sensor_ = sensor; }
  void set_odu_suction_temp_sensor(sensor::Sensor *sensor) { odu_suction_temp_sensor_ = sensor; }
  void set_odu_heat_exchanger_temp_sensor(sensor::Sensor *sensor) { odu_heat_exchanger_temp_sensor_ = sensor; }
  void set_compressor_load_sensor(sensor::Sensor *sensor) { compressor_load_sensor_ = sensor; }
  void set_compressor_current_sensor(sensor::Sensor *sensor) { compressor_current_sensor_ = sensor; }
  void set_idu_heat_exchanger_temp_sensor(sensor::Sensor *sensor) { idu_heat_exchanger_temp_sensor_ = sensor; }
  void set_idu_junction_temp_sensor(sensor::Sensor *sensor) { idu_junction_temp_sensor_ = sensor; }
  void set_idu_fan_speed_sensor(sensor::Sensor *sensor) { idu_fan_speed_sensor_ = sensor; }
  void set_time(time::RealTimeClock *time) { time_ = time; }
  void set_energy_sensor(sensor::Sensor *sensor) { energy_sensor_ = sensor; }
  void set_power_sensor(sensor::Sensor *sensor) { power_sensor_ = sensor; }
  void set_pwr_select(select::Select *pws_select) { pwr_select_ = pws_select; }
  void set_vertical_air_direction_select(select::Select *vertical_air_direction_select) {
    vertical_air_direction_select_ = vertical_air_direction_select;
  }
  void set_self_clean_sensor(binary_sensor::BinarySensor *self_clean_sensor) { self_clean_sensor_ = self_clean_sensor; }
  void set_horizontal_swing(bool enabled) { horizontal_swing_ = enabled; }
  void disable_heat_mode(bool disabled) { heat_mode_disabled_ = disabled; }
  void disable_wifi_led(bool disabled) { wifi_led_disabled_ = disabled; }
  void set_supported_presets(const std::vector<const char *> &presets) { supported_presets_ = presets; }
  void set_min_temp(uint8_t min_temp) { min_temp_ = min_temp; }
  void set_time_sync_interval(uint32_t interval) { time_sync_interval_ = interval; }

 protected:
  /// Override control to change settings of the climate device.
  void control(const climate::ClimateCall &call) override;

  /// Return the traits of this controller.
  climate::ClimateTraits traits() override;

  std::vector<uint8_t> rx_message_;
  std::vector<ToshibaCommand> command_queue_;
  uint32_t last_command_timestamp_ = 0;
  uint32_t last_rx_char_timestamp_ = 0;
  STATE power_state_ = STATE::OFF;
  // True while the unit is running its post-shutdown self-cleaning cycle.
  bool self_clean_running_ = false;
  optional<SPECIAL_MODE> special_mode_ = SPECIAL_MODE::STANDARD;
  select::Select *pwr_select_ = nullptr;
  select::Select *vertical_air_direction_select_ = nullptr;
  binary_sensor::BinarySensor *self_clean_sensor_ = nullptr;
  sensor::Sensor *indoor_temp_sensor_ = nullptr;
  sensor::Sensor *outdoor_temp_sensor_ = nullptr;
  sensor::Sensor *odu_discharge_temp_sensor_ = nullptr;
  sensor::Sensor *odu_suction_temp_sensor_ = nullptr;
  sensor::Sensor *odu_heat_exchanger_temp_sensor_ = nullptr;
  sensor::Sensor *compressor_load_sensor_ = nullptr;
  sensor::Sensor *compressor_current_sensor_ = nullptr;
  sensor::Sensor *idu_heat_exchanger_temp_sensor_ = nullptr;
  sensor::Sensor *idu_junction_temp_sensor_ = nullptr;
  sensor::Sensor *idu_fan_speed_sensor_ = nullptr;
  time::RealTimeClock *time_ = nullptr;
  sensor::Sensor *energy_sensor_ = nullptr;
  sensor::Sensor *power_sensor_ = nullptr;
  bool horizontal_swing_ = false;
  uint8_t min_temp_ = 17; // default min temp for units without 8° heating mode
  bool heat_mode_disabled_ = false;
  bool wifi_led_disabled_ = false;
  std::vector<const char*> supported_presets_;
  uint32_t last_time_sync_ = 0;
  uint32_t last_energy_sync_ = 0;
  uint32_t last_total_daily_energy_ = 0;
  uint32_t last_energy_update_ms_ = 0;
  uint16_t daily_energy_usage_[24] = {0};
  bool time_synced_ = false;
  uint32_t time_sync_interval_{86400000};

  bool scan_active_ = false;
  bool scan_started_ = false;
  bool scan_request_sent_ = false;
  bool scan_matched_response_ = false;
  uint8_t scan_register_ = 0x80;
  uint32_t scan_register_started_ = 0;
  uint32_t scan_last_packet_timestamp_ = 0;
  uint16_t scan_tested_ = 0;
  uint16_t scan_matched_ = 0;
  uint16_t scan_no_match_ = 0;

  void enqueue_command_(const ToshibaCommand &command);
  void send_to_uart(const ToshibaCommand command);
  void start_handshake();
  void parseResponse(std::vector<uint8_t> rawData);
  void requestData(ToshibaCommandType cmd);
  void process_command_queue_();
  void sendCmd(ToshibaCommandType cmd, uint8_t value);
  void getInitData();
  void handle_rx_byte_(uint8_t c);
  bool validate_message_();
  void set_self_clean_running_(bool running);
  void on_set_pwr_level(const std::string &value);
  void on_set_vertical_air_direction(const std::string &value);
  void publish_vertical_air_direction_(SWING swing_mode);
  void configure_supported_custom_modes_();
  virtual void process_scan_();
  void send_scan_request_();
  void complete_scan_register_();
  void finish_scan_();
  virtual void log_scan_packet_(const std::vector<uint8_t> &raw_data);
  int16_t extract_response_register_(const std::vector<uint8_t> &raw_data) const;
  void log_scan_ascii_(const std::vector<uint8_t> &raw_data) const;
#ifdef USE_TIME
  void check_time_sync_(uint32_t now);
  void sync_time_();
#endif
  void sync_energy_();
  void estimate_wattage_(uint32_t current_energy);

  friend class ToshibaPwrModeSelect;
  friend class ToshibaVerticalAirDirectionSelect;
};

class ToshibaDiagnosticMonitorUart : public ToshibaClimateUart {
 public:
  void scan() override { this->set_scan_enabled(true); }
  void set_scan_enabled(bool enabled) override;
  bool is_scan_enabled() const override { return this->scan_active_ && !this->monitor_stop_requested_; }

 protected:
  void process_scan_() override;
  void log_scan_packet_(const std::vector<uint8_t> &raw_data) override;

 private:
  bool monitor_stop_requested_ = false;
  uint8_t monitor_register_index_ = 0;
  uint32_t monitor_requests_ = 0;
  uint32_t monitor_matched_ = 0;
  uint32_t monitor_timeouts_ = 0;
  uint32_t monitor_cycles_completed_ = 0;

  void send_monitor_request_();
  void complete_monitor_request_();
  void finish_monitor_();
  void log_monitor_bytes_(const std::vector<uint8_t> &raw_data) const;
  void log_monitor_decoded_(const std::vector<uint8_t> &raw_data, int16_t response_register) const;
};

class ToshibaPwrModeSelect : public select::Select, public esphome::Parented<ToshibaClimateUart> {
 protected:
  virtual void control(const std::string &value) override;
};

class ToshibaVerticalAirDirectionSelect : public select::Select, public esphome::Parented<ToshibaClimateUart> {
 protected:
  virtual void control(const std::string &value) override;
};

}  // namespace toshiba_suzumi
}  // namespace esphome
