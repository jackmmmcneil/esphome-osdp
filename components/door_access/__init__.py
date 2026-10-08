"""Door access logic for an osdp_cp reader.

Credentials (cards, PINs, duress PINs, schedules) live on the ESP in NVS and
are managed at runtime from Home Assistant through ESPHome API actions, so
the door keeps working if Home Assistant or Wi-Fi is down.
"""

from esphome import automation
import esphome.codegen as cg
from esphome.components import osdp_cp, time
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_TIME_ID, CONF_TRIGGER_ID

CODEOWNERS = ["@jackmmmcneil"]
DEPENDENCIES = ["osdp_cp"]
AUTO_LOAD = ["sha256", "json"]

CONF_READER_ID = "reader_id"
CONF_MAX_USERS = "max_users"
CONF_UNLOCK_TIME = "unlock_time"
CONF_PIN_TIMEOUT = "pin_timeout"
CONF_CARD_PIN_TIMEOUT = "card_pin_timeout"
CONF_MAX_PIN_FAILURES = "max_pin_failures"
CONF_LOCKOUT_TIME = "lockout_time"
CONF_IDLE_COLOR = "idle_color"
CONF_LOCKDOWN_COLOR = "lockdown_color"
CONF_ON_GRANTED = "on_granted"
CONF_ON_DENIED = "on_denied"
CONF_ON_DURESS = "on_duress"
CONF_ON_COMMAND = "on_command"
CONF_ON_ENROLLED = "on_enrolled"
CONF_ON_ENROL_FAILED = "on_enrol_failed"

door_access_ns = cg.esphome_ns.namespace("door_access")
DoorAccess = door_access_ns.class_("DoorAccess", cg.Component)

STR = cg.std_string
TRIGGERS = {
    CONF_ON_GRANTED: ("GrantedTrigger", [(STR, "user"), (STR, "method")]),
    CONF_ON_DENIED: (
        "DeniedTrigger",
        [(STR, "user"), (STR, "method"), (STR, "reason"), (STR, "card")],
    ),
    CONF_ON_DURESS: ("DuressTrigger", [(STR, "user"), (STR, "method")]),
    CONF_ON_COMMAND: (
        "CommandTrigger",
        [(STR, "user"), (STR, "code"), (cg.bool_, "authenticated")],
    ),
    CONF_ON_ENROLLED: ("EnrolledTrigger", [(STR, "user"), (STR, "card")]),
    CONF_ON_ENROL_FAILED: ("EnrolFailedTrigger", [(STR, "user"), (STR, "reason")]),
}
TRIGGER_CLASSES = {
    key: door_access_ns.class_(cls, automation.Trigger.template(*[t for t, _ in args]))
    for key, (cls, args) in TRIGGERS.items()
}

LED_COLORS = osdp_cp.LED_COLORS

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(DoorAccess),
        cv.Required(CONF_READER_ID): cv.use_id(osdp_cp.OSDPControlPanel),
        cv.Optional(CONF_TIME_ID): cv.use_id(time.RealTimeClock),
        cv.Optional(CONF_MAX_USERS, default=32): cv.int_range(min=1, max=100),
        cv.Optional(CONF_UNLOCK_TIME, default="5s"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_PIN_TIMEOUT, default="5s"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_CARD_PIN_TIMEOUT, default="10s"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_MAX_PIN_FAILURES, default=5): cv.int_range(min=1, max=50),
        cv.Optional(CONF_LOCKOUT_TIME, default="60s"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_IDLE_COLOR, default="blue"): cv.enum(LED_COLORS, lower=True),
        cv.Optional(CONF_LOCKDOWN_COLOR, default="red"): cv.enum(LED_COLORS, lower=True),
        **{
            cv.Optional(key): automation.validate_automation(
                {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(TRIGGER_CLASSES[key])}
            )
            for key in TRIGGERS
        },
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    reader = await cg.get_variable(config[CONF_READER_ID])
    cg.add(var.set_reader(reader))
    if CONF_TIME_ID in config:
        clock = await cg.get_variable(config[CONF_TIME_ID])
        cg.add(var.set_time(clock))

    cg.add(var.set_max_users(config[CONF_MAX_USERS]))
    cg.add(var.set_unlock_time(config[CONF_UNLOCK_TIME].total_milliseconds))
    cg.add(var.set_pin_timeout(config[CONF_PIN_TIMEOUT].total_milliseconds))
    cg.add(var.set_card_pin_timeout(config[CONF_CARD_PIN_TIMEOUT].total_milliseconds))
    cg.add(var.set_max_pin_failures(config[CONF_MAX_PIN_FAILURES]))
    cg.add(var.set_lockout_time(config[CONF_LOCKOUT_TIME].total_milliseconds))
    cg.add(var.set_idle_color(config[CONF_IDLE_COLOR]))
    cg.add(var.set_lockdown_color(config[CONF_LOCKDOWN_COLOR]))

    for key, (_, args) in TRIGGERS.items():
        register = f"register_{key[3:]}_trigger"  # on_granted -> register_granted_trigger
        for conf in config.get(key, []):
            trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID])
            cg.add(getattr(var, register)(trigger))
            await automation.build_automation(trigger, args, conf)
