#include "speaker.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include <esp_timer.h>
#include <freertos/FreeRTOS.h>

namespace esphome {
namespace matrixio {

static const size_t BUFFER_COUNT = 32;
// Must be above the priority of the ESPHome main loop task (1), otherwise the FIFO runs dry.
static const UBaseType_t PLAYER_TASK_PRIORITY = 2;
// Time the player task waits for the first audio after it was started (TTS engines need a moment to produce it).
static const uint32_t STARTUP_TIMEOUT_MS = 4000;
// The keep-alive task tops the FIFO up with silence below this fill level (words). 1024 words are 32 ms at 16 kHz.
static const uint32_t KEEPALIVE_MIN_FILL_WORDS = 1024;
static const uint32_t POLL_INTERVAL_MS = 10;
// A chunk whose peak is below this (about -44 dBFS) counts as silence in the diagnostics.
static const int16_t DIAG_ACTIVE_PEAK = 200;
// Safety margin that keeps the FIFO from ever becoming completely full (see get_fpga_fifo_fill_()).
static const uint32_t FIFO_MARGIN_WORDS = 64;
// Changing the PAM8019 mode pops on the headphone output, so the output is switched while the amplifier is muted:
// mute, wait, switch, wait, unmute.
static const uint32_t OUTPUT_SWITCH_MUTE_MS = 30;
static const uint32_t OUTPUT_SWITCH_SETTLE_MS = 100;

void Speaker::setup() {
  ESP_LOGCONFIG(TAG, "Setting up MatrixIO Speaker...");
  this->buffer_queue_ = xQueueCreate(BUFFER_COUNT, sizeof(DataEvent));
  this->event_queue_ = xQueueCreate(BUFFER_COUNT, sizeof(TaskEvent));
  // The player task lives forever and is woken up by start_(). Creating it for every playback failed silently when
  // the heap was nearly used up (media player pipeline), the queue then never drained.
  if (xTaskCreate(Speaker::player_task, "speaker_task", 4096, (void *) this, PLAYER_TASK_PRIORITY,
                  &this->player_task_handle_) != pdPASS) {
    ESP_LOGE(TAG, "Failed to create the player task");
    this->mark_failed();
    return;
  }
  this->set_pcm_sampling_frequency(16000);
  this->write_output_();
  this->write_mute_();
  this->write_volume_();
  // The FPGA DAC output is high-impedance while its FIFO is empty (dac_fsm.v, S_IDLE). Starting to drive it again
  // after a long pause causes a click, so keep it fed with silence whenever the player task is not running.
  xTaskCreate(Speaker::keepalive_task, "speaker_keepalive", 4096, (void *) this, 1, nullptr);
}

void Speaker::dump_config() {
  ESP_LOGCONFIG(TAG, "Matrixio Speaker:");
  ESP_LOGCONFIG(TAG, "  Sampling frequency: %uHz", (unsigned) this->read_pcm_sampling_frequency());
  if (this->output_ == kHeadPhone) {
    ESP_LOGCONFIG(TAG, "  Output: Headphone");
  } else if (this->output_ == kSpeaker) {
    ESP_LOGCONFIG(TAG, "  Output: Speaker");
  } else {
    ESP_LOGCONFIG(TAG, "  Output not set");
  }
  ESP_LOGCONFIG(TAG, "  Volume: %d%%", (int) (this->volume_ * 100.0f + 0.5f));
  ESP_LOGCONFIG(TAG, "  Idle timeout: %ums", (unsigned) this->idle_timeout_ms_.load());
}

void Speaker::set_idle_timeout(uint32_t timeout_ms) {
  this->idle_timeout_ms_ = std::min<uint32_t>(std::max<uint32_t>(timeout_ms, 100), 10000);
}

void Speaker::start() {
  if (this->state_ == speaker::STATE_STOPPED) {
    this->state_ = speaker::STATE_STARTING;
  }
}

void Speaker::start_() {
  // Only touch the rate register if the rate changes
  if (this->audio_stream_info_.get_sample_rate() != this->pcm_sampling_frequency_ &&
      !this->set_pcm_sampling_frequency(this->audio_stream_info_.get_sample_rate())) {
    ESP_LOGE(TAG, "Unsupported sampling frequency: %u", (unsigned) this->audio_stream_info_.get_sample_rate());
    this->state_ = speaker::STATE_STOPPED;
    return;
  }
  this->stop_requested_ = false;
  this->finish_requested_ = false;
  this->fifo_fill_ = 0;
  this->diag_frames_ = 0;
  this->diag_active_frames_ = 0;
  this->diag_peak_ = 0;
  this->diag_underrun_ms_ = 0;
  this->diag_partial_ = 0;
  this->diag_start_ms_ = millis();
  // Chunks queued by play() before the main loop got here (state STARTING) must survive. The queue is emptied when
  // the speaker stops.
  this->state_ = speaker::STATE_RUNNING;
  this->player_active_ = true;
  xTaskNotifyGive(this->player_task_handle_);
}

void Speaker::stop() {
  if (this->state_ == speaker::STATE_STOPPED || this->state_ == speaker::STATE_STOPPING)
    return;
  if (this->state_ == speaker::STATE_STARTING) {
    this->state_ = speaker::STATE_STOPPED;
    return;
  }
  // Never block the main loop: the task polls this flag.
  this->state_ = speaker::STATE_STOPPING;
  this->stop_requested_ = true;
}

void Speaker::finish() {
  if (this->state_ == speaker::STATE_RUNNING) {
    this->finish_requested_ = true;
  } else {
    this->stop();
  }
}

void Speaker::watch_() {
  TaskEvent event;
  while (xQueueReceive(this->event_queue_, &event, 0) == pdTRUE) {
    switch (event.type) {
      case TaskEventType::STARTED: {
        // Read back what the FPGA is configured to do, a wrong value would explain a silent or odd playback
        uint16_t volume = 0, rate = 0, mute = 0, output = 0;
        this->conf_read(8, &volume);
        this->conf_read(9, &rate);
        this->conf_read(10, &mute);
        this->conf_read(11, &output);
        ESP_LOGD(TAG, "Started MatrixIO Audio Speaker (FPGA: volume %u, rate %u, mute %u, output %u)", (unsigned) volume,
                 (unsigned) rate, (unsigned) mute, (unsigned) output);
        break;
      }
      case TaskEventType::STOPPED: {
        const float rate = static_cast<float>(this->pcm_sampling_frequency_);
        const int peak = this->diag_peak_;
        ESP_LOGD(TAG, "Played %.2f s of audio (%.2f s not silent), peak %d (%.1f dBFS), FIFO ran dry for %u ms, %u partial writes, ran %.2f s",
                 this->diag_frames_ / rate, this->diag_active_frames_ / rate, peak,
                 peak > 0 ? 20.0f * log10f(peak / 32768.0f) : -99.0f, (unsigned) this->diag_underrun_ms_,
                 (unsigned) this->diag_partial_, (millis() - this->diag_start_ms_) / 1000.0f);
        this->state_ = speaker::STATE_STOPPED;
        xQueueReset(this->buffer_queue_);
        this->fifo_fill_ = 0;
        ESP_LOGD(TAG, "Stopped MatrixIO Audio Speaker");
        break;
      }
    }
  }
}

void Speaker::loop() {
  switch (this->state_) {
    case speaker::STATE_STARTING:
      this->start_();
      break;
    case speaker::STATE_RUNNING:
    case speaker::STATE_STOPPING:
      this->watch_();
      break;
    case speaker::STATE_STOPPED:
      break;
  }
}

void Speaker::player_task(void *params) {
  Speaker *this_speaker = (Speaker *) params;

  while (true) {
    // Wait for start_()
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    // No FIFO flush here: the FIFO holds silence from keepalive_task(), which keeps the DAC output driven.
    TaskEvent event;
    event.type = TaskEventType::STARTED;
    xQueueSend(this_speaker->event_queue_, &event, portMAX_DELAY);

    const uint32_t rate = this_speaker->pcm_sampling_frequency_;
    DataEvent data_event;
    uint32_t idle_ms = 0;
    bool received_audio = false;
    bool draining = false;
    uint32_t dry_ms = 0;  // FIFO (nearly) empty although more audio is expected, for the diagnostics

    while (!this_speaker->stop_requested_) {
      uint16_t fill = this_speaker->get_fpga_fifo_fill_();
      this_speaker->fifo_fill_ = fill;
      if (received_audio && !draining && fill < 32 && uxQueueMessagesWaiting(this_speaker->buffer_queue_) == 0)
        dry_ms += POLL_INTERVAL_MS;

      if (draining) {
        // finish() was requested and all data is written: wait until the FPGA has played it
        if (fill == 0)
          break;
        delay(POLL_INTERVAL_MS);
        continue;
      }

      if (this_speaker->paused_) {
        delay(POLL_INTERVAL_MS);
        continue;
      }

      // Only take the next chunk out of the queue if the whole chunk fits into the FIFO.
      if (fill + BUFFER_SIZE / sizeof(uint16_t) + FIFO_MARGIN_WORDS > FPGA_FIFO_SIZE) {
        delay(POLL_INTERVAL_MS);
        continue;
      }

      if (xQueueReceive(this_speaker->buffer_queue_, &data_event, POLL_INTERVAL_MS / portTICK_PERIOD_MS) != pdTRUE) {
        if (this_speaker->finish_requested_) {
          draining = true;
        } else {
          idle_ms += POLL_INTERVAL_MS;
          if (idle_ms >= (received_audio ? this_speaker->idle_timeout_ms_.load() : STARTUP_TIMEOUT_MS))
            break;  // No audio for idle_timeout_ms_: end of the response
        }
        continue;
      }
      idle_ms = 0;
      received_audio = true;
      if (dry_ms > 0) {  // more audio came after the FIFO had run dry: that was an underrun, not the end
        this_speaker->diag_underrun_ms_ += dry_ms;
        dry_ms = 0;
      }

      this_speaker->wb_write(data_event.data, data_event.len);

      // Tell the audio pipeline how much was played, accounting for the audio still queued in the FPGA.
      uint32_t frames = data_event.len / (2 * sizeof(int16_t));
      uint32_t queued_us = (uint64_t) fill * 1000000ULL / (2ULL * rate);
      this_speaker->audio_output_callback_.call(frames, esp_timer_get_time() + queued_us);
    }

    if (this_speaker->stop_requested_) {
      xQueueReset(this_speaker->buffer_queue_);
      this_speaker->flush_fpga_fifo_();
    }

    this_speaker->player_active_ = false;  // hand the FIFO back to keepalive_task()
    event.type = TaskEventType::STOPPED;
    xQueueSend(this_speaker->event_queue_, &event, portMAX_DELAY);
  }
}

void Speaker::keepalive_task(void *params) {
  Speaker *this_speaker = (Speaker *) params;
  uint8_t silence[BUFFER_SIZE] = {};
  while (true) {
    if (!this_speaker->player_active_ && this_speaker->get_fpga_fifo_fill_() < KEEPALIVE_MIN_FILL_WORDS) {
      this_speaker->wb_write(silence, sizeof(silence));
    }
    delay(POLL_INTERVAL_MS);
  }
}

size_t Speaker::play(const uint8_t *data, size_t length) { return this->play(data, length, 0); }

size_t Speaker::play(const uint8_t *data, size_t length, TickType_t ticks_to_wait) {
  if (this->state_ != speaker::STATE_RUNNING && this->state_ != speaker::STATE_STARTING) {
    this->start();
  }
  if (this->state_ == speaker::STATE_STOPPED || this->state_ == speaker::STATE_STOPPING) {
    return 0;
  }

  const bool mono = this->audio_stream_info_.get_channels() == 1;
  // Input bytes that fit into one queue entry (the FPGA always expects stereo)
  const size_t chunk_in = mono ? BUFFER_SIZE / 2 : BUFFER_SIZE;
  // Only whole samples (or whole stereo frames) are consumed
  length -= length % (mono ? sizeof(int16_t) : 2 * sizeof(int16_t));

  size_t consumed = 0;
  DataEvent event;
  while (consumed < length) {
    size_t to_copy = std::min(length - consumed, chunk_in);
    int chunk_peak = 0;
    uint32_t chunk_frames;
    if (mono) {
      const int16_t *src = reinterpret_cast<const int16_t *>(data + consumed);
      int16_t *dst = reinterpret_cast<int16_t *>(event.data);
      for (size_t i = 0; i < to_copy / sizeof(int16_t); i++) {
        *dst++ = src[i];
        *dst++ = src[i];
        chunk_peak = std::max(chunk_peak, std::abs(static_cast<int>(src[i])));
      }
      event.len = to_copy * 2;
      chunk_frames = to_copy / sizeof(int16_t);
    } else {
      memcpy(event.data, data + consumed, to_copy);
      event.len = to_copy;
      const int16_t *src = reinterpret_cast<const int16_t *>(event.data);
      for (size_t i = 0; i < to_copy / sizeof(int16_t); i++)
        chunk_peak = std::max(chunk_peak, std::abs(static_cast<int>(src[i])));
      chunk_frames = to_copy / (2 * sizeof(int16_t));
    }
    if (xQueueSend(this->buffer_queue_, &event, ticks_to_wait) != pdTRUE) {
      this->diag_partial_++;
      break;  // Queue full, the caller retries with the remaining data
    }
    consumed += to_copy;
    this->diag_frames_ += chunk_frames;
    if (chunk_peak > DIAG_ACTIVE_PEAK)
      this->diag_active_frames_ += chunk_frames;
    uint16_t seen = this->diag_peak_;
    while (chunk_peak > seen && !this->diag_peak_.compare_exchange_weak(seen, static_cast<uint16_t>(std::min(chunk_peak, 32768 - 1)))) {
    }
  }
  return consumed;
}

bool Speaker::has_buffered_data() const {
  if (this->buffer_queue_ == nullptr)
    return false;
  return uxQueueMessagesWaiting(this->buffer_queue_) > 0 ||
         (this->state_ == speaker::STATE_RUNNING && this->fifo_fill_ > 0);
}

uint16_t Speaker::get_fpga_fifo_fill_() {
  uint16_t write_pointer;
  uint16_t read_pointer;
  this->reg_read(0x802, &read_pointer);
  this->reg_read(0x803, &write_pointer);

  if (write_pointer >= read_pointer)
    return write_pointer - read_pointer;
  return FPGA_FIFO_SIZE - read_pointer + write_pointer;
}

void Speaker::flush_fpga_fifo_() {
  this->conf_write(12, 0x0001);
  this->conf_write(12, 0x0000);
}

void Speaker::set_output(OutputSelector output_selector) {
  // Restored select/number values arrive before setup(), which writes all settings itself.
  if (!this->is_ready()) {
    this->output_ = output_selector;
    return;
  }
  if (output_selector == this->output_)
    return;
  this->output_ = output_selector;
  this->cancel_timeout("output_unmute");
  this->switching_output_ = true;
  this->write_mute_();
  this->set_timeout("output_switch", OUTPUT_SWITCH_MUTE_MS, [this]() {
    this->write_output_();
    this->set_timeout("output_unmute", OUTPUT_SWITCH_SETTLE_MS, [this]() {
      this->switching_output_ = false;
      this->write_mute_();
    });
  });
}

void Speaker::write_output_() { this->conf_write(11, this->output_); }

void Speaker::write_mute_() {
  // The lowest volume of the amplifier (DC volume pin) is still audible, so 0 % uses its mute pin.
  bool mute = this->mute_state_ || this->volume_ <= 0.0f || this->switching_output_;
  this->conf_write(10, mute ? kMute : kUnMute);
}

void Speaker::set_mute_state(bool mute_state) {
  this->mute_state_ = mute_state;
  if (this->is_ready())
    this->write_mute_();
}

bool Speaker::set_pcm_sampling_frequency(uint32_t sampling_frequency) {
  for (int i = 0; PCM_SAMPLING_FREQUENCIES[i][0] != 0; i++) {
    if (sampling_frequency == PCM_SAMPLING_FREQUENCIES[i][0]) {
      this->pcm_sampling_frequency_ = sampling_frequency;
      this->conf_write(9, PCM_SAMPLING_FREQUENCIES[i][1]);
      return true;
    }
  }
  return false;
}

uint32_t Speaker::read_pcm_sampling_frequency() {
  uint16_t pcm_constant;
  this->conf_read(9, &pcm_constant);
  for (int i = 0; PCM_SAMPLING_FREQUENCIES[i][0] != 0; i++) {
    if (pcm_constant == PCM_SAMPLING_FREQUENCIES[i][1]) {
      return PCM_SAMPLING_FREQUENCIES[i][0];
    }
  }
  ESP_LOGE(TAG, "Reading sampling frequency failed. Got : %u", pcm_constant);
  return 0;
}

void Speaker::set_volume(float volume) {
  this->volume_ = std::min(1.0f, std::max(0.0f, volume));
  if (this->is_ready()) {
    this->write_volume_();
    this->write_mute_();
  }
}

void Speaker::write_volume_() {
  // hardware register: 0 = loudest, MAX_VOLUME_VALUE = quietest
  uint16_t volume_constant = (uint16_t) ((1.0f - this->volume_) * MAX_VOLUME_VALUE + 0.5f);
  this->conf_write(8, volume_constant);
}

}  // namespace matrixio
}  // namespace esphome
