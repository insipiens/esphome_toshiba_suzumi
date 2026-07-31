import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor, sensor, climate, uart, select
from esphome.const import (
    CONF_ID,
    CONF_DISABLED_BY_DEFAULT,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    UNIT_CELSIUS,
    UNIT_PERCENT,
    UNIT_AMPERE,
    UNIT_WATT_HOURS,
    UNIT_WATT,
    DEVICE_CLASS_TEMPERATURE,
    DEVICE_CLASS_CURRENT,
    DEVICE_CLASS_RUNNING,
    DEVICE_CLASS_ENERGY,
    DEVICE_CLASS_POWER,
    CONF_TIME_ID,
    STATE_CLASS_TOTAL_INCREASING,
)

DEPENDENCIES = ["uart"]
AUTO_LOAD = ["binary_sensor", "sensor", "select"]

CONF_INDOOR_TEMP = "indoor_temp"
CONF_OUTDOOR_TEMP = "outdoor_temp"
CONF_ODU_DISCHARGE_TEMP = "odu_discharge_temp"
CONF_ODU_SUCTION_TEMP = "odu_suction_temp"
CONF_ODU_HEAT_EXCHANGER_TEMP = "odu_heat_exchanger_temp"
CONF_COMPRESSOR_LOAD = "compressor_load"
CONF_COMPRESSOR_CURRENT = "compressor_current"
CONF_IDU_HEAT_EXCHANGER_TEMP = "idu_heat_exchanger_temp"
CONF_IDU_JUNCTION_TEMP = "idu_junction_temp"
CONF_IDU_FAN_SPEED = "idu_fan_speed"
CONF_F8_BYTE_1 = "f8_byte_1"
CONF_F8_BYTE_2 = "f8_byte_2"
CONF_F8_BYTE_3 = "f8_byte_3"
CONF_F8_BYTE_4 = "f8_byte_4"
CONF_PWR_SELECT = "power_select"
CONF_VERTICAL_AIR_DIRECTION = "vertical_air_direction"
CONF_SPECIAL_MODE = "special_mode"
CONF_SPECIAL_MODE_MODES = "modes"
CONF_SUPPORTED_PRESETS = "supported_presets"
CONF_SELF_CLEAN = "self_clean"
CONF_TIME_SYNC_INTERVAL = "time_sync_interval"
CONF_ENERGY = "energy"
CONF_POWER = "power"

FEATURE_HORIZONTAL_SWING = "horizontal_swing"
MIN_TEMP = "min_temp"
DISABLE_HEAT_MODE = "disable_heat_mode"
DISABLE_WIFI_LED = "disable_wifi_led"

toshiba_ns = cg.esphome_ns.namespace("toshiba_suzumi")
ToshibaClimateUart = toshiba_ns.class_("ToshibaDiagnosticMonitorUart", cg.PollingComponent, climate.Climate, uart.UARTDevice)
ToshibaPwrModeSelect = toshiba_ns.class_("ToshibaPwrModeSelect", select.Select)
ToshibaSpecialModeSelect = toshiba_ns.class_("ToshibaSpecialModeSelect", select.Select)
ToshibaVerticalAirDirectionSelect = toshiba_ns.class_("ToshibaVerticalAirDirectionSelect", select.Select)

raw_f8_sensor_schema = sensor.sensor_schema(
    accuracy_decimals=0,
    state_class=STATE_CLASS_MEASUREMENT,
    entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
).extend({cv.Optional(CONF_DISABLED_BY_DEFAULT, default=True): cv.boolean})

CONFIG_SCHEMA = climate.climate_schema(ToshibaClimateUart).extend(
    {
        cv.GenerateID(): cv.declare_id(ToshibaClimateUart),
        cv.Optional(CONF_INDOOR_TEMP): sensor.sensor_schema(unit_of_measurement=UNIT_CELSIUS, accuracy_decimals=0, device_class=DEVICE_CLASS_TEMPERATURE, state_class=STATE_CLASS_MEASUREMENT),
        cv.Optional(CONF_OUTDOOR_TEMP): sensor.sensor_schema(unit_of_measurement=UNIT_CELSIUS, accuracy_decimals=0, device_class=DEVICE_CLASS_TEMPERATURE, state_class=STATE_CLASS_MEASUREMENT),
        cv.Optional(CONF_ODU_DISCHARGE_TEMP): sensor.sensor_schema(unit_of_measurement=UNIT_CELSIUS, accuracy_decimals=0, device_class=DEVICE_CLASS_TEMPERATURE, state_class=STATE_CLASS_MEASUREMENT),
        cv.Optional(CONF_ODU_SUCTION_TEMP): sensor.sensor_schema(unit_of_measurement=UNIT_CELSIUS, accuracy_decimals=0, device_class=DEVICE_CLASS_TEMPERATURE, state_class=STATE_CLASS_MEASUREMENT),
        cv.Optional(CONF_ODU_HEAT_EXCHANGER_TEMP): sensor.sensor_schema(unit_of_measurement=UNIT_CELSIUS, accuracy_decimals=0, device_class=DEVICE_CLASS_TEMPERATURE, state_class=STATE_CLASS_MEASUREMENT),
        cv.Optional(CONF_COMPRESSOR_LOAD): sensor.sensor_schema(unit_of_measurement=UNIT_PERCENT, accuracy_decimals=1, state_class=STATE_CLASS_MEASUREMENT),
        cv.Optional(CONF_COMPRESSOR_CURRENT): sensor.sensor_schema(unit_of_measurement=UNIT_AMPERE, accuracy_decimals=1, device_class=DEVICE_CLASS_CURRENT, state_class=STATE_CLASS_MEASUREMENT),
        cv.Optional(CONF_IDU_HEAT_EXCHANGER_TEMP): sensor.sensor_schema(unit_of_measurement=UNIT_CELSIUS, accuracy_decimals=0, device_class=DEVICE_CLASS_TEMPERATURE, state_class=STATE_CLASS_MEASUREMENT),
        cv.Optional(CONF_IDU_JUNCTION_TEMP): sensor.sensor_schema(unit_of_measurement=UNIT_CELSIUS, accuracy_decimals=0, device_class=DEVICE_CLASS_TEMPERATURE, state_class=STATE_CLASS_MEASUREMENT),
        cv.Optional(CONF_IDU_FAN_SPEED): sensor.sensor_schema(accuracy_decimals=0, state_class=STATE_CLASS_MEASUREMENT),
        cv.Optional(CONF_F8_BYTE_1): raw_f8_sensor_schema,
        cv.Optional(CONF_F8_BYTE_2): raw_f8_sensor_schema,
        cv.Optional(CONF_F8_BYTE_3): raw_f8_sensor_schema,
        cv.Optional(CONF_F8_BYTE_4): raw_f8_sensor_schema,
        cv.Optional(CONF_PWR_SELECT): select.select_schema(ToshibaPwrModeSelect).extend({cv.GenerateID(): cv.declare_id(ToshibaPwrModeSelect)}),
        cv.Optional(CONF_VERTICAL_AIR_DIRECTION): select.select_schema(ToshibaVerticalAirDirectionSelect).extend({cv.GenerateID(): cv.declare_id(ToshibaVerticalAirDirectionSelect)}),
        cv.Optional(CONF_SELF_CLEAN): binary_sensor.binary_sensor_schema(device_class=DEVICE_CLASS_RUNNING),
        cv.Optional(FEATURE_HORIZONTAL_SWING): cv.boolean,
        cv.Optional(DISABLE_WIFI_LED): cv.boolean,
        cv.Optional(DISABLE_HEAT_MODE): cv.boolean,
        cv.Optional(CONF_SPECIAL_MODE): select.select_schema(ToshibaSpecialModeSelect).extend({
            cv.GenerateID(): cv.declare_id(ToshibaSpecialModeSelect),
            cv.Required(CONF_SPECIAL_MODE_MODES): cv.ensure_list(cv.one_of("Standard", "Hi POWER", "ECO", "Fireplace 1", "Fireplace 2", "8 degrees", "Silent#1", "Silent#2", "Sleep", "Floor", "Comfort")),
        }),
        cv.Optional(CONF_SUPPORTED_PRESETS): cv.ensure_list(cv.one_of("Standard", "Hi POWER", "ECO", "Fireplace 1", "Fireplace 2", "8 degrees", "Silent#1", "Silent#2", "Sleep", "Floor", "Comfort")),
        cv.Optional(MIN_TEMP): cv.int_,
        cv.Optional(CONF_TIME_ID): cv.use_id(cg.esphome_ns.namespace("time").class_("RealTimeClock")),
        cv.Optional(CONF_TIME_SYNC_INTERVAL, default="24h"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_ENERGY): sensor.sensor_schema(unit_of_measurement=UNIT_WATT_HOURS, accuracy_decimals=0, device_class=DEVICE_CLASS_ENERGY, state_class=STATE_CLASS_TOTAL_INCREASING),
        cv.Optional(CONF_POWER): sensor.sensor_schema(unit_of_measurement=UNIT_WATT, accuracy_decimals=1, device_class=DEVICE_CLASS_POWER, state_class=STATE_CLASS_MEASUREMENT),
    }
).extend(uart.UART_DEVICE_SCHEMA).extend(cv.polling_component_schema("120s"))

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await climate.register_climate(var, config)
    await uart.register_uart_device(var, config)

    sensor_setters = {
        CONF_INDOOR_TEMP: var.set_indoor_temp_sensor,
        CONF_OUTDOOR_TEMP: var.set_outdoor_temp_sensor,
        CONF_ODU_DISCHARGE_TEMP: var.set_odu_discharge_temp_sensor,
        CONF_ODU_SUCTION_TEMP: var.set_odu_suction_temp_sensor,
        CONF_ODU_HEAT_EXCHANGER_TEMP: var.set_odu_heat_exchanger_temp_sensor,
        CONF_COMPRESSOR_LOAD: var.set_compressor_load_sensor,
        CONF_COMPRESSOR_CURRENT: var.set_compressor_current_sensor,
        CONF_IDU_HEAT_EXCHANGER_TEMP: var.set_idu_heat_exchanger_temp_sensor,
        CONF_IDU_JUNCTION_TEMP: var.set_idu_junction_temp_sensor,
        CONF_IDU_FAN_SPEED: var.set_idu_fan_speed_sensor,
        CONF_F8_BYTE_1: var.set_f8_byte_1_sensor,
        CONF_F8_BYTE_2: var.set_f8_byte_2_sensor,
        CONF_F8_BYTE_3: var.set_f8_byte_3_sensor,
        CONF_F8_BYTE_4: var.set_f8_byte_4_sensor,
        CONF_ENERGY: var.set_energy_sensor,
        CONF_POWER: var.set_power_sensor,
    }
    for key, setter in sensor_setters.items():
        if key in config:
            sens = await sensor.new_sensor(config[key])
            cg.add(setter(sens))

    if CONF_PWR_SELECT in config:
        sel = await select.new_select(config[CONF_PWR_SELECT], options=["50 %", "75 %", "100 %"])
        await cg.register_parented(sel, config[CONF_ID])
        cg.add(var.set_pwr_select(sel))

    if CONF_VERTICAL_AIR_DIRECTION in config:
        sel = await select.new_select(config[CONF_VERTICAL_AIR_DIRECTION], options=["Off", "Swing", "Top", "Middle Top", "Middle", "Middle Bottom", "Bottom"])
        await cg.register_parented(sel, config[CONF_ID])
        cg.add(var.set_vertical_air_direction_select(sel))

    if CONF_SELF_CLEAN in config:
        sens = await binary_sensor.new_binary_sensor(config[CONF_SELF_CLEAN])
        cg.add(var.set_self_clean_sensor(sens))

    if FEATURE_HORIZONTAL_SWING in config:
        cg.add(var.set_horizontal_swing(True))
    if MIN_TEMP in config:
        cg.add(var.set_min_temp(config[MIN_TEMP]))
    if DISABLE_HEAT_MODE in config:
        cg.add(var.disable_heat_mode(True))
    if DISABLE_WIFI_LED in config:
        cg.add(var.disable_wifi_led(True))

    if CONF_SUPPORTED_PRESETS in config:
        presets = config[CONF_SUPPORTED_PRESETS]
        cg.add(var.set_supported_presets(presets))
        if "8 degrees" in presets:
            cg.add(var.set_min_temp(5))

    if CONF_SPECIAL_MODE in config:
        presets = config[CONF_SPECIAL_MODE][CONF_SPECIAL_MODE_MODES]
        cg.add(var.set_supported_presets(presets))
        if "8 degrees" in presets:
            cg.add(var.set_min_temp(5))

    if CONF_TIME_ID in config:
        time_ = await cg.get_variable(config[CONF_TIME_ID])
        cg.add(var.set_time(time_))
    if CONF_TIME_SYNC_INTERVAL in config:
        cg.add(var.set_time_sync_interval(config[CONF_TIME_SYNC_INTERVAL]))