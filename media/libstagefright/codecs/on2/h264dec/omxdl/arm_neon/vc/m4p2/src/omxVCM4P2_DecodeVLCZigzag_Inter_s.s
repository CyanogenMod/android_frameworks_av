;//
;// Copyright (C) 2007-2008 ARM Limited
;//
;// Licensed under the Apache License, Version 2.0 (the "License");
;// you may not use this file except in compliance with the License.
;// You may obtain a copy of the License at
;//
;//      http://www.apache.org/licenses/LICENSE-2.0
;//
;// Unless required by applicable law or agreed to in writing, software
;// distributed under the License is distributed on an "AS IS" BASIS,
;// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
;// See the License for the specific language governing permissions and
;// limitations under the License.
;//
;/**
; * 
; * File Name:  omxVCM4P2_DecodeVLCZigzag_Inter_s.s
; * OpenMAX DL: v1.0.2
; * Revision:   12290
; * Date:       Wednesday, April 9, 2008
; * 
; * 
; * 
; *
; * Description: 
; * Contains modules for zigzag scanning and VLC decoding
; * for inter block.
; *
; *
; *
; * Function: omxVCM4P2_DecodeVLCZigzag_Inter
; *
; * Description:
; * Performs VLC decoding and inverse zigzag scan for one inter coded block.
; *
; * Remarks:
; *
; * Parameters:
; * [in]    ppBitStream        pointer to the pointer to the current byte in
; *                    the bitstream buffer
; * [in]    pBitOffset        pointer to the bit position in the byte pointed
; *                    to by *ppBitStream. *pBitOffset is valid within    [0-7].
; * [in] shortVideoHeader     binary flag indicating presence of short_video_header;
; *                           escape modes 0-3 are used if shortVideoHeader==0,
; *                           and escape mode 4 is used when shortVideoHeader==1.
; * [out]    ppBitStream        *ppBitStream is updated after the block is
; *                    decoded, so that it points to the current byte
; *                    in the bit stream buffer
; * [out]    pBitOffset        *pBitOffset is updated so that it points to the
; *                    current bit position in the byte pointed by
; *                    *ppBitStream
; * [out]    pDst            pointer to the coefficient buffer of current
; *                    block. Must be 16-byte aligned
; *
; * Return Value:
; * OMX_Sts_BadArgErr - bad arguments
; *   -At least one of the following pointers is NULL: ppBitStream, *ppBitStream, pBitOffset, pDst, or
; *   -pDst is not 16-byte aligned, or
; *   -*pBitOffset exceeds [0,7].
; * OMX_Sts_Err - status error
; *   -At least one mark bit is equal to zero
; *   -Encountered an illegal stream code that cannot be found in the VLC table
; *   -Encountered and illegal code in the VLC FLC table
; *   -The number of coefficients is greater than 64
; *
; */


      INCLUDE omxtypes_s.h
      INCLUDE armCOMM_s.h
      INCLUDE armCOMM_BitDec_s.h


      M_VARIANTS ARM1136JS

     



     IF ARM1136JS
     
        ;// Import various tables needed for the function

        
        IMPORT          armVCM4P2_InterVlcL0L1             ;// Contains optimized and packed VLC Tables for both Last =1 and last=0
                                                               ;// Packed in Run:Level:Last format
        IMPORT          armVCM4P2_InterL0L1LMAX            ;// Contains LMAX table entries with both Last=0 and Last=1
        IMPORT          armVCM4P2_InterL0L1RMAX            ;// Contains RMAX table entries with both Last=0 and Last=1
        IMPORT          armVCM4P2_aClassicalZigzagScan     ;// contains classical Zigzag table entries with double the original values
        IMPORT          armVCM4P2_DecodeVLCZigzag_AC_unsafe



;//Input Arguments

ppBitStream          RN 0
pBitOffset           RN 1
pDst                 RN 2
shortVideoHeader     RN 3

;//Local Variables

Return               RN 0

pVlcTableL0L1        RN 4
pLMAXTableL0L1       RN 4
pRMAXTableL0L1       RN 4
pZigzagTable         RN 4
Count                RN 6


        
        ;// Allocate stack memory to store the VLC,Zigzag,LMAX and RMAX tables
     
        
        M_ALLOC4        ppVlcTableL0L1,4
        M_ALLOC4        ppLMAXTableL0L1,4
        M_ALLOC4        ppRMAXTableL0L1,4
        M_ALLOC4        ppZigzagTable,4
        
        
        M_START omxVCM4P2_DecodeVLCZigzag_Inter,r12

        

        
        LDR             pZigzagTable, =armVCM4P2_aClassicalZigzagScan       ;// Load zigzag table
        M_STR           pZigzagTable,ppZigzagTable                              ;// Store zigzag table on stack to pass as argument to unsafe function
        LDR             pVlcTableL0L1, =armVCM4P2_InterVlcL0L1              ;// Load optimized VLC table with both L=0 and L=1 entries
        M_STR           pVlcTableL0L1,ppVlcTableL0L1                            ;// Store optimized VLC table address on stack
        LDR             pLMAXTableL0L1, =armVCM4P2_InterL0L1LMAX            ;// Load Interleaved L=0 and L=1 LMAX Tables
        M_STR           pLMAXTableL0L1,ppLMAXTableL0L1                          ;// Store LMAX table address on stack
        LDR             pRMAXTableL0L1, =armVCM4P2_InterL0L1RMAX            ;// Load Interleaved L=0 and L=1 RMAX Tables
        MOV             Count,#0                                                ;// set start=0
        M_STR           pRMAXTableL0L1,ppRMAXTableL0L1                          ;// store RMAX table address on stack
                

        BL              armVCM4P2_DecodeVLCZigzag_AC_unsafe                 ;// call Unsafe Function for VLC Zigzag Decoding
         
       

        M_END
        ENDIF
        
        END
