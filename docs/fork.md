# Using this fork

This fork (`b4cedev/esphome_matrixio`) carries the port of the component to ESPHome 2026.9.1 plus a few
additions that the original project (`gnumpi/esphome_matrixio`) does not have yet: voice assistant LED
patterns like the Home Assistant Voice PE, sound direction (DOA) and a direction beam on the ring, a
microphone gain, speaker fixes (no click at the start of an answer), the audio output (headphone jack or
speaker connectors) and volume selectable in Home Assistant, and a microphone mute switch. The default branch
`integration/voice-2026` merges all of it. Stable states are tagged, e.g. `v2026.9.1-fork.2`.

Use a **tag**, not the branch: the branch moves with every push, a tag keeps your build reproducible.

## What you need

- A MATRIX Voice and Home Assistant with the ESPHome integration.
- **A Raspberry Pi with the 40 pin header** for the first flash (tested with a Pi 3B+ and Raspberry Pi OS Trixie):
  the Voice is plugged onto the header and flashed through the Pi's UART. After that, updates go over WiFi
  and the Pi is not needed any more. The Pi only flashes, it does **not** build the firmware.
  On a full size Pi (not a Zero) the Voice does not fit directly because of the Pi's USB/Ethernet ports, you need
  a **40 pin header extender** (a short, tightly fitting one: a loose one made the flash fail at higher baud rates).
- **A computer with Python 3.11 or newer** (Mac, Linux, or Windows with WSL) for building. A first ESPHome build
  downloads the toolchain and takes several minutes even on a fast machine, on a Pi it would take far longer.

## Build the firmware (on the computer)

1. Download `matrix_voice_2026.yaml` from this repository (at the tag you want) into an empty folder.
2. Create `secrets.yaml` next to it:

        wifi_ssid: "your-wifi"
        wifi_password: "your-password"

3. In `matrix_voice_2026.yaml`, replace the `external_components` block (it points at a local checkout) with:

        external_components:
          - source:
              type: git
              url: https://github.com/b4cedev/esphome_matrixio
              ref: v2026.9.1-fork.2
            components: [matrixio]

4. Install exactly the ESPHome version the component is pinned to (see the README) in a virtualenv and build:

        python3 -m venv .venv
        . .venv/bin/activate
        pip install esphome==2026.9.1
        esphome compile matrix_voice_2026.yaml

   The result is `.esphome/build/matrixio-voice/.pioenvs/matrixio-voice/firmware-factory.bin`.

## First flash (on the Pi)

1. Copy the firmware to the Pi, e.g. `scp .esphome/build/matrixio-voice/.pioenvs/matrixio-voice/firmware-factory.bin <user>@<pi>:`
2. Set up the Pi and flash as described in "Alternative: flash with esptool directly" of the [README](../README.md#alternative-flash-with-esptool-directly-raspberry-pi-os-bookworm-and-newer)
   (wiring, UART, esptool, the original 2.5 A power supply). Back up the original firmware first.

## Later updates (WiFi)

Back on the computer, in the virtualenv: `esphome run matrix_voice_2026.yaml --device <ip of the Voice>`.

The device shows up in Home Assistant as `matrixio-voice`. Wake word detection runs in Home Assistant
(openWakeWord or similar), so the Voice itself needs no wake word model.

## Updating

Pick a newer tag, change `ref:` and the YAML (compare with the new version of `matrix_voice_2026.yaml`), then
upload again over WiFi.

## Status

The component works on the author's Voice with Home Assistant 2026.9.x. The port is also offered to the
original project; as long as it is not merged there, this fork is the place to get it.
