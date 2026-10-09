import esphome.config_validation as cv
import esphome.codegen as cg
from esphome.const import CONF_ID
from esphome.components import audio, microphone

from .. import matrixio_ns, wb_device, wb_device_schema, register_wb_device  # noqa

DEPENDENCIES = ["matrixio"]
CODEOWNERS = ["@gnumpi"]

mics = matrixio_ns.class_("Microphone", microphone.Microphone, wb_device, cg.Component)

CONFIG_SCHEMA = microphone.MICROPHONE_SCHEMA.extend(
    {
        cv.GenerateID(): cv.declare_id(mics),
    }
).extend(wb_device_schema())


def _set_stream_limits(config):
    # Only one (mono) 16 bit channel is forwarded, at the fixed rate set in Microphone::setup()
    audio.set_stream_limits(
        min_bits_per_sample=16,
        max_bits_per_sample=16,
        min_channels=1,
        max_channels=1,
        min_sample_rate=16000,
        max_sample_rate=16000,
    )(config)
    return config


FINAL_VALIDATE_SCHEMA = _set_stream_limits


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await register_wb_device(var, config)
    await microphone.register_microphone(var, config)
