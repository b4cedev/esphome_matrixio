#pragma once

#include <cstdint>
#include <vector>

namespace esphome {
namespace matrixio {

struct DirectionResult {
  bool valid{false};       // enough coherent sound for a trustworthy direction
  float angle_deg{0.0f};   // towards the source, counter-clockwise from +x of the microphone coordinates
  float confidence{0.0f};  // 0..1, about 0.1..0.2 for noise, towards 1 for a single coherent source
  float level_db{0.0f};    // band level, for logging
  float over_floor_db{0.0f};  // band level above the tracked background level
};

/**
 * Broadband direction of arrival estimate for a planar microphone array (SRP-PHAT).
 *
 * Every frame is windowed and transformed, the phase of each microphone is normalised (PHAT) and the
 * steered response power is evaluated for a ring of directions in the band where the array does not
 * alias (about 300 Hz to 2 kHz for the 77 mm MATRIX Voice ring). Only bins that clearly stand out of a
 * slowly tracked noise floor take part, so noise does not produce a direction.
 */
class DirectionEstimator {
 public:
  static const int FRAME = 512;
  static const int CHANNELS = 8;
  static const int ANGLES = 72;  // 5 degree steps
  static const int BIN_LO = 10;  // 312 Hz at 16 kHz
  static const int BIN_HI = 64;  // 2 kHz
  static const int BINS = BIN_HI - BIN_LO + 1;

  // positions: metres (x, y) per channel; samples are blocks of FRAME samples per channel
  void setup(const float positions[][2], float sample_rate);
  DirectionResult process(const int16_t *samples);

  float valid_threshold{0.62f};
  float min_band_over_floor{8.0f};  // band energy must exceed the tracked floor by this factor (about 9 dB)

 private:
  void fft_();

  std::vector<float> window_, tw_re_, tw_im_;
  std::vector<uint16_t> bitrev_;
  std::vector<float> re_, im_;  // FFT work buffer

  std::vector<float> spec_re_, spec_im_;  // [channel][bin], phase normalised
  std::vector<float> start_re_, start_im_, step_re_, step_im_;  // steering [angle][channel]
  std::vector<float> noise_;                                    // noise floor per bin
  std::vector<float> weight_;                                   // bin weight of this frame
  std::vector<float> srp_;                                      // smoothed power per angle
  float band_floor_{0.0f};
  bool noise_ready_{false};
};

}  // namespace matrixio
}  // namespace esphome
