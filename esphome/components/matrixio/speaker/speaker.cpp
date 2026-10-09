#include "speaker.h"

#include <algorithm>
#include <cstring>

#include <esp_timer.h>
#include <freertos/FreeRTOS.h>

namespace esphome {
namespace matrixio {

static const size_t BUFFER_COUNT = 32;
// Must be above the priority of the ESPHome main loop task (1), otherwise the FIFO runs dry.
static const UBaseType_t PLAYER_TASK_PRIORITY = 2;
// The player task ends after this much time without new audio data.
static const uint32_t IDLE_TIMEOUT_MS = 4000;
static const uint32_t POLL_INTERVAL_MS = 10;
// Safety margin that keeps the FIFO from ever becoming completely full (see get_fpga_fifo_fill_()).
static const uint32_t FIFO_MARGIN_WORDS = 64;

void Speaker::setup() {
  ESP_LOGCONFIG(TAG, "Setting up MatrixIO Speaker...");
  this->buffer_queue_ = xQueueCreate(BUFFER_COUNT, sizeof(DataEvent));
  this->event_queue_ = xQueueCreate(BUFFER_COUNT, sizeof(TaskEvent));
  this->set_pcm_sampling_frequency(16000);
  this->write_output_();
  this->write_mute_();
  this->write_volume_();
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
}

void Speaker::start() {
  if (this->state_ == speaker::STATE_STOPPED) {
    this->state_ = speaker::STATE_STARTING;
  }
}

void Speaker::start_() {
  if (!this->set_pcm_sampling_frequency(this->audio_stream_info_.get_sample_rate())) {
    ESP_LOGE(TAG, "Unsupported sampling frequency: %u", (unsigned) this->audio_stream_info_.get_sample_rate());
    this->state_ = speaker::STATE_STOPPED;
    return;
  }
  this->stop_requested_ = false;
  this->finish_requested_ = false;
  this->fifo_fill_ = 0;
  xQueueReset(this->buffer_queue_);
  this->state_ = speaker::STATE_RUNNING;
  xTaskCreate(Speaker::player_task, "speaker_task", 8192, (void *) this, PLAYER_TASK_PRIORITY,
              &this->player_task_handle_);
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
      case TaskEventType::STARTED:
        ESP_LOGD(TAG, "Started MatrixIO Audio Speaker");
        break;
      case TaskEventType::STOPPED:
        this->state_ = speaker::STATE_STOPPED;
        if (this->player_task_handle_ != nullptr) {
          vTaskDelete(this->player_task_handle_);
          this->player_task_handle_ = nullptr;
        }
        xQueueReset(this->buffer_queue_);
        this->fifo_fill_ = 0;
        ESP_LOGD(TAG, "Stopped MatrixIO Audio Speaker");
        break;
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

  this_speaker->flush_fpga_fifo_();
  delay(POLL_INTERVAL_MS);

  TaskEvent event;
  event.type = TaskEventType::STARTED;
  xQueueSend(this_speaker->event_queue_, &event, portMAX_DELAY);

  const uint32_t rate = this_speaker->pcm_sampling_frequency_;
  DataEvent data_event;
  uint32_t idle_ms = 0;
  bool draining = false;

  while (!this_speaker->stop_requested_) {
    uint16_t fill = this_speaker->get_fpga_fifo_fill_();
    this_speaker->fifo_fill_ = fill;

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
        if (idle_ms >= IDLE_TIMEOUT_MS)
          break;  // End of audio from main thread
      }
      continue;
    }
    idle_ms = 0;

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

  event.type = TaskEventType::STOPPED;
  xQueueSend(this_speaker->event_queue_, &event, portMAX_DELAY);

  // The main loop deletes this task
  while (true) {
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
    if (mono) {
      const int16_t *src = reinterpret_cast<const int16_t *>(data + consumed);
      int16_t *dst = reinterpret_cast<int16_t *>(event.data);
      for (size_t i = 0; i < to_copy / sizeof(int16_t); i++) {
        *dst++ = src[i];
        *dst++ = src[i];
      }
      event.len = to_copy * 2;
    } else {
      memcpy(event.data, data + consumed, to_copy);
      event.len = to_copy;
    }
    if (xQueueSend(this->buffer_queue_, &event, ticks_to_wait) != pdTRUE) {
      break;  // Queue full, the caller retries with the remaining data
    }
    consumed += to_copy;
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

void Speaker::write_output_() { this->conf_write(11, this->output_); }

void Speaker::write_mute_() { this->conf_write(10, this->mute_state_ ? kMute : kUnMute); }

void Speaker::set_mute_state(bool mute_state) {
  this->mute_state_ = mute_state;
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
  this->write_volume_();
}

void Speaker::write_volume_() {
  // hardware register: 0 = loudest, MAX_VOLUME_VALUE = quietest
  uint16_t volume_constant = (uint16_t) ((1.0f - this->volume_) * MAX_VOLUME_VALUE + 0.5f);
  this->conf_write(8, volume_constant);
}

}  // namespace matrixio
}  // namespace esphome
