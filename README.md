# ESPHome MatrixIO
**Target: ESPHome-2026.9.1**

[![ESPHome-target](https://github.com/gnumpi/esphome_matrixio/actions/workflows/tox-target.yml/badge.svg)](https://github.com/gnumpi/esphome_matrixio/actions/workflows/tox-target.yml)
[![ESPHome-latest](https://github.com/gnumpi/esphome_matrixio/actions/workflows/tox-latest.yml/badge.svg)](https://github.com/gnumpi/esphome_matrixio/actions/workflows/tox-latest.yml)



This custom component gives access to the LED-Strip (everloop), microphones and speakers of the [Matrixio-Voice](https://matrix-io.github.io/matrix-documentation/matrix-voice/overview/) within [ESPHome](https://esphome.io/).

It allows to use a Matrixio-Voice as a satellite for [Home Assistant](https://www.home-assistant.io/) [voice control](https://www.home-assistant.io/voice_control/).

It can also be used as a media player for Home Assistant, built on ESPHome's native `speaker` media player
(see `examples/matrixio_media_player.yaml`). Further configuration files can be found in the 'examples' folder.

Older ESPHome versions (including 2023.x) and the `adf_pipeline` platform (`gnumpi/esphome_audio`) are no longer supported.

Right now, only one microphone of the array is used. I might add a beamforming in the future.

The implementation is based on the example code which MATRIX Labs provides on their project page: https://github.com/matrix-io.


## Installation
#### Prepare Raspberry PI
Follow "Step 1: Raspberry Pi Setup" of  https://matrix-io.github.io/matrix-documentation/matrix-voice/esp32/

Unfortunately, the packages of MATRIX Lab are now longer maintained. Checkout the repository of Kunal Bansal
https://github.com/qnlbnsl/Matrix-IO if you run into installation issues, especially with the new Rapsberry PI OS Bookworm.

#### Build firmware
    git clone https://github.com/gnumpi/esphome_matrixio.git
    cd esphome_matrixio
    source scripts/setup_esphome.sh
    esphome compile matrix_voice.yaml

copy .esphome/build/matrixio-voice/.pioenvs/matrixio-voice/firmware-factory.bin to raspberry pi.

#### Flash ESP32
On raspberry pi:

    sudo voice_esp32_enable

    voice_esptool --chip esp32 --port /dev/ttyS0 --baud 1500000 --before default_reset --after hard_reset write_flash -u --flash_mode dio --flash_freq 40m --flash_size detect 0x000 firmware-factory.bin


## Configuration

```yaml
esp32:
  board: esp32dev
  framework:
    type: esp-idf
    version: recommended

spi:
  clk_pin:  GPIO32
  mosi_pin: GPIO33
  miso_pin: GPIO21

matrixio:
  id: matrixio_dev
  cs_pin: GPIO23

light:
  - platform: matrixio
    name: everloop
    id: everloop

microphone:
  - platform: matrixio
    id: matrixio_mic

speaker:
  - platform: matrixio
    id: matrixio_speaker
    # [headphone,speakers], default: headphone
    audio_out: headphone
    # 0..100, in percent, default: 80
    volume: 90


```

`ota` needs a platform in current ESPHome (`ota: - platform: esphome`).

The `speaker` accepts 16 bit PCM at 8000, 16000, 32000, 44100 and 48000 Hz (mono is duplicated to stereo).
The optional `sample_rate` fixes the rate; it is required if the speaker is the output of a `mixer` speaker,
which is how the media player and voice announcements share the speaker.

### Direction of arrival

The microphone array has 8 microphones: channel 0 in the middle and channels 1 to 7 on a ring of 77 mm diameter.
By default only channel 0 is read and forwarded as the mono stream. With the optional `direction` block all
channels are read and the direction of sounds is estimated (SRP-PHAT between about 300 Hz and 2 kHz). The
audio stream to consumers such as `voice_assistant` stays mono.

```yaml
microphone:
  - platform: matrixio
    id: matrixio_mic
    direction:
      # keep detecting when nothing consumes the audio, e.g. for presence (default: false)
      always_on: true
      # show the direction as a beam on the Everloop, on top of the light's current colours
      beam_light: everloop
      angle:
        name: "Sound direction"            # degrees, 0 = USB port side, clockwise (90 = right)
      confidence:
        name: "Sound direction confidence"
      level:
        name: "Sound level over background"
      active:
        name: "Sound detected"
```

| Option | Default | Meaning |
| --- | --- | --- |
| `always_on` | `false` | Read the microphones also while nothing consumes the audio. |
| `detect_confidence` | `0.5` | How coherent a sound must be across the microphones (0.2 to 0.95) to count as detected (sensors). |
| `detect_level_db` | `6` | How far the sound must be above the tracked background level, in dB. |
| `min_frames` | `3` | Frames (32 ms each) in a row needed before a sound counts as detected. |
| `hold_time` | `3s` | `active` stays on this long after the last detected frame. |
| `smoothing` | `300ms` | Time constant of the direction smoothing. |
| `beam_light` | | Everloop light that shows the beam. |
| `beam_confidence`, `beam_level_db` | `0.62`, `9` | Stricter thresholds for the beam, changeable at runtime with `set_beam_confidence()` / `set_beam_level_db()`. |
| `led_offset`, `led_reverse` | `9`, `true` | Everloop LED 0 sits on the left (USB port up) and the LEDs count clockwise. |

The beam is switched from automations, for example to show it from the start of a voice assistant turn until its end,
and to freeze it on the direction of the speaker while the answer is played:

```yaml
voice_assistant:
  on_listening:
    - lambda: id(matrixio_mic).set_beam_enabled(true);   # visible, follows the sound
  on_stt_vad_end:
    - lambda: id(matrixio_mic).set_beam_tracking(false); # keep the last direction
  on_end:
    - lambda: id(matrixio_mic).set_beam_enabled(false);
```

Reading all 8 channels takes about 8.7 ms of every 32 ms audio frame on the SPI bus, the estimate itself runs in a
low priority task (4 to 19 ms, a frame is skipped when it is still busy).
