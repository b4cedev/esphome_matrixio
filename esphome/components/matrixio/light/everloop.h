#pragma once

#include "esphome/core/component.h"
#include "esphome/core/log.h"
#include <cstring>
#include "esphome/components/light/addressable_light.h"

#include "esphome/components/matrixio/wishbone.h"

namespace esphome {
namespace matrixio {

const uint32_t EVERLOOP_BASE_ADDRESS = 0x3000;
const uint32_t NUMBER_OF_LEDS = 18;

class Everloop : public light::AddressableLight, public matrixio::WishboneDevice {
public:
    Everloop();

    int32_t size() const override { return this->num_leds_; }

    void setup();
    void dump_config();
    float get_setup_priority() const override;

    light::LightTraits get_traits() override {
        auto traits = light::LightTraits();
        traits.set_supported_color_modes({light::ColorMode::RGB_WHITE});
        return traits;
    }

    void write_state(light::LightState *state) override { this->write_composed_(); }

    // Overlay (RGBW, 4 bytes per LED) added on top of the light state, e.g. a direction beam.
    // Pass all zeros to remove it. Call from the main loop only, like write_state().
    void set_overlay(const uint8_t *rgbw) {
        if (this->is_failed() || this->overlay_ == nullptr)
            return;
        memcpy(this->overlay_, rgbw, this->buffer_size_);
        this->write_composed_();
    }

    // Write an RGBW frame (4 bytes per LED) directly, bypassing the light state
    void write_raw(uint8_t *rgbw) {
        if (this->is_failed())
            return;
        this->wb_write(rgbw, this->buffer_size_);
    }

    void clear_effect_data() override {
        for (int i = 0; i < this->size(); i++)
            this->effect_data_[i] = 0;
    }

protected:
    light::ESPColorView get_view_internal(int32_t index) const override;

private:
    size_t buffer_size_{};
    uint8_t *effect_data_{nullptr};
    uint8_t *buf_{nullptr};
    uint8_t *overlay_{nullptr};
    uint8_t *out_{nullptr};

    void write_composed_();
    int32_t num_leds_;
};

} //namespace matrixio
} //namespace esphome
