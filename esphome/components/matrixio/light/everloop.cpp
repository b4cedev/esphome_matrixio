#include "everloop.h"

#include <cstring>

namespace esphome {
namespace matrixio {

static const char *const TAG = "matrixio_everloop";
  Everloop::Everloop() : matrixio::WishboneDevice(EVERLOOP_BASE_ADDRESS), num_leds_(NUMBER_OF_LEDS){
}

void Everloop::setup() {
  ExternalRAMAllocator<uint8_t> allocator(ExternalRAMAllocator<uint8_t>::ALLOW_FAILURE);
  this->buffer_size_ = this->size() * 4;
  this->buf_ = allocator.allocate(this->buffer_size_);
  if (this->buf_ == nullptr) {
      esph_log_e(TAG, "Failed to allocate buffer of size %u", this->buffer_size_);
      this->mark_failed();
      return;
  }
  this->effect_data_ = allocator.allocate(this->size());
  if (this->effect_data_ == nullptr) {
    esph_log_e(TAG, "Failed to allocate effect data of size %u", this->num_leds_);
    this->mark_failed();
    return;
  }
  this->overlay_ = allocator.allocate(this->buffer_size_);
  this->out_ = allocator.allocate(this->buffer_size_);
  if (this->overlay_ == nullptr || this->out_ == nullptr) {
      esph_log_e(TAG, "Failed to allocate overlay buffers");
      this->mark_failed();
      return;
  }
  memset(this->buf_, 0, this->buffer_size_);
  memset(this->overlay_, 0, this->buffer_size_);
}

// Write the light state with the overlay added on top (saturating). Without an overlay the state is
// written unchanged.
void Everloop::write_composed_() {
  if (this->is_failed())
    return;
  bool overlay_used = false;
  for (size_t i = 0; i < this->buffer_size_; i++) {
    if (this->overlay_[i] != 0) {
      overlay_used = true;
      break;
    }
  }
  if (!overlay_used) {
    this->wb_write(this->buf_, this->buffer_size_);
    return;
  }
  for (size_t i = 0; i < this->buffer_size_; i++) {
    const uint16_t sum = this->buf_[i] + this->overlay_[i];
    this->out_[i] = sum > 255 ? 255 : sum;
  }
  this->wb_write(this->out_, this->buffer_size_);
}

float Everloop::get_setup_priority() const { return setup_priority::HARDWARE; }

void Everloop::dump_config() {
  esph_log_config(TAG, "Matrixio Everloop:");
  esph_log_config(TAG, "  LEDs: %d", this->num_leds_);
}

light::ESPColorView Everloop::get_view_internal(int32_t index) const {
        size_t pos = index * 4;
        return {this->buf_ + pos + 0,
                this->buf_ + pos + 1,
                this->buf_ + pos + 2,
                this->buf_ + pos + 3,
                this->effect_data_ + index,
                &this->correction_
              };
    }

}
}
