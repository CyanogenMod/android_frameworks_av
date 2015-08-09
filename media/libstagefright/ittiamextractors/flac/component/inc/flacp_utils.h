/*****************************************************************************/
/*                                                                           */
/*                               FLAC PARSER(FLACP)                          */
/*                     ITTIAM SYSTEMS PVT LTD, BANGALORE                     */
/*                             COPYRIGHT(C) 2009                             */
/*                                                                           */
/*  This program  is  proprietary to  Ittiam  Systems  Private  Limited  and */
/*  is protected under Indian  Copyright Law as an unpublished work. Its use */
/*  and  disclosure  is  limited by  the terms  and  conditions of a license */
/*  agreement. It may not be copied or otherwise  reproduced or disclosed to */
/*  persons outside the licensee's organization except in accordance with the*/
/*  terms  and  conditions   of  such  an  agreement.  All  copies  and      */
/*  reproductions shall be the property of Ittiam Systems Private Limited and*/
/*  must bear this notice in its entirety.                                   */
/*                                                                           */
/*****************************************************************************/

/*****************************************************************************/
/*                                                                           */
/*  File Name         : flacp_utils.h                                        */
/*                                                                           */
/*  Description       : This file contains utility structures and function   */
/*                      declarations                                         */
/*                                                                           */
/*  List of Functions : None                                                 */
/*                                                                           */
/*  Issues / Problems : None                                                 */
/*                                                                           */
/*  Revision History  :                                                      */
/*                                                                           */
/*         DD MM YYYY   Author(s)       Changes (Describe the changes made)  */
/*         28 03 2009   Ittiam          Draft                                */
/*                                                                           */
/*****************************************************************************/

#ifndef FLACP_UTILS_H
#define FLACP_UTILS_H

/*****************************************************************************/
/* C linkage specifiers for C++ declarations                                 */
/*****************************************************************************/

#ifdef __cplusplus
extern "C"
{
#endif  /* __cplusplus */

/*****************************************************************************/
/* Constant Macros                                                           */
/*****************************************************************************/

/* Used to read an unsigned 16-bit word from the buffer */
#define READ_BUF_U16(buf)   (UWORD16)(((*(buf))<<8)|(*(buf+1)))

/* Used to extract a number of bits from a byte */
#define BITS(val, msb, lsb) ((((val) >> lsb) & ((1 << (msb - lsb + 1)) - 1)))

/* Used to extract 1 bit from a byte */
#define BIT(val, bit)       (((val) >> bit) & 0x1)

/* Used to convert to FOURCC type */
#define FOURCC(a,b,c,d) ((UWORD32) (((UWORD32)(a) & 0x000000FF) << 24) + \
                                   (((UWORD32)(b) & 0x000000FF) << 16) + \
                                   (((UWORD32)(c) & 0x000000FF) << 8) + \
                                   (((UWORD32)(d) & 0x000000FF)))

/* Macro to check whether  block is last block in the input file or not */
#define IS_LAST_BLOCK(block_header) (0x80 & block_header)

/* Macro to check whether frame sync word or not. 0xFFF8 is used as sync word*/
/* for constant frame size files. 0xFFF9 is used as sync word for variable	 */
/* frame size file files													 */
#define IS_FRAME_SYNC_WORD(frame_id) ((0xFFF8 == frame_id) || \
									  (0xFFF9 == frame_id))

/*****************************************************************************/
/* Constants                                                                 */
/*****************************************************************************/

/* Size of the buffer to be provided by system if seek is required along 	 */
/* with target sample number to seek to										 */
#define FLACP_SEEK_BUF_SIZE   	  (1024)

/* Minimum memory to be provided to store seek table block information if    */
/* available in the input file. 18 bytes is the size of one seek entry		 */
#define FLACP_MIN_SEEK_TABLE_SIZE (18)

/* bits */
#define BYTE_SIZE			 (8)

/* Buffer used internally to read stream info and frame header */
#define INTERNAL_BUF_SIZE	 (64)

/* Block identification code */
#define BLOCK_HEADER		 (0x7F)

/* Stream info metadata block size */
#define STREAM_INFO_BLOCK_SIZE (40)

/* Block header id size */
#define BLOCK_HEADER_ID_SIZE (4)

/* One seektable metadata block entry size */
#define SEEKTABLE_ENTRY_SIZE (18)

/* Maximum frame header size */
#define MAX_FRAME_HEADER_SIZE (16)

/*****************************************************************************/
/* Structures                                                                */
/*****************************************************************************/

/* This structure contains the memory details of a handle. This includes the */
/* pointer to the allocated memory block and the amount of memory that has   */
/* been used up so far                                                       */
typedef struct
{
     void    *memory_ptr;
     UWORD32 used_memory;
     UWORD32 total_memory;
} memory_handle_t;

/******************************************************************************/
/* Extern Function Declarations                                               */
/******************************************************************************/

/* Used to read a unsigned 32-bit integer from the buffer */
extern UWORD32 flacp_read_buf_u32(IN UWORD8 *buf);

/* Used to read a signed 32-bit integer from the buffer */
extern WORD32 flacp_read_buf_s32(IN UWORD8 *buf);

/* Used to read a unsigned 24-bit integer from the buffer */
extern UWORD32 flacp_read_buf_u24(IN UWORD8 *buf);

/* Used to read a unsigned 32-bit integer from the buffer */
extern UWORD64 flacp_read_buf_u64(IN UWORD8 *buf);

/* Used to read a signed 64-bit integer from the buffer */
extern WORD64 flacp_read_buf_s64(IN UWORD8 *buf);

/* Allocate 4 byte aligned memory of required size from the memory passed */
extern void *flacp_allocate_memory(IN void *memory_handle, IN UWORD32 size);

/* Function to read data from the input file */
extern WORD32 flacp_read_data_into_buf(IN  flacp_cb_funcs_t *cb_funcs,
                                       IN  WORD64           offset,
                                       IN  UWORD32          data_len,
                                       OUT void             *buf);

/* Function to convert UTF-8 encoded data into UWORD64 format */
extern void flacp_utf8_to_u64(IN  UWORD8  *buf, INOUT UWORD32 *buf_offset,
							  OUT UWORD64 *data);

/* Function to convert UTF-8 encoded data into UWORD32 format */
extern void flacp_utf8_to_u32(IN  UWORD8  *buf, INOUT UWORD32 *buf_offset,
							  OUT UWORD32 *data);

/*****************************************************************************/
/* C linkage specifiers for C++ declarations                                 */
/*****************************************************************************/

#ifdef __cplusplus
}                       /* End of extern "C" { */
#endif  /* __cplusplus */

#endif /* End of FLACP_UTILS_H */
