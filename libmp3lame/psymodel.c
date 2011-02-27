/*
 *      psymodel.c
 *
 *      Copyright (c) 1999-2000 Mark Taylor
 *      Copyright (c) 2001-2002 Naoki Shibata
 *      Copyright (c) 2000-2003 Takehiro Tominaga
 *      Copyright (c) 2000-2011 Robert Hegemann
 *      Copyright (c) 2000-2005 Gabriel Bouvigne
 *      Copyright (c) 2000-2005 Alexander Leidinger
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

/* $Id: psymodel.c,v 1.202 2011/02/27 00:52:55 robert Exp $ */


/*
PSYCHO ACOUSTICS


This routine computes the psycho acoustics, delayed by one granule.  

Input: buffer of PCM data (1024 samples).  

This window should be centered over the 576 sample granule window.
The routine will compute the psycho acoustics for
this granule, but return the psycho acoustics computed
for the *previous* granule.  This is because the block
type of the previous granule can only be determined
after we have computed the psycho acoustics for the following
granule.  

Output:  maskings and energies for each scalefactor band.
block type, PE, and some correlation measures.  
The PE is used by CBR modes to determine if extra bits
from the bit reservoir should be used.  The correlation
measures are used to determine mid/side or regular stereo.
*/
/*
Notation:

barks:  a non-linear frequency scale.  Mapping from frequency to
        barks is given by freq2bark()

scalefactor bands: The spectrum (frequencies) are broken into 
                   SBMAX "scalefactor bands".  Thes bands
                   are determined by the MPEG ISO spec.  In
                   the noise shaping/quantization code, we allocate
                   bits among the partition bands to achieve the
                   best possible quality

partition bands:   The spectrum is also broken into about
                   64 "partition bands".  Each partition 
                   band is about .34 barks wide.  There are about 2-5
                   partition bands for each scalefactor band.

LAME computes all psycho acoustic information for each partition
band.  Then at the end of the computations, this information
is mapped to scalefactor bands.  The energy in each scalefactor
band is taken as the sum of the energy in all partition bands
which overlap the scalefactor band.  The maskings can be computed
in the same way (and thus represent the average masking in that band)
or by taking the minmum value multiplied by the number of
partition bands used (which represents a minimum masking in that band).
*/
/*
The general outline is as follows:

1. compute the energy in each partition band
2. compute the tonality in each partition band
3. compute the strength of each partion band "masker"
4. compute the masking (via the spreading function applied to each masker)
5. Modifications for mid/side masking.  

Each partition band is considiered a "masker".  The strength
of the i'th masker in band j is given by:

    s3(bark(i)-bark(j))*strength(i)

The strength of the masker is a function of the energy and tonality.
The more tonal, the less masking.  LAME uses a simple linear formula
(controlled by NMT and TMN) which says the strength is given by the
energy divided by a linear function of the tonality.
*/
/*
s3() is the "spreading function".  It is given by a formula
determined via listening tests.  

The total masking in the j'th partition band is the sum over
all maskings i.  It is thus given by the convolution of
the strength with s3(), the "spreading function."

masking(j) = sum_over_i  s3(i-j)*strength(i)  = s3 o strength

where "o" = convolution operator.  s3 is given by a formula determined
via listening tests.  It is normalized so that s3 o 1 = 1.

Note: instead of a simple convolution, LAME also has the
option of using "additive masking"

The most critical part is step 2, computing the tonality of each
partition band.  LAME has two tonality estimators.  The first
is based on the ISO spec, and measures how predictiable the
signal is over time.  The more predictable, the more tonal.
The second measure is based on looking at the spectrum of
a single granule.  The more peaky the spectrum, the more
tonal.  By most indications, the latter approach is better.

Finally, in step 5, the maskings for the mid and side
channel are possibly increased.  Under certain circumstances,
noise in the mid & side channels is assumed to also
be masked by strong maskers in the L or R channels.


Other data computed by the psy-model:

ms_ratio        side-channel / mid-channel masking ratio (for previous granule)
ms_ratio_next   side-channel / mid-channel masking ratio for this granule

percep_entropy[2]     L and R values (prev granule) of PE - A measure of how 
                      much pre-echo is in the previous granule
percep_entropy_MS[2]  mid and side channel values (prev granule) of percep_entropy
energy[4]             L,R,M,S energy in each channel, prev granule
blocktype_d[2]        block type to use for previous granule
*/




#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "lame.h"
#include "machine.h"
#include "encoder.h"
#include "util.h"
#include "psymodel.h"
#include "lame_global_flags.h"
#include "fft.h"
#include "lame-analysis.h"


#define NSFIRLEN 21

#ifdef M_LN10
#define  LN_TO_LOG10  (M_LN10/10)
#else
#define  LN_TO_LOG10  0.2302585093
#endif

#ifdef NON_LINEAR_PSY

static const float non_linear_psy_constant = .3;

#define NON_LINEAR_SCALE_ITEM(x)   pow((x), non_linear_psy_constant)
#define NON_LINEAR_SCALE_SUM(x)    pow((x), 1/non_linear_psy_constant)

#if 0
#define NON_LINEAR_SCALE_ENERGY(x) pow(10, (x)/10)
#else
#define NON_LINEAR_SCALE_ENERGY(x) (x)
#endif

#else
/*
#define NON_LINEAR_SCALE_ITEM(x)   (x)
#define NON_LINEAR_SCALE_SUM(x)    (x)
*/
#define NON_LINEAR_SCALE_ENERGY(x) (x)

#endif


/*
   L3psycho_anal.  Compute psycho acoustics.

   Data returned to the calling program must be delayed by one 
   granule. 

   This is done in two places.  
   If we do not need to know the blocktype, the copying
   can be done here at the top of the program: we copy the data for
   the last granule (computed during the last call) before it is
   overwritten with the new data.  It looks like this:
  
   0. static psymodel_data 
   1. calling_program_data = psymodel_data
   2. compute psymodel_data
    
   For data which needs to know the blocktype, the copying must be
   done at the end of this loop, and the old values must be saved:
   
   0. static psymodel_data_old 
   1. compute psymodel_data
   2. compute possible block type of this granule
   3. compute final block type of previous granule based on #2.
   4. calling_program_data = psymodel_data_old
   5. psymodel_data_old = psymodel_data
*/





/* psycho_loudness_approx
   jd - 2001 mar 12
in:  energy   - BLKSIZE/2 elements of frequency magnitudes ^ 2
     gfp      - uses out_samplerate, ATHtype (also needed for ATHformula)
returns: loudness^2 approximation, a positive value roughly tuned for a value
         of 1.0 for signals near clipping.
notes:   When calibrated, feeding this function binary white noise at sample
         values +32767 or -32768 should return values that approach 3.
         ATHformula is used to approximate an equal loudness curve.
future:  Data indicates that the shape of the equal loudness curve varies
         with intensity.  This function might be improved by using an equal
         loudness curve shaped for typical playback levels (instead of the
         ATH, that is shaped for the threshold).  A flexible realization might
         simply bend the existing ATH curve to achieve the desired shape.
         However, the potential gain may not be enough to justify an effort.
*/
static  FLOAT
psycho_loudness_approx(FLOAT const *energy, FLOAT const *eql_w)
{
    int     i;
    FLOAT   loudness_power;

    loudness_power = 0.0;
    /* apply weights to power in freq. bands */
    for (i = 0; i < BLKSIZE / 2; ++i)
        loudness_power += energy[i] * eql_w[i];
    loudness_power *= VO_SCALE;

    return loudness_power;
}

static void
compute_ffts(lame_internal_flags * gfc,
             FLOAT fftenergy[HBLKSIZE],
             FLOAT(*fftenergy_s)[HBLKSIZE_s],
             FLOAT(*wsamp_l)[BLKSIZE],
             FLOAT(*wsamp_s)[3][BLKSIZE_s], int gr_out, int chn, const sample_t *const buffer[2]
    )
{
    SessionConfig_t const *const cfg = &gfc->cfg;
    int     b, j;
    PsyStateVar_t *psv = &gfc->sv_psy;
    plotting_data *plt = cfg->analysis ? gfc->pinfo : 0;
    if (chn < 2) {
        fft_long(gfc, *wsamp_l, chn, buffer);
        fft_short(gfc, *wsamp_s, chn, buffer);
    }
    /* FFT data for mid and side channel is derived from L & R */
    else if (chn == 2) {
        for (j = BLKSIZE - 1; j >= 0; --j) {
            FLOAT const l = wsamp_l[0][j];
            FLOAT const r = wsamp_l[1][j];
            wsamp_l[0][j] = (l + r) * (FLOAT) (SQRT2 * 0.5);
            wsamp_l[1][j] = (l - r) * (FLOAT) (SQRT2 * 0.5);
        }
        for (b = 2; b >= 0; --b) {
            for (j = BLKSIZE_s - 1; j >= 0; --j) {
                FLOAT const l = wsamp_s[0][b][j];
                FLOAT const r = wsamp_s[1][b][j];
                wsamp_s[0][b][j] = (l + r) * (FLOAT) (SQRT2 * 0.5);
                wsamp_s[1][b][j] = (l - r) * (FLOAT) (SQRT2 * 0.5);
            }
        }
    }

    /*********************************************************************
     *  compute energies
     *********************************************************************/
    fftenergy[0] = NON_LINEAR_SCALE_ENERGY(wsamp_l[0][0]);
    fftenergy[0] *= fftenergy[0];

    for (j = BLKSIZE / 2 - 1; j >= 0; --j) {
        FLOAT const re = (*wsamp_l)[BLKSIZE / 2 - j];
        FLOAT const im = (*wsamp_l)[BLKSIZE / 2 + j];
        fftenergy[BLKSIZE / 2 - j] = NON_LINEAR_SCALE_ENERGY((re * re + im * im) * 0.5f);
    }
    for (b = 2; b >= 0; --b) {
        fftenergy_s[b][0] = (*wsamp_s)[b][0];
        fftenergy_s[b][0] *= fftenergy_s[b][0];
        for (j = BLKSIZE_s / 2 - 1; j >= 0; --j) {
            FLOAT const re = (*wsamp_s)[b][BLKSIZE_s / 2 - j];
            FLOAT const im = (*wsamp_s)[b][BLKSIZE_s / 2 + j];
            fftenergy_s[b][BLKSIZE_s / 2 - j] = NON_LINEAR_SCALE_ENERGY((re * re + im * im) * 0.5f);
        }
    }
    /* total energy */
    {
        FLOAT   totalenergy = 0.0;
        for (j = 11; j < HBLKSIZE; j++)
            totalenergy += fftenergy[j];

        psv->tot_ener[chn] = totalenergy;
    }

    if (plt) {
        for (j = 0; j < HBLKSIZE; j++) {
            plt->energy[gr_out][chn][j] = plt->energy_save[chn][j];
            plt->energy_save[chn][j] = fftenergy[j];
        }
    }

    /*********************************************************************
     * compute loudness approximation (used for ATH auto-level adjustment) 
     *********************************************************************/
    if (chn < 2) {      /*no loudness for mid/side ch */
        gfc->ov_psy.loudness_sq[gr_out][chn] = psv->loudness_sq_save[chn];
        psv->loudness_sq_save[chn]
            = psycho_loudness_approx(fftenergy, gfc->ATH->eql_w);
    }
}

/* mask_add optimization */
/* init the limit values used to avoid computing log in mask_add when it is not necessary */

/* For example, with i = 10*log10(m2/m1)/10*16         (= log10(m2/m1)*16)
 *
 * abs(i)>8 is equivalent (as i is an integer) to
 * abs(i)>=9
 * i>=9 || i<=-9
 * equivalent to (as i is the biggest integer smaller than log10(m2/m1)*16 
 * or the smallest integer bigger than log10(m2/m1)*16 depending on the sign of log10(m2/m1)*16)
 * log10(m2/m1)>=9/16 || log10(m2/m1)<=-9/16
 * exp10 is strictly increasing thus this is equivalent to
 * m2/m1 >= 10^(9/16) || m2/m1<=10^(-9/16) which are comparisons to constants
 */


#define I1LIMIT 8       /* as in if(i>8)  */
#define I2LIMIT 23      /* as in if(i>24) -> changed 23 */
#define MLIMIT  15      /* as in if(m<15) */

static FLOAT ma_max_i1;
static FLOAT ma_max_i2;
static FLOAT ma_max_m;

    /*This is the masking table:
       According to tonality, values are going from 0dB (TMN)
       to 9.3dB (NMT).
       After additive masking computation, 8dB are added, so
       final values are going from 8dB to 17.3dB
     */
static const FLOAT tab[] = {
    1.0 /*pow(10, -0) */ ,
    0.79433 /*pow(10, -0.1) */ ,
    0.63096 /*pow(10, -0.2) */ ,
    0.63096 /*pow(10, -0.2) */ ,
    0.63096 /*pow(10, -0.2) */ ,
    0.63096 /*pow(10, -0.2) */ ,
    0.63096 /*pow(10, -0.2) */ ,
    0.25119 /*pow(10, -0.6) */ ,
    0.11749             /*pow(10, -0.93) */
};




static void
init_mask_add_max_values(void)
{
    ma_max_i1 = pow(10, (I1LIMIT + 1) / 16.0);
    ma_max_i2 = pow(10, (I2LIMIT + 1) / 16.0);
    ma_max_m = pow(10, (MLIMIT) / 10.0);
}




/* addition of simultaneous masking   Naoki Shibata 2000/7 */
inline static FLOAT
mask_add(FLOAT m1, FLOAT m2, int kk, int b, lame_internal_flags const *gfc, int shortblock)
{
    static const FLOAT table1[] = {
        3.3246 * 3.3246, 3.23837 * 3.23837, 3.15437 * 3.15437, 3.00412 * 3.00412, 2.86103 * 2.86103,
        2.65407 * 2.65407, 2.46209 * 2.46209, 2.284 * 2.284,
        2.11879 * 2.11879, 1.96552 * 1.96552, 1.82335 * 1.82335, 1.69146 * 1.69146,
        1.56911 * 1.56911, 1.46658 * 1.46658, 1.37074 * 1.37074, 1.31036 * 1.31036,
        1.25264 * 1.25264, 1.20648 * 1.20648, 1.16203 * 1.16203, 1.12765 * 1.12765,
        1.09428 * 1.09428, 1.0659 * 1.0659, 1.03826 * 1.03826, 1.01895 * 1.01895,
        1
    };

    static const FLOAT table2[] = {
        1.33352 * 1.33352, 1.35879 * 1.35879, 1.38454 * 1.38454, 1.39497 * 1.39497,
        1.40548 * 1.40548, 1.3537 * 1.3537, 1.30382 * 1.30382, 1.22321 * 1.22321,
        1.14758 * 1.14758,
        1
    };

    static const FLOAT table3[] = {
        2.35364 * 2.35364, 2.29259 * 2.29259, 2.23313 * 2.23313, 2.12675 * 2.12675,
        2.02545 * 2.02545, 1.87894 * 1.87894, 1.74303 * 1.74303, 1.61695 * 1.61695,
        1.49999 * 1.49999, 1.39148 * 1.39148, 1.29083 * 1.29083, 1.19746 * 1.19746,
        1.11084 * 1.11084, 1.03826 * 1.03826
    };


    int     i;
    FLOAT   ratio;


    if (m2 > m1) {
        if (m2 < (m1 * ma_max_i2))
            ratio = m2 / m1;
        else
            return (m1 + m2);
    }
    else {
        if (m1 >= (m2 * ma_max_i2))
            return (m1 + m2);
        ratio = m1 / m2;
    }
    /*i = abs(10*log10(m2 / m1)/10*16);
       m = 10*log10((m1+m2)/gfc->ATH->cb[k]); */


    /* Should always be true, just checking */
    assert(m1 >= 0);
    assert(m2 >= 0);


    m1 += m2;

    if ((unsigned int) (b + 3) <= 3 + 3) { /* approximately, 1 bark = 3 partitions */
        /* 65% of the cases */
        /* originally 'if(i > 8)' */
        if (ratio >= ma_max_i1) {
            /* 43% of the total */
            return m1;
        }

        /* 22% of the total */
        i = (int) (FAST_LOG10_X(ratio, 16.0));
        return m1 * table2[i];
    }

    /* m<15 equ log10((m1+m2)/gfc->ATH->cb[k])<1.5
     * equ (m1+m2)/gfc->ATH->cb[k]<10^1.5
     * equ (m1+m2)<10^1.5 * gfc->ATH->cb[k]
     */

    i = (int) FAST_LOG10_X(ratio, 16.0);
    if (shortblock) {
        m2 = gfc->ATH->cb_s[kk] * gfc->ATH->adjust_factor;
    }
    else {
        m2 = gfc->ATH->cb_l[kk] * gfc->ATH->adjust_factor;
    }
    assert(m2 >= 0);
    if (m1 < ma_max_m * m2) {
        /* 3% of the total */
        /* Originally if (m > 0) { */
        if (m1 > m2) {
            FLOAT   f, r;

            f = 1.0;
            if (i <= 13)
                f = table3[i];

            r = FAST_LOG10_X(m1 / m2, 10.0 / 15.0);
            return m1 * ((table1[i] - f) * r + f);
        }

        if (i > 13)
            return m1;

        return m1 * table3[i];
    }


    /* 10% of total */
    return m1 * table1[i];
}


/* addition of simultaneous masking   Naoki Shibata 2000/7 */
inline static FLOAT
vbrpsy_mask_add(FLOAT m1, FLOAT m2, int b)
{
    static const FLOAT table2[] = {
        1.33352 * 1.33352, 1.35879 * 1.35879, 1.38454 * 1.38454, 1.39497 * 1.39497,
        1.40548 * 1.40548, 1.3537 * 1.3537, 1.30382 * 1.30382, 1.22321 * 1.22321,
        1.14758 * 1.14758,
        1
    };

    FLOAT   ratio;

    if (m1 < 0) {
        m1 = 0;
    }
    if (m2 < 0) {
        m2 = 0;
    }
    if (m1 <= 0) {
        return m2;
    }
    if (m2 <= 0) {
        return m1;
    }
    if (m2 > m1) {
        ratio = m2 / m1;
    }
    else {
        ratio = m1 / m2;
    }
    if (-2 <= b && b <= 2) { /* approximately, 1 bark = 3 partitions */
        /* originally 'if(i > 8)' */
        if (ratio >= ma_max_i1) {
            return m1 + m2;
        }
        else {
            int     i = (int) (FAST_LOG10_X(ratio, 16.0));
            return (m1 + m2) * table2[i];
        }
    }
    if (ratio < ma_max_i2) {
        return m1 + m2;
    }
    if (m1 < m2) {
        m1 = m2;
    }
    return m1;
}


/*************************************************************** 
 * compute interchannel masking effects
 ***************************************************************/
static void
calc_interchannel_masking(III_psy_xmin * const thm0, III_psy_xmin * const thm1, FLOAT ratio)
{
    int     sb, sblock;
    for (sb = 0; sb < SBMAX_l; sb++) {
        FLOAT const l = thm0->l[sb];
        FLOAT const r = thm1->l[sb];
        thm0->l[sb] += r * ratio;
        thm1->l[sb] += l * ratio;
    }
    for (sb = 0; sb < SBMAX_s; sb++) {
        for (sblock = 0; sblock < 3; sblock++) {
            FLOAT const l = thm0->s[sb][sblock];
            FLOAT const r = thm1->s[sb][sblock];
            thm0->s[sb][sblock] += r * ratio;
            thm1->s[sb][sblock] += l * ratio;
        }
    }
}



/*************************************************************** 
 * compute M/S thresholds from Johnston & Ferreira 1992 ICASSP paper
 ***************************************************************/
static void
msfix1(lame_internal_flags * gfc)
{
    PsyStateVar_t *psv = &gfc->sv_psy;
    PsyConst_l_t const *const gdl = &gfc->cd_psy->l;
    PsyConst_s_t const *const gds = &gfc->cd_psy->s;
    int     sb, sblock;
    FLOAT   rside, rmid, mld;
    for (sb = 0; sb < SBMAX_l; sb++) {
        /* use this fix if L & R masking differs by 2db or less */
        /* if db = 10*log10(x2/x1) < 2 */
        /* if (x2 < 1.58*x1) { */
        if (psv->thm[0].l[sb] > 1.58 * psv->thm[1].l[sb]
            || psv->thm[1].l[sb] > 1.58 * psv->thm[0].l[sb])
            continue;

        mld = gdl->mld[sb] * psv->en[3].l[sb];
        rmid = Max(psv->thm[2].l[sb], Min(psv->thm[3].l[sb], mld));

        mld = gdl->mld[sb] * psv->en[2].l[sb];
        rside = Max(psv->thm[3].l[sb], Min(psv->thm[2].l[sb], mld));
        psv->thm[2].l[sb] = rmid;
        psv->thm[3].l[sb] = rside;
    }

    for (sb = 0; sb < SBMAX_s; sb++) {
        for (sblock = 0; sblock < 3; sblock++) {
            if (psv->thm[0].s[sb][sblock] > 1.58 * psv->thm[1].s[sb][sblock]
                || psv->thm[1].s[sb][sblock] > 1.58 * psv->thm[0].s[sb][sblock])
                continue;

            mld = gds->mld[sb] * psv->en[3].s[sb][sblock];
            rmid = Max(psv->thm[2].s[sb][sblock], Min(psv->thm[3].s[sb][sblock], mld));

            mld = gds->mld[sb] * psv->en[2].s[sb][sblock];
            rside = Max(psv->thm[3].s[sb][sblock], Min(psv->thm[2].s[sb][sblock], mld));

            psv->thm[2].s[sb][sblock] = rmid;
            psv->thm[3].s[sb][sblock] = rside;
        }
    }
}

/*************************************************************** 
 * Adjust M/S maskings if user set "msfix"
 ***************************************************************/
/* Naoki Shibata 2000 */
static void
ns_msfix(lame_internal_flags * gfc, FLOAT msfix, FLOAT athoffset_factor, FLOAT athadjust_factor)
{
    PsyStateVar_t *const psv = &gfc->sv_psy;
    PsyConst_l_t const *const gdl = &gfc->cd_psy->l;
    PsyConst_s_t const *const gds = &gfc->cd_psy->s;
    int     sb, sblock;
    FLOAT   msfix2 = msfix;
    FLOAT   athlower = athoffset_factor * athadjust_factor;

    msfix *= 2.0f;
    msfix2 *= 2.0f;
    for (sb = 0; sb < SBMAX_l; sb++) {
        FLOAT   thmLR, thmM, thmS, ath;
        ath = (gfc->ATH->cb_l[gdl->bm[sb]]) * athlower;
        thmLR = Min(Max(psv->thm[0].l[sb], ath), Max(psv->thm[1].l[sb], ath));
        thmM = Max(psv->thm[2].l[sb], ath);
        thmS = Max(psv->thm[3].l[sb], ath);
        if (thmLR * msfix < thmM + thmS) {
            FLOAT const f = thmLR * msfix2 / (thmM + thmS);
            thmM *= f;
            thmS *= f;
            assert(thmM + thmS > 0);
        }
        psv->thm[2].l[sb] = Min(thmM, psv->thm[2].l[sb]);
        psv->thm[3].l[sb] = Min(thmS, psv->thm[3].l[sb]);
    }

    athlower *= ((FLOAT) BLKSIZE_s / BLKSIZE);
    for (sb = 0; sb < SBMAX_s; sb++) {
        for (sblock = 0; sblock < 3; sblock++) {
            FLOAT   thmLR, thmM, thmS, ath;
            ath = (gfc->ATH->cb_s[gds->bm[sb]]) * athlower;
            thmLR = Min(Max(psv->thm[0].s[sb][sblock], ath), Max(psv->thm[1].s[sb][sblock], ath));
            thmM = Max(psv->thm[2].s[sb][sblock], ath);
            thmS = Max(psv->thm[3].s[sb][sblock], ath);

            if (thmLR * msfix < thmM + thmS) {
                FLOAT const f = thmLR * msfix / (thmM + thmS);
                thmM *= f;
                thmS *= f;
                assert(thmM + thmS > 0);
            }
            psv->thm[2].s[sb][sblock] = Min(psv->thm[2].s[sb][sblock], thmM);
            psv->thm[3].s[sb][sblock] = Min(psv->thm[3].s[sb][sblock], thmS);
        }
    }
}

/* short block threshold calculation (part 2)

    partition band bo_s[sfb] is at the transition from scalefactor
    band sfb to the next one sfb+1; enn and thmm have to be split
    between them
*/
static void
convert_partition2scalefac_s(lame_internal_flags * gfc, FLOAT const *eb, FLOAT const *thr, int chn,
                             int sblock)
{
    PsyStateVar_t *const psv = &gfc->sv_psy;
    PsyConst_s_t const *const gds = &gfc->cd_psy->s;
    FLOAT   enn, thmm;
    int     sb, b;
    enn = thmm = 0.0;
    for (sb = b = 0; sb < SBMAX_s; ++b, ++sb) {
        int const bo_s_sb = gds->bo[sb];
        int const npart_s = gds->npart;
        int const b_lim = bo_s_sb < npart_s ? bo_s_sb : npart_s;
        while (b < b_lim) {
            assert(eb[b] >= 0); /* iff failed, it may indicate some index error elsewhere */
            assert(thr[b] >= 0);
            enn += eb[b];
            thmm += thr[b];
            b++;
        }
        psv->en[chn].s[sb][sblock] = enn;
        psv->thm[chn].s[sb][sblock] = thmm;

        if (b >= npart_s) {
            ++sb;
            break;
        }
        assert(eb[b] >= 0); /* iff failed, it may indicate some index error elsewhere */
        assert(thr[b] >= 0);
        {
            /* at transition sfb -> sfb+1 */
            FLOAT const w_curr = gds->bo_weight[sb];
            FLOAT const w_next = 1.0 - w_curr;
            enn = w_curr * eb[b];
            thmm = w_curr * thr[b];
            psv->en[chn].s[sb][sblock] += enn;
            psv->thm[chn].s[sb][sblock] += thmm;
            enn = w_next * eb[b];
            thmm = w_next * thr[b];
        }
    }
    /* zero initialize the rest */
    for (; sb < SBMAX_s; ++sb) {
        psv->en[chn].s[sb][sblock] = 0;
        psv->thm[chn].s[sb][sblock] = 0;
    }
}

/* longblock threshold calculation (part 2) */
static void
convert_partition2scalefac_l(lame_internal_flags * gfc, FLOAT const *eb, FLOAT const *thr, int chn)
{
    PsyStateVar_t *const psv = &gfc->sv_psy;
    PsyConst_l_t const *const gdl = &gfc->cd_psy->l;
    FLOAT   enn, thmm;
    int     sb, b;
    enn = thmm = 0.0;
    for (sb = b = 0; sb < SBMAX_l; ++b, ++sb) {
        int const bo_l_sb = gdl->bo[sb];
        int const npart_l = gdl->npart;
        int const b_lim = bo_l_sb < npart_l ? bo_l_sb : npart_l;
        while (b < b_lim) {
            assert(eb[b] >= 0); /* iff failed, it may indicate some index error elsewhere */
            assert(thr[b] >= 0);
            enn += eb[b];
            thmm += thr[b];
            b++;
        }
        psv->en[chn].l[sb] = enn;
        psv->thm[chn].l[sb] = thmm;

        if (b >= npart_l) {
            ++sb;
            break;
        }
        assert(eb[b] >= 0);
        assert(thr[b] >= 0);
        {
            /* at transition sfb -> sfb+1 */
            FLOAT const w_curr = gdl->bo_weight[sb];
            FLOAT const w_next = 1.0 - w_curr;
            enn = w_curr * eb[b];
            thmm = w_curr * thr[b];
            psv->en[chn].l[sb] += enn;
            psv->thm[chn].l[sb] += thmm;
            enn = w_next * eb[b];
            thmm = w_next * thr[b];
        }
    }
    /* zero initialize the rest */
    for (; sb < SBMAX_l; ++sb) {
        psv->en[chn].l[sb] = 0;
        psv->thm[chn].l[sb] = 0;
    }
}


static void
compute_masking_s(lame_internal_flags * gfc,
                  const FLOAT(*fftenergy_s)[HBLKSIZE_s], FLOAT * eb, FLOAT * thr, int chn, int sblock)
{
    PsyStateVar_t *const psv = &gfc->sv_psy;
    PsyConst_s_t const *const gds = &gfc->cd_psy->s;
    int     i, j, b;

    for (b = j = 0; b < gds->npart; ++b) {
        FLOAT   ebb = 0, m = 0;
        int const n = gds->numlines[b];
        for (i = 0; i < n; ++i, ++j) {
            FLOAT const el = fftenergy_s[sblock][j];
            ebb += el;
            if (m < el)
                m = el;
        }
        eb[b] = ebb;
    }
    assert(b == gds->npart);
    assert(j == 129);
    for (j = b = 0; b < gds->npart; b++) {
        int     kk = gds->s3ind[b][0];
        FLOAT   ecb = gds->s3[j++] * eb[kk];
        ++kk;
        while (kk <= gds->s3ind[b][1]) {
            ecb += gds->s3[j] * eb[kk];
            ++j, ++kk;
        }

        {               /* limit calculated threshold by previous granule */
            FLOAT const x = rpelev_s * psv->nb_s1[chn][b];
            thr[b] = Min(ecb, x);
        }
        if (psv->blocktype_old[chn & 1] == SHORT_TYPE) {
            /* limit calculated threshold by even older granule */
            FLOAT const x = rpelev2_s * psv->nb_s2[chn][b];
            FLOAT const y = thr[b];
            thr[b] = Min(x, y);
        }

        psv->nb_s2[chn][b] = psv->nb_s1[chn][b];
        psv->nb_s1[chn][b] = ecb;
        assert(thr[b] >= 0);
    }
    for (; b <= CBANDS; ++b) {
        eb[b] = 0;
        thr[b] = 0;
    }
}

static void
block_type_set(lame_internal_flags * gfc, int *uselongblock, int *blocktype_d, int *blocktype)
{
    PsyStateVar_t *const psv = &gfc->sv_psy;
    SessionConfig_t const *const cfg = &gfc->cfg;
    int     chn;

    if (cfg->short_blocks == short_block_coupled
        /* force both channels to use the same block type */
        /* this is necessary if the frame is to be encoded in ms_stereo.  */
        /* But even without ms_stereo, FhG  does this */
        && !(uselongblock[0] && uselongblock[1]))
        uselongblock[0] = uselongblock[1] = 0;

    /* update the blocktype of the previous granule, since it depends on what
     * happend in this granule */
    for (chn = 0; chn < cfg->channels_out; chn++) {
        blocktype[chn] = NORM_TYPE;
        /* disable short blocks */
        if (cfg->short_blocks == short_block_dispensed)
            uselongblock[chn] = 1;
        if (cfg->short_blocks == short_block_forced)
            uselongblock[chn] = 0;

        if (uselongblock[chn]) {
            /* no attack : use long blocks */
            assert(psv->blocktype_old[chn] != START_TYPE);
            if (psv->blocktype_old[chn] == SHORT_TYPE)
                blocktype[chn] = STOP_TYPE;
        }
        else {
            /* attack : use short blocks */
            blocktype[chn] = SHORT_TYPE;
            if (psv->blocktype_old[chn] == NORM_TYPE) {
                psv->blocktype_old[chn] = START_TYPE;
            }
            if (psv->blocktype_old[chn] == STOP_TYPE)
                psv->blocktype_old[chn] = SHORT_TYPE;
        }

        blocktype_d[chn] = psv->blocktype_old[chn]; /* value returned to calling program */
        psv->blocktype_old[chn] = blocktype[chn]; /* save for next call to l3psy_anal */
    }
}


static inline FLOAT
NS_INTERP(FLOAT x, FLOAT y, FLOAT r)
{
    /* was pow((x),(r))*pow((y),1-(r)) */
    if (r >= 1.0)
        return x;       /* 99.7% of the time */
    if (r <= 0.0)
        return y;
    if (y > 0.0)
        return pow(x / y, r) * y; /* rest of the time */
    return 0.0;         /* never happens */
}



static  FLOAT
pecalc_s(III_psy_ratio const *mr, FLOAT masking_lower)
{
    FLOAT   pe_s;
    static const FLOAT regcoef_s[] = {
        11.8,           /* these values are tuned only for 44.1kHz... */
        13.6,
        17.2,
        32,
        46.5,
        51.3,
        57.5,
        67.1,
        71.5,
        84.6,
        97.6,
        130,
/*      255.8 */
    };
    unsigned int sb, sblock;

    pe_s = 1236.28 / 4;
    for (sb = 0; sb < SBMAX_s - 1; sb++) {
        for (sblock = 0; sblock < 3; sblock++) {
            FLOAT const thm = mr->thm.s[sb][sblock];
            assert(sb < dimension_of(regcoef_s));
            if (thm > 0.0) {
                FLOAT const x = thm * masking_lower;
                FLOAT const en = mr->en.s[sb][sblock];
                if (en > x) {
                    if (en > x * 1e10) {
                        pe_s += regcoef_s[sb] * (10.0 * LOG10);
                    }
                    else {
                        assert(x > 0);
                        pe_s += regcoef_s[sb] * FAST_LOG10(en / x);
                    }
                }
            }
        }
    }

    return pe_s;
}

static  FLOAT
pecalc_l(III_psy_ratio const *mr, FLOAT masking_lower)
{
    FLOAT   pe_l;
    static const FLOAT regcoef_l[] = {
        6.8,            /* these values are tuned only for 44.1kHz... */
        5.8,
        5.8,
        6.4,
        6.5,
        9.9,
        12.1,
        14.4,
        15,
        18.9,
        21.6,
        26.9,
        34.2,
        40.2,
        46.8,
        56.5,
        60.7,
        73.9,
        85.7,
        93.4,
        126.1,
/*      241.3 */
    };
    unsigned int sb;

    pe_l = 1124.23 / 4;
    for (sb = 0; sb < SBMAX_l - 1; sb++) {
        FLOAT const thm = mr->thm.l[sb];
        assert(sb < dimension_of(regcoef_l));
        if (thm > 0.0) {
            FLOAT const x = thm * masking_lower;
            FLOAT const en = mr->en.l[sb];
            if (en > x) {
                if (en > x * 1e10) {
                    pe_l += regcoef_l[sb] * (10.0 * LOG10);
                }
                else {
                    assert(x > 0);
                    pe_l += regcoef_l[sb] * FAST_LOG10(en / x);
                }
            }
        }
    }

    return pe_l;
}


static void
calc_energy(PsyConst_l_t const *l, FLOAT const *fftenergy, FLOAT * eb, FLOAT * max, FLOAT * avg)
{
    int     b, j;

    for (b = j = 0; b < l->npart; ++b) {
        FLOAT   ebb = 0, m = 0;
        int     i;
        for (i = 0; i < l->numlines[b]; ++i, ++j) {
            FLOAT const el = fftenergy[j];
            assert(el >= 0);
            ebb += el;
            if (m < el)
                m = el;
        }
        eb[b] = ebb;
        max[b] = m;
        avg[b] = ebb * l->rnumlines[b];
        assert(l->rnumlines[b] >= 0);
        assert(ebb >= 0);
        assert(eb[b] >= 0);
        assert(max[b] >= 0);
        assert(avg[b] >= 0);
    }
}


static void
calc_mask_index_l(lame_internal_flags const *gfc, FLOAT const *max,
                  FLOAT const *avg, unsigned char *mask_idx)
{
    PsyConst_l_t const *const gdl = &gfc->cd_psy->l;
    FLOAT   m, a;
    int     b, k;
    int const last_tab_entry = sizeof(tab) / sizeof(tab[0]) - 1;
    b = 0;
    a = avg[b] + avg[b + 1];
    assert(a >= 0);
    if (a > 0.0) {
        m = max[b];
        if (m < max[b + 1])
            m = max[b + 1];
        assert((gdl->numlines[b] + gdl->numlines[b + 1] - 1) > 0);
        a = 20.0 * (m * 2.0 - a)
            / (a * (gdl->numlines[b] + gdl->numlines[b + 1] - 1));
        k = (int) a;
        if (k > last_tab_entry)
            k = last_tab_entry;
        mask_idx[b] = k;
    }
    else {
        mask_idx[b] = 0;
    }

    for (b = 1; b < gdl->npart - 1; b++) {
        a = avg[b - 1] + avg[b] + avg[b + 1];
        assert(a >= 0);
        if (a > 0.0) {
            m = max[b - 1];
            if (m < max[b])
                m = max[b];
            if (m < max[b + 1])
                m = max[b + 1];
            assert((gdl->numlines[b - 1] + gdl->numlines[b] + gdl->numlines[b + 1] - 1) > 0);
            a = 20.0 * (m * 3.0 - a)
                / (a * (gdl->numlines[b - 1] + gdl->numlines[b] + gdl->numlines[b + 1] - 1));
            k = (int) a;
            if (k > last_tab_entry)
                k = last_tab_entry;
            mask_idx[b] = k;
        }
        else {
            mask_idx[b] = 0;
        }
    }
    assert(b > 0);
    assert(b == gdl->npart - 1);

    a = avg[b - 1] + avg[b];
    assert(a >= 0);
    if (a > 0.0) {
        m = max[b - 1];
        if (m < max[b])
            m = max[b];
        assert((gdl->numlines[b - 1] + gdl->numlines[b] - 1) > 0);
        a = 20.0 * (m * 2.0 - a)
            / (a * (gdl->numlines[b - 1] + gdl->numlines[b] - 1));
        k = (int) a;
        if (k > last_tab_entry)
            k = last_tab_entry;
        mask_idx[b] = k;
    }
    else {
        mask_idx[b] = 0;
    }
    assert(b == (gdl->npart - 1));
}


int
L3psycho_anal_ns(lame_internal_flags * gfc,
                 const sample_t *const buffer[2], int gr_out,
                 III_psy_ratio masking_ratio[2][2],
                 III_psy_ratio masking_MS_ratio[2][2],
                 FLOAT percep_entropy[2], FLOAT percep_MS_entropy[2],
                 FLOAT energy[4], int blocktype_d[2])
{
    SessionConfig_t const *const cfg = &gfc->cfg;
    PsyStateVar_t *const psv = &gfc->sv_psy;
    PsyConst_l_t const *const gdl = &gfc->cd_psy->l;
    PsyConst_s_t const *const gds = &gfc->cd_psy->s;
    plotting_data *plt = cfg->analysis ? gfc->pinfo : 0;

    /* fft and energy calculation   */
    FLOAT   wsamp_L[2][BLKSIZE];
    FLOAT   wsamp_S[2][3][BLKSIZE_s];

    /* convolution   */
    FLOAT   eb_l[CBANDS + 1], eb_s[CBANDS + 1];
    FLOAT   thr[CBANDS + 2];

    /* block type  */
    int     blocktype[2], uselongblock[2];

    /* usual variables like loop indices, etc..    */
    int     numchn, chn;
    int     b, i, j, k;
    int     sb, sblock;

    /* variables used for --nspsytune */
    FLOAT   ns_hpfsmpl[2][576];
    FLOAT   pcfact;

    unsigned char mask_idx_l[CBANDS + 2], mask_idx_s[CBANDS + 2];

    memset(mask_idx_s, 0, sizeof(mask_idx_s));
    memset(&ns_hpfsmpl[0][0], 0, sizeof(ns_hpfsmpl));

    numchn = cfg->channels_out;
    /* chn=2 and 3 = Mid and Side channels */
    if (cfg->mode == JOINT_STEREO)
        numchn = 4;

    if (cfg->vbr == vbr_off) {
        pcfact =
            gfc->sv_enc.ResvMax ==
            0 ? 0 : ((FLOAT) gfc->sv_enc.ResvSize) / gfc->sv_enc.ResvMax * 0.5;
    }
    else if (cfg->vbr == vbr_abr) {
        pcfact = 1.0;
    }
    else {
        /*static const FLOAT pcQns[10]={1.0,1.0,1.0,0.8,0.6,0.5,0.4,0.3,0.2,0.1};
           pcfact = pcQns[gfp->VBR_q]; */
        pcfact = 0.6;
    }

    /**********************************************************************
     *  Apply HPF of fs/4 to the input signal.
     *  This is used for attack detection / handling.
     **********************************************************************/
    /* Don't copy the input buffer into a temporary buffer */
    /* unroll the loop 2 times */
    for (chn = 0; chn < cfg->channels_out; chn++) {
        static const FLOAT fircoef[] = {
            -8.65163e-18 * 2, -0.00851586 * 2, -6.74764e-18 * 2, 0.0209036 * 2,
            -3.36639e-17 * 2, -0.0438162 * 2, -1.54175e-17 * 2, 0.0931738 * 2,
            -5.52212e-17 * 2, -0.313819 * 2
        };
        /* apply high pass filter of fs/4 */
        const sample_t *const firbuf = &buffer[chn][576 - 350 - NSFIRLEN + 192];
        assert(sizeof(fircoef) / sizeof(fircoef[0]) == ((NSFIRLEN - 1) / 2));
        for (i = 0; i < 576; i++) {
            FLOAT   sum1, sum2;
            sum1 = firbuf[i + 10];
            sum2 = 0.0;
            for (j = 0; j < ((NSFIRLEN - 1) / 2) - 1; j += 2) {
                sum1 += fircoef[j] * (firbuf[i + j] + firbuf[i + NSFIRLEN - j]);
                sum2 += fircoef[j + 1] * (firbuf[i + j + 1] + firbuf[i + NSFIRLEN - j - 1]);
            }
            ns_hpfsmpl[chn][i] = sum1 + sum2;
        }
        masking_ratio[gr_out][chn].en = psv->en[chn];
        masking_ratio[gr_out][chn].thm = psv->thm[chn];
        if (numchn > 2) {
            /* MS maskings  */
            /*percep_MS_entropy         [chn-2]     = gfc -> pe  [chn];  */
            masking_MS_ratio[gr_out][chn].en = psv->en[chn + 2];
            masking_MS_ratio[gr_out][chn].thm = psv->thm[chn + 2];
        }
    }

    for (chn = 0; chn < numchn; chn++) {
        FLOAT(*wsamp_l)[BLKSIZE];
        FLOAT(*wsamp_s)[3][BLKSIZE_s];
        FLOAT   en_subshort[12];
        FLOAT   en_short[4] = { 0 };
        FLOAT   attack_intensity[12];
        int     ns_uselongblock = 1;
        FLOAT   max[CBANDS], avg[CBANDS];
        int     ns_attacks[4] = { 0 };
        FLOAT   fftenergy[HBLKSIZE];
        FLOAT   fftenergy_s[3][HBLKSIZE_s];

        const FLOAT (*const_fftenergy_s)[HBLKSIZE_s] = (const FLOAT (*)[HBLKSIZE_s])fftenergy_s;

        /*  rh 20040301: the following loops do access one off the limits
         *  so I increase  the array dimensions by one and initialize the
         *  accessed values to zero
         */
        assert(gds->npart <= CBANDS);
        assert(gdl->npart <= CBANDS);

 /*************************************************************** 
  * determine the block type (window type)
  ***************************************************************/
        /* calculate energies of each sub-shortblocks */
        for (i = 0; i < 3; i++) {
            en_subshort[i] = psv->last_en_subshort[chn][i + 6];
            assert(psv->last_en_subshort[chn][i + 4] > 0);
            attack_intensity[i]
                = en_subshort[i] / psv->last_en_subshort[chn][i + 4];
            en_short[0] += en_subshort[i];
        }

        if (chn == 2) {
            for (i = 0; i < 576; i++) {
                FLOAT   l, r;
                l = ns_hpfsmpl[0][i];
                r = ns_hpfsmpl[1][i];
                ns_hpfsmpl[0][i] = l + r;
                ns_hpfsmpl[1][i] = l - r;
            }
        }
        {
            FLOAT const *pf = ns_hpfsmpl[chn & 1];
            for (i = 0; i < 9; i++) {
                FLOAT const *const pfe = pf + 576 / 9;
                FLOAT   p = 1.;
                for (; pf < pfe; pf++)
                    if (p < fabs(*pf))
                        p = fabs(*pf);

                psv->last_en_subshort[chn][i] = en_subshort[i + 3] = p;
                en_short[1 + i / 3] += p;
                if (p > en_subshort[i + 3 - 2]) {
                    assert(en_subshort[i + 3 - 2] > 0);
                    p = p / en_subshort[i + 3 - 2];
                }
                else if (en_subshort[i + 3 - 2] > p * 10.0) {
                    assert(p > 0);
                    p = en_subshort[i + 3 - 2] / (p * 10.0);
                }
                else
                    p = 0.0;
                attack_intensity[i + 3] = p;
            }
        }

        if (plt) {
            FLOAT   x = attack_intensity[0];
            for (i = 1; i < 12; i++)
                if (x < attack_intensity[i])
                    x = attack_intensity[i];
            plt->ers[gr_out][chn] = plt->ers_save[chn];
            plt->ers_save[chn] = x;
        }

        /* compare energies between sub-shortblocks */
        {
            FLOAT   x = gfc->cd_psy->attack_threshold[chn];
            for (i = 0; i < 12; i++) {
                if (ns_attacks[i / 3] == 0) {
                    if (attack_intensity[i] > x) {
                        ns_attacks[i / 3] = (i % 3) + 1;
                    }
                }
            }
        }
        /* should have energy change between short blocks,
           in order to avoid periodic signals */
        for (i = 1; i < 4; i++) {
            float   ratio;
            if (en_short[i - 1] > en_short[i]) {
                assert(en_short[i] > 0);
                ratio = en_short[i - 1] / en_short[i];
            }
            else {
                assert(en_short[i - 1] > 0);
                ratio = en_short[i] / en_short[i - 1];
            }
            if (ratio < 1.7) {
                ns_attacks[i] = 0;
                if (i == 1)
                    ns_attacks[0] = 0;
            }
        }

        if (ns_attacks[0] && psv->last_attacks[chn])
            ns_attacks[0] = 0;

        if (psv->last_attacks[chn] == 3 ||
            ns_attacks[0] + ns_attacks[1] + ns_attacks[2] + ns_attacks[3]) {
            ns_uselongblock = 0;

            if (ns_attacks[1] && ns_attacks[0])
                ns_attacks[1] = 0;
            if (ns_attacks[2] && ns_attacks[1])
                ns_attacks[2] = 0;
            if (ns_attacks[3] && ns_attacks[2])
                ns_attacks[3] = 0;
        }

        if (chn < 2) {
            uselongblock[chn] = ns_uselongblock;
        }
        else {
            if (ns_uselongblock == 0) {
                uselongblock[0] = uselongblock[1] = 0;
            }
        }

        /* there is a one granule delay.  Copy maskings computed last call
         * into masking_ratio to return to calling program.
         */
        energy[chn] = psv->tot_ener[chn];

 /*********************************************************************
  *  compute FFTs
  *********************************************************************/
        wsamp_s = wsamp_S + (chn & 1);
        wsamp_l = wsamp_L + (chn & 1);
        compute_ffts(gfc, fftenergy, fftenergy_s, wsamp_l, wsamp_s, gr_out, chn, buffer);

 /*********************************************************************
        *    Calculate the energy and the tonality of each partition.
 *********************************************************************/
        calc_energy(gdl, fftenergy, eb_l, max, avg);
        calc_mask_index_l(gfc, max, avg, mask_idx_l);

        /* compute masking thresholds for short blocks */
        for (sblock = 0; sblock < 3; sblock++) {
            FLOAT   enn, thmm;
            compute_masking_s(gfc, const_fftenergy_s, eb_s, thr, chn, sblock);
            convert_partition2scalefac_s(gfc, eb_s, thr, chn, sblock);

            /****   short block pre-echo control   ****/
            for (sb = 0; sb < SBMAX_s; sb++) {
                thmm = psv->thm[chn].s[sb][sblock];

                thmm *= NS_PREECHO_ATT0;
                if (ns_attacks[sblock] >= 2 || ns_attacks[sblock + 1] == 1) {
                    int const idx = (sblock != 0) ? sblock - 1 : 2;
                    double const p = NS_INTERP(psv->thm[chn].s[sb][idx],
                                               thmm, NS_PREECHO_ATT1 * pcfact);
                    thmm = Min(thmm, p);
                }

                if (ns_attacks[sblock] == 1) {
                    int const idx = (sblock != 0) ? sblock - 1 : 2;
                    double const p = NS_INTERP(psv->thm[chn].s[sb][idx],
                                               thmm, NS_PREECHO_ATT2 * pcfact);
                    thmm = Min(thmm, p);
                }
                else if ((sblock != 0 && ns_attacks[sblock - 1] == 3)
                         || (sblock == 0 && psv->last_attacks[chn] == 3)) {
                    int const idx = (sblock != 2) ? sblock + 1 : 0;
                    double const p = NS_INTERP(psv->thm[chn].s[sb][idx],
                                               thmm, NS_PREECHO_ATT2 * pcfact);
                    thmm = Min(thmm, p);
                }

                /* pulse like signal detection for fatboy.wav and so on */
                enn = en_subshort[sblock * 3 + 3] + en_subshort[sblock * 3 + 4]
                    + en_subshort[sblock * 3 + 5];
                if (en_subshort[sblock * 3 + 5] * 6 < enn) {
                    thmm *= 0.5;
                    if (en_subshort[sblock * 3 + 4] * 6 < enn)
                        thmm *= 0.5;
                }

                psv->thm[chn].s[sb][sblock] = thmm;
            }
        }
        psv->last_attacks[chn] = ns_attacks[2];

 /*********************************************************************
  *      convolve the partitioned energy and unpredictability
  *      with the spreading function, s3_l[b][k]
  ********************************************************************/
        k = 0;
        {
            for (b = 0; b < gdl->npart; b++) {
                FLOAT   eb2;
                FLOAT   ecb;
                /* convolve the partitioned energy with the spreading function */
                int     kk = gdl->s3ind[b][0];
                eb2 = eb_l[kk] * tab[mask_idx_l[kk]];
                ecb = gdl->s3[k++] * eb2;
                while (++kk <= gdl->s3ind[b][1]) {
                    eb2 = eb_l[kk] * tab[mask_idx_l[kk]];
                    ecb = mask_add(ecb, gdl->s3[k++] * eb2, kk, kk - b, gfc, 0);
                }
                ecb *= 0.158489319246111; /* pow(10,-0.8) */

                /****   long block pre-echo control   ****/
                /* dont use long block pre-echo control if previous granule was 
                 * a short block.  This is to avoid the situation:   
                 * frame0:  quiet (very low masking)  
                 * frame1:  surge  (triggers short blocks)
                 * frame2:  regular frame.  looks like pre-echo when compared to 
                 *          frame0, but all pre-echo was in frame1.
                 */
                /* chn=0,1   L and R channels
                   chn=2,3   S and M channels.
                 */

                if (psv->blocktype_old[chn & 1] == SHORT_TYPE)
                    thr[b] = ecb; /* Min(ecb, rpelev*psv->nb_l1[chn][b]); */
                else
                    thr[b] = NS_INTERP(Min(ecb,
                                           Min(rpelev * psv->nb_l1[chn][b],
                                               rpelev2 * psv->nb_l2[chn][b])), ecb, pcfact);

                psv->nb_l2[chn][b] = psv->nb_l1[chn][b];
                psv->nb_l1[chn][b] = ecb;
            }
        }
        for (; b <= CBANDS; ++b) {
            eb_l[b] = 0;
            thr[b] = 0;
        }
        /* compute masking thresholds for long blocks */
        convert_partition2scalefac_l(gfc, eb_l, thr, chn);

    }                   /* end loop over chn */

    if (cfg->mode == STEREO || cfg->mode == JOINT_STEREO) { /* but not for Dual Mono */
        if (cfg->interChRatio > 0.0) {
            III_psy_xmin *const thm0 = &psv->thm[0];
            III_psy_xmin *const thm1 = &psv->thm[1];
            calc_interchannel_masking(thm0, thm1, cfg->interChRatio);
        }
    }

    if (cfg->mode == JOINT_STEREO) {
        FLOAT   msfix;
        msfix1(gfc);
        msfix = cfg->msfix;
        if (fabs(msfix) > 0.0)
            ns_msfix(gfc, msfix, cfg->ATH_offset_factor, gfc->ATH->adjust_factor);
    }

    /*************************************************************** 
     * determine final block type
     ***************************************************************/
    block_type_set(gfc, uselongblock, blocktype_d, blocktype);

    /*********************************************************************
     * compute the value of PE to return ... no delay and advance
     *********************************************************************/
    for (chn = 0; chn < numchn; chn++) {
        FLOAT  *ppe;
        int     type;
        III_psy_ratio const *mr;

        if (chn > 1) {
            ppe = percep_MS_entropy - 2;
            type = NORM_TYPE;
            if (blocktype_d[0] == SHORT_TYPE || blocktype_d[1] == SHORT_TYPE)
                type = SHORT_TYPE;
            mr = &masking_MS_ratio[gr_out][chn - 2];
        }
        else {
            ppe = percep_entropy;
            type = blocktype_d[chn];
            mr = &masking_ratio[gr_out][chn];
        }

        if (type == SHORT_TYPE) {
            ppe[chn] = pecalc_s(mr, gfc->sv_qnt.masking_lower);
        }
        else {
            ppe[chn] = pecalc_l(mr, gfc->sv_qnt.masking_lower);
        }

        if (plt) {
            plt->pe[gr_out][chn] = ppe[chn];
        }

    }
    return 0;
}





static void
vbrpsy_compute_fft_l(lame_internal_flags * gfc, const sample_t *const buffer[2], int chn, int gr_out,
                     FLOAT fftenergy[HBLKSIZE], FLOAT(*wsamp_l)[BLKSIZE])
{
    SessionConfig_t const *const cfg = &gfc->cfg;
    PsyStateVar_t *psv = &gfc->sv_psy;
    plotting_data *plt = cfg->analysis ? gfc->pinfo : 0;
    int     j;

    if (chn < 2) {
        fft_long(gfc, *wsamp_l, chn, buffer);
    }
    else if (chn == 2) {
        /* FFT data for mid and side channel is derived from L & R */
        for (j = BLKSIZE - 1; j >= 0; --j) {
            FLOAT const l = wsamp_l[0][j];
            FLOAT const r = wsamp_l[1][j];
            wsamp_l[0][j] = (l + r) * (FLOAT) (SQRT2 * 0.5f);
            wsamp_l[1][j] = (l - r) * (FLOAT) (SQRT2 * 0.5f);
        }
    }

    /*********************************************************************
    *  compute energies
    *********************************************************************/
    fftenergy[0] = NON_LINEAR_SCALE_ENERGY(wsamp_l[0][0]);
    fftenergy[0] *= fftenergy[0];

    for (j = BLKSIZE / 2 - 1; j >= 0; --j) {
        FLOAT const re = (*wsamp_l)[BLKSIZE / 2 - j];
        FLOAT const im = (*wsamp_l)[BLKSIZE / 2 + j];
        fftenergy[BLKSIZE / 2 - j] = NON_LINEAR_SCALE_ENERGY((re * re + im * im) * 0.5f);
    }
    /* total energy */
    {
        FLOAT   totalenergy = 0.0f;
        for (j = 11; j < HBLKSIZE; j++)
            totalenergy += fftenergy[j];

        psv->tot_ener[chn] = totalenergy;
    }

    if (plt) {
        for (j = 0; j < HBLKSIZE; j++) {
            plt->energy[gr_out][chn][j] = plt->energy_save[chn][j];
            plt->energy_save[chn][j] = fftenergy[j];
        }
    }
}


static void
vbrpsy_compute_fft_s(lame_internal_flags const* gfc, const sample_t *const buffer[2], int chn, int sblock,
                     FLOAT(*fftenergy_s)[HBLKSIZE_s], FLOAT(*wsamp_s)[3][BLKSIZE_s])
{
    int     j;

    if (sblock == 0 && chn < 2) {
        fft_short(gfc, *wsamp_s, chn, buffer);
    }
    if (chn == 2) {
        /* FFT data for mid and side channel is derived from L & R */
        for (j = BLKSIZE_s - 1; j >= 0; --j) {
            FLOAT const l = wsamp_s[0][sblock][j];
            FLOAT const r = wsamp_s[1][sblock][j];
            wsamp_s[0][sblock][j] = (l + r) * (FLOAT) (SQRT2 * 0.5f);
            wsamp_s[1][sblock][j] = (l - r) * (FLOAT) (SQRT2 * 0.5f);
        }
    }

    /*********************************************************************
    *  compute energies
    *********************************************************************/
    fftenergy_s[sblock][0] = (*wsamp_s)[sblock][0];
    fftenergy_s[sblock][0] *= fftenergy_s[sblock][0];
    for (j = BLKSIZE_s / 2 - 1; j >= 0; --j) {
        FLOAT const re = (*wsamp_s)[sblock][BLKSIZE_s / 2 - j];
        FLOAT const im = (*wsamp_s)[sblock][BLKSIZE_s / 2 + j];
        fftenergy_s[sblock][BLKSIZE_s / 2 - j] =
            NON_LINEAR_SCALE_ENERGY((re * re + im * im) * 0.5f);
    }
}


    /*********************************************************************
    * compute loudness approximation (used for ATH auto-level adjustment) 
    *********************************************************************/
static void
vbrpsy_compute_loudness_approximation_l(lame_internal_flags * gfc, int gr_out, int chn,
                                        const FLOAT fftenergy[HBLKSIZE])
{
    PsyStateVar_t *psv = &gfc->sv_psy;
    if (chn < 2) {      /*no loudness for mid/side ch */
        gfc->ov_psy.loudness_sq[gr_out][chn] = psv->loudness_sq_save[chn];
        psv->loudness_sq_save[chn] = psycho_loudness_approx(fftenergy, gfc->ATH->eql_w);
    }
}


    /**********************************************************************
    *  Apply HPF of fs/4 to the input signal.
    *  This is used for attack detection / handling.
    **********************************************************************/
static void
vbrpsy_attack_detection(lame_internal_flags * gfc, const sample_t *const buffer[2], int gr_out,
                        III_psy_ratio masking_ratio[2][2], III_psy_ratio masking_MS_ratio[2][2],
                        FLOAT energy[4], FLOAT sub_short_factor[4][3], int ns_attacks[4][4],
                        int uselongblock[2])
{
    FLOAT   ns_hpfsmpl[2][576];
    SessionConfig_t const *const cfg = &gfc->cfg;
    PsyStateVar_t *const psv = &gfc->sv_psy;
    plotting_data *plt = cfg->analysis ? gfc->pinfo : 0;
    int const n_chn_out = cfg->channels_out;
    /* chn=2 and 3 = Mid and Side channels */
    int const n_chn_psy = (cfg->mode == JOINT_STEREO) ? 4 : n_chn_out;
    int     chn, i, j;

    memset(&ns_hpfsmpl[0][0], 0, sizeof(ns_hpfsmpl));
    /* Don't copy the input buffer into a temporary buffer */
    /* unroll the loop 2 times */
    for (chn = 0; chn < n_chn_out; chn++) {
        static const FLOAT fircoef[] = {
            -8.65163e-18 * 2, -0.00851586 * 2, -6.74764e-18 * 2, 0.0209036 * 2,
            -3.36639e-17 * 2, -0.0438162 * 2, -1.54175e-17 * 2, 0.0931738 * 2,
            -5.52212e-17 * 2, -0.313819 * 2
        };
        /* apply high pass filter of fs/4 */
        const sample_t *const firbuf = &buffer[chn][576 - 350 - NSFIRLEN + 192];
        assert(dimension_of(fircoef) == ((NSFIRLEN - 1) / 2));
        for (i = 0; i < 576; i++) {
            FLOAT   sum1, sum2;
            sum1 = firbuf[i + 10];
            sum2 = 0.0;
            for (j = 0; j < ((NSFIRLEN - 1) / 2) - 1; j += 2) {
                sum1 += fircoef[j] * (firbuf[i + j] + firbuf[i + NSFIRLEN - j]);
                sum2 += fircoef[j + 1] * (firbuf[i + j + 1] + firbuf[i + NSFIRLEN - j - 1]);
            }
            ns_hpfsmpl[chn][i] = sum1 + sum2;
        }
        masking_ratio[gr_out][chn].en = psv->en[chn];
        masking_ratio[gr_out][chn].thm = psv->thm[chn];
        if (n_chn_psy > 2) {
            /* MS maskings  */
            /*percep_MS_entropy         [chn-2]     = gfc -> pe  [chn];  */
            masking_MS_ratio[gr_out][chn].en = psv->en[chn + 2];
            masking_MS_ratio[gr_out][chn].thm = psv->thm[chn + 2];
        }
    }
    for (chn = 0; chn < n_chn_psy; chn++) {
        FLOAT   attack_intensity[12];
        FLOAT   en_subshort[12];
        FLOAT   en_short[4] = { 0, 0, 0, 0 };
        FLOAT const *pf = ns_hpfsmpl[chn & 1];
        int     ns_uselongblock = 1;

        if (chn == 2) {
            for (i = 0, j = 576; j > 0; ++i, --j) {
                FLOAT const l = ns_hpfsmpl[0][i];
                FLOAT const r = ns_hpfsmpl[1][i];
                ns_hpfsmpl[0][i] = l + r;
                ns_hpfsmpl[1][i] = l - r;
            }
        }
        /*************************************************************** 
        * determine the block type (window type)
        ***************************************************************/
        /* calculate energies of each sub-shortblocks */
        for (i = 0; i < 3; i++) {
            en_subshort[i] = psv->last_en_subshort[chn][i + 6];
            assert(psv->last_en_subshort[chn][i + 4] > 0);
            attack_intensity[i] = en_subshort[i] / psv->last_en_subshort[chn][i + 4];
            en_short[0] += en_subshort[i];
        }

        for (i = 0; i < 9; i++) {
            FLOAT const *const pfe = pf + 576 / 9;
            FLOAT   p = 1.;
            for (; pf < pfe; pf++)
                if (p < fabs(*pf))
                    p = fabs(*pf);
            psv->last_en_subshort[chn][i] = en_subshort[i + 3] = p;
            en_short[1 + i / 3] += p;
            if (p > en_subshort[i + 3 - 2]) {
                assert(en_subshort[i + 3 - 2] > 0);
                p = p / en_subshort[i + 3 - 2];
            }
            else if (en_subshort[i + 3 - 2] > p * 10.0f) {
                assert(p > 0);
                p = en_subshort[i + 3 - 2] / (p * 10.0f);
            }
            else {
                p = 0.0;
            }
            attack_intensity[i + 3] = p;
        }
                
        /* pulse like signal detection for fatboy.wav and so on */
        for (i = 0; i < 3; ++i) {
            FLOAT const enn =
                en_subshort[i * 3 + 3] + en_subshort[i * 3 + 4] + en_subshort[i * 3 + 5];
            FLOAT   factor = 1.f;
            if (en_subshort[i * 3 + 5] * 6 < enn) {
                factor *= 0.5f;
                if (en_subshort[i * 3 + 4] * 6 < enn) {
                    factor *= 0.5f;
                }
            }
            sub_short_factor[chn][i] = factor;
        }

        if (plt) {
            FLOAT   x = attack_intensity[0];
            for (i = 1; i < 12; i++) {
                if (x < attack_intensity[i]) {
                    x = attack_intensity[i];
                }
            }
            plt->ers[gr_out][chn] = plt->ers_save[chn];
            plt->ers_save[chn] = x;
        }

        /* compare energies between sub-shortblocks */
        {
            FLOAT   x = gfc->cd_psy->attack_threshold[chn];
            for (i = 0; i < 12; i++) {
                if (ns_attacks[chn][i / 3] == 0) {
                    if (attack_intensity[i] > x) {
                        ns_attacks[chn][i / 3] = (i % 3) + 1;
                    }
                }
            }
        }
        /* should have energy change between short blocks, in order to avoid periodic signals */
        /* Good samples to show the effect are Trumpet test songs */
        /* GB: tuned (1) to avoid too many short blocks for test sample TRUMPET */
        /* RH: tuned (2) to let enough short blocks through for test sample FSOL and SNAPS */
        for (i = 1; i < 4; i++) {
            FLOAT const u = en_short[i - 1];
            FLOAT const v = en_short[i];
            FLOAT const m = Max(u, v);
            if (m < 40000) { /* (2) */
                if (u < 1.7f * v && v < 1.7f * u) { /* (1) */
                    if (i == 1 && ns_attacks[chn][0] <= ns_attacks[chn][i]) {
                        ns_attacks[chn][0] = 0;
                    }
                    ns_attacks[chn][i] = 0;
                }
            }
        }
        
        if (ns_attacks[chn][0] <= psv->last_attacks[chn]) {
            ns_attacks[chn][0] = 0;
        }

        if (psv->last_attacks[chn] == 3 ||
            ns_attacks[chn][0] + ns_attacks[chn][1] + ns_attacks[chn][2] + ns_attacks[chn][3]) {
            ns_uselongblock = 0;

            if (ns_attacks[chn][1] && ns_attacks[chn][0]) {
                ns_attacks[chn][1] = 0;
            }
            if (ns_attacks[chn][2] && ns_attacks[chn][1]) {
                ns_attacks[chn][2] = 0;
            }
            if (ns_attacks[chn][3] && ns_attacks[chn][2]) {
                ns_attacks[chn][3] = 0;
            }
        }

        if (chn < 2) {
            uselongblock[chn] = ns_uselongblock;
        }
        else {
            if (ns_uselongblock == 0) {
                uselongblock[0] = uselongblock[1] = 0;
            }
        }

        /* there is a one granule delay.  Copy maskings computed last call
         * into masking_ratio to return to calling program.
         */
        energy[chn] = psv->tot_ener[chn];
    }
}


static void
vbrpsy_skip_masking_s(lame_internal_flags * gfc, int chn, int sblock)
{
    if (sblock == 0) {
        FLOAT  *nbs2 = &gfc->sv_psy.nb_s2[chn][0];
        FLOAT  *nbs1 = &gfc->sv_psy.nb_s1[chn][0];
        int const n = gfc->cd_psy->s.npart;
        int     b;
        for (b = 0; b < n; b++) {
            nbs2[b] = nbs1[b];
        }
    }
}


static void
psyvbr_calc_mask_index_s(lame_internal_flags const *gfc, FLOAT const *max,
                         FLOAT const *avg, unsigned char *mask_idx)
{
    PsyConst_s_t const *const gds = &gfc->cd_psy->s;
    FLOAT   m, a;
    int     b, k;
    int const last_tab_entry = dimension_of(tab) - 1;
    b = 0;
    a = avg[b] + avg[b + 1];
    assert(a >= 0);
    if (a > 0.0f) {
        m = max[b];
        if (m < max[b + 1])
            m = max[b + 1];
        assert((gds->numlines[b] + gds->numlines[b + 1] - 1) > 0);
        a = 20.0f * (m * 2.0f - a)
            / (a * (gds->numlines[b] + gds->numlines[b + 1] - 1));
        k = (int) a;
        if (k > last_tab_entry)
            k = last_tab_entry;
        mask_idx[b] = k;
    }
    else {
        mask_idx[b] = 0;
    }

    for (b = 1; b < gds->npart - 1; b++) {
        a = avg[b - 1] + avg[b] + avg[b + 1];
        assert(b + 1 < gds->npart);
        assert(a >= 0);
        if (a > 0.0) {
            m = max[b - 1];
            if (m < max[b])
                m = max[b];
            if (m < max[b + 1])
                m = max[b + 1];
            assert((gds->numlines[b - 1] + gds->numlines[b] + gds->numlines[b + 1] - 1) > 0);
            a = 20.0f * (m * 3.0f - a)
                / (a * (gds->numlines[b - 1] + gds->numlines[b] + gds->numlines[b + 1] - 1));
            k = (int) a;
            if (k > last_tab_entry)
                k = last_tab_entry;
            mask_idx[b] = k;
        }
        else {
            mask_idx[b] = 0;
        }
    }
    assert(b > 0);
    assert(b == gds->npart - 1);

    a = avg[b - 1] + avg[b];
    assert(a >= 0);
    if (a > 0.0) {
        m = max[b - 1];
        if (m < max[b])
            m = max[b];
        assert((gds->numlines[b - 1] + gds->numlines[b] - 1) > 0);
        a = 20.0f * (m * 2.0f - a)
            / (a * (gds->numlines[b - 1] + gds->numlines[b] - 1));
        k = (int) a;
        if (k > last_tab_entry)
            k = last_tab_entry;
        mask_idx[b] = k;
    }
    else {
        mask_idx[b] = 0;
    }
    assert(b == (gds->npart - 1));
}


static void
vbrpsy_compute_masking_s(lame_internal_flags * gfc, const FLOAT(*fftenergy_s)[HBLKSIZE_s], FLOAT * eb,
                         FLOAT * thr, int chn, int sblock)
{
    PsyStateVar_t *const psv = &gfc->sv_psy;
    PsyConst_s_t const *const gds = &gfc->cd_psy->s;
    FLOAT   max[CBANDS], avg[CBANDS];
    int     i, j, b;
    unsigned char mask_idx_s[CBANDS];

    memset(max, 0, sizeof(max));
    memset(avg, 0, sizeof(avg));

    for (b = j = 0; b < gds->npart; ++b) {
        FLOAT   ebb = 0, m = 0;
        int const n = gds->numlines[b];
        for (i = 0; i < n; ++i, ++j) {
            FLOAT const el = fftenergy_s[sblock][j];
            ebb += el;
            if (m < el)
                m = el;
        }
        eb[b] = ebb;
        assert(ebb >= 0);
        max[b] = m;
        assert(n > 0);
        avg[b] = ebb * gds->rnumlines[b];
        assert(avg[b] >= 0);
    }
    assert(b == gds->npart);
    assert(j == 129);
    psyvbr_calc_mask_index_s(gfc, max, avg, mask_idx_s);
    for (j = b = 0; b < gds->npart; b++) {
        int     kk = gds->s3ind[b][0];
        int const last = gds->s3ind[b][1];
        int     dd, dd_n;
        FLOAT   x, ecb, avg_mask;
        FLOAT const masking_lower = gds->masking_lower[b] * gfc->sv_qnt.masking_lower;
        
        dd = mask_idx_s[kk];
        dd_n = 1;
        ecb = gds->s3[j] * eb[kk] * tab[mask_idx_s[kk]];
        ++j, ++kk;
        while (kk <= last) {
            dd += mask_idx_s[kk];
            dd_n += 1;
            x = gds->s3[j] * eb[kk] * tab[mask_idx_s[kk]];
            ecb = vbrpsy_mask_add(ecb, x, kk - b);
            ++j, ++kk;
        }
        dd = (1 + 2 * dd) / (2 * dd_n);
        avg_mask = tab[dd] * 0.5f;
        ecb *= avg_mask;
#if 0                   /* we can do PRE ECHO control now here, or do it later */
        if (psv->blocktype_old[chn & 0x01] == SHORT_TYPE) {
            /* limit calculated threshold by even older granule */
            FLOAT const t1 = rpelev_s * psv->nb_s1[chn][b];
            FLOAT const t2 = rpelev2_s * psv->nb_s2[chn][b];
            FLOAT const tm = (t2 > 0) ? Min(ecb, t2) : ecb;
            thr[b] = (t1 > 0) ? NS_INTERP(Min(tm, t1), ecb, 0.6) : ecb;
        }
        else {
            /* limit calculated threshold by older granule */
            FLOAT const t1 = rpelev_s * psv->nb_s1[chn][b];
            thr[b] = (t1 > 0) ? NS_INTERP(Min(ecb, t1), ecb, 0.6) : ecb;
        }
#else /* we do it later */
        thr[b] = ecb;
#endif
        psv->nb_s2[chn][b] = psv->nb_s1[chn][b];
        psv->nb_s1[chn][b] = ecb;
        {
            /*  if THR exceeds EB, the quantization routines will take the difference
             *  from other bands. in case of strong tonal samples (tonaltest.wav)
             *  this leads to heavy distortions. that's why we limit THR here.
             */
            x = max[b];
            x *= gds->minval[b];
            x *= avg_mask;
            if (thr[b] > x) {
                thr[b] = x;
            }
        }
        if (masking_lower > 1) {
            thr[b] *= masking_lower;
        }
        if (thr[b] > eb[b]) {
            thr[b] = eb[b];
        }
        if (masking_lower < 1) {
            thr[b] *= masking_lower;
        }

        assert(thr[b] >= 0);
    }
    for (; b < CBANDS; ++b) {
        eb[b] = 0;
        thr[b] = 0;
    }
}


static void
vbrpsy_compute_masking_l(lame_internal_flags * gfc, const FLOAT fftenergy[HBLKSIZE], FLOAT eb_l[CBANDS],
                         FLOAT thr[CBANDS], int chn)
{
    PsyStateVar_t *const psv = &gfc->sv_psy;
    PsyConst_l_t const *const gdl = &gfc->cd_psy->l;
    FLOAT   max[CBANDS], avg[CBANDS];
    unsigned char mask_idx_l[CBANDS + 2];
    int     k, b;

 /*********************************************************************
    *    Calculate the energy and the tonality of each partition.
 *********************************************************************/
    calc_energy(gdl, fftenergy, eb_l, max, avg);
    calc_mask_index_l(gfc, max, avg, mask_idx_l);

 /*********************************************************************
    *      convolve the partitioned energy and unpredictability
    *      with the spreading function, s3_l[b][k]
 ********************************************************************/
    k = 0;
    for (b = 0; b < gdl->npart; b++) {
        FLOAT   x, ecb, avg_mask, t;
        FLOAT const masking_lower = gdl->masking_lower[b] * gfc->sv_qnt.masking_lower;
        /* convolve the partitioned energy with the spreading function */
        int     kk = gdl->s3ind[b][0];
        int const last = gdl->s3ind[b][1];
        int     dd = 0, dd_n = 0;
        dd = mask_idx_l[kk];
        dd_n += 1;
        ecb = gdl->s3[k] * eb_l[kk] * tab[mask_idx_l[kk]];
        ++k, ++kk;
        while (kk <= last) {
            dd += mask_idx_l[kk];
            dd_n += 1;
            x = gdl->s3[k] * eb_l[kk] * tab[mask_idx_l[kk]];
            t = vbrpsy_mask_add(ecb, x, kk - b);
#if 0
            ecb += eb_l[kk];
            if (ecb > t) {
                ecb = t;
            }
#else
            ecb = t;
#endif
            ++k, ++kk;
        }
        dd = (1 + 2 * dd) / (2 * dd_n);
        avg_mask = tab[dd] * 0.5f;
        ecb *= avg_mask;

        /****   long block pre-echo control   ****/
        /* dont use long block pre-echo control if previous granule was 
         * a short block.  This is to avoid the situation:   
         * frame0:  quiet (very low masking)  
         * frame1:  surge  (triggers short blocks)
         * frame2:  regular frame.  looks like pre-echo when compared to 
         *          frame0, but all pre-echo was in frame1.
         */
        /* chn=0,1   L and R channels
           chn=2,3   S and M channels.
         */
        if (psv->blocktype_old[chn & 0x01] == SHORT_TYPE) {
            FLOAT const ecb_limit = rpelev * psv->nb_l1[chn][b];
            if (ecb_limit > 0) {
                thr[b] = Min(ecb, ecb_limit);
            }
            else {
                /* Robert 071209:
                   Because we don't calculate long block psy when we know a granule
                   should be of short blocks, we don't have any clue how the granule
                   before would have looked like as a long block. So we have to guess
                   a little bit for this END_TYPE block.
                   Most of the time we get away with this sloppyness. (fingers crossed :)
                   The speed increase is worth it.
                 */
                thr[b] = Min(ecb, eb_l[b] * NS_PREECHO_ATT2);
            }
        }
        else {
            FLOAT   ecb_limit_2 = rpelev2 * psv->nb_l2[chn][b];
            FLOAT   ecb_limit_1 = rpelev * psv->nb_l1[chn][b];
            FLOAT   ecb_limit;
            if (ecb_limit_2 <= 0) {
                ecb_limit_2 = ecb;
            }
            if (ecb_limit_1 <= 0) {
                ecb_limit_1 = ecb;
            }
            if (psv->blocktype_old[chn & 0x01] == NORM_TYPE) {
                ecb_limit = Min(ecb_limit_1, ecb_limit_2);
            }
            else {
                ecb_limit = ecb_limit_1;
            }
            thr[b] = Min(ecb, ecb_limit);
        }
        psv->nb_l2[chn][b] = psv->nb_l1[chn][b];
        psv->nb_l1[chn][b] = ecb;
        {
            /*  if THR exceeds EB, the quantization routines will take the difference
             *  from other bands. in case of strong tonal samples (tonaltest.wav)
             *  this leads to heavy distortions. that's why we limit THR here.
             */
            x = max[b];
            x *= gdl->minval[b];
            x *= avg_mask;
            if (thr[b] > x) {
                thr[b] = x;
            }
        }
        if (masking_lower > 1) {
            thr[b] *= masking_lower;
        }
        if (thr[b] > eb_l[b]) {
            thr[b] = eb_l[b];
        }
        if (masking_lower < 1) {
            thr[b] *= masking_lower;
        }
        assert(thr[b] >= 0);
    }
    for (; b < CBANDS; ++b) {
        eb_l[b] = 0;
        thr[b] = 0;
    }
}


static void
vbrpsy_compute_block_type(SessionConfig_t const * cfg, int *uselongblock)
{
    int     chn;

    if (cfg->short_blocks == short_block_coupled
        /* force both channels to use the same block type */
        /* this is necessary if the frame is to be encoded in ms_stereo.  */
        /* But even without ms_stereo, FhG  does this */
        && !(uselongblock[0] && uselongblock[1]))
        uselongblock[0] = uselongblock[1] = 0;

    for (chn = 0; chn < cfg->channels_out; chn++) {
        /* disable short blocks */
        if (cfg->short_blocks == short_block_dispensed) {
            uselongblock[chn] = 1;
        }
        if (cfg->short_blocks == short_block_forced) {
            uselongblock[chn] = 0;
        }
    }
}


static void
vbrpsy_apply_block_type(PsyStateVar_t * psv, int nch, int const *uselongblock, int *blocktype_d)
{
    int     chn;

    /* update the blocktype of the previous granule, since it depends on what
     * happend in this granule */
    for (chn = 0; chn < nch; chn++) {
        int     blocktype = NORM_TYPE;
        /* disable short blocks */

        if (uselongblock[chn]) {
            /* no attack : use long blocks */
            assert(psv->blocktype_old[chn] != START_TYPE);
            if (psv->blocktype_old[chn] == SHORT_TYPE)
                blocktype = STOP_TYPE;
        }
        else {
            /* attack : use short blocks */
            blocktype = SHORT_TYPE;
            if (psv->blocktype_old[chn] == NORM_TYPE) {
                psv->blocktype_old[chn] = START_TYPE;
            }
            if (psv->blocktype_old[chn] == STOP_TYPE)
                psv->blocktype_old[chn] = SHORT_TYPE;
        }

        blocktype_d[chn] = psv->blocktype_old[chn]; /* value returned to calling program */
        psv->blocktype_old[chn] = blocktype; /* save for next call to l3psy_anal */
    }
}


/*************************************************************** 
 * compute M/S thresholds from Johnston & Ferreira 1992 ICASSP paper
 ***************************************************************/

static void
vbrpsy_compute_MS_thresholds(const FLOAT eb[4][CBANDS], FLOAT thr[4][CBANDS], const FLOAT cb_mld[CBANDS],
                             const FLOAT ath_cb[CBANDS], FLOAT athlower, FLOAT msfix, int n)
{
    FLOAT const msfix2 = msfix * 2.f;
    FLOAT   rside, rmid;
    int     b;
    for (b = 0; b < n; ++b) {
        FLOAT const ebM = eb[2][b];
        FLOAT const ebS = eb[3][b];
        FLOAT const thmL = thr[0][b];
        FLOAT const thmR = thr[1][b];
        FLOAT   thmM = thr[2][b];
        FLOAT   thmS = thr[3][b];

        /* use this fix if L & R masking differs by 2db or less */
        /* if db = 10*log10(x2/x1) < 2 */
        /* if (x2 < 1.58*x1) { */
        if (thmL <= 1.58f * thmR && thmR <= 1.58f * thmL) {
            FLOAT const mld_m = cb_mld[b] * ebS;
            FLOAT const mld_s = cb_mld[b] * ebM;
            rmid = Max(thmM, Min(thmS, mld_m));
            rside = Max(thmS, Min(thmM, mld_s));
        }
        else {
            rmid = thmM;
            rside = thmS;
        }
        if (msfix > 0.f) {
            /***************************************************************/
            /* Adjust M/S maskings if user set "msfix"                     */
            /***************************************************************/
            /* Naoki Shibata 2000 */
            FLOAT   thmLR, thmMS;
            FLOAT const ath = ath_cb[b] * athlower;
            thmLR = Min(Max(thmL, ath), Max(thmR, ath));
            thmM = Max(rmid, ath);
            thmS = Max(rside, ath);
            thmMS = thmM + thmS;
            if (thmMS > 0.f && (thmLR * msfix2) < thmMS) {
                FLOAT const f = thmLR * msfix2 / thmMS;
                thmM *= f;
                thmS *= f;
                assert(thmMS > 0.f);
            }
            rmid = Min(thmM, rmid);
            rside = Min(thmS, rside);
        }
        if (rmid > ebM) {
            rmid = ebM;
        }
        if (rside > ebS) {
            rside = ebS;
        }
        thr[2][b] = rmid;
        thr[3][b] = rside;
    }
}



/*
 * NOTE: the bitrate reduction from the inter-channel masking effect is low
 * compared to the chance of getting annyoing artefacts. L3psycho_anal_vbr does
 * not use this feature. (Robert 071216)
*/

int
L3psycho_anal_vbr(lame_internal_flags * gfc,
                  const sample_t *const buffer[2], int gr_out,
                  III_psy_ratio masking_ratio[2][2],
                  III_psy_ratio masking_MS_ratio[2][2],
                  FLOAT percep_entropy[2], FLOAT percep_MS_entropy[2],
                  FLOAT energy[4], int blocktype_d[2])
{
    SessionConfig_t const *const cfg = &gfc->cfg;
    PsyStateVar_t *const psv = &gfc->sv_psy;
    PsyConst_l_t const *const gdl = &gfc->cd_psy->l;
    PsyConst_s_t const *const gds = &gfc->cd_psy->s;
    plotting_data *plt = cfg->analysis ? gfc->pinfo : 0;

    /* fft and energy calculation   */
    FLOAT(*wsamp_l)[BLKSIZE];
    FLOAT(*wsamp_s)[3][BLKSIZE_s];
    FLOAT   fftenergy[HBLKSIZE];
    FLOAT   fftenergy_s[3][HBLKSIZE_s];
    FLOAT   wsamp_L[2][BLKSIZE];
    FLOAT   wsamp_S[2][3][BLKSIZE_s];
    FLOAT   eb[4][CBANDS], thr[4][CBANDS];

    FLOAT   sub_short_factor[4][3];
    FLOAT   thmm;
    FLOAT   pcfact = 0.6f;
    FLOAT const ath_factor = (cfg->msfix > 0.f) ? (cfg->ATH_offset_factor * gfc->ATH->adjust_factor) : 1.f;

    
    const FLOAT (*const_eb)[CBANDS] = (const FLOAT (*)[CBANDS])eb;
    const FLOAT (*const_fftenergy_s)[HBLKSIZE_s] = (const FLOAT (*)[HBLKSIZE_s])fftenergy_s;

    /* block type  */
    int     ns_attacks[4][4] = { {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0} };
    int     uselongblock[2];

    /* usual variables like loop indices, etc..    */
    int     chn, sb, sblock;

    /* chn=2 and 3 = Mid and Side channels */
    int const n_chn_psy = (cfg->mode == JOINT_STEREO) ? 4 : cfg->channels_out;

    vbrpsy_attack_detection(gfc, buffer, gr_out, masking_ratio, masking_MS_ratio, energy,
                            sub_short_factor, ns_attacks, uselongblock);

    vbrpsy_compute_block_type(cfg, uselongblock);

    /* LONG BLOCK CASE */
    {
        for (chn = 0; chn < n_chn_psy; chn++) {
            int const ch01 = chn & 0x01;

            wsamp_l = wsamp_L + ch01;
            vbrpsy_compute_fft_l(gfc, buffer, chn, gr_out, fftenergy, wsamp_l);
            vbrpsy_compute_loudness_approximation_l(gfc, gr_out, chn, fftenergy);
            vbrpsy_compute_masking_l(gfc, fftenergy, eb[chn], thr[chn], chn);
        }
        if ((uselongblock[0] + uselongblock[1]) == 2) {
            /* M/S channel */
            if (cfg->mode == JOINT_STEREO) {
                vbrpsy_compute_MS_thresholds(const_eb, thr, gdl->mld_cb, gfc->ATH->cb_l,
                                             ath_factor, cfg->msfix, gdl->npart);
            }
            /* L/R channel */
#if 0
            if (cfg->mode == STEREO || cfg->mode == JOINT_STEREO) {
            }
#endif
        }
        /* TODO: apply adaptive ATH masking here ?? */
        for (chn = 0; chn < n_chn_psy; chn++) {
            int const ch01 = chn & 0x01;
            if (uselongblock[ch01]) {
                convert_partition2scalefac_l(gfc, eb[chn], thr[chn], chn);
            }
        }
    }

    /* SHORT BLOCKS CASE */
    {
        for (sblock = 0; sblock < 3; sblock++) {
            for (chn = 0; chn < n_chn_psy; ++chn) {
                int const ch01 = chn & 0x01;

                if (uselongblock[ch01]) {
                    vbrpsy_skip_masking_s(gfc, chn, sblock);
                }
                else {
                    /* compute masking thresholds for short blocks */
                    wsamp_s = wsamp_S + ch01;
                    vbrpsy_compute_fft_s(gfc, buffer, chn, sblock, fftenergy_s, wsamp_s);
                    vbrpsy_compute_masking_s(gfc, const_fftenergy_s, eb[chn], thr[chn], chn, sblock);
                }
            }
            if ((uselongblock[0] + uselongblock[1]) == 0) {
                /* M/S channel */
                if (cfg->mode == JOINT_STEREO) {
                    vbrpsy_compute_MS_thresholds(const_eb, thr, gds->mld_cb, gfc->ATH->cb_s,
                                                 ath_factor, cfg->msfix, gds->npart);
                }
                /* L/R channel */
#if 0
                if (cfg->mode == STEREO || cfg->mode == JOINT_STEREO) {
                }
#endif
            }
            /* TODO: apply adaptive ATH masking here ?? */
            for (chn = 0; chn < n_chn_psy; ++chn) {
                int const ch01 = chn & 0x01;
                if (!uselongblock[ch01]) {
                    convert_partition2scalefac_s(gfc, eb[chn], thr[chn], chn, sblock);
                }
            }
        }

        /****   short block pre-echo control   ****/
        for (chn = 0; chn < n_chn_psy; chn++) {
            int const ch01 = chn & 0x01;

            if (uselongblock[ch01]) {
                continue;
            }
            for (sb = 0; sb < SBMAX_s; sb++) {
                FLOAT   new_thmm[3];
                for (sblock = 0; sblock < 3; sblock++) {
                    thmm = psv->thm[chn].s[sb][sblock];
                    thmm *= NS_PREECHO_ATT0;

                    if (ns_attacks[chn][sblock] >= 2 || ns_attacks[chn][sblock + 1] == 1) {
                        int const idx = (sblock != 0) ? sblock - 1 : 2;
                        double const p = NS_INTERP(psv->thm[chn].s[sb][idx],
                                                   thmm, NS_PREECHO_ATT1 * pcfact);
                        thmm = Min(thmm, p);
                    }
                    else if (ns_attacks[chn][sblock] == 1) {
                        int const idx = (sblock != 0) ? sblock - 1 : 2;
                        double const p = NS_INTERP(psv->thm[chn].s[sb][idx],
                                                   thmm, NS_PREECHO_ATT2 * pcfact);
                        thmm = Min(thmm, p);
                    }
                    else if ((sblock != 0 && ns_attacks[chn][sblock - 1] == 3)
                             || (sblock == 0 && psv->last_attacks[chn] == 3)) {
                        int const idx = (sblock != 2) ? sblock + 1 : 0;
                        double const p = NS_INTERP(psv->thm[chn].s[sb][idx],
                                                   thmm, NS_PREECHO_ATT2 * pcfact);
                        thmm = Min(thmm, p);
                    }

                    /* pulse like signal detection for fatboy.wav and so on */
                    thmm *= sub_short_factor[chn][sblock];

                    new_thmm[sblock] = thmm;
                }
                for (sblock = 0; sblock < 3; sblock++) {
                    psv->thm[chn].s[sb][sblock] = new_thmm[sblock];
                }
            }
        }
    }
    for (chn = 0; chn < n_chn_psy; chn++) {
        psv->last_attacks[chn] = ns_attacks[chn][2];
    }


    /*************************************************************** 
    * determine final block type
    ***************************************************************/
    vbrpsy_apply_block_type(psv, cfg->channels_out, uselongblock, blocktype_d);

    /*********************************************************************
    * compute the value of PE to return ... no delay and advance
    *********************************************************************/
    for (chn = 0; chn < n_chn_psy; chn++) {
        FLOAT  *ppe;
        int     type;
        III_psy_ratio const *mr;

        if (chn > 1) {
            ppe = percep_MS_entropy - 2;
            type = NORM_TYPE;
            if (blocktype_d[0] == SHORT_TYPE || blocktype_d[1] == SHORT_TYPE)
                type = SHORT_TYPE;
            mr = &masking_MS_ratio[gr_out][chn - 2];
        }
        else {
            ppe = percep_entropy;
            type = blocktype_d[chn];
            mr = &masking_ratio[gr_out][chn];
        }
        if (type == SHORT_TYPE) {
            ppe[chn] = pecalc_s(mr, gfc->sv_qnt.masking_lower);
        }
        else {
            ppe[chn] = pecalc_l(mr, gfc->sv_qnt.masking_lower);
        }

        if (plt) {
            plt->pe[gr_out][chn] = ppe[chn];
        }
    }
    return 0;
}





static  FLOAT
s3_func_x(FLOAT bark, FLOAT hf_slope)
{
    FLOAT   tempx = bark, tempy;

    if (tempx >= 0) {
        tempy = -tempx * 27;
    }
    else {
        tempy = tempx * hf_slope;
    }
    if (tempy <= -72.0) {
        return 0;
    }
    return exp(tempy * LN_TO_LOG10);
}

static  FLOAT
norm_s3_func_x(FLOAT hf_slope)
{
    double  lim_a = 0, lim_b = 0;
    {
        double  x = 0, l, h;
        for (x = 0; s3_func_x(x, hf_slope) > 1e-20; x -= 1);
        l = x;
        h = 0;
        while (fabs(h - l) > 1e-12) {
            x = (h + l) / 2;
            if (s3_func_x(x, hf_slope) > 0) {
                h = x;
            }
            else {
                l = x;
            }
        }
        lim_a = l;
    }
    {
        double  x = 0, l, h;
        for (x = 0; s3_func_x(x, hf_slope) > 1e-20; x += 1);
        l = 0;
        h = x;
        while (fabs(h - l) > 1e-12) {
            x = (h + l) / 2;
            if (s3_func_x(x, hf_slope) > 0) {
                l = x;
            }
            else {
                h = x;
            }
        }
        lim_b = h;
    }
    {
        double  sum = 0;
        int const m = 1000;
        int     i;
        for (i = 0; i <= m; ++i) {
            double  x = lim_a + i * (lim_b - lim_a) / m;
            double  y = s3_func_x(x, hf_slope);
            sum += y;
        }
        {
            double  norm = (m + 1) / (sum * (lim_b - lim_a));
            /*printf( "norm = %lf\n",norm); */
            return norm;
        }
    }
}



/* 
 *   The spreading function.  Values returned in units of energy
 */
static  FLOAT
s3_func(FLOAT bark)
{
    FLOAT   tempx, x, tempy, temp;
    tempx = bark;
    if (tempx >= 0)
        tempx *= 3;
    else
        tempx *= 1.5;

    if (tempx >= 0.5 && tempx <= 2.5) {
        temp = tempx - 0.5;
        x = 8.0 * (temp * temp - 2.0 * temp);
    }
    else
        x = 0.0;
    tempx += 0.474;
    tempy = 15.811389 + 7.5 * tempx - 17.5 * sqrt(1.0 + tempx * tempx);

    if (tempy <= -60.0)
        return 0.0;

    tempx = exp((x + tempy) * LN_TO_LOG10);

    /* Normalization.  The spreading function should be normalized so that:
       +inf
       /
       |  s3 [ bark ]  d(bark)   =  1
       /
       -inf
     */
    tempx /= .6609193;
    return tempx;
}

#if 0
static  FLOAT
norm_s3_func(void)
{
    double  lim_a = 0, lim_b = 0;
    double  x = 0, l, h;
    for (x = 0; s3_func(x) > 1e-20; x -= 1);
    l = x;
    h = 0;
    while (fabs(h - l) > 1e-12) {
        x = (h + l) / 2;
        if (s3_func(x) > 0) {
            h = x;
        }
        else {
            l = x;
        }
    }
    lim_a = l;
    for (x = 0; s3_func(x) > 1e-20; x += 1);
    l = 0;
    h = x;
    while (fabs(h - l) > 1e-12) {
        x = (h + l) / 2;
        if (s3_func(x) > 0) {
            l = x;
        }
        else {
            h = x;
        }
    }
    lim_b = h;
    {
        double  sum = 0;
        int const m = 1000;
        int     i;
        for (i = 0; i <= m; ++i) {
            double  x = lim_a + i * (lim_b - lim_a) / m;
            double  y = s3_func(x);
            sum += y;
        }
        {
            double  norm = (m + 1) / (sum * (lim_b - lim_a));
            /*printf( "norm = %lf\n",norm); */
            return norm;
        }
    }
}
#endif

static int
init_numline(int *numlines, int *bo, int *bm,
             FLOAT * bval, FLOAT * bval_width, FLOAT * mld, FLOAT * bo_w,
             FLOAT sfreq, int blksize, int const *scalepos, FLOAT deltafreq, int sbmax)
{
    FLOAT   b_frq[CBANDS + 1];
    FLOAT   f_tmp;
    FLOAT   sample_freq_frac = sfreq / (sbmax > 15 ? 2 * 576 : 2 * 192);
    int     partition[HBLKSIZE] = { 0 };
    int     i, j, k, ni;
    int     sfb;
    sfreq /= blksize;
    j = 0;
    ni = 0;
    /* compute numlines, the number of spectral lines in each partition band */
    /* each partition band should be about DELBARK wide. */
    for (i = 0; i < CBANDS; i++) {
        FLOAT   bark1;
        int     j2;
        bark1 = freq2bark(sfreq * j);

        b_frq[i] = sfreq * j;

        for (j2 = j; freq2bark(sfreq * j2) - bark1 < DELBARK && j2 <= blksize / 2; j2++);

        numlines[i] = j2 - j;
        ni = i + 1;

        while (j < j2) {
            assert(j < HBLKSIZE);
            partition[j++] = i;
        }
        if (j > blksize / 2) {
            j = blksize / 2;
            ++i;
            break;
        }
    }
    assert(i < CBANDS);
    b_frq[i] = sfreq * j;

    for (sfb = 0; sfb < sbmax; sfb++) {
        int     i1, i2, start, end;
        FLOAT   arg;
        start = scalepos[sfb];
        end = scalepos[sfb + 1];

        i1 = floor(.5 + deltafreq * (start - .5));
        if (i1 < 0)
            i1 = 0;
        i2 = floor(.5 + deltafreq * (end - .5));

        if (i2 > blksize / 2)
            i2 = blksize / 2;

        bm[sfb] = (partition[i1] + partition[i2]) / 2;
        bo[sfb] = partition[i2];

        f_tmp = sample_freq_frac * end;
        /* calculate how much of this band belongs to current scalefactor band */
        bo_w[sfb] = (f_tmp - b_frq[bo[sfb]]) / (b_frq[bo[sfb] + 1] - b_frq[bo[sfb]]);
        if (bo_w[sfb] < 0) {
            bo_w[sfb] = 0;
        }
        else {
            if (bo_w[sfb] > 1) {
                bo_w[sfb] = 1;
            }
        }
        /* setup stereo demasking thresholds */
        /* formula reverse enginerred from plot in paper */
        arg = freq2bark(sfreq * scalepos[sfb] * deltafreq);
        arg = (Min(arg, 15.5) / 15.5);

        mld[sfb] = pow(10.0, 1.25 * (1 - cos(PI * arg)) - 2.5);
    }

    /* compute bark values of each critical band */
    j = 0;
    for (k = 0; k < ni; k++) {
        int const w = numlines[k];
        FLOAT   bark1, bark2;

        bark1 = freq2bark(sfreq * (j));
        bark2 = freq2bark(sfreq * (j + w - 1));
        bval[k] = .5 * (bark1 + bark2);

        bark1 = freq2bark(sfreq * (j - .5));
        bark2 = freq2bark(sfreq * (j + w - .5));
        bval_width[k] = bark2 - bark1;
        j += w;
    }

    return ni;
}

static int
init_s3_values(FLOAT ** p,
               int (*s3ind)[2], int npart, FLOAT const *bval, FLOAT const *bval_width,
               FLOAT const *norm, int use_old_s3)
{
    FLOAT   s3[CBANDS][CBANDS];
    /* The s3 array is not linear in the bark scale.
     * bval[x] should be used to get the bark value.
     */
    int     i, j, k;
    int     numberOfNoneZero = 0;

    memset(&s3[0][0], 0, sizeof(s3));

    /* s[i][j], the value of the spreading function,
     * centered at band j (masker), for band i (maskee)
     *
     * i.e.: sum over j to spread into signal barkval=i
     * NOTE: i and j are used opposite as in the ISO docs
     */
    if (use_old_s3) {
        for (i = 0; i < npart; i++) {
            for (j = 0; j < npart; j++) {
                FLOAT   v = s3_func(bval[i] - bval[j]) * bval_width[j];
                s3[i][j] = v * norm[i];
            }
        }
    }
    else {
        for (j = 0; j < npart; j++) {
            FLOAT   hf_slope = 15 + Min(21 / bval[j], 12);
            FLOAT   s3_x_norm = norm_s3_func_x(hf_slope);
            for (i = 0; i < npart; i++) {
                FLOAT   v = s3_x_norm * s3_func_x(bval[i] - bval[j], hf_slope) * bval_width[j];
                s3[i][j] = v * norm[i];
            }
        }
    }
    for (i = 0; i < npart; i++) {
        for (j = 0; j < npart; j++) {
            if (s3[i][j] > 0.0f)
                break;
        }
        s3ind[i][0] = j;

        for (j = npart - 1; j > 0; j--) {
            if (s3[i][j] > 0.0f)
                break;
        }
        s3ind[i][1] = j;
        numberOfNoneZero += (s3ind[i][1] - s3ind[i][0] + 1);
    }
    *p = malloc(sizeof(FLOAT) * numberOfNoneZero);
    if (!*p)
        return -1;

    k = 0;
    for (i = 0; i < npart; i++)
        for (j = s3ind[i][0]; j <= s3ind[i][1]; j++)
            (*p)[k++] = s3[i][j];

    return 0;
}

static  FLOAT
stereo_demask(double f)
{
    /* setup stereo demasking thresholds */
    /* formula reverse enginerred from plot in paper */
    double  arg = freq2bark(f);
    arg = (Min(arg, 15.5) / 15.5);

    return pow(10.0, 1.25 * (1 - cos(PI * arg)) - 2.5);
}

int
psymodel_init(lame_global_flags const* gfp)
{
    lame_internal_flags *const gfc = gfp->internal_flags;
    SessionConfig_t *const cfg = &gfc->cfg;
    PsyStateVar_t *const psv = &gfc->sv_psy;
    PsyConst_t *gd;
    int     i, j, b, sb, k;
    int     use_old_s3 = 1;
    FLOAT   bvl_a = 13, bvl_b = 24;
    FLOAT   snr_l_a = 0, snr_l_b = 0;
    FLOAT   snr_s_a = -8.25, snr_s_b = -4.5;

    FLOAT   bval[CBANDS];
    FLOAT   bval_width[CBANDS];
    FLOAT   norm[CBANDS];
    FLOAT const sfreq = cfg->samplerate_out;
    
    FLOAT   xav = 10, xbv = 12;
    FLOAT const minval_low = (0.f - cfg->minval);

    if (gfc->cd_psy != 0) {
        return 0;
    }
    memset(norm, 0, sizeof(norm));

    gd = calloc(1, sizeof(PsyConst_t));
    gfc->cd_psy = gd;

    switch (gfp->experimentalZ) {
    default:
    case 0:
        use_old_s3 = 1;
        break;
    case 1:
        use_old_s3 = (cfg->vbr == vbr_mtrh || cfg->vbr == vbr_mt) ? 0 : 1;
        break;
    case 2:
        use_old_s3 = 0;
        break;
    case 3:
        bvl_a = 8;
        snr_l_a = -1.75;
        snr_l_b = -0.0125;
        snr_s_a = -8.25;
        snr_s_b = -2.25;
        break;
    }
    psv->blocktype_old[0] = psv->blocktype_old[1] = NORM_TYPE; /* the vbr header is long blocks */

    for (i = 0; i < 4; ++i) {
        for (j = 0; j < CBANDS; ++j) {
            psv->nb_l1[i][j] = 1e20;
            psv->nb_l2[i][j] = 1e20;
            psv->nb_s1[i][j] = psv->nb_s2[i][j] = 1.0;
        }
        for (sb = 0; sb < SBMAX_l; sb++) {
            psv->en[i].l[sb] = 1e20;
            psv->thm[i].l[sb] = 1e20;
        }
        for (j = 0; j < 3; ++j) {
            for (sb = 0; sb < SBMAX_s; sb++) {
                psv->en[i].s[sb][j] = 1e20;
                psv->thm[i].s[sb][j] = 1e20;
            }
            psv->last_attacks[i] = 0;
        }
        for (j = 0; j < 9; j++)
            psv->last_en_subshort[i][j] = 10.;
    }


    /* init. for loudness approx. -jd 2001 mar 27 */
    psv->loudness_sq_save[0] = psv->loudness_sq_save[1] = 0.0;



    /*************************************************************************
     * now compute the psychoacoustic model specific constants
     ************************************************************************/
    /* compute numlines, bo, bm, bval, bval_width, mld */
    gd->l.npart
        = init_numline(gd->l.numlines, gd->l.bo, gd->l.bm,
                       bval, bval_width, gd->l.mld, gd->l.bo_weight,
                       sfreq, BLKSIZE, gfc->scalefac_band.l, BLKSIZE / (2.0 * 576), SBMAX_l);
    assert(gd->l.npart < CBANDS);
    /* compute the spreading function */
    for (i = 0; i < gd->l.npart; i++) {
        double  snr = snr_l_a;
        if (bval[i] >= bvl_a) {
            snr = snr_l_b * (bval[i] - bvl_a) / (bvl_b - bvl_a)
                + snr_l_a * (bvl_b - bval[i]) / (bvl_b - bvl_a);
        }
        norm[i] = pow(10.0, snr / 10.0);
        if (gd->l.numlines[i] > 0) {
            gd->l.rnumlines[i] = 1.0 / gd->l.numlines[i];
        }
        else {
            gd->l.rnumlines[i] = 0;
        }
    }
    i = init_s3_values(&gd->l.s3, gd->l.s3ind, gd->l.npart, bval, bval_width, norm, use_old_s3);
    if (i)
        return i;
    
    /* compute long block specific values, ATH and MINVAL */
    j = 0;
    for (i = 0; i < gd->l.npart; i++) {
        double  x;

        /* ATH */
        x = FLOAT_MAX;
        for (k = 0; k < gd->l.numlines[i]; k++, j++) {
            FLOAT const freq = sfreq * j / (1000.0 * BLKSIZE);
            FLOAT   level;
            /* freq = Min(.1,freq); */ /* ATH below 100 Hz constant, not further climbing */
            level = ATHformula(cfg, freq * 1000) - 20; /* scale to FFT units; returned value is in dB */
            level = pow(10., 0.1 * level); /* convert from dB -> energy */
            level *= gd->l.numlines[i];
            if (x > level)
                x = level;
        }
        gfc->ATH->cb_l[i] = x;

        /* MINVAL.
           For low freq, the strength of the masking is limited by minval
           this is an ISO MPEG1 thing, dont know if it is really needed */
        /* FIXME: it does work to reduce low-freq problems in S53-Wind-Sax
           and lead-voice samples, but introduces some 3 kbps bit bloat too.
           TODO: Further refinement of the shape of this hack.
         */
        x = 20.0 * (bval[i] / xav - 1.0);
        if (x > 6) {
            x = 30;
        }
        if (x < minval_low) {
            x = minval_low;
        }
        if (cfg->samplerate_out < 44000) {
            x = 30;
        }
        x -= 8.;
        gd->l.minval[i] = pow(10.0, x / 10.) * gd->l.numlines[i];
    }
    
    /************************************************************************
     * do the same things for short blocks
     ************************************************************************/
    gd->s.npart
        = init_numline(gd->s.numlines, gd->s.bo, gd->s.bm,
                       bval, bval_width, gd->s.mld, gd->s.bo_weight,
                       sfreq, BLKSIZE_s, gfc->scalefac_band.s, BLKSIZE_s / (2.0 * 192), SBMAX_s);
    assert(gd->s.npart < CBANDS);

    /* SNR formula. short block is normalized by SNR. is it still right ? */
    j = 0;
    for (i = 0; i < gd->s.npart; i++) {
        double  x;
        double  snr = snr_s_a;
        if (bval[i] >= bvl_a) {
            snr = snr_s_b * (bval[i] - bvl_a) / (bvl_b - bvl_a)
                + snr_s_a * (bvl_b - bval[i]) / (bvl_b - bvl_a);
        }
        norm[i] = pow(10.0, snr / 10.0);
        if (gd->s.numlines[i] > 0) {
            gd->s.rnumlines[i] = 1.0 / gd->s.numlines[i];
        }
        else {
            gd->s.rnumlines[i] = 0;
        }

        /* ATH */
        x = FLOAT_MAX;
        for (k = 0; k < gd->s.numlines[i]; k++, j++) {
            FLOAT const freq = sfreq * j / (1000.0 * BLKSIZE_s);
            FLOAT   level;
            /* freq = Min(.1,freq); */ /* ATH below 100 Hz constant, not further climbing */
            level = ATHformula(cfg, freq * 1000) - 20; /* scale to FFT units; returned value is in dB */
            level = pow(10., 0.1 * level); /* convert from dB -> energy */
            level *= gd->s.numlines[i];
            if (x > level)
                x = level;
        }
        gfc->ATH->cb_s[i] = x;

        /* MINVAL.
           For low freq, the strength of the masking is limited by minval
           this is an ISO MPEG1 thing, dont know if it is really needed */
        x = 7.0 * (bval[i] / xbv - 1.0);
        if (bval[i] > xbv) {
            x *= 1 + log(1 + x) * 3.1;
        }
        if (bval[i] < xbv) {
            x *= 1 + log(1 - x) * 2.3;
        }
        if (x > 6) {
            x = 30;
        }
        if (x < minval_low) {
            x = minval_low;
        }
        if (cfg->samplerate_out < 44000) {
            x = 30;
        }
        x -= 8;
        gd->s.minval[i] = pow(10.0, x / 10) * gd->s.numlines[i];
    }
    
    i = init_s3_values(&gd->s.s3, gd->s.s3ind, gd->s.npart, bval, bval_width, norm, use_old_s3);
    if (i)
        return i;


    init_mask_add_max_values();
    init_fft(gfc);

    /* setup temporal masking */
    gd->decay = exp(-1.0 * LOG10 / (temporalmask_sustain_sec * sfreq / 192.0));

    {
        FLOAT   msfix;
        msfix = NS_MSFIX;
        if (cfg->use_safe_joint_stereo)
            msfix = 1.0;
        if (fabs(cfg->msfix) > 0.0)
            msfix = cfg->msfix;
        cfg->msfix = msfix;

        /* spread only from npart_l bands.  Normally, we use the spreading
         * function to convolve from npart_l down to npart_l bands 
         */
        for (b = 0; b < gd->l.npart; b++)
            if (gd->l.s3ind[b][1] > gd->l.npart - 1)
                gd->l.s3ind[b][1] = gd->l.npart - 1;
    }

    /*  prepare for ATH auto adjustment:
     *  we want to decrease the ATH by 12 dB per second
     */
#define  frame_duration (576. * cfg->mode_gr / sfreq)
    gfc->ATH->decay = pow(10., -12. / 10. * frame_duration);
    gfc->ATH->adjust_factor = 0.01; /* minimum, for leading low loudness */
    gfc->ATH->adjust_limit = 1.0; /* on lead, allow adjust up to maximum */
#undef  frame_duration

    assert(gd->l.bo[SBMAX_l - 1] <= gd->l.npart);
    assert(gd->s.bo[SBMAX_s - 1] <= gd->s.npart);

    if (cfg->ATHtype != -1) {
        /* compute equal loudness weights (eql_w) */
        FLOAT   freq;
        FLOAT const freq_inc = (FLOAT) cfg->samplerate_out / (FLOAT) (BLKSIZE);
        FLOAT   eql_balance = 0.0;
        freq = 0.0;
        for (i = 0; i < BLKSIZE / 2; ++i) {
            /* convert ATH dB to relative power (not dB) */
            /*  to determine eql_w */
            freq += freq_inc;
            gfc->ATH->eql_w[i] = 1. / pow(10, ATHformula(cfg, freq) / 10);
            eql_balance += gfc->ATH->eql_w[i];
        }
        eql_balance = 1.0 / eql_balance;
        for (i = BLKSIZE / 2; --i >= 0;) { /* scale weights */
            gfc->ATH->eql_w[i] *= eql_balance;
        }
    }
    {
        for (b = j = 0; b < gd->s.npart; ++b) {
            for (i = 0; i < gd->s.numlines[b]; ++i) {
                ++j;
            }
        }
        assert(j == 129);
        for (b = j = 0; b < gd->l.npart; ++b) {
            for (i = 0; i < gd->l.numlines[b]; ++i) {
                ++j;
            }
        }
        assert(j == 513);
    }
    j = 0;
    for (i = 0; i < gd->l.npart; i++) {
        FLOAT const freq = sfreq * (j + gd->l.numlines[i] / 2) / (1.0 * BLKSIZE);
        gd->l.mld_cb[i] = stereo_demask(freq);
        j += gd->l.numlines[i];
    }
    for (; i < CBANDS; ++i) {
        gd->l.mld_cb[i] = 1;
    }
    j = 0;
    for (i = 0; i < gd->s.npart; i++) {
        FLOAT const freq = sfreq * (j + gd->s.numlines[i] / 2) / (1.0 * BLKSIZE_s);
        gd->s.mld_cb[i] = stereo_demask(freq);
        j += gd->s.numlines[i];
    }
    for (; i < CBANDS; ++i) {
        gd->s.mld_cb[i] = 1;
    }

    /* short block attack threshold */
    {
        float   x = gfp->attackthre;
        float   y = gfp->attackthre_s;
        if (x < 0) {
            x = NSATTACKTHRE;
        }
        if (y < 0) {
            y = NSATTACKTHRE_S;
        }
        gd->attack_threshold[0] = gd->attack_threshold[1] = gd->attack_threshold[2] = x;
        gd->attack_threshold[3] = y;
    }
    {
        float sk_s = -10.f, sk_l = -4.7f;
        static float const sk[] = { -7.4, -7.4, -7.4, -9.5, -7.4, -6.1, -5.5, -4.7, -4.7, -4.7, -4.7 };
        if (gfp->VBR_q < 4) {
            sk_l = sk_s = sk[0];
        }
        else {
            sk_l = sk_s = sk[gfp->VBR_q] + gfp->VBR_q_frac * (sk[gfp->VBR_q]-sk[gfp->VBR_q+1]);
        }
        b = 0;
        for (; b < gd->s.npart; b++) {
            float m = (float)(gd->s.npart - b) / gd->s.npart;
            gd->s.masking_lower[b] = powf(10.f, sk_s * m * 0.1f);
        }
        for (; b < CBANDS; ++b) {
            gd->s.masking_lower[b] = 1.f;
        }    
        b = 0;
        for (; b < gd->l.npart; b++) {
            float m = (float)(gd->l.npart - b) / gd->l.npart;
            gd->l.masking_lower[b] = powf(10.f, sk_l * m * 0.1f);
        }
        for (; b < CBANDS; ++b) {
            gd->l.masking_lower[b] = 1.f;
        }    
    }
    return 0;
}
