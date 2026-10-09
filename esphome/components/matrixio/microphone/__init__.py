import esphome.config_validation as cv
import esphome.codegen as cg
from esphome.const import CONF_ID
from esphome.components import audio, light, microphone

from .. import matrixio_ns, wb_device, wb_device_schema, register_wb_device  # noqa

DEPENDENCIES = ["matrixio"]
CODEOWNERS = ["@gnumpi"]

mics = matrixio_ns.class_("Microphone", microphone.Microphone, wb_device, cg.Component)

CONF_LEVEL_LIGHT = "level_light"
CONF_LED_OFFSET = "led_offset"
CONF_LED_REVERSE = "led_reverse"
CONF_DIRECTION_CONFIDENCE = "direction_confidence"
CONF_DIRECTION_LEVEL_DB = "direction_level_db"
CONF_DIRECTION_MIN_FRAMES = "direction_min_frames"

CONFIG_SCHEMA = microphone.MICROPHONE_SCHEMA.extend(
    {
        cv.GenerateID(): cv.declare_id(mics),
        # Debug aid: read all 8 channels and show each microphone's level on the ring
        cv.Optional(CONF_LEVEL_LIGHT): cv.use_id(light.LightState),
        # Everloop LED 0 sits on the left (USB port up) and the LEDs count clockwise
        cv.Optional(CONF_LED_OFFSET, default=9): cv.int_range(min=0, max=17),
        cv.Optional(CONF_LED_REVERSE, default=True): cv.boolean,
        # Direction detection sensitivity (also changeable at runtime, see set_direction_*)
        # How coherent the sound must be across the microphones, 0.2 (very sensitive) .. 0.95 (strict)
        cv.Optional(CONF_DIRECTION_CONFIDENCE, default=0.62): cv.float_range(min=0.2, max=0.95),
        # How far the sound must be above the tracked background level
        cv.Optional(CONF_DIRECTION_LEVEL_DB, default=9.0): cv.float_range(min=0.0, max=30.0),
        # How many frames (32 ms each) in a row are needed before a direction is shown
        cv.Optional(CONF_DIRECTION_MIN_FRAMES, default=3): cv.int_range(min=1, max=30),
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

    if CONF_LEVEL_LIGHT in config:
        ring = await cg.get_variable(config[CONF_LEVEL_LIGHT])
        cg.add(var.set_level_light(ring))
        cg.add(var.set_led_mapping(config[CONF_LED_OFFSET], config[CONF_LED_REVERSE]))
        cg.add(var.set_direction_confidence(config[CONF_DIRECTION_CONFIDENCE]))
        cg.add(var.set_direction_level_db(config[CONF_DIRECTION_LEVEL_DB]))
        cg.add(var.set_direction_min_frames(config[CONF_DIRECTION_MIN_FRAMES]))
