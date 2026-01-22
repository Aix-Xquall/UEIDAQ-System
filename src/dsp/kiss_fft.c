#include "dsp/kiss_fft.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

struct kiss_fft_state {
  int nfft;
  int inverse;
};

static int IsPowerOfTwo(int n) {
  return (n > 0) && ((n & (n - 1)) == 0);
}

static void BitReverse(kiss_fft_cpx *a, int n) {
  int j = 0;
  for (int i = 1; i < n; ++i) {
    int bit = n >> 1;
    while (j & bit) {
      j ^= bit;
      bit >>= 1;
    }
    j ^= bit;
    if (i < j) {
      kiss_fft_cpx tmp = a[i];
      a[i] = a[j];
      a[j] = tmp;
    }
  }
}

static void FftRadix2(kiss_fft_cpx *a, int n, int inverse) {
  BitReverse(a, n);

  for (int len = 2; len <= n; len <<= 1) {
    const double angle = (inverse ? 2.0 : -2.0) * M_PI / (double)len;
    const kiss_fft_cpx wlen = {cos(angle), sin(angle)};
    const int half = len >> 1;

    for (int i = 0; i < n; i += len) {
      kiss_fft_cpx w = {1.0, 0.0};
      for (int j = 0; j < half; ++j) {
        const int idx1 = i + j;
        const int idx2 = idx1 + half;
        const kiss_fft_cpx u = a[idx1];
        const kiss_fft_cpx v = {
            a[idx2].r * w.r - a[idx2].i * w.i,
            a[idx2].r * w.i + a[idx2].i * w.r};

        a[idx1].r = u.r + v.r;
        a[idx1].i = u.i + v.i;
        a[idx2].r = u.r - v.r;
        a[idx2].i = u.i - v.i;

        const double wr = w.r * wlen.r - w.i * wlen.i;
        const double wi = w.r * wlen.i + w.i * wlen.r;
        w.r = wr;
        w.i = wi;
      }
    }
  }

  if (inverse) {
    const double inv_n = 1.0 / (double)n;
    for (int i = 0; i < n; ++i) {
      a[i].r *= inv_n;
      a[i].i *= inv_n;
    }
  }
}

static void FftDft(const kiss_fft_cpx *in, kiss_fft_cpx *out, int n, int inverse) {
  const double sign = inverse ? 1.0 : -1.0;
  const double factor = 2.0 * M_PI / (double)n;
  for (int k = 0; k < n; ++k) {
    double sum_r = 0.0;
    double sum_i = 0.0;
    for (int t = 0; t < n; ++t) {
      const double angle = sign * factor * ((double)k * (double)t);
      const double c = cos(angle);
      const double s = sin(angle);
      sum_r += in[t].r * c - in[t].i * s;
      sum_i += in[t].r * s + in[t].i * c;
    }
    out[k].r = sum_r;
    out[k].i = sum_i;
  }

  if (inverse) {
    const double inv_n = 1.0 / (double)n;
    for (int i = 0; i < n; ++i) {
      out[i].r *= inv_n;
      out[i].i *= inv_n;
    }
  }
}

kiss_fft_cfg kiss_fft_alloc(int nfft, int inverse_fft, void *mem, size_t *lenmem) {
  if (lenmem) {
    *lenmem = sizeof(struct kiss_fft_state);
  }

  if (mem == NULL) {
    struct kiss_fft_state *st =
        (struct kiss_fft_state *)malloc(sizeof(struct kiss_fft_state));
    if (!st)
      return NULL;
    st->nfft = nfft;
    st->inverse = inverse_fft ? 1 : 0;
    return st;
  }

  if (lenmem && *lenmem < sizeof(struct kiss_fft_state))
    return NULL;

  struct kiss_fft_state *st = (struct kiss_fft_state *)mem;
  st->nfft = nfft;
  st->inverse = inverse_fft ? 1 : 0;
  return st;
}

void kiss_fft_free(void *cfg) {
  if (cfg)
    free(cfg);
}

void kiss_fft(kiss_fft_cfg cfg, const kiss_fft_cpx *fin, kiss_fft_cpx *fout) {
  if (!cfg || !fin || !fout)
    return;

  const int n = cfg->nfft;
  if (n <= 0)
    return;

  const kiss_fft_cpx *input = fin;
  kiss_fft_cpx *tmp_in = NULL;

  if (fin == fout) {
    tmp_in = (kiss_fft_cpx *)malloc(sizeof(kiss_fft_cpx) * (size_t)n);
    if (!tmp_in)
      return;
    memcpy(tmp_in, fin, sizeof(kiss_fft_cpx) * (size_t)n);
    input = tmp_in;
  }

  if (IsPowerOfTwo(n)) {
    if (input != fout)
      memcpy(fout, input, sizeof(kiss_fft_cpx) * (size_t)n);
    FftRadix2(fout, n, cfg->inverse);
  } else {
    FftDft(input, fout, n, cfg->inverse);
  }

  if (tmp_in)
    free(tmp_in);
}
