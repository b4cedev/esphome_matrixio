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
  void set_output(OutputSelector output_selector) { this->output_ = output_selector; }

  bool set_pcm_sampling_frequency(uint32_t sampling_frequency);
  uint32_t read_pcm_sampling_frequency();

 protected:
  void start_();
  void watch_();

  static void player_task(void *params);

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
  std::atomic<uint16_t> fifo_fill_{0};

  uint32_t pcm_sampling_frequency_{16000};
  OutputSelector output_;
};

}  // namespace matrixio
}  // namespace esphome
