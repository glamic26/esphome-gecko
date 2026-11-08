import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import climate
from esphome.const import CONF_ID, CONF_NAME
from . import gecko_spa_ns, GeckoSpa

DEPENDENCIES = ["gecko_spa"]

GeckoSpaClimate = gecko_spa_ns.class_(
    "GeckoSpaClimate", climate.Climate, cg.Component
)

CONF_GECKO_SPA_ID = "gecko_spa_id"
# FORK: false = always heat mode, action from the heater (docs/fork/DECISIONS.md #4)
CONF_REPORT_COOL_MODE = "report_cool_mode"

CONFIG_SCHEMA = (
    climate.climate_schema(GeckoSpaClimate)
    .extend(
        {
            cv.GenerateID(CONF_GECKO_SPA_ID): cv.use_id(GeckoSpa),
            cv.Optional(CONF_REPORT_COOL_MODE, default=True): cv.boolean,
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_GECKO_SPA_ID])
    var = cg.new_Pvariable(config[CONF_ID], parent)
    cg.add(var.set_report_cool_mode(config[CONF_REPORT_COOL_MODE]))
    await cg.register_component(var, config)
    await climate.register_climate(var, config)
    cg.add(parent.set_climate(var))
