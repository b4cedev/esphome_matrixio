#include "microphone_array.h"
#include "../microphone_fir.h"

#include <driver/gpio.h>
#include <hal/gpio_types.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <esp_timer.h>
#include <algorithm>
#include <cmath>
#include <cstring>

static const char *const TAG = "matrixio_microphone";

static QueueHandle_t irq_queue;

// Microphone positions in metres (x, y), channel 0..7, from the ODAS matrix_voice.cfg. Channel 0 is in the
// centre, channels 1..7 are on a ring around the origin.
static const float MIC_POSITIONS[8][2] = {
    {+0.000000f, +0.000000f},  {-0.038133f, +0.003576f}, {-0.020980f, +0.032043f}, {+0.011971f, +0.036381f},
    {+0.035908f, +0.013323f},  {+0.032805f, -0.019767f}, {+0.004999f, -0.037972f}, {-0.026571f, -0.027584f}};

// Transfers over the Wishbone bus are limited to ~4 KB, read the channel buffer in pieces.
static const size_t READ_CHUNK_BYTES = 2048;

// A direction found this long before the beam is switched on is still used (the wake word comes first)
static const uint32_t BEAM_LOOKBACK_MS = 3000;
// Sensors are updated at most this often, and only when the direction moved by at least this much
static const uint32_t PUBLISH_INTERVAL_MS = 250;
static const float PUBLISH_MIN_CHANGE_DEG = 5.0f;

// The estimator works in the microphone coordinates (counter-clockwise from the right, 90 = USB port side).
// Sensors and logs use a compass angle: 0 = USB port side (up), increasing clockwise.
static float to_compass_deg(float angle_deg) {
  float compass = std::fmod(90.0f - angle_deg, 360.0f);
  return compass < 0.0f ? compass + 360.0f : compass;
}

static void irq_handler(void *args) {
  gpio_num_t gpio;
  gpio = static_cast<gpio_num_t>(esphome::matrixio::MICROPHONE_ARRAY_IRQ);
  xQueueSendToBackFromISR(irq_queue, &gpio, NULL);
}

namespace esphome {
namespace matrixio {

void Microphone::setup() {
  this->set_sampling_rate(16000);
  this->write_config();
  this->audio_stream_info_ = audio::AudioStreamInfo(16, 1, this->sampling_rate_);

  // IRQ handling
  irq_queue = xQueueCreate(10, sizeof(gpio_num_t));

  gpio_config_t gpioConfig;
  gpioConfig.pin_bit_mask = BIT(5); //GPIO_SEL_5;
  gpioConfig.mode = GPIO_MODE_INPUT;
  gpioConfig.pull_up_en = GPIO_PULLUP_DISABLE;
  gpioConfig.pull_down_en = GPIO_PULLDOWN_ENABLE;
  gpioConfig.intr_type = GPIO_INTR_ANYEDGE;

  gpio_config(&gpioConfig);
  gpio_install_isr_service(0);

  if (this->direction_enabled_) {
    this->raw_.resize(MICROPHONE_BUFFER_SIZE * sizeof(int16_t));
    this->dsp_buf_.resize(MICROPHONE_BUFFER_SIZE * sizeof(int16_t));
    this->direction_.setup(MIC_POSITIONS, 16000.0f);
    // Below the speaker task (priority 2), so playback never waits for the direction estimate
    xTaskCreate(Microphone::direction_task_, "mic_doa", 6144, this, 1, &this->dsp_task_);
    if (this->always_on_) {
      xQueueReset(irq_queue);
      this->attach_irq_();
    }
  }
}

void Microphone::dump_config(){
  esph_log_config(TAG, "Matrixio Microphones:");
  if (this->direction_enabled_) {
    esph_log_config(TAG, "  Direction of arrival: %s", this->always_on_ ? "always on" : "while the microphone runs");
    esph_log_config(TAG, "    detect: confidence >= %.2f, level >= %.1f dB over background, %u frames",
                    this->direction_.valid_threshold, 10.0f * std::log10(this->direction_.min_band_over_floor),
                    this->min_valid_frames_);
    if (this->beam_light_ != nullptr) {
      esph_log_config(TAG, "    beam: confidence >= %.2f, level >= %.1f dB (LED 0 offset %u, %s)", this->beam_confidence_,
                      this->beam_level_db_, this->led_offset_, this->led_reverse_ ? "clockwise" : "counter-clockwise");
    }
  }
}

void Microphone::write_config(){
  this->write_sampling_rate_and_gain();
  this->write_fir_coeffs();
}

void Microphone::attach_irq_() {
  if (this->irq_attached_) return;
  gpio_isr_handler_add(static_cast<gpio_num_t>(MICROPHONE_ARRAY_IRQ), irq_handler, NULL);
  this->irq_attached_ = true;
}

void Microphone::detach_irq_() {
  if (!this->irq_attached_) return;
  gpio_isr_handler_remove(static_cast<gpio_num_t>(MICROPHONE_ARRAY_IRQ));
  this->irq_attached_ = false;
}

void Microphone::start(){
    ESP_LOGD(TAG, "Starting Microphone.");
    if (this->is_failed())
    {
       ESP_LOGE(TAG, "Microphone not ready.");
       return;
    }
    if (this->state_ == microphone::STATE_RUNNING)
        return;  // Already running
    this->state_ = microphone::STATE_STARTING;
    xQueueReset(irq_queue);
    this->attach_irq_();
  };

void Microphone::stop() {
  if (this->state_ == microphone::STATE_STOPPED || this->is_failed()) return;
  if (this->state_ == microphone::STATE_STARTING) {
      this->state_ = microphone::STATE_STOPPED;
      if (!this->always_on_) this->detach_irq_();
      return;
  }

  // With always-on direction detection the FPGA keeps being read, only the consumer stops
  if (!this->always_on_) this->detach_irq_();
  this->state_ = microphone::STATE_STOPPING;
};

void Microphone::set_sampling_rate(uint32_t sampling_rate){
  for (int i = 0;; i++) {
    if (MIC_SAMPLING_FREQUENCIES[i][0] == 0){
      ESP_LOGE(TAG, "Unsupported sampling rate : %u", sampling_rate );
      this->mark_failed();
      return;
    }

    if (MIC_SAMPLING_FREQUENCIES[i][0] == sampling_rate) {
      this->sampling_rate_ = MIC_SAMPLING_FREQUENCIES[i][0];
      this->pdm_decimation_ = MIC_SAMPLING_FREQUENCIES[i][1];
      this->gain_ = MIC_SAMPLING_FREQUENCIES[i][2];
      break;
    }
  }
}

void Microphone::write_fir_coeffs(){
  const FIRCoeff* coeff_list = FIR_default;
  for (int i = 0;; i++) {
    if (coeff_list[i].rate_ == 0) {
      ESP_LOGE(TAG, "Unsupported sampling frequency for setting FIR coeffs: %u", this->sampling_rate_);
      this->mark_failed();
      return;
    }
    if (coeff_list[i].rate_ == this->sampling_rate_) {
      if (coeff_list[i].coeff_.size() == NUMBER_OF_FIR_TAPS) {
        std::valarray<int16_t> fir_coeff_ = coeff_list[i].coeff_;
        this->wb_write(reinterpret_cast<uint8_t *>(&fir_coeff_[0]), fir_coeff_.size() * sizeof(int16_t));
        return;
      } else {
        ESP_LOGE(TAG, "Size FIR Filter must be : %u", NUMBER_OF_FIR_TAPS );
        this->mark_failed();
      }
    }
  }
}

void Microphone::write_sampling_rate_and_gain(){
  this->conf_write(0x06, this->pdm_decimation_);
  this->conf_write(0x07, this->gain_);
}

void Microphone::read_() {
  // Wait for the FPGA to signal that new PCM data is available, never block the main loop
  gpio_num_t gpio;
  if (xQueueReceive(irq_queue, &gpio, 0) != pdTRUE) {
    return;
  }
  if (this->direction_enabled_) {
    this->read_all_channels_();
    return;
  }
  // The FPGA raises the IRQ every NUMBER_OF_SAMPLES (512) samples per channel (31.25 Hz at 16 kHz) and
  // stores the channels one after another, so the first 512 * 2 bytes are the complete mono stream.
  // The buffer is allocated once: with the media player the heap gets tight and a failing allocation here aborts.
  this->mono_.resize(NUMBER_OF_SAMPLES * sizeof(int16_t));
  this->wb_read(this->mono_.data(), this->mono_.size());
  this->data_callbacks_.call(this->mono_);
}

void Microphone::read_all_channels_() {
  const int64_t t0 = esp_timer_get_time();
  const size_t total = MICROPHONE_BUFFER_SIZE * sizeof(int16_t);
  for (size_t offset = 0; offset < total; offset += READ_CHUNK_BYTES) {
    this->wb_read_at(offset / sizeof(uint16_t), this->raw_.data() + offset, READ_CHUNK_BYTES);
  }
  this->read_us_ = static_cast<uint32_t>(esp_timer_get_time() - t0);

  // Channel 0 (the centre microphone) stays the mono stream for consumers
  if (this->data_callbacks_.size() > 0) {
    this->mono_.assign(this->raw_.begin(), this->raw_.begin() + NUMBER_OF_SAMPLES * sizeof(int16_t));
    this->data_callbacks_.call(this->mono_);
  }

  this->hand_over_frame_();
}

// The estimate takes up to ~9 ms, which is too long for the main loop, so it runs in its own task.
// If it is still busy with the previous frame, this one is skipped.
void Microphone::hand_over_frame_() {
  if (this->dsp_busy_.load()) {
    this->skipped_frames_++;
    return;
  }
  memcpy(this->dsp_buf_.data(), this->raw_.data(), this->raw_.size());
  this->dsp_busy_.store(true);
  xTaskNotifyGive(this->dsp_task_);
}

void Microphone::direction_task_(void *arg) {
  Microphone *self = static_cast<Microphone *>(arg);
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    const int64_t t0 = esp_timer_get_time();
    const DirectionResult result = self->direction_.process(reinterpret_cast<const int16_t *>(self->dsp_buf_.data()));
    self->dsp_us_ = static_cast<uint32_t>(esp_timer_get_time() - t0);
    portENTER_CRITICAL(&self->result_lock_);
    self->latest_ = result;
    portEXIT_CRITICAL(&self->result_lock_);
    self->result_seq_.fetch_add(1);
    self->dsp_busy_.store(false);
  }
}

void Microphone::handle_result_(const DirectionResult &raw) {
  const uint32_t now = millis();
  this->valid_frames_ = raw.valid ? std::min(this->valid_frames_ + 1, 255) : 0;
  const bool detected = this->valid_frames_ >= this->min_valid_frames_;
  if (detected) this->active_until_ms_ = now + this->active_hold_ms_;

  // Smooth the direction: circular average of the frames, weighted by confidence, with exponential forgetting.
  // A single wrong frame hardly moves it, a real change of the source takes over within a few frames.
  this->vec_x_ *= this->smoothing_decay_;
  this->vec_y_ *= this->smoothing_decay_;
  DirectionResult result = raw;
  if (raw.valid) {
    const float weight = raw.confidence * raw.confidence;
    const float rad = raw.angle_deg * static_cast<float>(M_PI) / 180.0f;
    this->vec_x_ += weight * std::cos(rad);
    this->vec_y_ += weight * std::sin(rad);
  }
  if (std::fabs(this->vec_x_) + std::fabs(this->vec_y_) > 1e-4f) {
    float smoothed = std::atan2(this->vec_y_, this->vec_x_) * 180.0f / static_cast<float>(M_PI);
    result.angle_deg = smoothed < 0.0f ? smoothed + 360.0f : smoothed;
  }

  this->publish_direction_(result, detected);
  this->show_beam_(result, detected);

  if (++this->frame_count_ % 32 == 0) {
    ESP_LOGD(TAG, "Direction %s %3.0f deg, confidence %.2f, %.0f dB over background, SPI %u us, DSP %u us, skipped %u",
             detected ? "valid  " : "invalid", to_compass_deg(result.angle_deg), result.confidence, result.over_floor_db,
             (unsigned) this->read_us_, (unsigned) this->dsp_us_, (unsigned) this->skipped_frames_);
  }
}

void Microphone::publish_direction_(const DirectionResult &result, bool detected) {
  const uint32_t now = millis();
  const bool active = detected || static_cast<int32_t>(this->active_until_ms_ - now) > 0;
  if (active != this->active_) {
    this->active_ = active;
#ifdef USE_BINARY_SENSOR
    if (this->active_sensor_ != nullptr) this->active_sensor_->publish_state(active);
#endif
  }

  if (!detected || now - this->last_publish_ms_ < PUBLISH_INTERVAL_MS) return;
  const float compass = to_compass_deg(result.angle_deg);
  if (this->last_published_angle_ >= 0.0f) {
    float change = std::fmod(std::fabs(compass - this->last_published_angle_), 360.0f);
    change = std::min(change, 360.0f - change);
    if (change < PUBLISH_MIN_CHANGE_DEG) return;
  }

  this->last_publish_ms_ = now;
  this->last_published_angle_ = compass;
#ifdef USE_SENSOR
  if (this->angle_sensor_ != nullptr) this->angle_sensor_->publish_state(std::round(compass));
  if (this->confidence_sensor_ != nullptr) this->confidence_sensor_->publish_state(result.confidence);
  if (this->level_sensor_ != nullptr) this->level_sensor_->publish_state(result.over_floor_db);
#endif
}

void Microphone::set_beam_enabled(bool enabled) {
  if (enabled && !this->beam_enabled_) {
    // Start with the direction of the last sound just before (e.g. the wake word), if there was one
    this->beam_level_ = 0.0f;
    if (this->has_last_beam_ && millis() - this->last_beam_ms_ < BEAM_LOOKBACK_MS) {
      this->beam_angle_ = this->last_beam_angle_;
      this->beam_level_ = 0.6f;
    }
  }
  this->beam_enabled_ = enabled;
  this->beam_tracking_ = enabled;
}

// Beam on the ring at the direction of the sound. It stays on the last direction until it is switched off
// (the turn is over) and follows new directions only while tracking. The beam is an overlay on top of
// whatever the light entity shows (e.g. the voice assistant colours).
void Microphone::show_beam_(const DirectionResult &result, bool detected) {
  if (this->beam_light_ == nullptr) return;

  const bool qualifies = detected && result.confidence >= this->beam_confidence_ &&
                         result.over_floor_db >= this->beam_level_db_;
  if (qualifies) {
    this->last_beam_angle_ = result.angle_deg;
    this->last_beam_ms_ = millis();
    this->has_last_beam_ = true;
  }
  if (!this->beam_enabled_) {
    this->beam_level_ *= 0.92f;
  } else if (qualifies && this->beam_tracking_) {
    this->beam_angle_ = result.angle_deg;
    this->beam_level_ = std::min(1.0f, std::max(0.4f, (result.confidence - (this->beam_confidence_ - 0.07f)) / 0.30f));
  }

  uint8_t overlay[NUMBER_OF_LEDS * 4];
  memset(overlay, 0, sizeof(overlay));
  if (this->beam_level_ > 0.02f) {
    // fractional LED position of the angle
    float position = this->beam_angle_ / (360.0f / NUMBER_OF_LEDS);
    if (this->led_reverse_) position = -position;
    position += this->led_offset_;
    for (int led = 0; led < NUMBER_OF_LEDS; led++) {
      // circular distance between the LED and the beam position, in LEDs
      float distance = std::fmod(position - led + 2.0f * NUMBER_OF_LEDS, (float) NUMBER_OF_LEDS);
      distance = std::min(distance, NUMBER_OF_LEDS - distance);
      const float weight = std::max(0.0f, 1.0f - distance / 1.5f);
      overlay[led * 4 + 3] = static_cast<uint8_t>(weight * this->beam_level_ * 150.0f);  // white
    }
  }
  if (memcmp(overlay, this->overlay_, sizeof(overlay)) != 0) {
    memcpy(this->overlay_, overlay, sizeof(overlay));
    this->beam_light_->set_overlay(this->overlay_);
  }
}

void Microphone::start_() {
  this->state_ = microphone::STATE_RUNNING;
  this->high_freq_.start();
}

void Microphone::stop_() {
  this->state_ = microphone::STATE_STOPPED;
  this->high_freq_.stop();
}

void Microphone::loop() {
  switch (this->state_) {
    case microphone::STATE_STOPPED:
      if (this->direction_enabled_ && this->always_on_) {
        this->read_();
      }
      break;
    case microphone::STATE_STARTING:
      this->start_();
      break;
    case microphone::STATE_RUNNING:
      if (this->data_callbacks_.size() > 0 || this->direction_enabled_) {
        this->read_();
      }
      break;
    case microphone::STATE_STOPPING:
      this->stop_();
      break;
  }

  if (this->direction_enabled_) {
    const uint32_t seq = this->result_seq_.load();
    if (seq != this->seen_seq_) {
      this->seen_seq_ = seq;
      DirectionResult result;
      portENTER_CRITICAL(&this->result_lock_);
      result = this->latest_;
      portEXIT_CRITICAL(&this->result_lock_);
      this->last_result_ms_ = millis();
      this->handle_result_(result);
    } else if (millis() - this->last_result_ms_ > 300) {
      // No frames arrive (microphone stopped): let the detection and the beam run out
      DirectionResult none;
      if (!this->beam_enabled_) this->beam_level_ = 0.0f;
      this->last_result_ms_ = millis();
      this->handle_result_(none);
    }
  }
}

} //namespace matrixio
} //namespace esphome
