#pragma once
#include <valarray>

#include "esphome/core/component.h"
#include "esphome/core/log.h"
#include "esphome/components/microphone/microphone.h"

#include "../matrixio.h"
#include "../wishbone.h"
#include "../light/everloop.h"
#include "direction_estimator.h"

#include <cmath>
#include <vector>

namespace esphome {
namespace matrixio {


class Microphone : public microphone::Microphone, public WishboneDevice, public Component {

public:
  Microphone() : WishboneDevice(MICROPHONE_BASE_ADDRESS) {}

  void set_sampling_rate(uint32_t sampling_rate);
  void setup() override;
  void dump_config() override;
  void write_config();

  void start() override;
  void stop() override;
  void loop() override;

  // Debug: read all channels and show per-microphone levels on the Everloop
  void set_level_light(light::LightState *ring) {
    this->level_light_ = static_cast<Everloop *>(ring->get_output());
  }
  // Sensitivity of the direction detection, may be called at runtime
  void set_direction_confidence(float confidence) { this->direction_.valid_threshold = confidence; }
  void set_direction_level_db(float db) { this->direction_.min_band_over_floor = std::pow(10.0f, db / 10.0f); }
  void set_direction_min_frames(uint8_t frames) { this->min_valid_frames_ = frames; }
  void set_led_mapping(uint8_t offset, bool reverse) {
    this->led_offset_ = offset;
    this->led_reverse_ = reverse;
  }

private:
  void start_();
  void stop_();
  void read_();
  void read_all_channels_();
  void show_direction_(const DirectionResult &result);
  void write_fir_coeffs();
  void write_sampling_rate_and_gain();

  uint32_t sampling_rate_;
  uint16_t gain_;
  uint16_t pdm_decimation_;
  HighFrequencyLoopRequester high_freq_;

  Everloop *level_light_{nullptr};
  uint8_t led_offset_{0};
  bool led_reverse_{false};
  std::vector<uint8_t> raw_;
  DirectionEstimator direction_;
  float beam_angle_{0.0f};
  float beam_level_{0.0f};
  uint8_t valid_frames_{0};
  uint8_t min_valid_frames_{3};
  uint32_t read_us_{0};
  uint32_t dsp_us_{0};
  uint32_t frame_count_{0};
};


}
}
