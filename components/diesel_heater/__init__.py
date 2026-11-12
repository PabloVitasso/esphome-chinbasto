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
CONF_DUTY_CONTROL = "duty_control"
CONF_UP_SWITCH = "up_switch"
CONF_DOWN_SWITCH = "down_switch"
CONF_DUMP_PACKETS = "dump_packets"

# Sensors: config key -> (C++ setter, required). Read publish_policy.h for
# how often each goes out.
# Discrete: a change goes out at once, everything again every 5 min.
# Measurements: one 60 s window. pump_frequency is the last value,
# fan_speed the mean, chamber_temperature the last value and
# chamber_temperature_max the highest.
SENSORS = {
    "state": ("set_state_sensor", True),
    "on_off": ("set_on_off_sensor", True),
    "error": ("set_error_sensor", True),
    "active_error": ("set_active_error_sensor", False),
    "set_temperature": ("set_demand_sensor", False),
    "link": ("set_link_sensor", False),
    "pump_frequency": ("set_pump_sensor", True),
    "fan_speed": ("set_fan_sensor", True),
    "chamber_temperature": ("set_chamber_sensor", True),
    "chamber_temperature_max": ("set_chamber_max_sensor", False),
}

# Text sensors: config key -> C++ setter. All optional.
# debug_data: one JSON message per update with the counters, the raw
# protocol fields and the last exchange as hex. Only while debug is on:
# call set_debug(true) from a template switch.
TEXT_SENSORS = {
    "state_text": "set_state_text_sensor",
    "error_text": "set_error_text_sensor",
    "debug_data": "set_debug_data_sensor",
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
    cv.Optional(CONF_DUTY_CONTROL): number.number_schema(HeaterDutyNumber),
    cv.Optional(CONF_UP_SWITCH): cv.use_id(switch.Switch),
    cv.Optional(CONF_DOWN_SWITCH): cv.use_id(switch.Switch),
    cv.Optional(CONF_DUMP_PACKETS, default=False): cv.boolean,
})
SCHEMA = SCHEMA.extend({
    (cv.Required(key) if required else cv.Optional(key)): cv.use_id(sensor.Sensor)
    for key, (_, required) in SENSORS.items()
})
SCHEMA = SCHEMA.extend(
    {cv.Optional(key): cv.use_id(text_sensor.TextSensor) for key in TEXT_SENSORS}
)

CONFIG_SCHEMA = cv.All(
    SCHEMA.extend(cv.polling_component_schema("5s")),
    validate_duty_control,
)


async def to_code(config):
    uart_var = await cg.get_variable(config[CONF_UART_ID])
    var = cg.new_Pvariable(config[CONF_ID], uart_var)

    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)
    cg.add(var.set_dump_packets(config[CONF_DUMP_PACKETS]))

    setters = {key: setter for key, (setter, _) in SENSORS.items()}
    setters.update(TEXT_SENSORS)
    for key, setter in setters.items():
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
