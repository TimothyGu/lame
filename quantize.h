/**********************************************************************
 * ISO MPEG Audio Subgroup Software Simulation Group (1996)
 * ISO 13818-3 MPEG-2 Audio Encoder - Lower Sampling Frequency Extension
 *
 * $Id: quantize.h,v 1.3 2000/02/01 14:09:14 takehiro Exp $
 *
 * $Log: quantize.h,v $
 * Revision 1.3  2000/02/01 14:09:14  takehiro
 * code clean up. changed definition of structure to optimize array index calculation
 *
 * Revision 1.2  2000/02/01 11:26:32  takehiro
 * scalefactor's structure changed
 *
 * Revision 1.1.1.1  1999/11/24 08:43:45  markt
 * initial checkin of LAME
 * Starting with LAME 3.57beta with some modifications
 *
 * Revision 1.1  1996/02/14 04:04:23  rowlands
 * Initial revision
 *
 * Received from Mike Coleman
 **********************************************************************/

#ifndef LOOP_DOT_H
#define LOOP_DOT_H
#include "util.h"
#include "l3side.h"

/**********************************************************************
 *   date   programmers                comment                        *
 * 25. 6.92  Toshiyuki Ishino          Ver 1.0                        *
 * 29.10.92  Masahiro Iwadare          Ver 2.0                        *
 * 17. 4.93  Masahiro Iwadare          Updated for IS Modification    *
 *                                                                    *
 *********************************************************************/

extern int cont_flag;


extern int pretab[];

void iteration_loop( FLOAT8 pe[2][2], FLOAT8 ms_ratio[2], 
		     FLOAT8 xr_org[2][2][576], III_psy_ratio ratio[2][2],
		     III_side_info_t *l3_side, int l3_enc[2][2][576], 
		     III_scalefac_t scalefac[2][2], frame_params *fr_ps);

void VBR_iteration_loop( FLOAT8 pe[2][2], FLOAT8 ms_ratio[2], 
		     FLOAT8 xr_org[2][2][576], III_psy_ratio ratio[2][2],
		     III_side_info_t *l3_side, int l3_enc[2][2][576], 
		     III_scalefac_t scalefac[2][2], frame_params *fr_ps);




#define maximum(A,B) ( (A) > (B) ? (A) : (B) )
#define minimum(A,B) ( (A) < (B) ? (A) : (B) )
#define signum( A ) ( (A) > 0 ? 1 : -1 )


extern int bit_buffer[50000];

#endif
