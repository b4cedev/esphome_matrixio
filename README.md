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
