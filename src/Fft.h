#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace granular {
struct Cpx { float re = 0, im = 0; };
inline Cpx operator+(Cpx a, Cpx b) noexcept { return {a.re+b.re, a.im+b.im}; }
inline Cpx operator-(Cpx a, Cpx b) noexcept { return {a.re-b.re, a.im-b.im}; }
inline Cpx operator*(Cpx a, Cpx b) noexcept { return {a.re*b.re-a.im*b.im, a.re*b.im+a.im*b.re}; }
inline Cpx operator*(Cpx a, float s) noexcept { return {a.re*s, a.im*s}; }
inline Cpx conj(Cpx a) noexcept { return {a.re, -a.im}; }

// In-place iterative radix-2 FFT. Sizes are powers of two; allocation happens in prepare().
class Fft {
public:
    void prepare(size_t n) {
        n_ = n;
        rev_.assign(n, 0);
        size_t bits = 0;
        while ((size_t(1) << bits) < n) ++bits;
        for (size_t i = 0; i < n; ++i) {
            size_t r = 0;
            for (size_t b = 0; b < bits; ++b) if (i & (size_t(1) << b)) r |= size_t(1) << (bits-1-b);
            rev_[i] = static_cast<uint32_t>(r);
        }
        twiddle_.resize(n/2);
        for (size_t i = 0; i < n/2; ++i) {
            const double a = -2.0 * 3.14159265358979323846 * static_cast<double>(i) / static_cast<double>(n);
            twiddle_[i] = {static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a))};
        }
    }
    size_t size() const noexcept { return n_; }
    void forward(Cpx* x) const noexcept {
        for (size_t i = 0; i < n_; ++i) {
            const size_t j = rev_[i];
            if (j > i) { const Cpx t = x[i]; x[i] = x[j]; x[j] = t; }
        }
        for (size_t len = 2; len <= n_; len <<= 1) {
            const size_t half = len >> 1, step = n_ / len;
            for (size_t start = 0; start < n_; start += len) {
                for (size_t k = 0; k < half; ++k) {
                    const Cpx w = twiddle_[k*step];
                    const Cpx a = x[start+k], b = x[start+k+half] * w;
                    x[start+k] = a + b; x[start+k+half] = a - b;
                }
            }
        }
    }
    // Unnormalized inverse (divide by n yourself): swapping re/im conjugates the transform.
    void inverse(Cpx* x) const noexcept {
        for (size_t i = 0; i < n_; ++i) { const float t = x[i].re; x[i].re = x[i].im; x[i].im = t; }
        forward(x);
        for (size_t i = 0; i < n_; ++i) { const float t = x[i].re; x[i].re = x[i].im; x[i].im = t; }
    }
private:
    size_t n_ = 0;
    std::vector<uint32_t> rev_;
    std::vector<Cpx> twiddle_;
};
} // namespace granular
