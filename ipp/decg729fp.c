/*/////////////////////////////////////////////////////////////////////////////
//
//                  INTEL CORPORATION PROPRIETARY INFORMATION
//     This software is supplied under the terms of a license agreement or
//     nondisclosure agreement with Intel Corporation and may not be copied
//     or disclosed except in accordance with the terms of that agreement.
//          Copyright(c) 2004-2010 Intel Corporation. All Rights Reserved.
//
//     Intel(R) Integrated Performance Primitives
//     USC - Unified Speech Codec interface library
//
// By downloading and installing USC codec, you hereby agree that the
// accompanying Materials are being provided to you under the terms and
// conditions of the End User License Agreement for the Intel(R) Integrated
// Performance Primitives product previously accepted by you. Please refer
// to the file ippEULA.rtf or ippEULA.txt located in the root directory of your Intel(R) IPP
// product installation for more information.
//
// A speech coding standards promoted by ITU, ETSI, 3GPP and other
// organizations. Implementations of these standards, or the standard enabled
// platforms may require licenses from various entities, including
// Intel Corporation.
//
//
// Purpose: G.729 floating-point speech codec: decode API functions.
//
*/
#include <math.h>
#include "aux_fnxs.h"
#include "owng729fp.h"

static void post_filter_I(G729FPDecoder_Obj* decoderObj, Ipp32f *pSynth, Ipp32f *pLPC, Ipp32s pitchDelay, Ipp32s dominant,
                          Ipp32s Vad, Ipp32s pstLPCOrder, Ipp32f *dst,Ipp32s rate);
static void Post_G729A(G729FPDecoder_Obj *decoderObj, Ipp32f *pSrcDstSynthSpeech, Ipp32f *pSrcLPC,
                        Ipp32s *pSrcDecodedPitch, Ipp32s Vad);

/* filter coefficients (fc = 100 Hz ) */

static __ALIGN32 CONST Ipp32f b100[3] = {0.93980581E+00f, -0.18795834E+01f,  0.93980581E+00f};
static __ALIGN32 CONST Ipp32f a100[3] = {1.00000000E+00f,  0.19330735E+01f, -0.93589199E+00f};
/* tables of positions for each track */
static __ALIGN32 CONST Ipp16s trackTbl0[32] = {
 1, 3, 8, 6, 18, 16, 11, 13, 38, 36, 31, 33, 21, 23, 28, 26, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};

static __ALIGN32 CONST Ipp16s trackTbl1[32] = {
0, 2, 5, 4, 12, 10, 7, 9, 25, 24, 20, 22, 14, 15, 19, 17,
36, 31, 21, 26, 1, 6, 16, 11, 27, 29, 32, 30, 39, 37, 34, 35};

static Ipp32s ownDecoderSize(G729Codec_Type codecType)
{
   Ipp32s codecSize, fltsize;

   codecSize = sizeof(G729FPDecoder_Obj);
   ippsIIRGetStateSize_32f(2,&fltsize);
   codecSize += fltsize;
   PHDGetSize(&fltsize);
   codecSize += fltsize;
   ippsWinHybridGetStateSize_G729E_32f(&fltsize);
   codecSize += fltsize;
   if(codecType!=G729A_CODEC) {
      PSTGetSize(&fltsize);
      codecSize += fltsize;
   }

   return codecSize;
}

G729_CODECFUN( APIG729_Status, apiG729FPDecoder_Alloc,
         (G729Codec_Type codecType, Ipp32s *pCodecSize))
{
   if ((codecType != G729_CODEC)&&(codecType != G729A_CODEC)
      &&(codecType != G729D_CODEC)&&(codecType != G729E_CODEC)&&(codecType != G729I_CODEC)){
      return APIG729_StsBadCodecType;
   }

   *pCodecSize = ownDecoderSize(codecType);

   return APIG729_StsNoErr;

}



G729_CODECFUN( APIG729_Status, apiG729FPDecoder_Init,
         (G729FPDecoder_Obj* decoderObj, G729Codec_Type codecType))
{
   Ipp32s i,fltsize;
   void* pBuf;
   Ipp32f coeff[6];

   Ipp8s* oldMemBuff;

   if ((codecType != G729_CODEC)&&(codecType != G729A_CODEC)
      &&(codecType != G729D_CODEC)&&(codecType != G729E_CODEC)&&(codecType != G729I_CODEC)){
      return APIG729_StsBadCodecType;
   }

   oldMemBuff = decoderObj->Mem.base; /* if Reinit */

   ippsZero_16s((Ipp16s*)decoderObj,sizeof(G729FPDecoder_Obj)>>1) ;

   decoderObj->objPrm.objSize = ownDecoderSize(codecType);
   decoderObj->objPrm.key = DEC_KEY;
   decoderObj->objPrm.codecType = codecType;

   coeff[0] = b100[0];
   coeff[1] = b100[1];
   coeff[2] = b100[2];
   coeff[3] = a100[0];
   coeff[4] = -a100[1];
   coeff[5] = -a100[2];
   pBuf = (Ipp8s*)decoderObj + sizeof(G729FPDecoder_Obj);
   ippsIIRInit_32f(&decoderObj->iirstate,coeff,2,NULL,pBuf);

   ippsIIRGetStateSize_32f(2,&fltsize);
   decoderObj->phdMem = (Ipp8s *)((Ipp8s*)pBuf + fltsize);
   PHDGetSize(&fltsize);
   decoderObj->pHWState = (IppsWinHybridState_G729E_32f *)((Ipp8s*)decoderObj->phdMem + fltsize);

   ippsZero_32f(decoderObj->OldExcitationBuffer, PITCH_LAG_MAX+INTERPOL_LEN);
   decoderObj->fBetaPreFilter        = PITCH_SHARPMIN;
   decoderObj->prevPitchDelay      = 60;
   decoderObj->fCodeGain    = 0.;
   decoderObj->fPitchGain   = 0.;
   ippsCopy_32f(InitLSP, decoderObj->OldLSP, LPC_ORDER);

   decoderObj->PastQuantEnergy[0]=decoderObj->PastQuantEnergy[1]=decoderObj->PastQuantEnergy[2]=decoderObj->PastQuantEnergy[3]=-14.0;
   for(i=0; i<MOVING_AVER_ORDER; i++)
     ippsCopy_32f (InitFrequences, &decoderObj->PrevFreq[i][0], LPC_ORDER );
   decoderObj->prevMA = 0;
   ippsCopy_32f (InitFrequences, decoderObj->prevLSF, LPC_ORDER );
   /* for G.729B */
   decoderObj->sFESeed = 21845;
   /* CNG variables */
   decoderObj->prevFrameType = 3;
   decoderObj->sCNGSeed = INIT_SEED_VAL;
   decoderObj-> SID = 0.f;
   decoderObj->fCurrGain = 0.f;
   ownCOS_G729_32f((Ipp32f*)InitFrequences, decoderObj->SIDLSP, LPC_ORDER);
   decoderObj->fSIDGain = SIDGainTbl[0];
   ippsZero_32f(decoderObj->SynFltMemory, BWD_LPC_ORDER);
   PHDInit(decoderObj->phdMem);
   if(codecType==G729A_CODEC) {
      ippsZero_32f(decoderObj->PstFltMemoryA, LPC_ORDER);
      decoderObj->fPastGain = 1.0;
      ippsZero_32f(decoderObj->ResidualBufferA, PITCH_LAG_MAX+SUBFR_LEN);
      decoderObj->ResidualMemory  = decoderObj->ResidualBufferA + PITCH_LAG_MAX;
      ippsZero_32f(decoderObj->PstSynMemoryA, LPC_ORDER);
      decoderObj->fPreemphMemoryA = 0.f;
   } else {
      //PHDGetSize(&fltsize);
      ippsWinHybridGetStateSize_G729E_32f(&fltsize);
      decoderObj->pstMem = (Ipp8s *)((Ipp8s*)decoderObj->pHWState + fltsize);

      ippsZero_32f(decoderObj->SynthBuffer, BWD_ANALISIS_WND_LEN);
      decoderObj->prevFracPitchDelay = 0;
      ippsWinHybridInit_G729E_32f(decoderObj->pHWState);
      ippsZero_32f(decoderObj->BackwardUnqLPC, BWD_LPC_ORDERP1);
      decoderObj->BackwardUnqLPC[0]   = 1.;
      ippsZero_32f(decoderObj->BackwardLPCMemory, BWD_LPC_ORDERP1);
      decoderObj->BackwardLPCMemory[0] = 1.;
      decoderObj->lPrevVoicing = 0;
      decoderObj->lPrevBFI     = 0;
      decoderObj->prevLPCMode    = 0;
      decoderObj->fFEInterpolationCoeff     = 0.;
      decoderObj->fInterpolationCoeff        = 1.1f;       /* Filter interpolation parameter */
      ippsZero_32f(decoderObj->PrevFlt, BWD_LPC_ORDERP1);
      decoderObj->PrevFlt[0] = 1.;
      decoderObj->lPrevPitchPT     = 30;
      decoderObj->lStatPitchPT     = 0;
      decoderObj->lStatPitch2PT     = 0;
      decoderObj->lStatFracPT     = 0;
      ippsZero_32f(decoderObj->OldBackwardLPC, BWD_LPC_ORDERP1);
      decoderObj->OldBackwardLPC[0]   = 1.;
      decoderObj->OldBackwardRC[0] = decoderObj->OldBackwardRC[1] = 0.f;
      decoderObj->fPitchGainMemory   = 0.;
      decoderObj->fCodeGainMemory   = 0.;
      decoderObj->fGainMuting       = 1.;
      decoderObj->lBFICounter      = 0;
      decoderObj->sBWDStatInd       = 0;
      decoderObj->lVoicing = 60;
      decoderObj->g1PST = GAMMA1_POSTFLT_E;
      decoderObj->g2PST = GAMMA2_POSTFLT_E;
      decoderObj->gHarmPST  = GAMMA_HARM_POSTFLT_E;
      decoderObj->sBWDFrmCounter = 0;
      decoderObj->sFWDFrmCounter = 0;
      PSTInit(decoderObj->pstMem);
   }

   apiG729FPDecoder_InitBuff(decoderObj,oldMemBuff);

   return APIG729_StsNoErr;
}

G729_CODECFUN( APIG729_Status, apiG729FPDecoder_InitBuff,
         (G729FPDecoder_Obj* decoderObj, Ipp8s *buff))
{
    if(buff==NULL) return APIG729_StsBadArgErr;
    if(decoderObj==NULL) return APIG729_StsBadArgErr;

   if(NULL==decoderObj || NULL==buff)
      return APIG729_StsBadArgErr;

   decoderObj->Mem.base = buff;
   decoderObj->Mem.CurPtr = decoderObj->Mem.base;
   decoderObj->Mem.VecPtr = (Ipp32s *)(decoderObj->Mem.base+G729FP_ENCODER_SCRATCH_MEMORY_SIZE);

   return APIG729_StsNoErr;
}

G729_CODECFUN( APIG729_Status, apiG729FPDecode,
         (G729FPDecoder_Obj* decoderObj,const Ipp8u* src, Ipp32s frametype, Ipp16s* dst))
{
   LOCAL_ALIGN_ARRAY(32, Ipp32f, backwardLPC, 2*BWD_LPC_ORDERP1, decoderObj);   /* LPC Backward filter */
   LOCAL_ALIGN_ARRAY(32, Ipp32f, forwardLPC, 2*LPC_ORDERP1, decoderObj);       /* LPC Forward filter */
   LOCAL_ALIGN_ARRAY(32, Ipp32f, backwardReflectCoeff, BWD_LPC_ORDER, decoderObj);        /* LPC backward reflection coefficients */
   LOCAL_ALIGN_ARRAY(32, Ipp32f, forwardAutoCorr, BWD_LPC_ORDERP1, decoderObj);       /* Autocorrelations (backward) */
   LOCAL_ALIGN_ARRAY(32, Ipp32f, CurrLSP, LPC_ORDER, decoderObj);           /* LSPs             */
   LOCAL_ALIGN_ARRAY(32, Ipp32f, ACELPCodeVec, SUBFR_LEN, decoderObj);        /* ACELP codevector */
   LOCAL_ALIGN_ARRAY(32, Ipp32f, PhaseDispExc, SUBFR_LEN, decoderObj);  /* excitation after phase dispersion */
   LOCAL_ALIGN_ARRAY(32, Ipp32f, flDst, FRM_LEN, decoderObj);
   LOCAL_ALIGN_ARRAY(32, Ipp32f, TmpAlignVec, WINDOW_LEN,decoderObj);
   LOCAL_ARRAY(Ipp32f, tmpLSP, LPC_ORDER, decoderObj);
   LOCAL_ARRAY(Ipp32s, decPrm, 20, decoderObj);
   LOCAL_ARRAY(Ipp32s, T2, 2, decoderObj);          /* Decoded Pitch              */
   LOCAL_ARRAY(Ipp32s, delayLine, 2, decoderObj);

   Ipp32f *Excitation,*pSynth;
   Ipp32s i, j, NSbfr;
   Ipp32s PitchDelay, index;
   Ipp32s bfi;                      /* Bad frame indicator */
   Ipp32s badPitch=0;                 /* bad pitch indicator */
   Ipp32s LPCMode;                  /* Backward / Forward mode indication */
   Ipp32f PitchGain, CodeGain;      /* fixed and adaptive codebook gain */
   Ipp32f tmp, energy;
   Ipp32f *pLPC;
   Ipp32s rate, highStatIndicator=0, aqLen, isSaturateAZ;
   Ipp32s FrameType, pstLPCOrder, isBWDDominant=0, Vad, parm2;
   Ipp32f tmp_lev;

   Ipp32s nsubfr;
   IppStatus sts;
   Ipp32s   *pDecPrm;
   const  Ipp8u *pParm;
   Ipp32f fTmp;

   if(NULL==decoderObj || NULL==src || NULL ==dst)
      return APIG729_StsBadArgErr;
   if(decoderObj->objPrm.objSize <= 0)
      return APIG729_StsNotInitialized;
   if(DEC_KEY != decoderObj->objPrm.key)
      return APIG729_StsBadCodecType;

   Excitation = decoderObj->OldExcitationBuffer + PITCH_LAG_MAX + INTERPOL_LEN;
   pSynth = decoderObj->SynthBuffer + BWD_SYNTH_MEM;

   pParm = src;

   if(frametype == -1){ /* erased*/
      decPrm[1] = 0; /* FrameType =sil ???*/
      decPrm[0] = 1; /* bfi = 1, corrupted*/
      ippsCopy_32f ((Ipp32f*)decoderObj->prevSID, (Ipp32f*)&decPrm[2], 4);
   }else if(frametype == 0){ /* untransmitted sil*/
      decPrm[1] = 0; /* FrameType =sil */
      decPrm[0] = 0; /* bfi = 0, */
      ippsCopy_32f ((Ipp32f*)decoderObj->prevSID, (Ipp32f*)&decPrm[2], 4);
   }else if(frametype == 1){ /* SID*/
      decPrm[1] = 1; /* FrameType=sid */
      decPrm[0] = 0; /* bfi = 0*/
      i=0;
      decPrm[1+1] = ExtractBitsG729FP(&pParm,&i,1);
      decPrm[1+2] = ExtractBitsG729FP(&pParm,&i,5);
      decPrm[1+3] = ExtractBitsG729FP(&pParm,&i,4);
      decPrm[1+4] = ExtractBitsG729FP(&pParm,&i,5);
      ippsCopy_32f ((Ipp32f*)&decPrm[2], (Ipp32f*)decoderObj->prevSID, 4);
   }else if(frametype == 2){ /* active frame D*/

      decPrm[0] = 0; /* bfi = 0*/
      decPrm[1] = 2; /* FrameType=voice d */
      i=0;
      decPrm[2] = ExtractBitsG729FP(&pParm,&i,1+N_BITS_1ST_STAGE);
      decPrm[3] = ExtractBitsG729FP(&pParm,&i,N_BITS_2ND_STAGE*2);
      decPrm[4] = ExtractBitsG729FP(&pParm,&i,8);
      decPrm[5] = ExtractBitsG729FP(&pParm,&i,9);
      decPrm[6] = ExtractBitsG729FP(&pParm,&i,2);
      decPrm[7] = ExtractBitsG729FP(&pParm,&i,6);
      decPrm[8] = ExtractBitsG729FP(&pParm,&i,4);
      decPrm[9] = ExtractBitsG729FP(&pParm,&i,9);
      decPrm[10] = ExtractBitsG729FP(&pParm,&i,2);
      decPrm[11] = ExtractBitsG729FP(&pParm,&i,6);

      rate = G729E_MODE;
   }else if(frametype == 3){ /* active frame */
      i=0;
      decPrm[1] = 3; /* FrameType=voice e */
      decPrm[0] = 0; /* bfi = 0*/
      decPrm[1+1] = ExtractBitsG729FP(&pParm,&i,1+N_BITS_1ST_STAGE);
      decPrm[1+2] = ExtractBitsG729FP(&pParm,&i,N_BITS_2ND_STAGE*2);
      decPrm[1+3] = ExtractBitsG729FP(&pParm,&i,8);
      decPrm[1+4] = ExtractBitsG729FP(&pParm,&i,1);
      decPrm[1+5] = ExtractBitsG729FP(&pParm,&i,13);
      decPrm[1+6] = ExtractBitsG729FP(&pParm,&i,4);
      decPrm[1+7] = ExtractBitsG729FP(&pParm,&i,7);
      decPrm[1+8] = ExtractBitsG729FP(&pParm,&i,5);
      decPrm[1+9] = ExtractBitsG729FP(&pParm,&i,13);
      decPrm[1+10] = ExtractBitsG729FP(&pParm,&i,4);
      decPrm[1+11] = ExtractBitsG729FP(&pParm,&i,7);
      decPrm[1+4] = (Parity(decPrm[1+3])+decPrm[1+4]) & 1; /*  parity error (1) */
      rate = G729_BASE;
   }else if(frametype == 4){ /* active frame E*/
      i=0;
      decPrm[0] = 0; /* bfi = 0*/
      decPrm[1] = 4; /* FrameType */
      if(ExtractBitsG729FP(&pParm,&i,2)==0) {
         decPrm[2] = 0; /*LPCMode*/

         decPrm[3] = ExtractBitsG729FP(&pParm,&i,1+N_BITS_1ST_STAGE);
         decPrm[4] = ExtractBitsG729FP(&pParm,&i,N_BITS_2ND_STAGE*2);
         decPrm[5] = ExtractBitsG729FP(&pParm,&i,8);
         decPrm[6] = ExtractBitsG729FP(&pParm,&i,1);
         decPrm[7] = ExtractBitsG729FP(&pParm,&i,7);
         decPrm[8] = ExtractBitsG729FP(&pParm,&i,7);
         decPrm[9] = ExtractBitsG729FP(&pParm,&i,7);
         decPrm[10] = ExtractBitsG729FP(&pParm,&i,7);
         decPrm[11] = ExtractBitsG729FP(&pParm,&i,7);
         decPrm[12] = ExtractBitsG729FP(&pParm,&i,7);
         decPrm[13] = ExtractBitsG729FP(&pParm,&i,5);
         decPrm[14] = ExtractBitsG729FP(&pParm,&i,7);
         decPrm[15] = ExtractBitsG729FP(&pParm,&i,7);
         decPrm[16] = ExtractBitsG729FP(&pParm,&i,7);
         decPrm[17] = ExtractBitsG729FP(&pParm,&i,7);
         decPrm[18] = ExtractBitsG729FP(&pParm,&i,7);
         decPrm[19] = ExtractBitsG729FP(&pParm,&i,7);

         i = decPrm[5]>>1;
         i &= 1;
         decPrm[6] += i;
         decPrm[6] = (Parity(decPrm[5])+decPrm[6]) & 1;/* parm[6] = parity error (1) */
      } else {
         decPrm[2] = 1; /*LPCMode*/

         decPrm[3] = ExtractBitsG729FP(&pParm,&i,8);
         decPrm[4] = ExtractBitsG729FP(&pParm,&i,1);
         decPrm[5] = ExtractBitsG729FP(&pParm,&i,13);
         decPrm[6] = ExtractBitsG729FP(&pParm,&i,10);
         decPrm[7] = ExtractBitsG729FP(&pParm,&i,7);
         decPrm[8] = ExtractBitsG729FP(&pParm,&i,7);
         decPrm[9] = ExtractBitsG729FP(&pParm,&i,7);
         decPrm[10] = ExtractBitsG729FP(&pParm,&i,7);
         decPrm[11] = ExtractBitsG729FP(&pParm,&i,5);
         decPrm[12] = ExtractBitsG729FP(&pParm,&i,13);
         decPrm[13] = ExtractBitsG729FP(&pParm,&i,10);
         decPrm[14] = ExtractBitsG729FP(&pParm,&i,7);
         decPrm[15] = ExtractBitsG729FP(&pParm,&i,7);
         decPrm[16] = ExtractBitsG729FP(&pParm,&i,7);
         decPrm[17] = ExtractBitsG729FP(&pParm,&i,7);

         i = decPrm[3]>>1;
         i &= 1;
         decPrm[4] += i;
         decPrm[4] = (Parity(decPrm[3])+decPrm[4]) & 1;/* parm[4] = parity error (1) */
      }
      rate = G729E_MODE;
   }
   pDecPrm = &decPrm[0];
   parm2 = pDecPrm[2];

   bfi = *pDecPrm++;
   FrameType = *pDecPrm++;

   if(bfi == 1) {
      if(decoderObj->objPrm.codecType!=G729A_CODEC) {
         FrameType = decoderObj->prevFrameType;
         if(FrameType == 1) FrameType = 0;
      } else {
           if(decoderObj->prevFrameType == 1) FrameType = 1;
          else FrameType = 0;
      }
      pDecPrm[-1] = FrameType;
   }

   Vad = FrameType;
   rate = FrameType - 2;
   /* Decoding the Backward/Forward LPC mode */
   if( rate != G729E_MODE) LPCMode = 0;
   else {
      if (bfi != 0) {
         LPCMode = decoderObj->prevLPCMode; /* Frame erased => LPCMode = previous LPCMode */
         *pDecPrm++ = LPCMode;
      } else {
         LPCMode = *pDecPrm++;
      }
      if(decoderObj->lPrevBFI != 0) decoderObj->lVoicing = decoderObj->lPrevVoicing;

      /* Backward LPC mode */
      ippsWinHybrid_G729E_32f(decoderObj->SynthBuffer, forwardAutoCorr, decoderObj->pHWState);

      /* Lag windowing    */
      ippsMul_32f_I(lagBwd, &forwardAutoCorr[1], BWD_LPC_ORDER);
      if (forwardAutoCorr[0]<1.0) forwardAutoCorr[0]=1.0;

      sts = ippsLevinsonDurbin_G729_32f(forwardAutoCorr, BWD_LPC_ORDER, &backwardLPC[BWD_LPC_ORDERP1], backwardReflectCoeff, &tmp_lev);
      if(sts == ippStsOverflow) {
         ippsCopy_32f(decoderObj->OldBackwardLPC,&backwardLPC[BWD_LPC_ORDERP1],BWD_LPC_ORDER+1);
         backwardReflectCoeff[0] = decoderObj->OldBackwardRC[0];
         backwardReflectCoeff[1] = decoderObj->OldBackwardRC[1];
      } else {
         ippsCopy_32f(&backwardLPC[BWD_LPC_ORDERP1],decoderObj->OldBackwardLPC,BWD_LPC_ORDER+1);
         decoderObj->OldBackwardRC[0] = backwardReflectCoeff[0];
         decoderObj->OldBackwardRC[1] = backwardReflectCoeff[1];
      }

      /* Tests saturation of backwardLPC */
      isSaturateAZ = 0;
      for (i=BWD_LPC_ORDERP1; i<2*BWD_LPC_ORDERP1; i++) if (backwardLPC[i] >= 8.) {isSaturateAZ = 1;break;}
      if (isSaturateAZ == 1) ippsCopy_32f(decoderObj->BackwardLPCMemory, &backwardLPC[BWD_LPC_ORDERP1], BWD_LPC_ORDERP1);
      else ippsCopy_32f(&backwardLPC[BWD_LPC_ORDERP1], decoderObj->BackwardLPCMemory, BWD_LPC_ORDERP1);

      /* Additional bandwidth expansion on backward filter */
      WeightLPCCoeff_G729(&backwardLPC[BWD_LPC_ORDERP1], BWD_GAMMA, BWD_LPC_ORDER, &backwardLPC[BWD_LPC_ORDERP1]);

      if(LPCMode == 1) {
         if (!isVarZero(decoderObj->fFEInterpolationCoeff)) {
            /* Interpolation of the backward filter after a bad frame */
            tmp = 1.f - decoderObj->fFEInterpolationCoeff;
            pLPC = backwardLPC + BWD_LPC_ORDERP1;
            ippsInterpolateC_G729_32f(pLPC, tmp, decoderObj->BackwardUnqLPC, decoderObj->fFEInterpolationCoeff, pLPC, BWD_LPC_ORDERP1);
         }
      }
   }
   if( bfi == 0) {
      decoderObj->fGainMuting = 1.;
      decoderObj->lBFICounter = 0;
   } else if((decoderObj->lPrevBFI == 0) && (decoderObj->prevFrameType >3)) {
      /* store the last good backward filter when the frame is erased */
      ippsCopy_32f(&backwardLPC[BWD_LPC_ORDERP1], decoderObj->BackwardUnqLPC, BWD_LPC_ORDERP1);
   }
   /* Update next frame synthesis signal */
   ippsCopy_32f(&decoderObj->SynthBuffer[FRM_LEN], &decoderObj->SynthBuffer[0], BWD_SYNTH_MEM);

   if(FrameType < 2) {
      /* SID or Untr or Erased Frame */
      if(pDecPrm[-1] != 0) {
         decoderObj->fSIDGain = SIDGainTbl[pDecPrm[3]];

         /* Inverse quantization of the LSP */
         ippsLSFDecode_G729B_32f(&pDecPrm[0], (Ipp32f*)decoderObj->PrevFreq, decoderObj->SIDLSP);
      } else {
         /* non SID Frame
           Case of 1st SID frame erased : quantize-decode
           energy estimate stored in fSIDGain         */
         if(decoderObj->prevFrameType > 1) {
            QuantSIDGain_G729B(&decoderObj->SID, 0, &fTmp, &i);
            decoderObj->fSIDGain = SIDGainTbl[i];
         }
      }
      if(decoderObj->prevFrameType > 1) {
         decoderObj->fCurrGain = decoderObj->fSIDGain;
      } else {
         decoderObj->fCurrGain *= GAIN_INT_FACTOR;
         decoderObj->fCurrGain += INV_GAIN_INT_FACTOR * decoderObj->fSIDGain;
      }

      if(isVarZero(decoderObj->fCurrGain)) {
         ippsZero_32f(Excitation,FRM_LEN);
         PhaseDispersionUpdate_G729D(0.f,decoderObj->fCurrGain,decoderObj->phdMem);
         PhaseDispersionUpdate_G729D(0.f,decoderObj->fCurrGain,decoderObj->phdMem);
      } else {
         ComfortNoiseExcitation_G729(decoderObj->fCurrGain, Excitation, &decoderObj->sCNGSeed, DECODER,NULL,decoderObj->phdMem,(Ipp8s *)TmpAlignVec);
      }

      /* Interpolate the Lsp vectors */
      ippsInterpolateC_G729_32f(decoderObj->OldLSP, 0.5, decoderObj->SIDLSP, 0.5, tmpLSP, LPC_ORDER);

      ippsLSPToLPC_G729_32f(tmpLSP, forwardLPC);
      ippsLSPToLPC_G729_32f(decoderObj->SIDLSP, &forwardLPC[LPC_ORDER+1]);
      ippsCopy_32f(decoderObj->SIDLSP, decoderObj->OldLSP, LPC_ORDER);

      pLPC = forwardLPC;
      if(decoderObj->objPrm.codecType!=G729A_CODEC) {
         for (NSbfr = 0; NSbfr < FRM_LEN; NSbfr += SUBFR_LEN) {
            ippsSynthesisFilter_G729_32f(pLPC, LPC_ORDER, &Excitation[NSbfr], &pSynth[NSbfr], SUBFR_LEN, &decoderObj->SynFltMemory[BWD_LPC_ORDER-LPC_ORDER]);
            ippsMove_32f(&pSynth[NSbfr+SUBFR_LEN-BWD_LPC_ORDER], decoderObj->SynFltMemory, BWD_LPC_ORDER);

            T2[0] = decoderObj->prevPitchDelay;
            pLPC += LPC_ORDERP1;
         }
      } else {
         for (NSbfr = 0,i=0; NSbfr < FRM_LEN; NSbfr += SUBFR_LEN,i++) {
            ippsSynthesisFilter_G729_32f(pLPC, LPC_ORDER, &Excitation[NSbfr], &flDst[NSbfr], SUBFR_LEN, decoderObj->SynFltMemory);
            ippsCopy_32f(&flDst[NSbfr+SUBFR_LEN-LPC_ORDER], decoderObj->SynFltMemory, LPC_ORDER);

            T2[i] = decoderObj->prevPitchDelay;
            pLPC += LPC_ORDERP1;
         }
      }
      decoderObj->fBetaPreFilter = PITCH_SHARPMIN;
      decoderObj->fInterpolationCoeff = 1.1f;
      /* Reset for gain decoding in case of frame erasure */
      decoderObj->sBWDStatInd = 0;
      highStatIndicator = 0;
      /* Reset for pitch tracking  in case of frame erasure */
      decoderObj->lStatPitchPT = 0;
      /* Update the previous filter for the next frame */
      ippsCopy_32f(&forwardLPC[LPC_ORDERP1], decoderObj->PrevFlt, LPC_ORDERP1);
      for(i=LPC_ORDERP1; i<BWD_LPC_ORDERP1; i++) decoderObj->PrevFlt[i] = 0.;
   } else {
      /* Active frame */
      decoderObj->sCNGSeed = INIT_SEED_VAL;
      /* Forward mode */
      if (LPCMode == 0) {

         /* Decode the LSFs to CurrLSP */
         if(bfi){
            ippsCopy_32f(decoderObj->prevLSF, CurrLSP, LPC_ORDER );
            ippsLSFDecodeErased_G729_32f(decoderObj->prevMA, (Ipp32f*)decoderObj->PrevFreq, decoderObj->prevLSF);
         }else{
            Ipp32s indexes[4];
            indexes[0] = (pDecPrm[0] >> N_BITS_1ST_STAGE) & 1;
            indexes[1] = pDecPrm[0] & (Ipp16s)(N_ELEM_1ST_STAGE - 1);
            indexes[2] = (pDecPrm[1] >> N_BITS_2ND_STAGE) & (Ipp16s)(N_ELEM_2ND_STAGE - 1);
            indexes[3] = pDecPrm[1] & (Ipp16s)(N_ELEM_2ND_STAGE - 1);

            decoderObj->prevMA = indexes[0];
            ippsLSFDecode_G729_32f(indexes, (Ipp32f*)decoderObj->PrevFreq, CurrLSP);

            ippsCopy_32f(CurrLSP, decoderObj->prevLSF, LPC_ORDER );

         }

         /* Convert to LSPs */
         ownCOS_G729_32f(CurrLSP, CurrLSP, LPC_ORDER);

         pDecPrm += 2;
         if( decoderObj->prevLPCMode == 0) { /* Interpolation of LPC for the 2 subframes */
            ippsInterpolateC_G729_32f(decoderObj->OldLSP, 0.5, CurrLSP, 0.5, tmpLSP, LPC_ORDER);

            ippsLSPToLPC_G729_32f(tmpLSP, forwardLPC);
            ippsLSPToLPC_G729_32f(CurrLSP, &forwardLPC[LPC_ORDER+1]);
         } else {
            /* no interpolation */
            ippsLSPToLPC_G729_32f(CurrLSP, forwardLPC); /* Subframe 1*/
            ippsCopy_32f(forwardLPC, &forwardLPC[LPC_ORDERP1], LPC_ORDERP1); /* Subframe 2 */
         }

         /* update the LSFs for the next frame */
         ippsCopy_32f(CurrLSP, decoderObj->OldLSP, LPC_ORDER);

         decoderObj->fInterpolationCoeff = 1.1f;
         pLPC = forwardLPC;
         aqLen = LPC_ORDER;
         /* update the previous filter for the next frame */
         ippsCopy_32f(&forwardLPC[LPC_ORDERP1], decoderObj->PrevFlt, LPC_ORDERP1);
         for(i=LPC_ORDERP1; i<BWD_LPC_ORDERP1; i++) decoderObj->PrevFlt[i] = 0.;
      } else {
         InterpolatedBackwardFilter_G729(backwardLPC, decoderObj->PrevFlt, &decoderObj->fInterpolationCoeff);
         pLPC = backwardLPC;
         aqLen = BWD_LPC_ORDER;
         /* update the previous filter for the next frame */
         ippsCopy_32f(&backwardLPC[BWD_LPC_ORDERP1], decoderObj->PrevFlt, BWD_LPC_ORDERP1);
      }

      for (NSbfr = 0,nsubfr=0; NSbfr < FRM_LEN; NSbfr += SUBFR_LEN,nsubfr++) {
         index = *pDecPrm++;            /* pitch index */

         if(NSbfr == 0) {
            if (rate == G729D_MODE) i = 0;      /* no pitch parity at 6.4 kb/s */
            else i = *pDecPrm++;           /* get parity check result */
            badPitch = bfi + i;
         }

         DecodeAdaptCodebookDelays(&decoderObj->prevPitchDelay,
             &decoderObj->prevFracPitchDelay,delayLine,NSbfr,
             badPitch,index,(G729Codec_Type)rate);
         PitchDelay = delayLine[0];
         T2[nsubfr] = PitchDelay;
         /* adaptive codebook decode  */
         ippsDecodeAdaptiveVector_G729_32f_I(delayLine, &Excitation[NSbfr]);
         if(decoderObj->objPrm.codecType!=G729A_CODEC) {
            /* Pitch tracking*/
            if( rate == G729E_MODE) {
               PitchTracking_G729FPE(&decoderObj->prevPitchDelay, &decoderObj->prevFracPitchDelay, &decoderObj->lPrevPitchPT, &decoderObj->lStatPitchPT,
                         &decoderObj->lStatPitch2PT, &decoderObj->lStatFracPT);
            } else {
               i = decoderObj->prevPitchDelay;
               j = decoderObj->prevFracPitchDelay;
               PitchTracking_G729FPE(&i, &j, &decoderObj->lPrevPitchPT, &decoderObj->lStatPitchPT,
                          &decoderObj->lStatPitch2PT, &decoderObj->lStatFracPT);
            }
         }
         /* Fixed codebook decode*/
         ippsZero_32f(ACELPCodeVec,SUBFR_LEN);
         if (rate == G729_BASE) {
            /* case base mode or annex A*/
            LOCAL_ARRAY(Ipp32s, Position, 4, decoderObj);
            Ipp32s lTmpIndex, sign;

            if(bfi != 0)        /* Bad frame */
            {
               lTmpIndex = (Ipp32s)Rand_16s(&decoderObj->sFESeed) & (Ipp16s)0x1fff;     /* 13 bits random */
               sign = (Ipp32s)Rand_16s(&decoderObj->sFESeed) & (Ipp16s)0x000f;     /*  4 bits random */
            }else{
               lTmpIndex = pDecPrm[0];
               sign = pDecPrm[1];
            }

            /* First pulse's positions*/
            Position[0] = (lTmpIndex&7)*5;

            /* Second pulse's positions*/
            lTmpIndex >>= 3;
            Position[1] = (lTmpIndex&7)*5 + 1;
            /* Third pulse's positions*/
            lTmpIndex >>= 3;
            Position[2] = (lTmpIndex&7)*5 + 2;

            /* Forth pulse's positions*/
            lTmpIndex >>= 3;
            j = lTmpIndex;
            lTmpIndex >>= 1;
            Position[3] = (lTmpIndex&7)*5 + 3 + (j&1);

            /* Decode signs of 4 pulse and compose the algebraic codeword */
            for (j=0; j<4; j++) {
               if (((sign>>j) & 1) != 0) {
                  ACELPCodeVec[Position[j]] = 1.0;
               } else {
                  ACELPCodeVec[Position[j]] = -1.0;
               }
            }
            pDecPrm += 2;
            /* for gain decoding in case of frame erasure */
            decoderObj->sBWDStatInd = 0;
            highStatIndicator = 0;
            LOCAL_ARRAY_FREE(Ipp32s, Position, 4, decoderObj);
         } else if (rate == G729D_MODE) {
            /* case annex D*/
            LOCAL_ARRAY(Ipp32s, Position, 2, decoderObj);
            Ipp32s lTmpIndex, sign;

            if(bfi != 0)        /* Bad frame */
            {
               lTmpIndex = (Ipp32s)Rand_16s(&decoderObj->sFESeed) & (Ipp16s)0x1fff;     /* 13 bits random */
               sign = (Ipp32s)Rand_16s(&decoderObj->sFESeed) & (Ipp16s)0x000f;     /*  4 bits random */
            }else{
               lTmpIndex = pDecPrm[0];
               sign = pDecPrm[1];
            }

            /* decode the positions of 4 pulses */
            i = lTmpIndex & 15;
            Position[0] = trackTbl0[i];

            lTmpIndex >>= 4;
            i = lTmpIndex & 31;
            Position[1] = trackTbl1[i];

            /* find the algebraic codeword */

            /* decode the signs of 2 pulses */
            for (j=0; j<2; j++) {
               if (((sign>>j) & 1) != 0) {
                  ACELPCodeVec[Position[j]] += 1.0;
               } else {
                  ACELPCodeVec[Position[j]] -= 1.0;
               }
            }
            pDecPrm += 2;
            /* for gain decoding in case of frame erasure */
            decoderObj->sBWDStatInd = 0;
            highStatIndicator = 0;
            LOCAL_ARRAY_FREE(Ipp32s, Position, 2, decoderObj);
         } else if (rate == G729E_MODE) {
            /* case annex E*/
            LOCAL_ARRAY(Ipp32s, tmp_parm, 5, decoderObj);

            if(bfi)
            {
               tmp_parm[0] = (Ipp32s)Rand_16s(&decoderObj->sFESeed);
               tmp_parm[1] = (Ipp32s)Rand_16s(&decoderObj->sFESeed);
               tmp_parm[2] = (Ipp32s)Rand_16s(&decoderObj->sFESeed);
               tmp_parm[3] = (Ipp32s)Rand_16s(&decoderObj->sFESeed);
               tmp_parm[4] = (Ipp32s)Rand_16s(&decoderObj->sFESeed);
            }else{
               for(i=0;i<5;i++)
                  tmp_parm[i] = pDecPrm[i];
            }

            if (LPCMode == 0) {
               Ipp32s pos1, pos2;
               Ipp32f sign;

               /* decode the positions and signs of pulses and build the codeword */

               pos1 = ((tmp_parm[0] & 7) * 5);
               if (((tmp_parm[0]>>3) & 1) == 0) sign = 1.;
               else sign = -1.;
               ACELPCodeVec[pos1] = sign;

               pos2 = (((tmp_parm[0]>>4) & 7) * 5);
               if (pos2 > pos1) sign = -sign;

               ACELPCodeVec[pos2] += sign;

               pos1 = ((tmp_parm[1] & 7) * 5) + 1;
               if (((tmp_parm[1]>>3) & 1) == 0) sign = 1.;
               else sign = -1.;
               ACELPCodeVec[pos1] = sign;

               pos2 = (((tmp_parm[1]>>4) & 7) * 5) + 1;
               if (pos2 > pos1) sign = -sign;

               ACELPCodeVec[pos2] += sign;

               pos1 = ((tmp_parm[2] & 7) * 5) + 2;
               if (((tmp_parm[2]>>3) & 1) == 0) sign = 1.;
               else sign = -1.;
               ACELPCodeVec[pos1] = sign;

               pos2 = (((tmp_parm[2]>>4) & 7) * 5) + 2;
               if (pos2 > pos1) sign = -sign;

               ACELPCodeVec[pos2] += sign;

               pos1 = ((tmp_parm[3] & 7) * 5) + 3;
               if (((tmp_parm[3]>>3) & 1) == 0) sign = 1.;
               else sign = -1.;
               ACELPCodeVec[pos1] = sign;

               pos2 = (((tmp_parm[3]>>4) & 7) * 5) + 3;
               if (pos2 > pos1) sign = -sign;

               ACELPCodeVec[pos2] += sign;

               pos1 = ((tmp_parm[4] & 7) * 5) + 4;
               if (((tmp_parm[4]>>3) & 1) == 0) sign = 1.;
               else sign = -1.;
               ACELPCodeVec[pos1] = sign;

               pos2 = (((tmp_parm[4]>>4) & 7) * 5) + 4;
               if (pos2 > pos1) sign = -sign;

               ACELPCodeVec[pos2] += sign;
               /* for gain decoding in case of frame erasure */
               decoderObj->sBWDStatInd = 0;
               highStatIndicator = 0;
            } else {
               Ipp32s lIdxCounter, track, pos1, pos2, pos3;
               Ipp32f sign;

               /* decode the positions and signs of pulses and build the codeword */

               track = (tmp_parm[0]>>10) & 7;
               CLIP_TO_UPLEVEL(track,NUM_TRACK_ACELP);

               for (lIdxCounter=0; lIdxCounter<2; lIdxCounter++) {
                  pos1 = ((tmp_parm[lIdxCounter] & 7) * 5) + track;
                  if (((tmp_parm[lIdxCounter]>>3) & 1) == 0) sign = 1.;
                  else sign = -1.;
                  ACELPCodeVec[pos1] = sign;

                  pos2 = (((tmp_parm[lIdxCounter]>>4) & 7) * 5) + track;
                  if (pos2 > pos1) sign = -sign;

                  ACELPCodeVec[pos2] += sign;

                  pos3 = (((tmp_parm[lIdxCounter]>>7) & 7) * 5) + track;
                  if (pos3 > pos2) sign = -sign;

                  ACELPCodeVec[pos3] += sign;

                  track++;
                  if (track > NUM_TRACK_ACELP) track = 0;
               }

               for (lIdxCounter=2; lIdxCounter<5; lIdxCounter++) {
                  pos1 = ((tmp_parm[lIdxCounter] & 7) * 5) + track;
                  if (((tmp_parm[lIdxCounter]>>3) & 1) == 0) sign = 1.;
                  else sign = -1.;
                  ACELPCodeVec[pos1] = sign;

                  pos2 = (((tmp_parm[lIdxCounter]>>4) & 7) * 5) + track;
                  if (pos2 > pos1) sign = -sign;

                  ACELPCodeVec[pos2] += sign;

                  track++;
                  if (track > NUM_TRACK_ACELP) track = 0;
               }
               /* for gain decoding in case of frame erasure */
               decoderObj->sBWDStatInd++;
               if (decoderObj->sBWDStatInd >= 30) {
                  highStatIndicator = 1;
                  decoderObj->sBWDStatInd = 30;
               } else highStatIndicator = 0;
            }
            pDecPrm += 5;
            LOCAL_ARRAY_FREE(Ipp32s, tmp_parm, 5, decoderObj);
         }
         ippsHarmonicFilter_32f_I(decoderObj->fBetaPreFilter,PitchDelay,&ACELPCodeVec[PitchDelay],SUBFR_LEN-PitchDelay);
         /* Decode pitch and codebook gains.              */
         index = *pDecPrm++;      /* index of energy VQ */
         if (bfi != 0)  {
            Ipp32f av_pred_en;

            if(rate == G729E_MODE) {
               if (decoderObj->lBFICounter < 2) {
                  if (highStatIndicator) decoderObj->fPitchGain = 1.;
                  else decoderObj->fPitchGain = 0.95f;
                  decoderObj->fCodeGain = decoderObj->fCodeGainMemory;
               } else {
                  decoderObj->fPitchGain  = decoderObj->fPitchGainMemory * (decoderObj->fGainMuting);
                  decoderObj->fCodeGain = decoderObj->fCodeGainMemory * (decoderObj->fGainMuting);
                  if (highStatIndicator) {
                     if (decoderObj->lBFICounter > 10) decoderObj->fGainMuting *= 0.995f;
                  } else decoderObj->fGainMuting *= 0.98f;
               }
            } else {
               decoderObj->fPitchGain *= 0.9f;
               if(decoderObj->fPitchGain > 0.9f) decoderObj->fPitchGain=0.9f;
               decoderObj->fCodeGain *= 0.98f;
            }

            /* Update table of past quantized energies      */

            av_pred_en = 0.0f;
            for (i = 0; i < 4; i++)
               av_pred_en += decoderObj->PastQuantEnergy[i];
            av_pred_en = av_pred_en*0.25f - 4.0f;
            if (av_pred_en < -14.0) av_pred_en = -14.0f;

            for (i = 3; i > 0; i--)
               decoderObj->PastQuantEnergy[i] = decoderObj->PastQuantEnergy[i-1];
            decoderObj->PastQuantEnergy[0] = av_pred_en;
         } else {
            DecodeGain_G729(index, ACELPCodeVec, SUBFR_LEN, &decoderObj->fPitchGain, &decoderObj->fCodeGain, rate, decoderObj->PastQuantEnergy);
         }
         /* Update previous gains */
         decoderObj->fPitchGainMemory = decoderObj->fPitchGain;
         decoderObj->fCodeGainMemory = decoderObj->fCodeGain;
         /* - Update pitch sharpening "fBetaPreFilter" with quantized fPitchGain */
         decoderObj->fBetaPreFilter = decoderObj->fPitchGain;
         CLIP_TO_UPLEVEL(decoderObj->fBetaPreFilter,PITCH_SHARPMAX);
         CLIP_TO_LOWLEVEL(decoderObj->fBetaPreFilter,PITCH_SHARPMIN);

         /* Synthesis of speech corresponding to Excitation[]  */
         if(decoderObj->objPrm.codecType!=G729A_CODEC) {
            if(bfi != 0) {       /* Bad frame */
               decoderObj->lBFICounter++;
               if (decoderObj->lVoicing == 0 ) {
                  PitchGain = 0.;
                  CodeGain = decoderObj->fCodeGain;
               } else {
                  PitchGain = decoderObj->fPitchGain;
                  CodeGain = 0.;
               }
            } else {
               PitchGain = decoderObj->fPitchGain;
               CodeGain = decoderObj->fCodeGain;
            }
            ippsInterpolateC_G729_32f(&Excitation[NSbfr], PitchGain, ACELPCodeVec, CodeGain, &Excitation[NSbfr], SUBFR_LEN);

            if (rate == G729D_MODE) {
               PhaseDispersion_G729D(&Excitation[NSbfr], PhaseDispExc, decoderObj->fCodeGain, decoderObj->fPitchGain, ACELPCodeVec,decoderObj->phdMem,(Ipp8s *)TmpAlignVec);
               ippsSynthesisFilter_G729_32f(pLPC, aqLen, PhaseDispExc, &pSynth[NSbfr], SUBFR_LEN, &decoderObj->SynFltMemory[BWD_LPC_ORDER-aqLen]);
            } else {
               ippsSynthesisFilter_G729_32f(pLPC, aqLen, &Excitation[NSbfr], &pSynth[NSbfr], SUBFR_LEN, &decoderObj->SynFltMemory[BWD_LPC_ORDER-aqLen]);

               /* Updates state machine for phase dispersion in
                6.4 kbps mode, if running at other rate */
               PhaseDispersionUpdate_G729D(decoderObj->fPitchGain, decoderObj->fCodeGain,decoderObj->phdMem);
            }

            ippsCopy_32f(&pSynth[NSbfr+SUBFR_LEN-BWD_LPC_ORDER], decoderObj->SynFltMemory, BWD_LPC_ORDER);
         } else {
            ippsInterpolateC_G729_32f(&Excitation[NSbfr], decoderObj->fPitchGain, ACELPCodeVec, decoderObj->fCodeGain, &Excitation[NSbfr], SUBFR_LEN);

            ippsSynthesisFilter_G729_32f(pLPC, LPC_ORDER, &Excitation[NSbfr], &flDst[NSbfr], SUBFR_LEN, decoderObj->SynFltMemory);
            for (i = 0; i < LPC_ORDER; i++)  decoderObj->SynFltMemory[i] =flDst[NSbfr+(SUBFR_LEN-LPC_ORDER)+i];
         }
         pLPC += aqLen+1;    /* interpolated LPC parameters for next subframe */
      }
   }

   if(bfi == 0) {
      Ipp64f dTmpVar;

      ippsDotProd_32f64f(Excitation, Excitation, FRM_LEN, &dTmpVar);
      decoderObj->SID = (Ipp32f)dTmpVar;
   }
   decoderObj->prevFrameType = FrameType;
   ippsMove_32f(&decoderObj->OldExcitationBuffer[FRM_LEN], &decoderObj->OldExcitationBuffer[0], PITCH_LAG_MAX+INTERPOL_LEN);

   if(decoderObj->objPrm.codecType!=G729A_CODEC) {
      energy = CalcEnergy_dB_G729(pSynth, FRM_LEN);
      if (energy >= 40.) isBackwardModeDominant_G729(&isBWDDominant, LPCMode, &decoderObj->sBWDFrmCounter,&decoderObj->sFWDFrmCounter);

      decoderObj->lPrevBFI     = bfi;
      decoderObj->prevLPCMode    = LPCMode;
      decoderObj->lPrevVoicing = decoderObj->lVoicing;

      if (bfi != 0) decoderObj->fFEInterpolationCoeff = 1.;
      else {
         if (LPCMode == 0) decoderObj->fFEInterpolationCoeff = 0;
         else {
            if (isBWDDominant == 1) decoderObj->fFEInterpolationCoeff -= 0.1f;
            else decoderObj->fFEInterpolationCoeff -= 0.5f;
            if (decoderObj->fFEInterpolationCoeff < 0)  decoderObj->fFEInterpolationCoeff= 0;
         }
      }
   }
   if(decoderObj->objPrm.codecType!=G729A_CODEC) {
      /* Control adaptive parameters for postfiltering */
      if( LPCMode == 0) {
         pLPC = forwardLPC;
         pstLPCOrder = LPC_ORDER;
      } else {
         pLPC = backwardLPC;
         pstLPCOrder = BWD_LPC_ORDER;
      }
      post_filter_I(decoderObj, pSynth, pLPC, T2[0], ((parm2 == 1) && (isBWDDominant == 1)), Vad, pstLPCOrder, flDst,rate);
   } else {
      Post_G729A(decoderObj,flDst, forwardLPC, T2, Vad);
   }
   /* Highpass filter */
   ippsIIR_32f_I(flDst,FRM_LEN,decoderObj->iirstate);

   /* Round to nearest and convert to Ipp16s*/
   {
      for(i=0;i<FRM_LEN;i++) {
         fTmp = flDst[i];
         if (fTmp >= 0.0)
            fTmp += 0.5;
         else  fTmp -= 0.5;
         if (fTmp >  32767.0 ) fTmp =  32767.0;
         if (fTmp < -32768.0 ) fTmp = -32768.0;
         dst[i] = (Ipp16s) fTmp;
      }
   }
   //ippsConvert_32f16s_Sfs(flDst,dst,FRM_LEN,ippRndNear,0); /*A few difference between original code*/

   CLEAR_SCRATCH_MEMORY(decoderObj);

   return APIG729_StsNoErr;
}
static void post_filter_I(G729FPDecoder_Obj* decoderObj, Ipp32f *pSynth, Ipp32f *pLPC, Ipp32s pitchDelay, Ipp32s dominant,
                          Ipp32s Vad, Ipp32s pstLPCOrder, Ipp32f *dst,Ipp32s rate)
{
   Ipp32s i, lSFVoice, len;

   if (rate != G729E_MODE) {
      len = SHORTTERM_POSTFLT_LEN;
      decoderObj->g1PST = GAMMA1_POSTFLT;
      decoderObj->g2PST = GAMMA2_POSTFLT;
      decoderObj->gHarmPST = GAMMA_HARM_POSTFLT;
   } else {
      len = SHORTTERM_POSTFLT_LEN_E;
      /* If backward mode is dominant => progressively reduce postfiltering */
      if (dominant) {
         decoderObj->gHarmPST -= 0.0125f;
         CLIP_TO_LOWLEVEL(decoderObj->gHarmPST,0);
         decoderObj->g1PST -= 0.035f;
         CLIP_TO_LOWLEVEL(decoderObj->g1PST,0);
         decoderObj->g2PST -= 0.0325f;
         CLIP_TO_LOWLEVEL(decoderObj->g2PST,0);
      } else {
         decoderObj->gHarmPST += 0.0125f;
         CLIP_TO_UPLEVEL(decoderObj->gHarmPST,GAMMA_HARM_POSTFLT_E);
         decoderObj->g1PST += 0.035f;
         CLIP_TO_UPLEVEL(decoderObj->g1PST,GAMMA1_POSTFLT_E);
         decoderObj->g2PST += 0.0325f;
         CLIP_TO_UPLEVEL(decoderObj->g2PST,GAMMA2_POSTFLT_E);
      }
   }

   decoderObj->lVoicing = 0;
   for(i=0; i<FRM_LEN; i+=SUBFR_LEN) {
      Post_G729E(decoderObj, pitchDelay, &pSynth[i], pLPC, &dst[i], &lSFVoice, len, pstLPCOrder, Vad);
      if (lSFVoice != 0) decoderObj->lVoicing = lSFVoice;
      pLPC += pstLPCOrder+1;
   }
   return;
}

static void Post_G729A(G729FPDecoder_Obj *decoderObj, Ipp32f *pSrcDstSynthSpeech, Ipp32f *pSrcLPC,
                        Ipp32s *pSrcDecodedPitch, Ipp32s Vad)
{
   LOCAL_ALIGN_ARRAY(32, Ipp32f, SynthBuffer, (FRM_LEN+LPC_ORDER),decoderObj);  /* Synthesis                  */
   LOCAL_ALIGN_ARRAY(32, Ipp32f, pResidual, SUBFR_LEN,decoderObj);      /* ResidualMemory after pitch postfiltering */
   LOCAL_ALIGN_ARRAY(32, Ipp32f, pSynthPST, FRM_LEN,decoderObj);       /* post filtered synthesis speech   */
   LOCAL_ARRAY(Ipp32f, LPCGama2, LPC_ORDERP1,decoderObj);                         /* bandwidth expanded LP parameters */
   LOCAL_ARRAY(Ipp32f, LPCGama1, LPC_ORDERP1,decoderObj);
   LOCAL_ARRAY(Ipp32f, ImpRespPST, PST_IMPRESP_LEN,decoderObj);
   Ipp32f *pLPC;
   Ipp32s   PitchDelay, pitchMaxBound, pitchMinBound;     /* closed-loop pitch search range  */
   Ipp32s   i, NSbfr;            /* index for beginning of subframe */
   Ipp32f fTmp, fTmp1, fTmp2;
   Ipp32f *pSynth;
   Ipp64f sum, sum1;
   Ipp64f dGain,dg0;
   Ipp32f fMaxCorr;
   Ipp32f gainM1, gain;
   Ipp32f fEnergy, fEnergy0;

   pLPC = pSrcLPC;

   pSynth = &SynthBuffer[LPC_ORDER];

   ippsCopy_32f(decoderObj->PstFltMemoryA,SynthBuffer,LPC_ORDER);
   ippsCopy_32f(pSrcDstSynthSpeech,pSynth,FRM_LEN);

   for (NSbfr = 0; NSbfr < FRM_LEN; NSbfr += SUBFR_LEN) {
      /* Find pitch range t0_min - t0_max */

      pitchMinBound = *pSrcDecodedPitch++ - 3;
      pitchMaxBound = pitchMinBound+6;
      if (pitchMaxBound > PITCH_LAG_MAX) {
         pitchMaxBound = PITCH_LAG_MAX;
         pitchMinBound = pitchMaxBound-6;
      }

      /* Find weighted filter coefficients LPCGama2 and LPCGama1 */

      WeightLPCCoeff_G729(pLPC, GAMMA2_POSTFLT, LPC_ORDER, LPCGama2);
      WeightLPCCoeff_G729(pLPC, GAMMA1_POSTFLT, LPC_ORDER, LPCGama1 );

      /* filtering of synthesis speech by A(z/GAMMA2_POSTFLT) to find ResidualMemory */

      ippsConvBiased_32f(LPCGama2,LPC_ORDER+1,&pSynth[NSbfr],SUBFR_LEN+LPC_ORDER,decoderObj->ResidualMemory,SUBFR_LEN,LPC_ORDER);

      /* pitch postfiltering */

      if (Vad > 1) {

         ippsCrossCorrLagMax_32f64f(decoderObj->ResidualMemory, &decoderObj->ResidualMemory[-pitchMaxBound], SUBFR_LEN, pitchMaxBound-pitchMinBound, &sum, &PitchDelay);
         PitchDelay = (pitchMaxBound-pitchMinBound-PitchDelay) + pitchMinBound;
         fMaxCorr = (Ipp32f)sum;

         /* Compute the energy of the signal delayed by PitchDelay */

         ippsDotProd_32f64f(&decoderObj->ResidualMemory[-PitchDelay], &decoderObj->ResidualMemory[-PitchDelay], SUBFR_LEN, &sum);
         fEnergy = (Ipp32f)(sum+0.5);

         /* Compute the signal energy in the present subframe */

         ippsDotProd_32f64f(decoderObj->ResidualMemory, decoderObj->ResidualMemory, SUBFR_LEN, &sum);
         fEnergy0 = (Ipp32f)(sum+0.5);

         if (fMaxCorr < 0.0) fMaxCorr = 0.0;

         /* prediction gain (dB)= -10 log(1-fMaxCorr*fMaxCorr/(fEnergy*fEnergy0)) */

         fTmp = fMaxCorr*fMaxCorr;
         if (fTmp < fEnergy*fEnergy0*0.5) {       /* if prediction gain < 3 dB   */
            ippsCopy_32f(decoderObj->ResidualMemory, pResidual, SUBFR_LEN);
         } else {
            if (fMaxCorr > fEnergy) {    /* if pitch gain > 1 */
               gainM1 = INV_GAMMA_POSTFLT_G729A;
               gain = GAMMA2_POSTFLT_G729A;
            } else {
               fMaxCorr *= GAMMA_POSTFLT_G729A;
               gain = 1.0f/(fMaxCorr+fEnergy) * fMaxCorr;
               gainM1   = 1.0f - gain;
            }
            ippsInterpolateC_G729_32f(decoderObj->ResidualMemory, gainM1, &decoderObj->ResidualMemory[-PitchDelay], gain, pResidual, SUBFR_LEN);
         }
      } else
         ippsCopy_32f(decoderObj->ResidualMemory,pResidual,SUBFR_LEN);

      /* impulse response of A(z/GAMMA2_POSTFLT)/A(z/GAMMA1_POSTFLT) */

      ippsCopy_32f(LPCGama2, ImpRespPST, LPC_ORDERP1);
      ippsZero_32f(&ImpRespPST[LPC_ORDERP1],PST_IMPRESP_LEN-LPC_ORDERP1);
      ippsSynthesisFilter_G729_32f(LPCGama1, LPC_ORDER, ImpRespPST, ImpRespPST, PST_IMPRESP_LEN, &ImpRespPST[LPC_ORDER+1]);

      /* 1st correlation of impulse response */
      ippsDotProd_32f64f(ImpRespPST, ImpRespPST, PST_IMPRESP_LEN, &sum);
      ippsDotProd_32f64f(ImpRespPST, &ImpRespPST[1], PST_IMPRESP_LEN-1, &sum1);
      if(sum1 <= 0.0) {
         fTmp2 = 0.0;
      } else {
         fTmp1 = (Ipp32f)sum;
         fTmp2 = (Ipp32f)sum1;
         fTmp2 = fTmp2*TILT_FLT_FACTOR/fTmp1;
      }
      ippsPreemphasize_32f_I(fTmp2, pResidual, SUBFR_LEN,&decoderObj->fPreemphMemoryA);

      /* filtering through  1/A(z/GAMMA1_POSTFLT) */

      ippsSynthesisFilter_G729_32f(LPCGama1, LPC_ORDER, pResidual, &pSynthPST[NSbfr], SUBFR_LEN, decoderObj->PstSynMemoryA);
      for (i = 0; i < LPC_ORDER; i++)  decoderObj->PstSynMemoryA[i] =pSynthPST[NSbfr+(SUBFR_LEN-LPC_ORDER)+i];



      /* scale output to input */
      ippsDotProd_32f64f(&pSynthPST[NSbfr], &pSynthPST[NSbfr], SUBFR_LEN, &dGain);
      if(isVarZero(dGain)) {
         decoderObj->fPastGain = 0.f;
      } else {
         ippsDotProd_32f64f(&pSynth[NSbfr], &pSynth[NSbfr], SUBFR_LEN, &dg0);
         if(dg0 > 0) {
            dg0 = sqrt(dg0/ dGain);
            dg0 *=  AGC_FACTOR_1M_G729A;
         }

         /* compute gain(n) = AGC_FACTOR gain(n-1) + (1-AGC_FACTOR)gain_in/dGain */
         ippsGainControl_G729_32f_I((Ipp32f)dg0, AGC_FACTOR_G729A, &pSynthPST[NSbfr], &decoderObj->fPastGain);
      }

      /* update residual memory */

      ippsMove_32f(&decoderObj->ResidualMemory[SUBFR_LEN-PITCH_LAG_MAX], &decoderObj->ResidualMemory[-PITCH_LAG_MAX], PITCH_LAG_MAX);

      pLPC += LPC_ORDERP1;
   }

   /* update pSynth[] buffer */

   ippsCopy_32f(&pSynth[FRM_LEN-LPC_ORDER], decoderObj->PstFltMemoryA, LPC_ORDER);

   /* overwrite synthesis speech by postfiltered synthesis speech */

   ippsCopy_32f(pSynthPST, pSrcDstSynthSpeech, FRM_LEN);

   LOCAL_ARRAY_FREE(Ipp32f, ImpRespPST, PST_IMPRESP_LEN,decoderObj);
   LOCAL_ARRAY_FREE(Ipp32f, LPCGama1, LPC_ORDERP1,decoderObj);
   LOCAL_ARRAY_FREE(Ipp32f, LPCGama2, LPC_ORDERP1,decoderObj);
   LOCAL_ALIGN_ARRAY_FREE(32, Ipp32f, pSynthPST, FRM_LEN,decoderObj);       /* post filtered synthesis speech   */
   LOCAL_ALIGN_ARRAY_FREE(32, Ipp32f, pResidual, SUBFR_LEN,decoderObj);      /* ResidualMemory after pitch postfiltering */
   LOCAL_ALIGN_ARRAY_FREE(32, Ipp32f, SynthBuffer, (FRM_LEN+LPC_ORDER),decoderObj);  /* Synthesis                  */

   return;
}
