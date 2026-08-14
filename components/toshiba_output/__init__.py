import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import climate, sensor
from esphome.const import (
    CONF_ID,
    DEVICE_CLASS_POWER,
    STATE_CLASS_MEASUREMENT,
    UNIT_WATT,
)

CONF_CLIMATE_ID = "climate_id"
CONF_IDU_CURRENT = "idu_current"
CONF_BRANCH_TEMPERATURE = "branch_temperature"
CONF_FAN_SPEED = "fan_speed"
CONF_COOLING_OUTPUT = "cooling_output"
CONF_HEATING_OUTPUT = "heating_output"
CONF_COOLING_COEFFICIENT = "cooling_coefficient"
CONF_HEATING_COEFFICIENT = "heating_coefficient"

DEPENDENCIES = ["climate", "sensor"]

toshiba_output_ns = cg.esphome_ns.namespace("toshiba_output")
ToshibaOutputEstimator = toshiba_output_ns.class_(
    "ToshibaOutputEstimator", cg.PollingComponent
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(ToshibaOutputEstimator),
        cv.Required(CONF_CLIMATE_ID): cv.use_id(climate.Climate),
        cv.Optional(CONF_IDU_CURRENT): cv.use_id(sensor.Sensor),
        cv.Required(CONF_BRANCH_TEMPERATURE): cv.use_id(sensor.Sensor),
        cv.Required(CONF_FAN_SPEED): cv.use_id(sensor.Sensor),
        cv.Optional(CONF_COOLING_OUTPUT): sensor.sensor_schema(
            unit_of_measurement=UNIT_WATT,
            accuracy_decimals=0,
            device_class=DEVICE_CLASS_POWER,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_HEATING_OUTPUT): sensor.sensor_schema(
            unit_of_measurement=UNIT_WATT,
            accuracy_decimals=0,
            device_class=DEVICE_CLASS_POWER,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        # Cooling coefficient fitted from the August 2026 B13J2FVG
        # air-side calibration: W / (raw air-velocity register count * K).
        # The Toshiba register is retained on its raw scale; measurements show
        # approximately 0.1 m/s per count rather than physical fan RPM.
        cv.Optional(CONF_COOLING_COEFFICIENT, default=2.72): cv.positive_float,
        # Initial heating value scales the cooling coefficient by the B13's
        # rated 4.2 / 3.5 kW heating/cooling capacity ratio. Recalibrate in heat.
        cv.Optional(CONF_HEATING_COEFFICIENT, default=3.264): cv.positive_float,
    }
).extend(cv.polling_component_schema("5s"))


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    climate_var = await cg.get_variable(config[CONF_CLIMATE_ID])
    branch_var = await cg.get_variable(config[CONF_BRANCH_TEMPERATURE])
    fan_var = await cg.get_variable(config[CONF_FAN_SPEED])

    cg.add(var.set_climate(climate_var))
    cg.add(var.set_branch_temperature_sensor(branch_var))
    cg.add(var.set_fan_speed_sensor(fan_var))
    cg.add(var.set_cooling_coefficient(config[CONF_COOLING_COEFFICIENT]))
    cg.add(var.set_heating_coefficient(config[CONF_HEATING_COEFFICIENT]))

    # Retained only as an optional diagnostic reference. It is deliberately
    # not used to gate thermal output because delivered cooling has been
    # observed while Toshiba reports IDU current as zero.
    if CONF_IDU_CURRENT in config:
        current_var = await cg.get_variable(config[CONF_IDU_CURRENT])
        cg.add(var.set_idu_current_sensor(current_var))

    if CONF_COOLING_OUTPUT in config:
        sens = await sensor.new_sensor(config[CONF_COOLING_OUTPUT])
        cg.add(var.set_cooling_output_sensor(sens))

    if CONF_HEATING_OUTPUT in config:
        sens = await sensor.new_sensor(config[CONF_HEATING_OUTPUT])
        cg.add(var.set_heating_output_sensor(sens))
