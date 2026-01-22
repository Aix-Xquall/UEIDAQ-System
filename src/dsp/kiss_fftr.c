#include "dsp/kiss_fftr.h"

#include <stdlib.h>
#include <string.h>

struct kiss_fftr_state {
  int nfft;
  int inverse;
  kiss_fft_cfg substate;
};

kiss_fftr_cfg kiss_fftr_alloc(int nfft, int inverse_fft, void *mem, size_t *lenmem) {
  if (lenmem) {
    *lenmem = sizeof(struct kiss_fftr_state);
  }

  struct kiss_fftr_state *st = NULL;
  if (mem == NULL) {
    st = (struct kiss_fftr_state *)malloc(sizeof(struct kiss_fftr_state));
    if (!st)
      return NULL;
  } else {
    if (lenmem && *lenmem < sizeof(struct kiss_fftr_state))
      return NULL;
    st = (struct kiss_fftr_state *)mem;
  }

  st->nfft = nfft;
  st->inverse = inverse_fft ? 1 : 0;
  st->substate = kiss_fft_alloc(nfft, inverse_fft, NULL, NULL);
  if (!st->substate) {
    if (mem == NULL)
      free(st);
    return NULL;
  }
  return st;
}

void kiss_fftr_free(void *cfg) {
  if (!cfg)
    return;
  struct kiss_fftr_state *st = (struct kiss_fftr_state *)cfg;
  if (st->substate)
    kiss_fft_free(st->substate);
  free(st);
}

void kiss_fftr(kiss_fftr_cfg cfg, const kiss_fft_scalar *timedata, kiss_fft_cpx *freqdata) {
  if (!cfg || !timedata || !freqdata)
    return;

  const int n = cfg->nfft;
  if (n <= 0)
    return;

  kiss_fft_cpx *tmp = (kiss_fft_cpx *)malloc(sizeof(kiss_fft_cpx) * (size_t)n);
  if (!tmp)
    return;

  for (int i = 0; i < n; ++i) {
    tmp[i].r = timedata[i];
    tmp[i].i = 0.0;
  }

  kiss_fft(cfg->substate, tmp, tmp);

  const int bins = n / 2 + 1;
  for (int k = 0; k < bins; ++k) {
    freqdata[k] = tmp[k];
  }

  free(tmp);
}

void kiss_fftri(kiss_fftr_cfg cfg, const kiss_fft_cpx *freqdata, kiss_fft_scalar *timedata) {
  if (!cfg || !freqdata || !timedata)
    return;

  const int n = cfg->nfft;
  if (n <= 0)
    return;

  kiss_fft_cpx *tmp = (kiss_fft_cpx *)malloc(sizeof(kiss_fft_cpx) * (size_t)n);
  if (!tmp)
    return;

  const int bins = n / 2 + 1;
  tmp[0] = freqdata[0];
  if (n % 2 == 0) {
    for (int k = 1; k < bins - 1; ++k) {
      tmp[k] = freqdata[k];
      tmp[n - k].r = freqdata[k].r;
      tmp[n - k].i = -freqdata[k].i;
    }
    tmp[n / 2] = freqdata[bins - 1];
  } else if (bins > 1) {
    for (int k = 1; k < bins; ++k) {
      tmp[k] = freqdata[k];
      tmp[n - k].r = freqdata[k].r;
      tmp[n - k].i = -freqdata[k].i;
    }
  }

  kiss_fft(cfg->substate, tmp, tmp);

  for (int i = 0; i < n; ++i) {
    timedata[i] = tmp[i].r;
  }

  free(tmp);
}
