#include "direction_estimator.h"

#include <algorithm>
#include <cmath>

namespace esphome {
namespace matrixio {

static const float SPEED_OF_SOUND = 343.0f;
static const float PI_F = 3.14159265358979f;

void DirectionEstimator::setup(const float positions[][2], float sample_rate) {
  const int n = FRAME;
  window_.resize(n);
  for (int i = 0; i < n; i++) window_[i] = 0.5f - 0.5f * std::cos(2.0f * PI_F * i / n);

  tw_re_.resize(n / 2);
  tw_im_.resize(n / 2);
  for (int k = 0; k < n / 2; k++) {
    tw_re_[k] = std::cos(2.0f * PI_F * k / n);
    tw_im_[k] = -std::sin(2.0f * PI_F * k / n);
  }
  bitrev_.resize(n);
  int bits = 0;
  while ((1 << bits) < n) bits++;
  for (int i = 0; i < n; i++) {
    int r = 0;
    for (int b = 0; b < bits; b++)
      if (i & (1 << b)) r |= 1 << (bits - 1 - b);
    bitrev_[i] = r;
  }
  re_.resize(n);
  im_.resize(n);

  spec_re_.assign(CHANNELS * BINS, 0.0f);
  spec_im_.assign(CHANNELS * BINS, 0.0f);
  noise_.assign(BINS, 0.0f);
  weight_.assign(BINS, 0.0f);
  srp_.assign(ANGLES, 0.0f);

  // A source in direction u reaches microphone i earlier by tau_i = r_i . u / c, so its bin f has the phase
  // exp(+j 2 pi f tau_i). Compensate with exp(-j 2 pi f tau_i); the phasor of consecutive bins is a
  // constant rotation, so no sin/cos is needed per bin.
  const float bin_hz = sample_rate / n;
  start_re_.resize(ANGLES * CHANNELS);
  start_im_.resize(ANGLES * CHANNELS);
  step_re_.resize(ANGLES * CHANNELS);
  step_im_.resize(ANGLES * CHANNELS);
  for (int a = 0; a < ANGLES; a++) {
    const float theta = 2.0f * PI_F * a / ANGLES;
    const float ux = std::cos(theta), uy = std::sin(theta);
    for (int c = 0; c < CHANNELS; c++) {
      const float tau = (positions[c][0] * ux + positions[c][1] * uy) / SPEED_OF_SOUND;
      const float phase0 = 2.0f * PI_F * BIN_LO * bin_hz * tau;
      const float dphase = 2.0f * PI_F * bin_hz * tau;
      start_re_[a * CHANNELS + c] = std::cos(phase0);
      start_im_[a * CHANNELS + c] = -std::sin(phase0);
      step_re_[a * CHANNELS + c] = std::cos(dphase);
      step_im_[a * CHANNELS + c] = -std::sin(dphase);
    }
  }
}

void DirectionEstimator::fft_() {
  const int n = FRAME;
  for (int i = 0; i < n; i++) {
    const int j = bitrev_[i];
    if (j > i) {
      std::swap(re_[i], re_[j]);
      std::swap(im_[i], im_[j]);
    }
  }
  for (int len = 2; len <= n; len <<= 1) {
    const int half = len / 2, stride = n / len;
    for (int i = 0; i < n; i += len) {
      for (int k = 0; k < half; k++) {
        const float wr = tw_re_[k * stride], wi = tw_im_[k * stride];
        const int p = i + k, q = i + k + half;
        const float tr = wr * re_[q] - wi * im_[q];
        const float ti = wr * im_[q] + wi * re_[q];
        re_[q] = re_[p] - tr;
        im_[q] = im_[p] - ti;
        re_[p] += tr;
        im_[p] += ti;
      }
    }
  }
}

DirectionResult DirectionEstimator::process(const int16_t *samples) {
  const int n = FRAME;
  float energy[BINS] = {};

  // Two real channels per complex FFT: z = a + j b, A[k] = (Z[k] + conj Z[n-k]) / 2, B[k] = (Z[k] - conj Z[n-k]) / 2j
  for (int pair = 0; pair < CHANNELS / 2; pair++) {
    const int16_t *a = samples + (2 * pair) * n;
    const int16_t *b = samples + (2 * pair + 1) * n;
    for (int i = 0; i < n; i++) {
      re_[i] = a[i] * window_[i];
      im_[i] = b[i] * window_[i];
    }
    this->fft_();
    for (int k = BIN_LO; k <= BIN_HI; k++) {
      const float zr = re_[k], zi = im_[k], nr = re_[n - k], ni = im_[n - k];
      const float ar = 0.5f * (zr + nr), ai = 0.5f * (zi - ni);
      const float br = 0.5f * (zi + ni), bi = -0.5f * (zr - nr);
      const int bin = k - BIN_LO;
      const float ma = std::sqrt(ar * ar + ai * ai) + 1e-9f;
      const float mb = std::sqrt(br * br + bi * bi) + 1e-9f;
      energy[bin] += ma * ma + mb * mb;
      spec_re_[(2 * pair) * BINS + bin] = ar / ma;  // PHAT: keep the phase only
      spec_im_[(2 * pair) * BINS + bin] = ai / ma;
      spec_re_[(2 * pair + 1) * BINS + bin] = br / mb;
      spec_im_[(2 * pair + 1) * BINS + bin] = bi / mb;
    }
  }

  // Noise floor per bin: follows minima immediately and creeps up slowly (about 6 % per second)
  float weight_sum = 0.0f, band_energy = 0.0f;
  for (int b = 0; b < BINS; b++) {
    if (!noise_ready_ || energy[b] < noise_[b])
      noise_[b] = energy[b];
    else
      noise_[b] *= 1.002f;
    const float snr = energy[b] / (noise_[b] + 1e-6f);
    weight_[b] = std::min(1.0f, std::max(0.0f, (snr - 3.0f) / 5.0f));
    weight_sum += weight_[b];
    band_energy += energy[b];
  }
  if (!noise_ready_ || band_energy < band_floor_)
    band_floor_ = band_energy;
  else
    band_floor_ *= 1.002f;
  noise_ready_ = true;
  const bool loud_enough = band_energy > min_band_over_floor * band_floor_;

  DirectionResult result;
  result.level_db = 10.0f * std::log10(band_energy / BINS / CHANNELS + 1.0f);

  if (loud_enough && weight_sum >= 10.0f) {
    for (int a = 0; a < ANGLES; a++) {
      float cur_re[CHANNELS], cur_im[CHANNELS];
      for (int c = 0; c < CHANNELS; c++) {
        cur_re[c] = start_re_[a * CHANNELS + c];
        cur_im[c] = start_im_[a * CHANNELS + c];
      }
      float power = 0.0f;
      for (int b = 0; b < BINS; b++) {
        float sum_re = 0.0f, sum_im = 0.0f;
        for (int c = 0; c < CHANNELS; c++) {
          const float yr = spec_re_[c * BINS + b], yi = spec_im_[c * BINS + b];
          sum_re += yr * cur_re[c] - yi * cur_im[c];
          sum_im += yr * cur_im[c] + yi * cur_re[c];
          const float sr = step_re_[a * CHANNELS + c], si = step_im_[a * CHANNELS + c];
          const float nr = cur_re[c] * sr - cur_im[c] * si;
          cur_im[c] = cur_re[c] * si + cur_im[c] * sr;
          cur_re[c] = nr;
        }
        power += weight_[b] * (sum_re * sum_re + sum_im * sum_im);
      }
      // 1 = perfectly coherent; the +4 shrinks the value when only a few bins take part (chance coherence)
      const float p = power / (CHANNELS * CHANNELS * (weight_sum + 4.0f));
      srp_[a] = 0.6f * srp_[a] + 0.4f * p;
    }
  } else {
    for (float &p : srp_) p *= 0.9f;
  }

  int best = 0;
  for (int a = 1; a < ANGLES; a++)
    if (srp_[a] > srp_[best]) best = a;
  // parabolic interpolation around the peak
  const float l = srp_[(best + ANGLES - 1) % ANGLES], m = srp_[best], r = srp_[(best + 1) % ANGLES];
  const float denom = l - 2.0f * m + r;
  const float shift = std::fabs(denom) > 1e-9f ? 0.5f * (l - r) / denom : 0.0f;
  result.angle_deg = std::fmod((best + std::max(-0.5f, std::min(0.5f, shift))) * 360.0f / ANGLES + 360.0f, 360.0f);
  result.confidence = m;
  result.valid = m >= valid_threshold;
  return result;
}

}  // namespace matrixio
}  // namespace esphome
