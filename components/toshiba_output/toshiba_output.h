#pragma once

#include <cmath>
#include "esphome/core/component.h"
#include "esphome/components/climate/climate.h"
#include "esphome/components/sensor/sensor.h"

namespace esphome {
namespace toshiba_output {

class ToshibaOutputEstimator : public PollingComponent {
 public:
  void set_climate(climate::Climate *value) { climate_ = value; }
  void set_idu_current_sensor(sensor::Sensor *value) { idu_current_ = value; }
  void set_branch_temperature_sensor(sensor::Sensor *value) { branch_temp_ = value; }
  void set_fan_speed_sensor(sensor::Sensor *value) { fan_speed_ = value; }
  void set_cooling_output_sensor(sensor::Sensor *value) { cooling_output_ = value; }
  void set_heating_output_sensor(sensor::Sensor *value) { heating_output_ = value; }
  void set_cooling_coefficient(float value) { cooling_coefficient_ = value; }
  void set_heating_coefficient(float value) { heating_coefficient_ = value; }

  void update() override {
    if (climate_ == nullptr || idu_current_ == nullptr || branch_temp_ == nullptr || fan_speed_ == nullptr)
      return;

    const float current = idu_current_->state;
    const float branch = branch_temp_->state;
    const float fan = fan_speed_->state;
    const float room = climate_->current_temperature;

    if (!std::isfinite(current) || !std::isfinite(branch) || !std::isfinite(fan) || !std::isfinite(room)) {
      if (cooling_output_ != nullptr) cooling_output_->publish_state(NAN);
      if (heating_output_ != nullptr) heating_output_->publish_state(NAN);
      return;
    }

    float cooling = 0.0f;
    float heating = 0.0f;

    if (current > 0.0f && fan > 0.0f) {
      if (climate_->mode == climate::CLIMATE_MODE_COOL || climate_->mode == climate::CLIMATE_MODE_DRY) {
        const float dt = room - branch;
        if (dt > 0.0f) cooling = cooling_coefficient_ * fan * dt;
      } else if (climate_->mode == climate::CLIMATE_MODE_HEAT) {
        const float dt = branch - room;
        if (dt > 0.0f) heating = heating_coefficient_ * fan * dt;
      } else if (climate_->mode == climate::CLIMATE_MODE_HEAT_COOL) {
        if (branch < room)
          cooling = cooling_coefficient_ * fan * (room - branch);
        else if (branch > room)
          heating = heating_coefficient_ * fan * (branch - room);
      }
    }

    if (cooling_output_ != nullptr) cooling_output_->publish_state(cooling);
    if (heating_output_ != nullptr) heating_output_->publish_state(heating);
  }

 protected:
  climate::Climate *climate_{nullptr};
  sensor::Sensor *idu_current_{nullptr};
  sensor::Sensor *branch_temp_{nullptr};
  sensor::Sensor *fan_speed_{nullptr};
  sensor::Sensor *cooling_output_{nullptr};
  sensor::Sensor *heating_output_{nullptr};
  float cooling_coefficient_{2.72f};
  float heating_coefficient_{3.264f};
};

}  // namespace toshiba_output
}  // namespace esphome
