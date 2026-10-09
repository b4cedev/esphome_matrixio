import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import audio, speaker
from esphome.const import CONF_BITS_PER_SAMPLE, CONF_ID, CONF_SAMPLE_RATE

from .. import matrixio_ns, wb_device, wb_device_schema, register_wb_device  # noqa

DEPENDENCIES = ["matrixio"]
CODEOWNERS = ["@gnumpi"]

matrix_speaker = matrixio_ns.class_("Speaker", speaker.Speaker, wb_device, cg.Component)

CONF_AUDIO_OUT = "audio_out"
CONF_VOLUME = "volume"
CONF_IDLE_TIMEOUT = "idle_timeout"

# sample rates supported by the FPGA, see PCM_SAMPLING_FREQUENCIES in matrixio.h
PCM_SAMPLE_RATES = (8000, 16000, 32000, 44100, 48000)

OutputSelector = matrixio_ns.enum("OutputSelector")
OUTPUTS = {"speakers": OutputSelector.kSpeaker, "headphone": OutputSelector.kHeadPhone}


CONFIG_SCHEMA = speaker.SPEAKER_SCHEMA.extend(
    {
        cv.GenerateID(): cv.declare_id(matrix_speaker),
        cv.Optional(CONF_AUDIO_OUT, default="headphone"): cv.enum(OUTPUTS, upper=False),
        cv.Optional(CONF_VOLUME, default=80): cv.All(int, cv.Range(min=1, max=100)),
        # Playback stops after this long without new audio.
        cv.Optional(CONF_IDLE_TIMEOUT, default="500ms"): cv.All(
            cv.positive_time_period_milliseconds,
            cv.Range(
                min=cv.TimePeriod(milliseconds=100),
                max=cv.TimePeriod(seconds=10),
            ),
        ),
        # Fixes the sample rate of the audio the speaker accepts. Required if the speaker is the
        # output of a `mixer` speaker, otherwise the rate follows the audio stream.
        cv.Optional(CONF_BITS_PER_SAMPLE, default=16): cv.one_of(16, int=True),
        cv.Optional(CONF_SAMPLE_RATE): cv.one_of(*PCM_SAMPLE_RATES, int=True),
    }
).extend(wb_device_schema())


def _set_stream_limits(config):
    # 16 bit PCM, mono is duplicated to stereo in Speaker::play(). The FPGA supports the
    # sample rates listed in PCM_SAMPLING_FREQUENCIES (matrixio.h).
    audio.set_stream_limits(
        min_bits_per_sample=16,
        max_bits_per_sample=16,
        min_channels=1,
        max_channels=2,
        min_sample_rate=config.get(CONF_SAMPLE_RATE, PCM_SAMPLE_RATES[0]),
        max_sample_rate=config.get(CONF_SAMPLE_RATE, PCM_SAMPLE_RATES[-1]),
    )(config)
    return config


FINAL_VALIDATE_SCHEMA = _set_stream_limits


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    cg.add(var.set_output(config[CONF_AUDIO_OUT]))
    cg.add(var.set_initial_volume(config[CONF_VOLUME]))
    cg.add(var.set_idle_timeout(config[CONF_IDLE_TIMEOUT].total_milliseconds))
    await cg.register_component(var, config)
    await register_wb_device(var, config)
    await speaker.register_speaker(var, config)
