#pragma once
#include <valarray>

#include "esphome/core/component.h"
#include "esphome/core/log.h"
#include "esphome/components/microphone/microphone.h"
#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif

#include "../matrixio.h"
#include "../wishbone.h"
#include "../light/everloop.h"
#include "direction_estimator.h"

#include <atomic>
#include <cmath>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

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

  // --- Direction of arrival (optional, reads all 8 channels) ---
  void enable_direction(bool always_on) {
    this->direction_enabled_ = true;
    this->always_on_ = always_on;
  }
  // Reporting: when sound counts as detected, shown on the sensors
  void set_detect_confidence(float confidence) { this->direction_.valid_threshold = confidence; }
  void set_detect_level_db(float db) { this->direction_.min_band_over_floor = std::pow(10.0f, db / 10.0f); }
  void set_min_frames(uint8_t frames) { this->min_valid_frames_ = frames; }
  // Beam on the Everloop: stricter thresholds, may be changed at runtime, can be switched by automations
  void set_beam_light(light::LightState *ring) { this->beam_light_ = static_cast<Everloop *>(ring->get_output()); }
  void set_beam_confidence(float confidence) { this->beam_confidence_ = confidence; }
  void set_beam_level_db(float db) { this->beam_level_db_ = db; }
  // Show the beam (and follow new directions). While visible but not tracking, the last direction is held.
  void set_beam_enabled(bool enabled);
  void set_beam_tracking(bool tracking) { this->beam_tracking_ = tracking; }
  void set_hold_time_ms(uint32_t ms) { this->active_hold_ms_ = ms; }
  // Beamforming: the stream for consumers is the delay-and-sum of all 8 microphones, steered at the detected
  // direction (plain sum while there is none) instead of the centre microphone alone. May be switched at runtime.
  void set_beamforming(bool enabled) { this->beamforming_ = enabled; }
  bool get_beamforming() const { return this->beamforming_; }
  // Time constant of the direction smoothing (one frame is 32 ms), 0 = none
  void set_smoothing_ms(uint32_t ms) { this->smoothing_decay_ = ms == 0 ? 0.0f : std::exp(-32.0f / ms); }
  void set_led_mapping(float offset, bool reverse) {
    this->led_offset_ = offset;
    this->led_reverse_ = reverse;
  }
#ifdef USE_SENSOR
  void set_angle_sensor(sensor::Sensor *sensor) { this->angle_sensor_ = sensor; }
  void set_confidence_sensor(sensor::Sensor *sensor) { this->confidence_sensor_ = sensor; }
  void set_level_sensor(sensor::Sensor *sensor) { this->level_sensor_ = sensor; }
#endif
#ifdef USE_BINARY_SENSOR
  void set_active_sensor(binary_sensor::BinarySensor *sensor) { this->active_sensor_ = sensor; }
#endif

private:
  void start_();
  void stop_();
  void read_();
  void read_all_channels_();
  void attach_irq_();
  void detach_irq_();
  void hand_over_frame_();
  void beamform_();
  void handle_result_(const DirectionResult &result);
  void show_beam_(const DirectionResult &result, bool detected);
  void publish_direction_(const DirectionResult &result, bool detected);
  static void direction_task_(void *arg);
  void write_fir_coeffs();
  void write_sampling_rate_and_gain();

  uint32_t sampling_rate_;
  uint16_t gain_;
  uint16_t pdm_decimation_;
  HighFrequencyLoopRequester high_freq_;
  bool irq_attached_{false};

  // Direction of arrival
  bool direction_enabled_{false};
  bool always_on_{false};
  DirectionEstimator direction_;
  std::vector<uint8_t> mono_;     // channel 0 for the consumers, reused every frame
  std::vector<uint8_t> raw_;      // frame as read from the FPGA (main loop)
  std::vector<uint8_t> dsp_buf_;  // frame being processed by the DSP task
  TaskHandle_t dsp_task_{nullptr};
  std::atomic<bool> dsp_busy_{false};
  std::atomic<uint32_t> result_seq_{0};
  uint32_t seen_seq_{0};
  uint32_t last_result_ms_{0};
  DirectionResult latest_;  // written by the DSP task under result_lock_
  portMUX_TYPE result_lock_ = portMUX_INITIALIZER_UNLOCKED;
  uint32_t dsp_us_{0};
  uint32_t read_us_{0};
  uint32_t frame_count_{0};
  uint32_t skipped_frames_{0};

  uint8_t valid_frames_{0};
  uint8_t min_valid_frames_{3};
  uint32_t active_until_ms_{0};
  uint32_t active_hold_ms_{3000};
  float smoothing_decay_{0.9f};
  float vec_x_{0.0f};  // confidence weighted sum of direction vectors, decays every frame
  float vec_y_{0.0f};
  bool active_{false};
  uint32_t last_publish_ms_{0};
  float last_published_angle_{-1000.0f};

  // Beamforming (main loop only)
  static const int BF_HISTORY = 8;  // samples kept from the previous frame per microphone
  bool beamforming_{false};
  bool steering_{false};            // a direction is known: steer at steer_angle_ (microphone coordinates)
  float steer_angle_{0.0f};
  float delay_[8]{};                // delay of every microphone in samples, as used for the last frame
  int16_t history_[8][BF_HISTORY]{};

  // Beam
  Everloop *beam_light_{nullptr};
  bool beam_enabled_{true};
  bool beam_tracking_{true};
  float last_beam_angle_{0.0f};  // last direction that met the beam thresholds, even while the beam is off
  uint32_t last_beam_ms_{0};
  bool has_last_beam_{false};
  float beam_confidence_{0.62f};
  float beam_level_db_{9.0f};
  float led_offset_{8.5f};
  bool led_reverse_{true};
  float beam_angle_{0.0f};
  float beam_level_{0.0f};
  uint8_t overlay_[NUMBER_OF_LEDS * 4]{};

#ifdef USE_SENSOR
  sensor::Sensor *angle_sensor_{nullptr};
  sensor::Sensor *confidence_sensor_{nullptr};
  sensor::Sensor *level_sensor_{nullptr};
#endif
#ifdef USE_BINARY_SENSOR
  binary_sensor::BinarySensor *active_sensor_{nullptr};
#endif
};


}
}
