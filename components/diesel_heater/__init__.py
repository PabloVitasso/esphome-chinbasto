import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import number, sensor, switch, text_sensor, uart
from esphome.const import CONF_ID

AUTO_LOAD = ["number", "text_sensor", "switch"]
DEPENDENCIES = ["uart"]

# Define C++ namespace and class
heater_uart_ns = cg.esphome_ns.namespace("heater_uart")
# The generated component ID can be referenced from template-button lambdas to
# call the public request_on() and request_off() experimental controls.
HeaterUart = heater_uart_ns.class_(
    "HeaterUart", cg.PollingComponent, uart.UARTDevice
)
HeaterDutyNumber = heater_uart_ns.class_("HeaterDutyNumber", number.Number)

# Define config keys
CONF_UART_ID = "uart_id"
CONF_SET_TEMPERATURE = "set_temperature"
CONF_STATE = "state"
CONF_ERROR = "error"
CONF_STATE_TEXT = "state_text"
CONF_ERROR_TEXT = "error_text"
CONF_PUMP_FREQUENCY = "pump_frequency"
CONF_FAN_SPEED = "fan_speed"
CONF_CHAMBER_TEMPERATURE = "chamber_temperature"
CONF_DUTY_CYCLE = "duty_cycle"
CONF_ON_OFF = "on_off"
CONF_DUTY_CONTROL = "duty_control"
CONF_UP_SWITCH = "up_switch"
CONF_DOWN_SWITCH = "down_switch"
CONF_DUMP_PACKETS = "dump_packets"

# Optional sensors: config key -> C++ setter. Each one may be omitted.
# Diagnostics: the link watchdog and the reject counters.
# Research: raw protocol fields. Their meaning is unverified, so they are
# published without scaling. Read heater_protocol.h for the byte map.
OPTIONAL_SENSORS = {
    "active_error": "set_active_error_sensor",
    "link": "set_link_sensor",
    "frame_age": "set_frame_age_sensor",
    "frames_ok": "set_frames_ok_sensor",
    "frames_bad_start": "set_frames_bad_start_sensor",
    "frames_bad_marker": "set_frames_bad_marker_sensor",
    "frames_bad_crc": "set_frames_bad_crc_sensor",
    "range_rejects": "set_range_rejects_sensor",
    "controller_temperature_raw": "set_ctrl_temperature_raw_sensor",
    "controller_pump_min_raw": "set_ctrl_pump_min_raw_sensor",
    "controller_pump_max_raw": "set_ctrl_pump_max_raw_sensor",
    "controller_fan_min_raw": "set_ctrl_fan_min_raw_sensor",
    "controller_fan_max_raw": "set_ctrl_fan_max_raw_sensor",
    "controller_voltage_raw": "set_ctrl_voltage_raw_sensor",
    "controller_mode_raw": "set_ctrl_mode_raw_sensor",
    "supply_voltage_raw": "set_supply_voltage_raw_sensor",
    "fan_voltage_raw": "set_fan_voltage_raw_sensor",
    "glow_plug_voltage_raw": "set_glow_voltage_raw_sensor",
    "glow_plug_current_raw": "set_glow_current_raw_sensor",
}

OPTIONAL_TEXT_SENSORS = {
    CONF_STATE_TEXT: "set_state_text_sensor",
    CONF_ERROR_TEXT: "set_error_text_sensor",
    # The last valid 48-byte exchange as hex. On change, at most every 30 s.
    "exchange_hex": "set_exchange_hex_sensor",
}


def validate_duty_control(config):
    duty_keys = (CONF_DUTY_CONTROL, CONF_UP_SWITCH, CONF_DOWN_SWITCH)
    configured = [key for key in duty_keys if key in config]
    if configured and len(configured) != len(duty_keys):
        raise cv.Invalid(
            "duty_control, up_switch and down_switch must be configured together"
        )
    return config


SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(HeaterUart),
    cv.Required(CONF_UART_ID): cv.use_id(uart.UARTComponent),
    cv.Required(CONF_SET_TEMPERATURE): cv.use_id(sensor.Sensor),
    cv.Required(CONF_STATE): cv.use_id(sensor.Sensor),
    cv.Required(CONF_ERROR): cv.use_id(sensor.Sensor),
    cv.Required(CONF_ON_OFF): cv.use_id(sensor.Sensor),
    cv.Required(CONF_PUMP_FREQUENCY): cv.use_id(sensor.Sensor),
    cv.Required(CONF_FAN_SPEED): cv.use_id(sensor.Sensor),
    cv.Required(CONF_CHAMBER_TEMPERATURE): cv.use_id(sensor.Sensor),
    cv.Required(CONF_DUTY_CYCLE): cv.use_id(sensor.Sensor),
    cv.Optional(CONF_DUTY_CONTROL): number.number_schema(HeaterDutyNumber),
    cv.Optional(CONF_UP_SWITCH): cv.use_id(switch.Switch),
    cv.Optional(CONF_DOWN_SWITCH): cv.use_id(switch.Switch),
    cv.Optional(CONF_DUMP_PACKETS, default=False): cv.boolean,
})
SCHEMA = SCHEMA.extend(
    {cv.Optional(key): cv.use_id(sensor.Sensor) for key in OPTIONAL_SENSORS}
)
SCHEMA = SCHEMA.extend(
    {cv.Optional(key): cv.use_id(text_sensor.TextSensor) for key in OPTIONAL_TEXT_SENSORS}
)

CONFIG_SCHEMA = cv.All(
    SCHEMA.extend(cv.polling_component_schema("5s")),
    validate_duty_control,
)


async def to_code(config):
    uart_var = await cg.get_variable(config[CONF_UART_ID])
    set_temp = await cg.get_variable(config[CONF_SET_TEMPERATURE])
    state = await cg.get_variable(config[CONF_STATE])
    error = await cg.get_variable(config[CONF_ERROR])
    on_off = await cg.get_variable(config[CONF_ON_OFF])
    pump = await cg.get_variable(config[CONF_PUMP_FREQUENCY])
    fan = await cg.get_variable(config[CONF_FAN_SPEED])
    chamber = await cg.get_variable(config[CONF_CHAMBER_TEMPERATURE])
    duty = await cg.get_variable(config[CONF_DUTY_CYCLE])

    var = cg.new_Pvariable(config[CONF_ID],
                           uart_var,
                           set_temp,
                           state,
                           error,
                           on_off,
                           pump,
                           fan,
                           chamber,
                           duty)

    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)
    cg.add(var.set_dump_packets(config[CONF_DUMP_PACKETS]))

    for key, setter in {**OPTIONAL_SENSORS, **OPTIONAL_TEXT_SENSORS}.items():
        if key not in config:
            continue
        target = await cg.get_variable(config[key])
        cg.add(getattr(var, setter)(target))

    if CONF_DUTY_CONTROL in config:
        up_switch = await cg.get_variable(config[CONF_UP_SWITCH])
        down_switch = await cg.get_variable(config[CONF_DOWN_SWITCH])
        duty_control = await number.new_number(
            config[CONF_DUTY_CONTROL],
            var,
            min_value=8,
            max_value=35,
            step=1,
        )
        cg.add(var.set_duty_control(duty_control, up_switch, down_switch))
