import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import (
    CONF_ID,
    DEVICE_CLASS_TEMPERATURE,
    ICON_TIMER,
    STATE_CLASS_MEASUREMENT,
    UNIT_CELSIUS,
    UNIT_MINUTE,
)
from . import gecko_spa_ns, GeckoSpa

DEPENDENCIES = ["gecko_spa"]

CONF_GECKO_SPA_ID = "gecko_spa_id"
CONF_SENSOR_TYPE = "type"

_PARENT = {cv.GenerateID(CONF_GECKO_SPA_ID): cv.use_id(GeckoSpa)}

# FORK: one schema per type so each can have its own unit
# (docs/fork/DECISIONS.md #6). `type: pump_timer` behaves as before.
CONFIG_SCHEMA = cv.typed_schema(
    {
        "pump_timer": sensor.sensor_schema(
            unit_of_measurement=UNIT_MINUTE,
            icon=ICON_TIMER,
            accuracy_decimals=0,
        ).extend(_PARENT),
        "set_point": sensor.sensor_schema(
            unit_of_measurement=UNIT_CELSIUS,
            accuracy_decimals=1,
            device_class=DEVICE_CLASS_TEMPERATURE,
            state_class=STATE_CLASS_MEASUREMENT,
        ).extend(_PARENT),
    },
    key=CONF_SENSOR_TYPE,
    lower=True,
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_GECKO_SPA_ID])
    var = await sensor.new_sensor(config)

    sensor_type = config[CONF_SENSOR_TYPE]
    if sensor_type == "pump_timer":
        cg.add(parent.set_pump_timer_sensor(var))
    elif sensor_type == "set_point":
        cg.add(parent.set_set_point_sensor(var))
