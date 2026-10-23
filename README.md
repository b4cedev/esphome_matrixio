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
### With the MATRIX tools
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

### Alternative: flash with esptool directly (Raspberry Pi OS Bookworm and newer)
Use this if the steps above do not work: the MATRIX packages (`apt.matrix.one`, `voice_esp32_enable`,
`voice_esptool`) can no longer be installed, and their scripts use the sysfs GPIO numbers that changed with
kernel 6.6. Everything needed is the Pi's UART, two GPIOs and esptool. Build the firmware as described above
and copy `firmware-factory.bin` to the Pi.
Tested with a Pi 3B+, Raspberry Pi OS Trixie (64 bit) and esptool 4.8. The Pi powers the Voice through the
40 pin header, so use the original 2.5 A power supply (under-voltage made the flash unreliable) and unplug the
Voice's USB cable.

Wiring on the header: GPIO14/15 = ESP32 UART0, GPIO25 = EN (reset), GPIO24 = IO0 (low on reset = download mode).

1. Free the UART: `sudo raspi-config nonint do_serial_cons 1; sudo raspi-config nonint do_serial_hw 0` and add to
   `/boot/firmware/config.txt`:

        dtoverlay=disable-bt        # PL011 on GPIO14/15 -> /dev/serial0 = ttyAMA0 (the mini UART is unreliable)
        gpio=24,25,26=op,dh         # ESP32 boots normally, FPGA not in reset

2. `python3 -m venv ~/esptool-venv && ~/esptool-venv/bin/pip install 'esptool==4.8.*'`, then reboot.
3. Enter download mode, flash, start the firmware again (`pinctrl` ships with Raspberry Pi OS):

        pinctrl set 24 op dl; pinctrl set 25 op dl; sleep 0.1; pinctrl set 25 op dh; sleep 0.3
        ~/esptool-venv/bin/esptool.py --chip esp32 --port /dev/serial0 --baud 230400 --before no_reset --after no_reset \
            write_flash -z --flash_mode dio --flash_freq 40m --flash_size detect 0x0 firmware-factory.bin
        pinctrl set 24 op dh; pinctrl set 25 op dl; sleep 0.1; pinctrl set 25 op dh

   460800 baud produced transfer errors with a short, loosely fitting header extender, 230400 baud was reliable.
   The log of the running firmware is on the same port (115200 baud, e.g. `picocom -b 115200 /dev/serial0`).
   `firmware-factory.bin` is written to 0x0 and therefore also replaces bootloader and partition table.
   Before flashing, back up the old image with `read_flash 0 0x400000 backup.bin` (it contains the WiFi password).


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
The optional `sample_rate` fixes the rate; it is required if the speaker is the output of a `mixer` speaker or of a
`speaker` media player.

### Media player

The `speaker` media player plays media and announcements from Home Assistant (this is also how announcements such as
`assist_satellite.announce` reach the Voice: the voice assistant only handles them with a media player).
The MATRIX Voice has **no PSRAM**, about 160 KB of heap are free after boot, so the media player has to be small:

```yaml
media_player:
  - platform: speaker
    name: MatrixIO Media Player
    id: matrixio_media_player
    buffer_size: 8000        # the default of 1 MB is meant for PSRAM and fails with ESP_ERR_NO_MEM
    announcement_pipeline:   # one pipeline straight to the speaker, no mixer
      speaker: matrixio_speaker
      format: WAV            # FLAC needs ~50 KB more heap for its decoder
      sample_rate: 16000
      num_channels: 1
```

What this means in practice (tested on the device):

* With one pipeline, an announcement (including the voice assistant's answer) interrupts the media instead of ducking
  it. Two pipelines plus a mixer, or the FLAC decoder, run out of memory.
* The voice assistant's microphone buffers and the player pipeline do not fit at the same time. While media plays the
  voice assistant should be stopped and started again afterwards, see `matrix_voice_2026_media.yaml` (no wake word
  while music plays).
* The media player and the direction of arrival do not fit together; `matrix_voice_2026.yaml` has the direction of
  arrival, `matrix_voice_2026_media.yaml` the media player and announcements.

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
| `led_offset`, `led_reverse` | `8.5`, `true` | The Everloop LEDs count clockwise and LED 4 sits at 12 o'clock with the USB port up, so the beam is drawn at LED `led_offset - angle / 20°` (the sound angle counter-clockwise from the right). |

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
