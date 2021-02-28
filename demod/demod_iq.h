

#include <math.h>
#include <complex.h>

typedef unsigned char  ui8_t;
typedef unsigned short ui16_t;
typedef unsigned int   ui32_t;
typedef char  i8_t;
typedef short i16_t;
typedef int   i32_t;


typedef struct {
    int sr;       // sample_rate
    int LOG2N;
    int N;
    int N2;
    float *xn;
    float complex  *ew;
    float complex  *Fm;
    float complex  *X;
    float complex  *Z;
    float complex  *cx;
    float complex  *win; // float real
} dft_t;


typedef struct {
    FILE *fp;
    //
    int sr;       // sample_rate
    int bps;      // bits/sample
    int nch;      // channels
    int ch;       // select channel
    //
    int symlen;
    float sps;    // samples per symbol
    float _spb;   // samples per bit
    float br;     // baud rate
    //
    ui32_t sample_in;
    ui32_t sample_out;
    ui32_t delay;
    int buffered;
    int L;
    int M;
    int K;
    float *match;
    float *bufs;
    float dc_ofs;
    float dc;
    //
    int N_norm;
    int Nvar;
    float xsum;
    float qsum;
    float *xs;
    float *qs;

    // IQ-data
    int opt_iq;
    int N_IQBUF;
    float complex *raw_iqbuf;
    float complex *rot_iqbuf;

    //
    char *rawbits;
    char *hdr;
    int hdrlen;

    //
    float BT; // bw/time (ISI)
    float h;  // modulation index

    // DFT
    dft_t DFT;

    double df;
    int len_sq;

    ui32_t sample_posframe;
    ui32_t sample_posnoise;

    double V_noise;
    double V_signal;
    double SNRdB;

} dsp_t;


typedef struct {
    int sr;       // sample_rate
    int bps;      // bits_sample  bits/sample
    int nch;      // channels
    int sel_ch;   // select wav channel
} pcm_t;



float read_wav_header(pcm_t *, FILE *);
int f32buf_sample(dsp_t *, int);
int read_slbit(dsp_t *, int, int*, int, int, int, float, int);

int getCorrDFT(dsp_t *, ui32_t, float *, ui32_t *);
int headcmp(dsp_t *, int, ui32_t, int, int);
int get_fqofs_rs41(dsp_t *, ui32_t, float *, float *);
float get_bufvar(dsp_t *, int);
float get_bufmu(dsp_t *, int);

int init_buffers(dsp_t *);
int free_buffers(dsp_t *);

ui32_t get_sample(dsp_t *);

