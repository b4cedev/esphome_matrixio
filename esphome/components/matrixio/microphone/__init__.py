import esphome.config_validation as cv
import esphome.codegen as cg
from esphome.const import (
    CONF_ID,
    DEVICE_CLASS_SOUND,
    STATE_CLASS_MEASUREMENT,
    UNIT_DECIBEL,
    UNIT_DEGREES,
)
from esphome.components import audio, binary_sensor, light, microphone, sensor

from .. import matrixio_ns, wb_device, wb_device_schema, register_wb_device  # noqa

DEPENDENCIES = ["matrixio"]
AUTO_LOAD = ["sensor", "binary_sensor"]
CODEOWNERS = ["@gnumpi"]

mics = matrixio_ns.class_("Microphone", microphone.Microphone, wb_device, cg.Component)

CONF_DIRECTION = "direction"
CONF_ALWAYS_ON = "always_on"
CONF_DETECT_CONFIDENCE = "detect_confidence"
CONF_DETECT_LEVEL_DB = "detect_level_db"
CONF_MIN_FRAMES = "min_frames"
CONF_HOLD_TIME = "hold_time"
CONF_SMOOTHING = "smoothing"
CONF_BEAM_LIGHT = "beam_light"
CONF_BEAM_CONFIDENCE = "beam_confidence"
CONF_BEAM_LEVEL_DB = "beam_level_db"
CONF_BEAMFORMING = "beamforming"
CONF_LED_OFFSET = "led_offset"
CONF_LED_REVERSE = "led_reverse"
CONF_ANGLE = "angle"
CONF_CONFIDENCE = "confidence"
CONF_LEVEL = "level"
CONF_ACTIVE = "active"

# Direction of arrival: reads all 8 microphones and estimates where a sound comes from (SRP-PHAT).
DIRECTION_SCHEMA = cv.Schema(
    {
        # Keep detecting while nothing consumes the audio (e.g. for presence). Costs ~26 % of the SPI bus.
        cv.Optional(CONF_ALWAYS_ON, default=False): cv.boolean,
        # A sound counts as detected (sensors) when it is this coherent across the microphones, 0.2 .. 0.95
        cv.Optional(CONF_DETECT_CONFIDENCE, default=0.5): cv.float_range(min=0.2, max=0.95),
        # ... and this far above the tracked background level
        cv.Optional(CONF_DETECT_LEVEL_DB, default=6.0): cv.float_range(min=0.0, max=30.0),
        # ... for this many frames (32 ms each) in a row
        cv.Optional(CONF_MIN_FRAMES, default=3): cv.int_range(min=1, max=30),
        # "Sound detected" stays on this long after the last detected frame (avoids flapping)
        cv.Optional(CONF_HOLD_TIME, default="3s"): cv.positive_time_period_milliseconds,
        # Smoothing of the direction (time constant), longer = calmer but slower to follow a moving source
        cv.Optional(CONF_SMOOTHING, default="300ms"): cv.positive_time_period_milliseconds,
        # Send the delay-and-sum of all 8 microphones, steered at the detected direction, instead of the centre
        # microphone alone (better signal to noise ratio). Can be switched at runtime with set_beamforming().
        cv.Optional(CONF_BEAMFORMING, default=False): cv.boolean,
        # Show the direction as a beam on this Everloop light (on top of its current colours)
        cv.Optional(CONF_BEAM_LIGHT): cv.use_id(light.LightState),
        # The beam is stricter than the detection, adjustable at runtime (set_beam_*)
        cv.Optional(CONF_BEAM_CONFIDENCE, default=0.62): cv.float_range(min=0.2, max=0.95),
        cv.Optional(CONF_BEAM_LEVEL_DB, default=9.0): cv.float_range(min=0.0, max=30.0),
        # The Everloop LEDs count clockwise and LED 4 sits exactly at 12 o'clock (USB port up), so an angle of 90 degrees
        # is LED 8.5 - 90 / 20 = 4
        cv.Optional(CONF_LED_OFFSET, default=8.5): cv.float_range(min=0.0, max=18.0),
        cv.Optional(CONF_LED_REVERSE, default=True): cv.boolean,
        # Direction in degrees like a compass: 0 = USB port side, increasing clockwise (90 = right, 180 = opposite)
        cv.Optional(CONF_ANGLE): sensor.sensor_schema(
            unit_of_measurement=UNIT_DEGREES,
            icon="mdi:compass-outline",
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_CONFIDENCE): sensor.sensor_schema(
            icon="mdi:target",
            accuracy_decimals=2,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_LEVEL): sensor.sensor_schema(
            unit_of_measurement=UNIT_DECIBEL,
            icon="mdi:volume-high",
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_ACTIVE): binary_sensor.binary_sensor_schema(
            device_class=DEVICE_CLASS_SOUND,
        ),
    }
)

CONFIG_SCHEMA = microphone.MICROPHONE_SCHEMA.extend(
    {
        cv.GenerateID(): cv.declare_id(mics),
        cv.Optional(CONF_DIRECTION): DIRECTION_SCHEMA,
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

    if CONF_DIRECTION in config:
        direction = config[CONF_DIRECTION]
        cg.add(var.enable_direction(direction[CONF_ALWAYS_ON]))
        cg.add(var.set_detect_confidence(direction[CONF_DETECT_CONFIDENCE]))
        cg.add(var.set_detect_level_db(direction[CONF_DETECT_LEVEL_DB]))
        cg.add(var.set_min_frames(direction[CONF_MIN_FRAMES]))
        cg.add(var.set_hold_time_ms(direction[CONF_HOLD_TIME]))
        cg.add(var.set_smoothing_ms(direction[CONF_SMOOTHING]))
        cg.add(var.set_beamforming(direction[CONF_BEAMFORMING]))
        cg.add(var.set_beam_confidence(direction[CONF_BEAM_CONFIDENCE]))
        cg.add(var.set_beam_level_db(direction[CONF_BEAM_LEVEL_DB]))
        cg.add(var.set_led_mapping(direction[CONF_LED_OFFSET], direction[CONF_LED_REVERSE]))
        if CONF_BEAM_LIGHT in direction:
            ring = await cg.get_variable(direction[CONF_BEAM_LIGHT])
            cg.add(var.set_beam_light(ring))
        if CONF_ANGLE in direction:
            cg.add(var.set_angle_sensor(await sensor.new_sensor(direction[CONF_ANGLE])))
        if CONF_CONFIDENCE in direction:
            cg.add(var.set_confidence_sensor(await sensor.new_sensor(direction[CONF_CONFIDENCE])))
        if CONF_LEVEL in direction:
            cg.add(var.set_level_sensor(await sensor.new_sensor(direction[CONF_LEVEL])))
        if CONF_ACTIVE in direction:
            cg.add(var.set_active_sensor(await binary_sensor.new_binary_sensor(direction[CONF_ACTIVE])))
