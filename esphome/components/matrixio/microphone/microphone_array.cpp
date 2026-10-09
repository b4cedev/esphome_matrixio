#include "microphone_array.h"
#include "../microphone_fir.h"

#include <driver/gpio.h>
#include <hal/gpio_types.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <esp_timer.h>
#include <algorithm>
#include <cmath>

static const char *const TAG = "matrixio_microphone";

static QueueHandle_t irq_queue;

// Microphone positions in metres (x, y), channel 0..7, from the ODAS matrix_voice.cfg. Channel 0 is in the
// centre, channels 1..7 are on a ring around the origin.
static const float MIC_POSITIONS[8][2] = {
    {+0.000000f, +0.000000f},  {-0.038133f, +0.003576f}, {-0.020980f, +0.032043f}, {+0.011971f, +0.036381f},
    {+0.035908f, +0.013323f},  {+0.032805f, -0.019767f}, {+0.004999f, -0.037972f}, {-0.026571f, -0.027584f}};

// Transfers over the Wishbone bus are limited to ~4 KB, read the channel buffer in pieces.
static const size_t READ_CHUNK_BYTES = 2048;

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

  if (this->level_light_ != nullptr) {
    this->raw_.resize(MICROPHONE_BUFFER_SIZE * sizeof(int16_t));
    this->direction_.setup(MIC_POSITIONS, 16000.0f);
    this->start();  // no consumer will ask for data in this debug mode
  }
}

void Microphone::dump_config(){
  esph_log_config(TAG, "Matrixio Microphones:");
  if (this->level_light_ != nullptr) {
    esph_log_config(TAG, "  Debug: direction of arrival on the Everloop (offset %u, %s)", this->led_offset_,
                    this->led_reverse_ ? "clockwise" : "counter-clockwise");
  }
}

void Microphone::write_config(){
  this->write_sampling_rate_and_gain();
  this->write_fir_coeffs();
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
    gpio_isr_handler_add(static_cast<gpio_num_t>(MICROPHONE_ARRAY_IRQ), irq_handler, NULL);
  };

void Microphone::stop() {
  if (this->state_ == microphone::STATE_STOPPED || this->is_failed()) return;
  if (this->state_ == microphone::STATE_STARTING) {
      this->state_ = microphone::STATE_STOPPED;
      return;
  }

  gpio_isr_handler_remove(static_cast<gpio_num_t>(MICROPHONE_ARRAY_IRQ));
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
  // The FPGA raises the IRQ every NUMBER_OF_SAMPLES (512) samples per channel (31.25 Hz at 16 kHz) and
  // stores the channels one after another, so the first 512 * 2 bytes are the complete mono stream.
  if (this->level_light_ != nullptr) {
    this->read_all_channels_();
    return;
  }
  const size_t bytes_to_read = NUMBER_OF_SAMPLES * sizeof(int16_t);
  std::vector<uint8_t> data(bytes_to_read);
  this->wb_read(data.data(), data.size());
  this->data_callbacks_.call(data);
}

void Microphone::read_all_channels_() {
  const int64_t t0 = esp_timer_get_time();
  const size_t total = MICROPHONE_BUFFER_SIZE * sizeof(int16_t);
  for (size_t offset = 0; offset < total; offset += READ_CHUNK_BYTES) {
    this->wb_read_at(offset / sizeof(uint16_t), this->raw_.data() + offset, READ_CHUNK_BYTES);
  }
  this->read_us_ = static_cast<uint32_t>(esp_timer_get_time() - t0);

  // Channel 0 stays the mono stream for consumers
  if (this->data_callbacks_.size() > 0) {
    std::vector<uint8_t> mono(this->raw_.begin(), this->raw_.begin() + NUMBER_OF_SAMPLES * sizeof(int16_t));
    this->data_callbacks_.call(mono);
  }

  const int64_t t1 = esp_timer_get_time();
  const DirectionResult result = this->direction_.process(reinterpret_cast<const int16_t *>(this->raw_.data()));
  this->dsp_us_ = static_cast<uint32_t>(esp_timer_get_time() - t1);

  this->show_direction_(result);

  if (++this->frame_count_ % 8 == 0) {
    ESP_LOGW(TAG, "MICDBG DOA %s %3.0f deg, confidence %.2f, band %.0f dB, SPI read %u us, DSP %u us",
             result.valid ? "valid  " : "invalid", result.angle_deg, result.confidence, result.level_db,
             (unsigned) this->read_us_, (unsigned) this->dsp_us_);
  }
}

// Beam on the ring at the direction of the sound, faded out when the direction is lost
void Microphone::show_direction_(const DirectionResult &result) {
  // Show the beam only after a few valid frames in a row, so short clicks (typing, cars) do not flash
  this->valid_frames_ = result.valid ? std::min(this->valid_frames_ + 1, 255) : 0;
  if (this->valid_frames_ >= this->min_valid_frames_) {
    this->beam_angle_ = result.angle_deg;
    this->beam_level_ = std::min(1.0f, std::max(0.0f, (result.confidence - (this->direction_.valid_threshold - 0.07f)) / 0.30f));
  } else {
    this->beam_level_ *= 0.92f;
  }

  static uint8_t frame[NUMBER_OF_LEDS * 4];
  memset(frame, 0, sizeof(frame));
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
      const uint8_t value = static_cast<uint8_t>(weight * this->beam_level_ * 150.0f);
      frame[led * 4 + 1] = value / 2;  // green
      frame[led * 4 + 2] = value;      // blue
    }
  }
  this->level_light_->write_raw(frame);
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
      break;
    case microphone::STATE_STARTING:
      this->start_();
      break;
    case microphone::STATE_RUNNING:
      if (this->data_callbacks_.size() > 0 || this->level_light_ != nullptr) {
        this->read_();
      }
      break;
    case microphone::STATE_STOPPING:
      this->stop_();
      break;
  }
}

} //namespace matrixio
} //namespace esphome
