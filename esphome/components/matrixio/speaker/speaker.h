#pragma once

#include <atomic>
#include <vector>

#include "esphome/core/component.h"
#include "esphome/core/log.h"
#include "esphome/components/speaker/speaker.h"

#include "../matrixio.h"
#include "../wishbone.h"

namespace esphome {
namespace matrixio {

enum class TaskEventType : uint8_t {
  STARTED = 0,
  STOPPED,
};

struct TaskEvent {
  TaskEventType type;
};

struct DataEvent {
  size_t len;
  uint8_t data[BUFFER_SIZE];  // always 16 bit stereo, as expected by the FPGA
};

static const char *const TAG = "matrixio_speaker";

class Speaker : public speaker::Speaker, public matrixio::WishboneDevice, public Component {
 public:
  Speaker() : matrixio::WishboneDevice(AUDIO_OUT_BASE_ADDRESS), output_(OutputSelector::kHeadPhone) {}

  void setup() override;
  void dump_config() override;
  void loop() override;

  void start() override;
  void stop() override;
  void finish() override;

  size_t play(const uint8_t *data, size_t length) override;
  size_t play(const uint8_t *data, size_t length, TickType_t ticks_to_wait) override;
  bool has_buffered_data() const override;

  void set_pause_state(bool pause_state) override { this->paused_ = pause_state; }
  bool get_pause_state() const override { return this->paused_; }

  void set_volume(float volume) override;
  void set_mute_state(bool mute_state) override;

  void set_initial_volume(uint8_t volume_percentage) { this->volume_ = volume_percentage / 100.0f; }
  /// Selects headphone or speakers; can be changed at runtime (written to the FPGA right away).
  void set_output(OutputSelector output_selector);
  OutputSelector get_output() const { return this->output_; }
  /// Playback stops after this many milliseconds without new audio (100..10000).
  void set_idle_timeout(uint32_t timeout_ms);
  uint32_t get_idle_timeout() const { return this->idle_timeout_ms_; }

  bool set_pcm_sampling_frequency(uint32_t sampling_frequency);
  uint32_t read_pcm_sampling_frequency();

 protected:
  void start_();
  void watch_();

  static void player_task(void *params);
  /// Keeps the FPGA FIFO (and so the DAC output) alive with silence while the player task is not running.
  static void keepalive_task(void *params);

  /// Number of 16 bit words currently queued in the FPGA FIFO. The player task never fills the FIFO completely, so
  /// equal read and write pointers always mean "empty".
  uint16_t get_fpga_fifo_fill_();
  void flush_fpga_fifo_();
  void write_output_();
  void write_volume_();
  void write_mute_();

  TaskHandle_t player_task_handle_{nullptr};
  QueueHandle_t buffer_queue_{nullptr};
  QueueHandle_t event_queue_{nullptr};

  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> finish_requested_{false};
  std::atomic<bool> paused_{false};
  std::atomic<bool> player_active_{false};
  std::atomic<uint16_t> fifo_fill_{0};

  // Diagnostics of one playback, logged when the speaker stops (see watch_())
  std::atomic<uint32_t> diag_frames_{0};         // frames accepted by play()
  std::atomic<uint32_t> diag_active_frames_{0};  // frames in chunks that are not (nearly) silent
  std::atomic<uint16_t> diag_peak_{0};           // highest absolute sample
  std::atomic<uint32_t> diag_underrun_ms_{0};    // time the FIFO ran (nearly) dry while audio was expected
  std::atomic<uint32_t> diag_partial_{0};        // play() calls that could not queue everything
  uint32_t diag_start_ms_{0};
  // written from the main loop (e.g. by Home Assistant), read by the player task
  std::atomic<uint32_t> idle_timeout_ms_{500};

  uint32_t pcm_sampling_frequency_{16000};
  OutputSelector output_;
};

}  // namespace matrixio
}  // namespace esphome
