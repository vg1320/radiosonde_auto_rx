
/*
 *  dfm09 (dfm06)
 *  sync header: correlation/matched filter
 *  files: dfm09dm_dft.c demod_dft.h demod_dft.c
 *  compile:
 *      gcc -c demod_dft.c
 *      gcc dfm09dm_dft.c demod_dft.o -lm -o dfm09dm_dft
 *
 *  author: zilog80
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifdef CYGWIN
  #include <fcntl.h>  // cygwin: _setmode()
  #include <io.h>
#endif


typedef unsigned char ui8_t;
typedef unsigned int ui32_t;

//#include "demod_dft.c"
#include "demod_dft.h"


typedef struct {
    int frnr;
    int sonde_typ;
    ui32_t SN6;
    ui32_t SN;
    int week; int gpssec;
    int jahr; int monat; int tag;
    int std; int min; float sek;
    double lat; double lon; double alt;
    double dir; double horiV; double vertV;
    float meas24[5];
    float status[2];
    float _frmcnt;
    char sonde_id[16]; // "ID__:xxxxxxxx\0\0"
} gpx_t;

gpx_t gpx;

typedef struct {
    int ec;
    float ts;
} pcksts_t;

pcksts_t pck[9];

char dat_str[9][13+1];

// JSON Buffer to store sonde ID
char json_sonde_id[] = "DFMxx-xxxxxxxx\0\0";

int option_verbose = 0,  // ausfuehrliche Anzeige
    option_raw = 0,      // rohe Frames
    option_inv = 0,      // invertiert Signal
    option_auto = 0,
    option_dist = 0,     // continuous pcks 0..8
    option_ecc = 0,
    option_ptu = 0,
    option_ths = 0,
    option_json = 0,     // JSON blob output (for auto_rx)
    wavloaded = 0;
int wav_channel = 0;     // audio channel: left

int ptu_out = 0;

int start = 0;


//#define HEADLEN 32
// DFM09: Manchester2: 01->1,10->0
char rawheader[] = "10011010100110010101101001010101"; //->"0100010111001111"; // 0x45CF (big endian)

#define BITFRAME_LEN     280
char frame_bits[BITFRAME_LEN+4] = "0100010111001111";

/* ------------------------------------------------------------------------------------ */

#define BAUD_RATE   2500

/* ------------------------------------------------------------------------------------ */


#define B 8 // codeword: 8 bit
#define S 4 // davon 4 bit data

#define HEAD 0        //  16 bit
#define CONF (16+0)   //  56 bit
#define DAT1 (16+56)  // 104 bit
#define DAT2 (16+160) // 104 bit
               // frame: 280 bit

ui8_t H[4][8] =  // Parity-Check
             {{ 0, 1, 1, 1, 1, 0, 0, 0},
              { 1, 0, 1, 1, 0, 1, 0, 0},
              { 1, 1, 0, 1, 0, 0, 1, 0},
              { 1, 1, 1, 0, 0, 0, 0, 1}};
ui8_t He[8] = { 0x7, 0xB, 0xD, 0xE, 0x8, 0x4, 0x2, 0x1}; // Spalten von H:
                                                         // 1-bit-error-Syndrome
ui8_t hamming_conf[ 7*B];  //  7*8=56
ui8_t hamming_dat1[13*B];  // 13*8=104
ui8_t hamming_dat2[13*B];

ui8_t block_conf[ 7*S];  //  7*4=28
ui8_t block_dat1[13*S];  // 13*4=52
ui8_t block_dat2[13*S];

ui32_t bits2val(ui8_t *bits, int len) { // big endian
    int j;
    ui32_t val;
    if ((len < 0) || (len > 32)) return -1; // = 0xFFFF
    val = 0;
    for (j = 0; j < len; j++) {
        val |= (bits[j] << (len-1-j));
    }
    return val;
}

void deinterleave(char *str, int L, ui8_t *block) {
    int i, j;
    for (j = 0; j < B; j++) {  // L = 7, 13
        for (i = 0; i < L; i++) {
            if (str[L*j+i] >= 0x30 && str[L*j+i] <= 0x31) {
                block[B*i+j] = str[L*j+i] - 0x30; // ASCII -> bit
            }
        }
    }
}

int check(ui8_t code[8]) {
    int i, j;               // Bei Demodulierung durch Nulldurchgaenge, wenn durch Fehler ausser Takt,
    ui32_t synval = 0;      // verschieben sich die bits. Fuer Hamming-Decode waere es besser,
    ui8_t syndrom[4];       // sync zu Beginn mit Header und dann Takt beibehalten fuer decision.
    int ret=0;

    for (i = 0; i < 4; i++) { // S = 4
        syndrom[i] = 0;
        for (j = 0; j < 8; j++) { // B = 8
            syndrom[i] ^= H[i][j] & code[j];
        }
    }
    synval = bits2val(syndrom, 4);
    if (synval) {
        ret = -1;
        for (j = 0; j < 8; j++) {   // 1-bit-error
            if (synval == He[j]) {  // reicht auf databits zu pruefen, d.h.
                ret = j+1;          // (systematischer Code) He[0..3]
                break;
            }
        }
    }
    else ret = 0;
    if (ret > 0) code[ret-1] ^= 0x1;

    return ret;
}

int hamming(ui8_t *ham, int L, ui8_t *sym) {
    int i, j;
    int ecc = 0, ret = 0;      // L = 7, 13
    for (i = 0; i < L; i++) {  // L * 2 nibble (data+parity)
        if (option_ecc) {
            ecc = check(ham+B*i);
            if (ecc > 0) ret |= (1<<i);
            if (ecc < 0) ret |= ecc; // -1
        }
        for (j = 0; j < S; j++) {  // systematic: bits 0..S-1 data
            sym[S*i+j] = ham[B*i+j];
        }
    }
    return ret;
}

char nib2chr(ui8_t nib) {
    char c = '_';
    if (nib < 0x10) {
        if (nib < 0xA)  c = 0x30 + nib;
        else            c = 0x41 + nib-0xA;
    }
    return c;
}

int dat_out(ui8_t *dat_bits, int ec) {
    int i, ret = 0;
    static int fr_id;
    // int jahr = 0, monat = 0, tag = 0, std = 0, min = 0;
    int frnr = 0;
    int msek = 0;
    int lat = 0, lon = 0, alt = 0;
    int nib;
    int dvv;  // signed/unsigned 16bit

    fr_id = bits2val(dat_bits+48, 4);

    if (fr_id >= 0 && fr_id <= 8) {
        for (i = 0; i < 13; i++) {
            nib = bits2val(dat_bits+4*i, 4);
            dat_str[fr_id][i] = nib2chr(nib);
        }
        dat_str[fr_id][13] = '\0';

        pck[fr_id].ts = gpx._frmcnt; // time_stamp,frame_count,...
        if (option_ecc) {
            pck[fr_id].ec = ec; // option_ecc laesst -1 garnicht durch
            if (ec > 0) {
                ui8_t ecn = 0;
                for (i = 0; i < 15; i++) {
                    if ( (ec>>i)&1 ) ecn++;
                }
                pck[fr_id].ec = ecn;
                if ((option_dist || option_json) && ecn > 4) pck[fr_id].ec = -2; // threshold: #errors > 4
            }
        }
    }

    if (fr_id == 0) {
        start = 0x1000;
        frnr = bits2val(dat_bits+24, 8);
        gpx.frnr = frnr;
    }

    if (fr_id == 1) {
        // 00..31: ? GPS-Sats in Sicht?
        msek = bits2val(dat_bits+32, 16);  // UTC (= GPS - 18sec  ab 1.1.2017)
        gpx.sek = msek/1000.0;
    }

    if (fr_id == 2) {
        lat = bits2val(dat_bits, 32);
        gpx.lat = lat/1e7;
        dvv = (short)bits2val(dat_bits+32, 16);  // (short)? zusammen mit dir sollte unsigned sein
        gpx.horiV = dvv/1e2;
    }

    if (fr_id == 3) {
        lon = bits2val(dat_bits, 32);
        gpx.lon = lon/1e7;
        dvv = bits2val(dat_bits+32, 16) & 0xFFFF;  // unsigned
        gpx.dir = dvv/1e2;
    }

    if (fr_id == 4) {
        alt = bits2val(dat_bits, 32);
        gpx.alt = alt/1e2;
        dvv = (short)bits2val(dat_bits+32, 16);  // signed
        gpx.vertV = dvv/1e2;
    }

    if (fr_id == 5) {
    }

    if (fr_id == 6) { // sat data
    }

    if (fr_id == 7) { // sat data
    }

    if (fr_id == 8) {
        gpx.jahr  = bits2val(dat_bits,   12);
        gpx.monat = bits2val(dat_bits+12, 4);
        gpx.tag   = bits2val(dat_bits+16, 5);
        gpx.std   = bits2val(dat_bits+21, 5);
        gpx.min   = bits2val(dat_bits+26, 6);
    }

    ret = fr_id;
    return ret;
}

// DFM-06 (NXP8)
float fl20(int d) {  // float20
    int val, p;
    float f;
    p = (d>>16) & 0xF;
    val = d & 0xFFFF;
    f = val/(float)(1<<p);
    return  f;
}
/*
float flo20(int d) {
    int m, e;
    float f1, f;
    m = d & 0xFFFF;
    e = (d >> 16) & 0xF;
    f =  m / pow(2,e);
    return  f;
}
*/

// DFM-09 (STM32)
float fl24(int d) {  // float24
    int val, p;
    float f;
    p = (d>>20) & 0xF;
    val = d & 0xFFFFF;
    f = val/(float)(1<<p);
    return  f;
}

// temperature approximation
float get_Temp(float *meas) { // meas[0..4]
// NTC-Thermistor EPCOS B57540G0502
// R/T No 8402, R25=Ro=5k
// B0/100=3450
// 1/T = 1/To + 1/B log(r) , r=R/Ro
// GRAW calibration data -80C..+40C on EEPROM ?
// meas0 = g*(R + Rs)
// meas3 = g*Rs , Rs: dfm6:10k, dfm9:20k
// meas4 = g*Rf , Rf=220k
    float B0 = 3260.0;       // B/Kelvin, fit -55C..+40C
    float T0 = 25 + 273.15;  // t0=25C
    float R0 = 5.0e3;        // R0=R25=5k
    float Rf = 220e3;        // Rf = 220k
    float g = meas[4]/Rf;
    float R = (meas[0]-meas[3]) / g; // meas[0,3,4] > 0 ?
    float T = 0;                     // T/Kelvin
    if (meas[0]*meas[3]*meas[4] == 0) R = 0;
    if (R > 0)  T = 1/(1/T0 + 1/B0 * log(R/R0));
    return  T - 273.15; // Celsius
//  DFM-06: meas20 * 16 = meas24
//      -> (meas24[0]-meas24[3])/meas24[4]=(meas20[0]-meas20[3])/meas20[4]
}
float get_Temp2(float *meas) { // meas[0..4]
// NTC-Thermistor EPCOS B57540G0502
// R/T No 8402, R25=Ro=5k
// B0/100=3450
// 1/T = 1/To + 1/B log(r) , r=R/Ro
// GRAW calibration data -80C..+40C on EEPROM ?
// meas0 = g*(R+Rs)+ofs
// meas3 = g*Rs+ofs , Rs: dfm6:10k, dfm9:20k
// meas4 = g*Rf+ofs , Rf=220k
    float f  = meas[0],
          f1 = meas[3],
          f2 = meas[4];
    float B0 = 3260.0;      // B/Kelvin, fit -55C..+40C
    float T0 = 25 + 273.15; // t0=25C
    float R0 = 5.0e3;       // R0=R25=5k
    float Rf2 = 220e3;      // Rf2 = Rf = 220k
    float g_o = f2/Rf2;     // approx gain
    float Rs_o = f1/g_o;    // = Rf2 * f1/f2;
    float Rf1 = Rs_o;       // Rf1 = Rs: dfm6:10k, dfm9:20k
    float g = g_o;          // gain
    float Rb = 0.0;         // offset
    float R = 0;            // thermistor
    float T = 0;            // T/Kelvin

    if       ( 8e3 < Rs_o && Rs_o < 12e3) Rf1 = 10e3;  // dfm6
    else if  (18e3 < Rs_o && Rs_o < 22e3) Rf1 = 20e3;  // dfm9
    g = (f2 - f1) / (Rf2 - Rf1);
    Rb = (f1*Rf2-f2*Rf1)/(f2-f1); // ofs/g

    R = (f-f1)/g;                    // meas[0,3,4] > 0 ?
    if (R > 0)  T = 1/(1/T0 + 1/B0 * log(R/R0));

    if (option_ptu && ptu_out && option_verbose == 3) {
        printf("  (Rso: %.1f , Rb: %.1f)", Rs_o/1e3, Rb/1e3);
    }

    return  T - 273.15;
//  DFM-06: meas20 * 16 = meas24
}
float get_Temp4(float *meas) { // meas[0..4]
// NTC-Thermistor EPCOS B57540G0502
// [  T/C  ,   R/R25   , alpha ] :
// [ -55.0 ,  51.991   ,   6.4 ]
// [ -50.0 ,  37.989   ,   6.2 ]
// [ -45.0 ,  28.07    ,   5.9 ]
// [ -40.0 ,  20.96    ,   5.7 ]
// [ -35.0 ,  15.809   ,   5.5 ]
// [ -30.0 ,  12.037   ,   5.4 ]
// [ -25.0 ,   9.2484  ,   5.2 ]
// [ -20.0 ,   7.1668  ,   5.0 ]
// [ -15.0 ,   5.5993  ,   4.9 ]
// [ -10.0 ,   4.4087  ,   4.7 ]
// [  -5.0 ,   3.4971  ,   4.6 ]
// [   0.0 ,   2.7936  ,   4.4 ]
// [   5.0 ,   2.2468  ,   4.3 ]
// [  10.0 ,   1.8187  ,   4.2 ]
// [  15.0 ,   1.4813  ,   4.0 ]
// [  20.0 ,   1.2136  ,   3.9 ]
// [  25.0 ,   1.0000  ,   3.8 ]
// [  30.0 ,   0.82845 ,   3.7 ]
// [  35.0 ,   0.68991 ,   3.6 ]
// [  40.0 ,   0.57742 ,   3.5 ]
// -> Steinhart–Hart coefficients (polyfit):
    float p0 = 1.09698417e-03,
          p1 = 2.39564629e-04,
          p2 = 2.48821437e-06,
          p3 = 5.84354921e-08;
// T/K = 1/( p0 + p1*ln(R) + p2*ln(R)^2 + p3*ln(R)^3 )
    float Rf = 220e3;    // Rf = 220k
    float g = meas[4]/Rf;
    float R = (meas[0]-meas[3]) / g; // meas[0,3,4] > 0 ?
    float T = 0; // T/Kelvin
    if (R > 0)  T = 1/( p0 + p1*log(R) + p2*log(R)*log(R) + p3*log(R)*log(R)*log(R) );
    return  T - 273.15; // Celsius
//  DFM-06: meas20 * 16 = meas24
//      -> (meas24[0]-meas24[3])/meas24[4]=(meas20[0]-meas20[3])/meas20[4]
}


#define SNbit 0x0100
int conf_out(ui8_t *conf_bits, int ec) {
    int ret = 0;
    int val;
    ui8_t conf_id;
    ui8_t hl;
    ui32_t SN6, SN;
    static int chAbit, chA[2];
    static int chCbit, chC[2];
    static int chDbit, chD[2];
    static int ch7bit, ch7[2];
    static ui32_t SN_A, SN_C, SN_D, SN_7;
    static ui8_t max_ch;
    static ui8_t nul_ch;
    static ui8_t sn2_ch, sn_ch;
    static ui32_t SN_X;
    static int chXbit, chX[2];
    static ui8_t dfm6typ;

    conf_id = bits2val(conf_bits, 4);

    if (conf_id > 4 && bits2val(conf_bits+8, 4*5) == 0) nul_ch = bits2val(conf_bits, 8);

    dfm6typ = ((nul_ch & 0xF0)==0x50) && (nul_ch & 0x0F);
    if (dfm6typ) ptu_out = 6;
    if (dfm6typ  && (gpx.sonde_typ & 0xF) > 6)
    {   // reset if 0x5A, 0x5B (DFM-06)
        gpx.sonde_typ = 0;
        max_ch = conf_id;
    }

    if (conf_id > 4 && conf_id > max_ch) max_ch = conf_id; // mind. 5 Kanaele // reset? lower 0xsCaaaab?

    if (conf_id > 4 && conf_id == (nul_ch>>4)+1)
    {
        sn2_ch = bits2val(conf_bits, 8);

        if (option_auto)
        {
            sn_ch = ((sn2_ch>>4) & 0xF);
            if (conf_id == sn_ch)
            {
                if ( (nul_ch & 0x58) == 0x58 ) { // 0x5A, 0x5B
                    SN6 = bits2val(conf_bits+4, 4*6);   // DFM-06: Kanal 6
                    if (SN6 == gpx.SN6  &&  SN6 != 0) { // nur Nibble-Werte 0..9
                        gpx.sonde_typ = SNbit | 6;
                        ptu_out = 6;
                        sprintf(gpx.sonde_id, "ID06:%6X", gpx.SN6);
                        sprintf(json_sonde_id, "DFM06-%6X", gpx.SN6);
                    }
                    else { // reset
                        gpx.sonde_typ = 0;
                        sprintf(json_sonde_id, "DFMxx-xxxxxxxx"); //json_sonde_id[0] = '\0';
                    }
                    gpx.SN6 = SN6;
                }
                else if (   (sn2_ch & 0xF) == 0xC    // 0xsCaaaab, s==sn_ch , s: 0xA=DFM-09 , 0xC=DFM-17? 0xD=?
                         || (sn2_ch & 0xF) == 0x0 )  // 0xs0aaaab, s==sn_ch , s: 0x7,0x8: pilotsonde PS-15?
                {
                    val = bits2val(conf_bits+8, 4*5);
                    hl =  (val & 1);
                    chX[hl] = (val >> 4) & 0xFFFF;
                    chXbit |= 1 << hl;
                    if (chXbit == 3) {
                        SN = (chX[0] << 16) | chX[1];
                        if ( SN == SN_X || SN_X == 0 ) {

                            gpx.sonde_typ = SNbit | sn_ch;
                            gpx.SN = SN;

                            if (sn_ch == 0xA /*&& (sn2_ch & 0xF) == 0xC*/) ptu_out = 9; else ptu_out = 0;
                            // PS-15 ? (sn2_ch & 0xF) == 0x0 :  ptu_out = 0
                            // DFM-17? (sn_ch == 0xC) ptu_out = 9 ? // test 0xD ...?

                            if ( (gpx.sonde_typ & 0xF) == 0xA) {
                                sprintf(gpx.sonde_id, "ID09:%6u", gpx.SN);
                                sprintf(json_sonde_id, "DFM09-%6u", gpx.SN);
                            }
                            else {
                                sprintf(gpx.sonde_id, "ID-%1X:%6u", gpx.sonde_typ & 0xF, gpx.SN);
                                sprintf(json_sonde_id, "DFMx%1X-%6u", gpx.sonde_typ & 0xF,gpx.SN);
                            }
                        }
                        else { // reset
                            gpx.sonde_typ = 0;
                            sprintf(json_sonde_id, "DFMxx-xxxxxxxx"); //json_sonde_id[0] = '\0';
                        }
                        SN_X = SN;
                        chXbit = 0;
                    }
                }
                ret = (gpx.sonde_typ & 0xF);
            }
        }
    }

    if (option_auto == 0) {

        // gibt es Kanaele > 6 (2-teilige ID)?
        // if (conf_id > 6) gpx.SN6 = 0;  // -> DFM-09,PS-15  // SNbit?
        //
        // SN/ID immer im letzten Kanal? davor xy00000-Kanal? (mind. 1)
        if ((gpx.sonde_typ & 0xF) < 7  &&  conf_id == 6) {
            SN6 = bits2val(conf_bits+4, 4*6);   // DFM-06: Kanal 6
            if (SN6 == gpx.SN6  &&  SN6 != 0) { // nur Nibble-Werte 0..9
                gpx.sonde_typ = SNbit | 6;
                ptu_out = 6;
                ret = 6;
                sprintf(gpx.sonde_id, "ID06:%6X", gpx.SN6);
                sprintf(json_sonde_id, "DFM06-%6X", gpx.SN6);
            }
            else {
                gpx.sonde_typ = 0;
            }
            gpx.SN6 = SN6;
        }
        if (conf_id == 0xA) {  // 0xACxxxxy ,  DFM-09
            val = bits2val(conf_bits+8, 4*5);
            hl =  (val & 1);  // val&0xF 0,1?
            chA[hl] = (val >> 4) & 0xFFFF;
            chAbit |= 1 << hl;
            if (chAbit == 3) {  // DFM-09: Kanal A
                SN = (chA[0] << 16) | chA[1];
                if ( SN == SN_A ) {
                    gpx.sonde_typ = SNbit | 0xA;
                    gpx.SN = SN;
                    ptu_out = 9;
                    ret = 9;
                    sprintf(gpx.sonde_id, "ID09:%6u", gpx.SN);
                    sprintf(json_sonde_id, "DFM09-%6u", gpx.SN);
                }
                else {
                    gpx.sonde_typ = 0;
                }
                SN_A = SN;
                chAbit = 0;
            }
        }
        if (conf_id == 0xC) {  // 0xCCxxxxy ,  DFM-17?
            val = bits2val(conf_bits+8, 4*5);
            hl =  (val & 1);
            chC[hl] = (val >> 4) & 0xFFFF;
            chCbit |= 1 << hl;
            if (chCbit == 3) {  // DFM-17? Kanal C
                SN = (chC[0] << 16) | chC[1];
                if ( SN == SN_C ) {
                    gpx.sonde_typ = SNbit | 0xC;
                    gpx.SN = SN;
                    ptu_out = 9; // ?
                    ret = 17;
                    sprintf(gpx.sonde_id, "ID-%1X:%6u", gpx.sonde_typ & 0xF, gpx.SN);
                    sprintf(json_sonde_id, "DFM17-%6u", gpx.SN);
                }
                else {
                    gpx.sonde_typ = 0;
                }
                SN_C = SN;
                chCbit = 0;
            }
        }
        if (conf_id == 0xD) {  // 0xDCxxxxy ,  DFM-17?
            val = bits2val(conf_bits+8, 4*5);
            hl =  (val & 1);
            chD[hl] = (val >> 4) & 0xFFFF;
            chDbit |= 1 << hl;
            if (chDbit == 3) {  // DFM-17? Kanal D
                SN = (chD[0] << 16) | chD[1];
                if ( SN == SN_D ) {
                    gpx.sonde_typ = SNbit | 0xD;
                    gpx.SN = SN;
                    ptu_out = 0; // ...
                    ret = 18;
                    sprintf(gpx.sonde_id, "ID-%1X:%6u", gpx.sonde_typ & 0xF, gpx.SN);
                    sprintf(json_sonde_id, "DFM17-%6u", gpx.SN);
                }
                else {
                    gpx.sonde_typ = 0;
                }
                SN_D = SN;
                chDbit = 0;
            }
        }
        if (conf_id == 0x7) {  // 0x70xxxxy ,  pilotsonde PS-15?
            val = bits2val(conf_bits+8, 4*5);
            hl =  (val & 1);
            ch7[hl] = (val >> 4) & 0xFFFF;
            ch7bit |= 1 << hl;
            if (ch7bit == 3) {  // PS-15: Kanal 7
                SN = (ch7[0] << 16) | ch7[1];
                if ( SN == SN_7 ) {
                    gpx.sonde_typ = SNbit | 0x7;
                    gpx.SN = SN;
                    ptu_out = 0;
                    ret = 15;
                    sprintf(gpx.sonde_id, "ID15:%6u", gpx.SN);
                    sprintf(json_sonde_id, "DFM15-%6u", gpx.SN);
                }
                else {
                    gpx.sonde_typ = 0;
                }
                SN_7 = SN;
                ch7bit = 0;
            }
        }
    }


    if (conf_id >= 0 && conf_id <= 4) {
        val = bits2val(conf_bits+4, 4*6);
        gpx.meas24[conf_id] = fl24(val);
        // DFM-09 (STM32): 24bit 0exxxxx
        // DFM-06 (NXP8):  20bit 0exxxx0
        //   fl20(bits2val(conf_bits+4, 4*5))
        //       = fl20(exxxx)
        //       = fl24(exxxx0)/2^4
        //   meas20 * 16 = meas24
    }

    // STM32-status: Bat, MCU-Temp
    if ((gpx.sonde_typ & 0xF) == 0xA) { // DFM-09 (STM32)
        if (conf_id == 0x5) { // voltage
            val = bits2val(conf_bits+8, 4*4);
            gpx.status[0] = val/1000.0;
        }
        if (conf_id == 0x6) { // T-intern (STM32)
            val = bits2val(conf_bits+8, 4*4);
            gpx.status[1] = val/100.0;
        }
    }

    return ret;
}

void print_gpx() {
    int i, j;
    int contgps = 0;
    int output = 0;
    int jsonout = 0;


    output |= start;

    if (option_json && start == 0) { // JSON: initial reset
        sprintf(json_sonde_id, "DFMxx-xxxxxxxx"); //json_sonde_id[0] = '\0';
    }


    for (i = 0; i < 9/*8*/; i++) { // trigger: pck8
        if ( !( (option_dist || option_json) && pck[i].ec < 0) )
        {
            if (pck[8].ts - pck[i].ts < 6.0)  { output |= (1<<i); }
        }
        //if (option_dist && pck[i].ec < 0) { output &= ~(1<<i); }

    }

    jsonout = output;

    contgps = ((output & 0x11F) == 0x11F); // 0,1,2,3,8

    if (option_dist && !contgps) {
        output = 0;
    }
    if (option_json && !contgps) {
        jsonout = 0;
    }

    if (output & 0xF000) {

        if (option_raw == 2) {
            for (i = 0; i < 9; i++) {
                printf(" %s", dat_str[i]);
                if (option_ecc) printf(" (%1X) ", pck[i].ec&0xF);
            }
            for (i = 0; i < 9; i++) {
                for (j = 0; j < 13; j++) dat_str[i][j] = ' ';
            }
        }
        else {
            if (option_auto && option_verbose >= 2) printf("<%c> ", option_inv?'-':'+');
            printf("[%3d] ", gpx.frnr);
            printf("%4d-%02d-%02d ", gpx.jahr, gpx.monat, gpx.tag);
            printf("%02d:%02d:%04.1f ", gpx.std, gpx.min, gpx.sek);
                                                 if (option_verbose >= 2 && option_ecc) printf("(%1X,%1X,%1X) ", pck[0].ec&0xF, pck[8].ec&0xF, pck[1].ec&0xF);
            printf(" ");
            printf(" lat: %.5f ", gpx.lat);      if (option_verbose >= 2 && option_ecc) printf("(%1X)  ", pck[2].ec&0xF);
            printf(" lon: %.5f ", gpx.lon);      if (option_verbose >= 2 && option_ecc) printf("(%1X)  ", pck[3].ec&0xF);
            printf(" alt: %.1f ", gpx.alt);      if (option_verbose >= 2 && option_ecc) printf("(%1X)  ", pck[4].ec&0xF);
            printf(" vH: %5.2f ", gpx.horiV);
            printf(" D: %5.1f ", gpx.dir);
            printf(" vV: %5.2f ", gpx.vertV);
            if (option_ptu  &&  ptu_out) {
                float t = get_Temp(gpx.meas24);
                if (t > -270.0) printf("  T=%.1fC ", t);
                if (option_verbose == 3) {
                    float t2 = get_Temp2(gpx.meas24);
                    float t4 = get_Temp4(gpx.meas24);
                    if (t2 > -270.0) printf("  T2=%.1fC ", t2);
                    if (t4 > -270.0) printf(" T4=%.1fC  ", t4);
                    printf(" f0: %.2f ", gpx.meas24[0]);
                    printf(" f3: %.2f ", gpx.meas24[3]);
                    printf(" f4: %.2f ", gpx.meas24[4]);
                }
            }
            if (option_verbose == 3  &&  (gpx.sonde_typ & 0xF) == 0xA) {
                printf("  U: %.2fV ", gpx.status[0]);
                printf("  Ti: %.1fK ", gpx.status[1]);
            }
            if (option_verbose)
            {
                if (gpx.sonde_typ & SNbit) {
                    printf(" (%s) ", gpx.sonde_id);
                    gpx.sonde_typ ^= SNbit;
                }
            }
        }
        printf("\n");

        if (option_json && jsonout)
        {
            // Print JSON blob     // valid sonde_ID?
            printf("{ \"frame\": %d, \"id\": \"%s\", \"datetime\": \"%04d-%02d-%02dT%02d:%02d:%06.3fZ\", \"lat\": %.5f, \"lon\": %.5f, \"alt\": %.5f, \"vel_h\": %.5f, \"heading\": %.5f, \"vel_v\": %.5f",
                   gpx.frnr, json_sonde_id, gpx.jahr, gpx.monat, gpx.tag, gpx.std, gpx.min, gpx.sek, gpx.lat, gpx.lon, gpx.alt, gpx.horiV, gpx.dir, gpx.vertV);
            if (ptu_out) { // get temperature
                float t = get_Temp(gpx.meas24); // ecc-valid temperature?
                if (t > -270.0) printf(", \"temp\": %.1f", t);
            }
            printf(" }\n");
            printf("\n");
        }

    }

    for (i = 0; i < 9; i++) pck[i].ec = -1;
}

int print_frame(float frmcnt) {
    int i;
    int nib = 0;
    int frid = -1;
    int ret0, ret1, ret2;
    int ret = 0;

    gpx._frmcnt = frmcnt;

    deinterleave(frame_bits+CONF,  7, hamming_conf);
    deinterleave(frame_bits+DAT1, 13, hamming_dat1);
    deinterleave(frame_bits+DAT2, 13, hamming_dat2);

    ret0 = hamming(hamming_conf,  7, block_conf);
    ret1 = hamming(hamming_dat1, 13, block_dat1);
    ret2 = hamming(hamming_dat2, 13, block_dat2);
    ret = ret0 | ret1 | ret2;

    if (option_raw == 1) {

        for (i = 0; i < 7; i++) {
            nib = bits2val(block_conf+S*i, S);
            printf("%01X", nib & 0xFF);
        }
        if (option_ecc) {
            if      (ret0 == 0) printf(" [OK] ");
            else if (ret0  > 0) printf(" [KO] ");
            else                printf(" [NO] ");
        }
        printf("  ");
        for (i = 0; i < 13; i++) {
            nib = bits2val(block_dat1+S*i, S);
            printf("%01X", nib & 0xFF);
        }
        if (option_ecc) {
            if      (ret1 == 0) printf(" [OK] ");
            else if (ret1  > 0) printf(" [KO] ");
            else                printf(" [NO] ");
        }
        printf("  ");
        for (i = 0; i < 13; i++) {
            nib = bits2val(block_dat2+S*i, S);
            printf("%01X", nib & 0xFF);
        }
        if (option_ecc) {
            if      (ret2 == 0) printf(" [OK] ");
            else if (ret2  > 0) printf(" [KO] ");
            else                printf(" [NO] ");
        }
        printf("\n");

    }
    else if (option_ecc) {

        if (ret0 == 0 || ret0 > 0) {
            conf_out(block_conf, ret0);
        }
        if (ret1 == 0 || ret1 > 0) {
            frid = dat_out(block_dat1, ret1);
            if (frid == 8) print_gpx();
        }
        if (ret2 == 0 || ret2 > 0) {
            frid = dat_out(block_dat2, ret2);
            if (frid == 8) print_gpx();
        }

    }
    else {

        conf_out(block_conf, ret0);
        frid = dat_out(block_dat1, ret1);
        if (frid == 8) print_gpx();
        frid = dat_out(block_dat2, ret2);
        if (frid == 8) print_gpx();

    }

    return ret;
}

/* -------------------------------------------------------------------------- */


int main(int argc, char **argv) {

    FILE *fp = NULL;
    char *fpname = NULL;
    float spb = 0.0;
    int header_found = 0;
    int ret = 0;

    int bit;
    int bitpos = 0;
    int bitQ;
    int pos;
    int herrs, herr1;
    int headerlen = 0;
    int frm = 0, nfrms = 8; // nfrms=1,2,4,8

    int k, K;
    float mv;
    unsigned int mv_pos, mv0_pos;
    int mp = 0;

    float frm_cnt = 0.0;

    float thres = 0.65;

    int symlen = 2;
    int bitofs = 2; // +1 .. +2
    int shift = 0;


#ifdef CYGWIN
    _setmode(fileno(stdin), _O_BINARY);  // _setmode(_fileno(stdin), _O_BINARY);
#endif
    setbuf(stdout, NULL);

    fpname = argv[0];
    ++argv;
    while ((*argv) && (!wavloaded)) {
        if      ( (strcmp(*argv, "-h") == 0) || (strcmp(*argv, "--help") == 0) ) {
            fprintf(stderr, "%s [options] audio.wav\n", fpname);
            fprintf(stderr, "  options:\n");
            fprintf(stderr, "       -v, -vv\n");
            fprintf(stderr, "       -r, --raw\n");
            fprintf(stderr, "       -i, --invert\n");
            fprintf(stderr, "       --ecc        (Hamming ECC)\n");
            fprintf(stderr, "       --ths <x>    (peak threshold; default=%.1f)\n", thres);
            fprintf(stderr, "       --json       (JSON output)\n");
            return 0;
        }
        else if ( (strcmp(*argv, "-v") == 0) || (strcmp(*argv, "--verbose") == 0) ) {
            option_verbose = 1;
        }
        else if ( (strcmp(*argv, "-vv" ) == 0) ) { option_verbose = 2; }
        else if ( (strcmp(*argv, "-vvv") == 0) ) { option_verbose = 3; }
        else if ( (strcmp(*argv, "-r") == 0) || (strcmp(*argv, "--raw") == 0) ) {
            option_raw = 1;
        }
        else if ( (strcmp(*argv, "-R") == 0) || (strcmp(*argv, "--RAW") == 0) ) {
            option_raw = 2;
        }
        else if ( (strcmp(*argv, "-i") == 0) || (strcmp(*argv, "--invert") == 0) ) {
            option_inv = 0x1;
        }
        else if ( (strcmp(*argv, "--ecc") == 0) ) { option_ecc = 1; }
        else if ( (strcmp(*argv, "--ptu") == 0) ) {
            option_ptu = 1;
            //ptu_out = 1; // force ptu (non PS-15)
        }
        else if ( (strcmp(*argv, "--auto") == 0) ) { option_auto = 1; }
        else if ( (strcmp(*argv, "--dist") == 0) ) { option_dist = 1; option_ecc = 1; }
        else if ( (strcmp(*argv, "--json") == 0) ) { option_json = 1; option_ecc = 1; }
        else if ( (strcmp(*argv, "--ch2") == 0) ) { wav_channel = 1; }  // right channel (default: 0=left)
        else if ( (strcmp(*argv, "--ths") == 0) ) {
            ++argv;
            if (*argv) {
                thres = atof(*argv);
            }
            else return -1;
        }
        else if ( (strcmp(*argv, "-d") == 0) ) {
            ++argv;
            if (*argv) {
                shift = atoi(*argv);
                if (shift >  4) shift =  4;
                if (shift < -4) shift = -4;
            }
            else return -1;
        }
        else {
            fp = fopen(*argv, "rb");
            if (fp == NULL) {
                fprintf(stderr, "%s konnte nicht geoeffnet werden\n", *argv);
                return -1;
            }
            wavloaded = 1;
        }
        ++argv;
    }
    if (!wavloaded) fp = stdin;


    spb = read_wav_header(fp, (float)BAUD_RATE, wav_channel);
    if ( spb < 0 ) {
        fclose(fp);
        fprintf(stderr, "error: wav header\n");
        return -1;
    }
    if ( spb < 8 ) {
        fprintf(stderr, "note: sample rate low\n");
    }


    for (k = 0; k < 9; k++) pck[k].ec = -1; // init ecc-status


    symlen = 2;
    bitofs += shift;

    headerlen = strlen(rawheader);
    K = init_buffers(rawheader, headerlen, 0); // shape=0 (alt. shape=1)
    if ( K < 0 ) {
        fprintf(stderr, "error: init buffers\n");
        return -1;
    };

    k = 0;
    mv = 0;
    mv_pos = 0;

    while ( f32buf_sample(fp, option_inv) != EOF ) {

        k += 1;
        if (k >= K-4) {
            mv0_pos = mv_pos;
            mp = getCorrDFT(K, 0, &mv, &mv_pos);
            if (option_auto == 0 && mv < 0) mv = 0;
            k = 0;
        }
        else {
            mv = 0.0;
            continue;
        }

        if ( mp > 0 && (mv > thres || mv < -thres)) {

            if (mv_pos > mv0_pos) {

                header_found = 0;
                herrs = headcmp(symlen, rawheader, headerlen, mv_pos, mv<0, 0); // symlen=2
                herr1 = 0;
                if (herrs <= 3 && herrs > 0) {
                    herr1 = headcmp(symlen, rawheader, headerlen, mv_pos+1, mv<0, 0);
                    if (herr1 < herrs) {
                        herrs = herr1;
                        herr1 = 1;
                    }
                }
                if (herrs <= 1) {
                    header_found = 1; // herrs <= 1 bitfehler in header
                    if (mv < 0) header_found = -header_found;
                }

                if (header_found < 0) {
                    // read_sbit(option_inv) buffer reset?
                    if (option_auto) option_inv ^= 0x1;
                    else header_found = 0;
                }

                if (header_found) {

                    bitpos = 0;
                    pos = headerlen;
                    pos /= 2;

                    //if (fabs(mv) > 0.85) nfrms = 8; else nfrms = 4; // test OK/KO/NO count

                    frm = 0;
                    while ( frm < nfrms ) { // nfrms=1,2,4,8
                        frm_cnt = mv_pos/(spb*2.0*BITFRAME_LEN) + frm;
                        while ( pos < BITFRAME_LEN ) {
                            bitQ = read_sbit(fp, symlen, &bit, option_inv, bitofs, bitpos==0); // symlen=2
                            if (bitQ == EOF) { frm = nfrms; break; }
                            frame_bits[pos] = 0x30 + bit;
                            pos++;
                            bitpos += 1;
                        }
                        frame_bits[pos] = '\0';
                        ret = print_frame(frm_cnt);
                        if (pos < BITFRAME_LEN) break;
                        pos = 0;
                        frm += 1;
                        //if (ret < 0) frms += 1;
                    }

                    header_found = 0;
                    pos = headerlen;
                }
            }
        }

    }


    free_buffers();

    fclose(fp);

    return 0;
}

