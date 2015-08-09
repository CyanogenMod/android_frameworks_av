/*****************************************************************************/
/*                                                                           */
/*                 Ittiam CAST Type Definitions and Constants                */
/*                     ITTIAM SYSTEMS PVT LTD, BANGALORE                     */
/*                             COPYRIGHT(C) 2007                             */
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
/*  File Name         : cast_types.h                                         */
/*                                                                           */
/*  Description       : This file contains all the necessary constants and   */
/*                      type definitions according to CAST specifications.   */
/*                                                                           */
/*                                                                           */
/*  Issues / Problems : None                                                 */
/*                                                                           */
/*  Revision History  :                                                      */
/*                                                                           */
/*         DD MM YYYY   Author(s)       Changes (Describe the changes made)  */
/*         28 09 2006   Ittiam          Draft                                */
/*                                                                           */
/*****************************************************************************/

#ifndef CAST_TYPES_H
#define CAST_TYPES_H

/*****************************************************************************/
/* Constants                                                                 */
/*****************************************************************************/

/* Indicates the input parameter state                                */    

/* Parameter declared with IN, Holds Input value                      */    
#define  IN

/* Parameter declared with OUT, will be used to hold output value     */    
#define  OUT

/* Parameter declared with INOUT, will have input value and will hold */	 
/* output value 													  */	 
#define  INOUT

/*****************************************************************************/
/* Typedefs                                                                  */
/*****************************************************************************/

/* Typedef's for built-in  datatypes */
typedef char           WORD8;
typedef unsigned char  UWORD8;

typedef short          WORD16;
typedef unsigned short UWORD16;

typedef int            WORD32;
typedef unsigned int   UWORD32;

typedef float          FLOAT;
typedef double         DOUBLE;

/*****************************************************************************/
/* Structures                                                                */
/*****************************************************************************/

/* Defined to hold the unsigned 64 bit data */
typedef struct
{
    UWORD32 lsw;  /* Holds lower 32 bits */
    UWORD32 msw;  /* Holds upper 32 bits */
} UWORD64;

/* Defined to hold the signed 64 bit data */
typedef struct
{
    UWORD32 lsw;  /* Holds lower 32 bits */
    WORD32  msw;  /* Holds upper 32 bits */
} WORD64;

#endif /* CAST_TYPES_H */
