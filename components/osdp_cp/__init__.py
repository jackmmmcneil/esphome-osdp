"""ESPHome external component: OSDP Control Panel (CP) for one RS-485 reader.

Wraps LibOSDP v3.2.7 (https://github.com/goToMain/libosdp, Apache-2.0), vendored
into this folder as libosdp_* files by tools/vendor_libosdp.py, so an ESP32 can poll an
OSDP reader, receive card reads and keypad presses,
and drive the reader's LED and buzzer.
"""

from esphome import automation, pins
import esphome.codegen as cg
from esphome.components import binary_sensor, key_provider, text_sensor, uart
import esphome.config_validation as cv
from esphome.core import CORE
import esphome.final_validate as fv
from esphome.const import (
    CONF_ADDRESS,
    CONF_COLOR,
    CONF_DURATION,
    CONF_FLOW_CONTROL_PIN,
    CONF_ID,
    CONF_NUMBER,
    CONF_TRIGGER_ID,
    CONF_TX_PIN,
    CONF_UART_ID,
    DEVICE_CLASS_CONNECTIVITY,
    ENTITY_CATEGORY_DIAGNOSTIC,
)

CODEOWNERS = ["@jackmmmcneil"]
DEPENDENCIES = ["uart"]
AUTO_LOAD = ["binary_sensor", "key_provider", "text_sensor"]

CONF_SCBK = "scbk"
CONF_ENFORCE_SECURE = "enforce_secure"
CONF_SCAN = "scan"
CONF_DIRECT_DRIVE = "direct_drive"
CONF_B_PIN = "b_pin"
CONF_IDLE_COLOR = "idle_color"
CONF_ONLINE = "online"
CONF_SECURE_CHANNEL = "secure_channel"
CONF_LAST_CARD = "last_card"
CONF_ON_CARD = "on_card"
CONF_OFF_COLOR = "off_color"
CONF_ON_TIME = "on_time"
CONF_OFF_TIME = "off_time"
CONF_PERMANENT = "permanent"
CONF_BEEPS = "beeps"

osdp_ns = cg.esphome_ns.namespace("osdp_cp")
OSDPControlPanel = osdp_ns.class_(
    "OSDPControlPanel", key_provider.KeyProvider, cg.Component, uart.UARTDevice
)
CardTrigger = osdp_ns.class_(
    "CardTrigger", automation.Trigger.template(cg.std_string, cg.int_)
)
LedAction = osdp_ns.class_("LedAction", automation.Action)
BuzzerAction = osdp_ns.class_("BuzzerAction", automation.Action)

# OSDP LED colour codes (osdp_led_color_e)
LED_COLORS = {
    "off": 0,
    "red": 1,
    "green": 2,
    "amber": 3,
    "blue": 4,
    "magenta": 5,
    "cyan": 6,
    "white": 7,
}


def _validate_scbk(value):
    value = cv.string_strict(value).replace(":", "").replace(" ", "").lower()
    if len(value) != 32:
        raise cv.Invalid("scbk must be 16 bytes written as 32 hex characters")
    try:
        bytes.fromhex(value)
    except ValueError as err:
        raise cv.Invalid("scbk must contain only hex characters") from err
    return value



def _validate_secure(config):
    if config[CONF_ENFORCE_SECURE] and CONF_SCBK not in config:
        raise cv.Invalid("enforce_secure requires an scbk")
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(OSDPControlPanel),
            cv.Optional(CONF_ADDRESS, default=0): cv.int_range(min=0, max=126),
            cv.Optional(CONF_FLOW_CONTROL_PIN): pins.gpio_output_pin_schema,
            cv.Optional(CONF_SCBK): _validate_scbk,
            cv.Optional(CONF_ENFORCE_SECURE, default=False): cv.boolean,
            cv.Optional(CONF_SCAN, default=False): cv.boolean,
            cv.Optional(CONF_DIRECT_DRIVE): cv.All(
                cv.only_on_esp32,
                cv.Schema(
                    {cv.Required(CONF_B_PIN): pins.internal_gpio_output_pin_number}
                ),
            ),
            cv.Optional(CONF_IDLE_COLOR): cv.enum(LED_COLORS, lower=True),
            cv.Optional(CONF_ONLINE): binary_sensor.binary_sensor_schema(
                device_class=DEVICE_CLASS_CONNECTIVITY,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_SECURE_CHANNEL): binary_sensor.binary_sensor_schema(
                icon="mdi:shield-lock",
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_LAST_CARD): text_sensor.text_sensor_schema(
                icon="mdi:card-account-details",
            ),
            cv.Optional(CONF_ON_CARD): automation.validate_automation(
                {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(CardTrigger)}
            ),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(uart.UART_DEVICE_SCHEMA),
    _validate_secure,
)

_uart_final_validate = uart.final_validate_device_schema(
    "osdp_cp",
    require_tx=True,
    require_rx=True,
    data_bits=8,
    parity="NONE",
    stop_bits=1,
)


def _uart_conf(config, full_config):
    for conf in full_config.get("uart", []):
        if conf[CONF_ID] == config[CONF_UART_ID]:
            return conf
    raise cv.Invalid("uart for osdp_cp not found")


def _final_validate(config):
    _uart_final_validate(config)
    if CONF_DIRECT_DRIVE in config:
        uart_conf = _uart_conf(config, fv.full_config.get())
        if CONF_FLOW_CONTROL_PIN in uart_conf:
            raise cv.Invalid(
                "direct_drive replaces the RS-485 module; remove flow_control_pin from the uart"
            )
        if CONF_FLOW_CONTROL_PIN in config:
            raise cv.Invalid("direct_drive can't be combined with flow_control_pin")
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)

    cg.add(var.set_address(config[CONF_ADDRESS]))
    cg.add(var.set_enforce_secure(config[CONF_ENFORCE_SECURE]))
    cg.add(var.set_scan(config[CONF_SCAN]))
    if CONF_DIRECT_DRIVE in config:
        a_pin = _uart_conf(config, CORE.config)[CONF_TX_PIN][CONF_NUMBER]
        cg.add(var.set_direct_drive(a_pin, config[CONF_DIRECT_DRIVE][CONF_B_PIN]))
    if CONF_FLOW_CONTROL_PIN in config:
        pin = await cg.gpio_pin_expression(config[CONF_FLOW_CONTROL_PIN])
        cg.add(var.set_flow_control_pin(pin))
    if CONF_SCBK in config:
        cg.add(var.set_scbk(config[CONF_SCBK]))
    if CONF_IDLE_COLOR in config:
        cg.add(var.set_idle_color(config[CONF_IDLE_COLOR]))

    if CONF_ONLINE in config:
        sens = await binary_sensor.new_binary_sensor(config[CONF_ONLINE])
        cg.add(var.set_online_sensor(sens))
    if CONF_SECURE_CHANNEL in config:
        sens = await binary_sensor.new_binary_sensor(config[CONF_SECURE_CHANNEL])
        cg.add(var.set_secure_channel_sensor(sens))
    if CONF_LAST_CARD in config:
        sens = await text_sensor.new_text_sensor(config[CONF_LAST_CARD])
        cg.add(var.set_last_card_sensor(sens))

    for conf in config.get(CONF_ON_CARD, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID])
        cg.add(var.register_card_trigger(trigger))
        await automation.build_automation(
            trigger, [(cg.std_string, "card"), (cg.int_, "bits")], conf
        )


OSDP_ID_SCHEMA = cv.Schema({cv.GenerateID(): cv.use_id(OSDPControlPanel)})

LED_ACTION_SCHEMA = OSDP_ID_SCHEMA.extend(
    {
        cv.Required(CONF_COLOR): cv.enum(LED_COLORS, lower=True),
        cv.Optional(CONF_OFF_COLOR, default="off"): cv.enum(LED_COLORS, lower=True),
        cv.Optional(CONF_ON_TIME, default="500ms"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_OFF_TIME, default="0ms"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_DURATION, default="2s"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_PERMANENT, default=False): cv.boolean,
    }
)


@automation.register_action(
    "osdp_cp.led", LedAction, LED_ACTION_SCHEMA, synchronous=True
)
async def led_action_to_code(config, action_id, template_args, args):
    parent = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_args, parent)
    cg.add(var.set_color(config[CONF_COLOR]))
    cg.add(var.set_off_color(config[CONF_OFF_COLOR]))
    cg.add(var.set_on_time(config[CONF_ON_TIME].total_milliseconds))
    cg.add(var.set_off_time(config[CONF_OFF_TIME].total_milliseconds))
    cg.add(var.set_duration(config[CONF_DURATION].total_milliseconds))
    cg.add(var.set_permanent(config[CONF_PERMANENT]))
    return var


BUZZER_ACTION_SCHEMA = OSDP_ID_SCHEMA.extend(
    {
        cv.Optional(CONF_BEEPS, default=1): cv.int_range(min=1, max=255),
        cv.Optional(CONF_ON_TIME, default="200ms"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_OFF_TIME, default="200ms"): cv.positive_time_period_milliseconds,
    }
)


@automation.register_action(
    "osdp_cp.buzzer", BuzzerAction, BUZZER_ACTION_SCHEMA, synchronous=True
)
async def buzzer_action_to_code(config, action_id, template_args, args):
    parent = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_args, parent)
    cg.add(var.set_beeps(config[CONF_BEEPS]))
    cg.add(var.set_on_time(config[CONF_ON_TIME].total_milliseconds))
    cg.add(var.set_off_time(config[CONF_OFF_TIME].total_milliseconds))
    return var
