/* -----------------------------------------------------------------------
   ffi.c - Copyright (c) 1998 Geoffrey Keating
   
   PowerPC Foreign Function Interface 

   $Id: ffi.c,v 1.1.1.1 1998/11/29 16:48:16 green Exp $

   Permission is hereby granted, free of charge, to any person obtaining
   a copy of this software and associated documentation files (the
   ``Software''), to deal in the Software without restriction, including
   without limitation the rights to use, copy, modify, merge, publish,
   distribute, sublicense, and/or sell copies of the Software, and to
   permit persons to whom the Software is furnished to do so, subject to
   the following conditions:

   The above copyright notice and this permission notice shall be included
   in all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED ``AS IS'', WITHOUT WARRANTY OF ANY KIND, EXPRESS
   OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
   IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY CLAIM, DAMAGES OR
   OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
   ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
   OTHER DEALINGS IN THE SOFTWARE.
   ----------------------------------------------------------------------- */

#include <ffi.h>
#include <ffi_common.h>

#include <stdlib.h>
#include <stdio.h>

#if __GNUC__ > 3 || (__GNUC__ == 3 && __GNUC_MINOR__ > 1)
# define hidden __attribute__ ((visibility ("hidden")))
#else
# define hidden
#endif


extern void ffi_closure_SYSV(void);
extern void hidden ffi_closure_LINUX64(void);

enum {
  /* The assembly depends on these exact flags.  */
  FLAG_RETURNS_NOTHING  = 1 << (31-30), /* These go in cr7 */
  FLAG_RETURNS_FP       = 1 << (31-29),
  FLAG_RETURNS_64BITS   = 1 << (31-28),

  FLAG_ARG_NEEDS_COPY   = 1 << (31- 7),
  FLAG_FP_ARGUMENTS     = 1 << (31- 6), /* cr1.eq; specified by ABI */
  FLAG_4_GPR_ARGUMENTS  = 1 << (31- 5),
  FLAG_RETVAL_REFERENCE = 1 << (31- 4)
};

/* About the SYSV ABI.  */
enum {
  NUM_GPR_ARG_REGISTERS = 8,
  NUM_FPR_ARG_REGISTERS = 8
};
enum { ASM_NEEDS_REGISTERS = 4 };

/* ffi_prep_args_SYSV is called by the assembly routine once stack space
   has been allocated for the function's arguments.

   The stack layout we want looks like this:

   |   Return address from ffi_call_SYSV 4bytes	|	higher addresses
   |--------------------------------------------|
   |   Previous backchain pointer	4	| 	stack pointer here
   |--------------------------------------------|<+ <<<	on entry to
   |   Saved r28-r31			4*4	| |	ffi_call_SYSV
   |--------------------------------------------| |
   |   GPR registers r3-r10		8*4	| |	ffi_call_SYSV
   |--------------------------------------------| |
   |   FPR registers f1-f8 (optional)	8*8	| |
   |--------------------------------------------| |	stack	|
   |   Space for copied structures		| |	grows	|
   |--------------------------------------------| |	down    V
   |   Parameters that didn't fit in registers  | |
   |--------------------------------------------| |	lower addresses
   |   Space for callee's LR		4	| |
   |--------------------------------------------| |	stack pointer here
   |   Current backchain pointer	4	|-/	during
   |--------------------------------------------|   <<<	ffi_call_SYSV

   */

/*@-exportheader@*/
void ffi_prep_args_SYSV(extended_cif *ecif, unsigned *const stack)
/*@=exportheader@*/
{
  const unsigned bytes = ecif->cif->bytes;
  const unsigned flags = ecif->cif->flags;
  
  /* 'stacktop' points at the previous backchain pointer.  */
  unsigned *const stacktop = stack + (ecif->cif->bytes / sizeof(unsigned));

  /* 'gpr_base' points at the space for gpr3, and grows upwards as
     we use GPR registers.  */
  unsigned *gpr_base = stacktop - ASM_NEEDS_REGISTERS - NUM_GPR_ARG_REGISTERS;
  int intarg_count = 0;

  /* 'fpr_base' points at the space for fpr1, and grows upwards as
     we use FPR registers.  */
  double *fpr_base = (double *)gpr_base - NUM_FPR_ARG_REGISTERS;
  int fparg_count = 0;

  /* 'copy_space' grows down as we put structures in it.  It should
     stay 16-byte aligned.  */
  char *copy_space = ((flags & FLAG_FP_ARGUMENTS)
		      ? (char *)fpr_base
		      : (char *)gpr_base);

  /* 'next_arg' grows up as we put parameters in it.  */
  unsigned *next_arg = stack + 2;

  int i;
  ffi_type **ptr;
  double double_tmp;
  void **p_argv;
  size_t struct_copy_size;
  unsigned gprvalue;

  /* Check that everything starts aligned properly.  */
  FFI_ASSERT(((unsigned)(char *)stack & 0xF) == 0);
  FFI_ASSERT(((unsigned)(char *)copy_space & 0xF) == 0);
  FFI_ASSERT(((unsigned)(char *)stacktop & 0xF) == 0);
  FFI_ASSERT((bytes & 0xF) == 0);
  FFI_ASSERT(copy_space >= (char *)next_arg);

  /* Deal with return values that are actually pass-by-reference.  */
  if (flags & FLAG_RETVAL_REFERENCE)
  {
    *gpr_base++ = (unsigned long)(char *)ecif->rvalue;
    intarg_count++;
  }

  /* Now for the arguments.  */
  p_argv = ecif->avalue;
  for (ptr = ecif->cif->arg_types, i = ecif->cif->nargs;
       i > 0;
       i--, ptr++, p_argv++)
    {
      switch ((*ptr)->type)
	{
	case FFI_TYPE_FLOAT:
	  double_tmp = *(float *)*p_argv;
	  if (fparg_count >= NUM_FPR_ARG_REGISTERS)
	    {
	      *(float *)next_arg = (float)double_tmp;
	      next_arg += 1;
	    }
	  else
	    *fpr_base++ = double_tmp;
	  fparg_count++;
	  FFI_ASSERT(flags & FLAG_FP_ARGUMENTS);
	  break;

	case FFI_TYPE_DOUBLE:
	  double_tmp = *(double *)*p_argv;

	  if (fparg_count >= NUM_FPR_ARG_REGISTERS)
	    {
	      if (intarg_count%2 != 0)
		{
		  intarg_count++;
		  next_arg++;
		}
	      *(double *)next_arg = double_tmp;
	      next_arg += 2;
	    }
	  else
	    *fpr_base++ = double_tmp;
	  fparg_count++;
	  FFI_ASSERT(flags & FLAG_FP_ARGUMENTS);
	  break;

	case FFI_TYPE_UINT64:
	case FFI_TYPE_SINT64:
	  if (intarg_count == NUM_GPR_ARG_REGISTERS-1)
	    intarg_count++;
	  if (intarg_count >= NUM_GPR_ARG_REGISTERS)
	    {
	      if (intarg_count%2 != 0)
		{
		  intarg_count++;
		  next_arg++;
		}
	      *(long long *)next_arg = *(long long *)*p_argv;
	      next_arg += 2;
	    }
	  else
	    {
              /* whoops: abi states only certain register pairs
               * can be used for passing long long int
               * specifically (r3,r4), (r5,r6), (r7,r8), 
               * (r9,r10) and if next arg is long long but
               * not correct starting register of pair then skip
               * until the proper starting register
	       */
              if (intarg_count%2 != 0)
                {
                  intarg_count ++;
                  gpr_base++;
                }
	      *(long long *)gpr_base = *(long long *)*p_argv;
	      gpr_base += 2;
	    }
	  intarg_count += 2;
	  break;

	case FFI_TYPE_STRUCT:
#if FFI_TYPE_LONGDOUBLE != FFI_TYPE_DOUBLE
	case FFI_TYPE_LONGDOUBLE:
#endif
	  struct_copy_size = ((*ptr)->size + 15) & ~0xF;
	  copy_space -= struct_copy_size;
	  memcpy(copy_space, (char *)*p_argv, (*ptr)->size);
	  
	  gprvalue = (unsigned long)copy_space;

	  FFI_ASSERT(copy_space > (char *)next_arg);
	  FFI_ASSERT(flags & FLAG_ARG_NEEDS_COPY);
	  goto putgpr;

	case FFI_TYPE_UINT8:
	  gprvalue = *(unsigned char *)*p_argv;
	  goto putgpr;
	case FFI_TYPE_SINT8:
	  gprvalue = *(signed char *)*p_argv;
	  goto putgpr;
	case FFI_TYPE_UINT16:
	  gprvalue = *(unsigned short *)*p_argv;
	  goto putgpr;
	case FFI_TYPE_SINT16:
	  gprvalue = *(signed short *)*p_argv;
	  goto putgpr;

	case FFI_TYPE_INT:
	case FFI_TYPE_UINT32:
	case FFI_TYPE_SINT32:
	case FFI_TYPE_POINTER:
	  gprvalue = *(unsigned *)*p_argv;
	putgpr:
	  if (intarg_count >= NUM_GPR_ARG_REGISTERS)
	    *next_arg++ = gprvalue;
	  else
	    *gpr_base++ = gprvalue;
	  intarg_count++;
	  break;
	}
    }

  /* Check that we didn't overrun the stack...  */
  FFI_ASSERT(copy_space >= (char *)next_arg);
  FFI_ASSERT(gpr_base <= stacktop - ASM_NEEDS_REGISTERS);
  FFI_ASSERT((unsigned *)fpr_base
	     <= stacktop - ASM_NEEDS_REGISTERS - NUM_GPR_ARG_REGISTERS);
  FFI_ASSERT(flags & FLAG_4_GPR_ARGUMENTS || intarg_count <= 4);
}

/* About the LINUX64 ABI.  */
enum {
  NUM_GPR_ARG_REGISTERS64 = 8,
  NUM_FPR_ARG_REGISTERS64 = 13
};
enum { ASM_NEEDS_REGISTERS64 = 4 };

/* ffi_prep_args64 is called by the assembly routine once stack space
   has been allocated for the function's arguments.

   The stack layout we want looks like this:

   |   Ret addr from ffi_call_LINUX64	8bytes	|	higher addresses
   |--------------------------------------------|
   |   CR save area			8bytes	|
   |--------------------------------------------|
   |   Previous backchain pointer	8	| 	stack pointer here
   |--------------------------------------------|<+ <<<	on entry to
   |   Saved r28-r31			4*8	| |	ffi_call_LINUX64
   |--------------------------------------------| |
   |   GPR registers r3-r10		8*8	| |
   |--------------------------------------------| |
   |   FPR registers f1-f13 (optional)	13*8	| |
   |--------------------------------------------| |
   |   Parameter save area		        | |
   |--------------------------------------------| |
   |   TOC save area			8	| |
   |--------------------------------------------| |	stack	|
   |   Linker doubleword		8	| |	grows	|
   |--------------------------------------------| |	down	V
   |   Compiler doubleword		8	| |
   |--------------------------------------------| |	lower addresses
   |   Space for callee's LR		8	| |
   |--------------------------------------------| |
   |   CR save area			8	| |
   |--------------------------------------------| |	stack pointer here
   |   Current backchain pointer	8	|-/	during
   |--------------------------------------------|   <<<	ffi_call_LINUX64

   */

/*@-exportheader@*/
void hidden ffi_prep_args64(extended_cif *ecif, unsigned long *const stack)
/*@=exportheader@*/
{
  const unsigned long bytes = ecif->cif->bytes;
  const unsigned long flags = ecif->cif->flags;

  /* 'stacktop' points at the previous backchain pointer.  */
  unsigned long *const stacktop = stack + (bytes / sizeof(unsigned long));

  /* 'next_arg' points at the space for gpr3, and grows upwards as
     we use GPR registers, then continues at rest.  */
  unsigned long *const gpr_base = stacktop - ASM_NEEDS_REGISTERS64
				  - NUM_GPR_ARG_REGISTERS64;
  unsigned long *const gpr_end = gpr_base + NUM_GPR_ARG_REGISTERS64;
  unsigned long *const rest = stack + 6 + NUM_GPR_ARG_REGISTERS64;
  unsigned long *next_arg = gpr_base;

  /* 'fpr_base' points at the space for fpr3, and grows upwards as
     we use FPR registers.  */
  double *fpr_base = (double *)gpr_base - NUM_FPR_ARG_REGISTERS64;
  int fparg_count = 0;

  int i, words;
  ffi_type **ptr;
  double double_tmp;
  void **p_argv;
  unsigned long gprvalue;

  /* Check that everything starts aligned properly.  */
  FFI_ASSERT(((unsigned long)(char *)stack & 0xF) == 0);
  FFI_ASSERT(((unsigned long)(char *)stacktop & 0xF) == 0);
  FFI_ASSERT((bytes & 0xF) == 0);

  /* Deal with return values that are actually pass-by-reference.  */
  if (flags & FLAG_RETVAL_REFERENCE)
    *next_arg++ = (unsigned long)(char *)ecif->rvalue;

  /* Now for the arguments.  */
  p_argv = ecif->avalue;
  for (ptr = ecif->cif->arg_types, i = ecif->cif->nargs;
       i > 0;
       i--, ptr++, p_argv++)
    {
      switch ((*ptr)->type)
	{
	case FFI_TYPE_FLOAT:
	  double_tmp = *(float *)*p_argv;
	  *(float *)next_arg = (float)double_tmp;
	  if (++next_arg == gpr_end)
	    next_arg = rest;
	  if (fparg_count < NUM_FPR_ARG_REGISTERS64)
	    *fpr_base++ = double_tmp;
	  fparg_count++;
	  FFI_ASSERT(flags & FLAG_FP_ARGUMENTS);
	  break;

	case FFI_TYPE_DOUBLE:
	  double_tmp = *(double *)*p_argv;
	  *(double *)next_arg = double_tmp;
	  if (++next_arg == gpr_end)
	    next_arg = rest;
	  if (fparg_count < NUM_FPR_ARG_REGISTERS64)
	    *fpr_base++ = double_tmp;
	  fparg_count++;
	  FFI_ASSERT(flags & FLAG_FP_ARGUMENTS);
	  break;

#if FFI_TYPE_LONGDOUBLE != FFI_TYPE_DOUBLE
	case FFI_TYPE_LONGDOUBLE:
	  double_tmp = ((double *) *p_argv)[0];
	  *(double *) next_arg = double_tmp;
	  if (++next_arg == gpr_end)
	    next_arg = rest;
	  if (fparg_count < NUM_FPR_ARG_REGISTERS64)
	    *fpr_base++ = double_tmp;
	  fparg_count++;
	  double_tmp = ((double *) *p_argv)[1];
	  *(double *) next_arg = double_tmp;
	  if (++next_arg == gpr_end)
	    next_arg = rest;
	  if (fparg_count < NUM_FPR_ARG_REGISTERS64)
	    *fpr_base++ = double_tmp;
	  fparg_count++;
	  FFI_ASSERT(flags & FLAG_FP_ARGUMENTS);
	  break;
#endif

	case FFI_TYPE_STRUCT:
	  words = ((*ptr)->size + 7) / 8;
	  if (next_arg >= gpr_base && next_arg + words > gpr_end)
	    {
	      size_t first = (char *) gpr_end - (char *) next_arg;
	      memcpy((char *) next_arg, (char *) *p_argv, first);
	      memcpy((char *) rest, (char *) *p_argv + first,
		     (*ptr)->size - first);
	      next_arg = (unsigned long *) ((char *) rest + words * 8 - first);
	    }
	  else
	    {
	      char *where = (char *) next_arg;

	      /* Structures with size less than eight bytes are passed
		 left-padded.  */
	      if ((*ptr)->size < 8)
		where += 8 - (*ptr)->size;

	      memcpy (where, (char *) *p_argv, (*ptr)->size);
	      next_arg += words;
	      if (next_arg == gpr_end)
		next_arg = rest;
	    }
	  break;

	case FFI_TYPE_UINT8:
	  gprvalue = *(unsigned char *)*p_argv;
	  goto putgpr;
	case FFI_TYPE_SINT8:
	  gprvalue = *(signed char *)*p_argv;
	  goto putgpr;
	case FFI_TYPE_UINT16:
	  gprvalue = *(unsigned short *)*p_argv;
	  goto putgpr;
	case FFI_TYPE_SINT16:
	  gprvalue = *(signed short *)*p_argv;
	  goto putgpr;
	case FFI_TYPE_UINT32:
	  gprvalue = *(unsigned int *)*p_argv;
	  goto putgpr;
	case FFI_TYPE_INT:
	case FFI_TYPE_SINT32:
	  gprvalue = *(signed int *)*p_argv;
	  goto putgpr;
	
	case FFI_TYPE_UINT64:
	case FFI_TYPE_SINT64:
	case FFI_TYPE_POINTER:
	  gprvalue = *(unsigned long *)*p_argv;
	putgpr:
	  *next_arg++ = gprvalue;
	  if (next_arg == gpr_end)
	    next_arg = rest;
	  break;
	}
    }

  FFI_ASSERT(flags & FLAG_4_GPR_ARGUMENTS
	     || (next_arg >= gpr_base && next_arg <= gpr_base + 4));
}



/* Perform machine dependent cif processing */
ffi_status ffi_prep_cif_machdep(ffi_cif *cif)
{
  /* All this is for the SYSV and LINUX64 ABI.  */
  int i;
  ffi_type **ptr;
  unsigned bytes;
  int fparg_count = 0, intarg_count = 0;
  unsigned flags = 0;
  unsigned struct_copy_size = 0;
  unsigned type = cif->rtype->type;

  if (cif->abi != FFI_LINUX64)
    {    
      /* All the machine-independent calculation of cif->bytes will be wrong.
	 Redo the calculation for SYSV.  */

      /* Space for the frame pointer, callee's LR, and the asm's temp regs.  */
      bytes = (2 + ASM_NEEDS_REGISTERS) * sizeof(int);

      /* Space for the GPR registers.  */
      bytes += NUM_GPR_ARG_REGISTERS * sizeof(int);
    }
  else
    {
      /* 64-bit ABI.  */

      /* Space for backchain, CR, LR, cc/ld doubleword, TOC and the asm's temp
	 regs.  */
      bytes = (6 + ASM_NEEDS_REGISTERS64) * sizeof(long);

      /* Space for the mandatory parm save area and general registers.  */
      bytes += 2 * NUM_GPR_ARG_REGISTERS64 * sizeof(long);

#if FFI_TYPE_LONGDOUBLE != FFI_TYPE_DOUBLE
      if (type == FFI_TYPE_LONGDOUBLE)
	type = FFI_TYPE_DOUBLE;
#endif
    }

  /* Return value handling.  The rules for SYSV are as follows:
     - 32-bit (or less) integer values are returned in gpr3;
     - Structures of size <= 4 bytes also returned in gpr3;
     - 64-bit integer values and structures between 5 and 8 bytes are returned
       in gpr3 and gpr4;
     - Single/double FP values are returned in fpr1;
     - Larger structures and long double (if not equivalent to double) values
       are allocated space and a pointer is passed as the first argument.
     For LINUX64:
     - integer values in gpr3;
     - Structures/Unions by reference;
     - Single/double FP values in fpr1, long double in fpr1,fpr2.  */
  switch (type)
    {
    case FFI_TYPE_DOUBLE:
      flags |= FLAG_RETURNS_64BITS;
      /* Fall through.  */
    case FFI_TYPE_FLOAT:
      flags |= FLAG_RETURNS_FP;
      break;

    case FFI_TYPE_UINT64:
    case FFI_TYPE_SINT64:
      flags |= FLAG_RETURNS_64BITS;
      break;

    case FFI_TYPE_STRUCT:
      if (cif->abi != FFI_GCC_SYSV && cif->abi != FFI_LINUX64)
	if (cif->rtype->size <= 4)
	  break;
	else if (cif->rtype->size <= 8)
	  {
	    flags |= FLAG_RETURNS_64BITS;
	    break;
	  }
      /* else fall through.  */
#if FFI_TYPE_LONGDOUBLE != FFI_TYPE_DOUBLE
    case FFI_TYPE_LONGDOUBLE:
#endif
      intarg_count++;
      flags |= FLAG_RETVAL_REFERENCE;
      /* Fall through.  */
    case FFI_TYPE_VOID:
      flags |= FLAG_RETURNS_NOTHING;
      break;

    default:
      /* Returns 32-bit integer, or similar.  Nothing to do here.  */
      break;
    }

  if (cif->abi != FFI_LINUX64)
    /* The first NUM_GPR_ARG_REGISTERS words of integer arguments, and the
       first NUM_FPR_ARG_REGISTERS fp arguments, go in registers; the rest
       goes on the stack.  Structures and long doubles (if not equivalent
       to double) are passed as a pointer to a copy of the structure.
       Stuff on the stack needs to keep proper alignment.  */
    for (ptr = cif->arg_types, i = cif->nargs; i > 0; i--, ptr++)
      {
	switch ((*ptr)->type)
	  {
	  case FFI_TYPE_FLOAT:
	    fparg_count++;
	    /* floating singles are not 8-aligned on stack */
	    break;

	  case FFI_TYPE_DOUBLE:
	    fparg_count++;
	    /* If this FP arg is going on the stack, it must be
	       8-byte-aligned.  */
	    if (fparg_count > NUM_FPR_ARG_REGISTERS
		&& intarg_count%2 != 0)
	      intarg_count++;
	    break;

	  case FFI_TYPE_UINT64:
	  case FFI_TYPE_SINT64:
	    /* 'long long' arguments are passed as two words, but
	       either both words must fit in registers or both go
	       on the stack.  If they go on the stack, they must
	       be 8-byte-aligned.  */
	    if (intarg_count == NUM_GPR_ARG_REGISTERS-1
		|| (intarg_count >= NUM_GPR_ARG_REGISTERS
		    && intarg_count%2 != 0))
	      intarg_count++;
	    intarg_count += 2;
	    break;

	  case FFI_TYPE_STRUCT:
#if FFI_TYPE_LONGDOUBLE != FFI_TYPE_DOUBLE
	  case FFI_TYPE_LONGDOUBLE:
#endif
	    /* We must allocate space for a copy of these to enforce
	       pass-by-value.  Pad the space up to a multiple of 16
	       bytes (the maximum alignment required for anything under
	       the SYSV ABI).  */
	    struct_copy_size += ((*ptr)->size + 15) & ~0xF;
	    /* Fall through (allocate space for the pointer).  */

	  default:
	    /* Everything else is passed as a 4-byte word in a GPR, either
	       the object itself or a pointer to it.  */
	    intarg_count++;
	    break;
	  }
      }
  else
    for (ptr = cif->arg_types, i = cif->nargs; i > 0; i--, ptr++)
      {
	switch ((*ptr)->type)
	  {
#if FFI_TYPE_LONGDOUBLE != FFI_TYPE_DOUBLE
	  case FFI_TYPE_LONGDOUBLE:
	    fparg_count += 2;
	    intarg_count += 2;
	    break;
#endif
	  case FFI_TYPE_FLOAT:
	  case FFI_TYPE_DOUBLE:
	    fparg_count++;
	    intarg_count++;
	    break;

	  case FFI_TYPE_STRUCT:
	    intarg_count += ((*ptr)->size + 7) / 8;
	    break;

	  default:
	    /* Everything else is passed as a 8-byte word in a GPR, either
	       the object itself or a pointer to it.  */
	    intarg_count++;
	    break;
	  }
      }

  if (fparg_count != 0)
    flags |= FLAG_FP_ARGUMENTS;
  if (intarg_count > 4)
    flags |= FLAG_4_GPR_ARGUMENTS;
  if (struct_copy_size != 0)
    flags |= FLAG_ARG_NEEDS_COPY;

  if (cif->abi != FFI_LINUX64)
    {
      /* Space for the FPR registers, if needed.  */
      if (fparg_count != 0)
	bytes += NUM_FPR_ARG_REGISTERS * sizeof(double);

      /* Stack space.  */
      if (intarg_count > NUM_GPR_ARG_REGISTERS)
	bytes += (intarg_count - NUM_GPR_ARG_REGISTERS) * sizeof(int);
      if (fparg_count > NUM_FPR_ARG_REGISTERS)
	bytes += (fparg_count - NUM_FPR_ARG_REGISTERS) * sizeof(double);
    }
  else
    {
      /* Space for the FPR registers, if needed.  */
      if (fparg_count != 0)
	bytes += NUM_FPR_ARG_REGISTERS64 * sizeof(double);

      /* Stack space.  */
      if (intarg_count > NUM_GPR_ARG_REGISTERS64)
	bytes += (intarg_count - NUM_GPR_ARG_REGISTERS64) * sizeof(long);
    }

  /* The stack space allocated needs to be a multiple of 16 bytes.  */
  bytes = (bytes + 15) & ~0xF;

  /* Add in the space for the copied structures.  */
  bytes += struct_copy_size;

  cif->flags = flags;
  cif->bytes = bytes;

  return FFI_OK;
}

/*@-declundef@*/
/*@-exportheader@*/
extern void ffi_call_SYSV(/*@out@*/ extended_cif *, 
			  unsigned, unsigned, 
			  /*@out@*/ unsigned *, 
			  void (*fn)());
extern void hidden ffi_call_LINUX64(/*@out@*/ extended_cif *, 
				    unsigned long, unsigned long,
				    /*@out@*/ unsigned long *, 
				    void (*fn)());
/*@=declundef@*/
/*@=exportheader@*/

void ffi_call(/*@dependent@*/ ffi_cif *cif, 
	      void (*fn)(), 
	      /*@out@*/ void *rvalue, 
	      /*@dependent@*/ void **avalue)
{
  extended_cif ecif;

  ecif.cif = cif;
  ecif.avalue = avalue;
  
  /* If the return value is a struct and we don't have a return	*/
  /* value address then we need to make one		        */

  if ((rvalue == NULL) && 
      (cif->rtype->type == FFI_TYPE_STRUCT))
    {
      /*@-sysunrecog@*/
      ecif.rvalue = alloca(cif->rtype->size);
      /*@=sysunrecog@*/
    }
  else
    ecif.rvalue = rvalue;
    
  
  switch (cif->abi) 
    {
#ifndef POWERPC64
    case FFI_SYSV:
    case FFI_GCC_SYSV:
      /*@-usedef@*/
      ffi_call_SYSV(&ecif, -cif->bytes, 
		    cif->flags, ecif.rvalue, fn);
      /*@=usedef@*/
      break;
#else
    case FFI_LINUX64:
      /*@-usedef@*/
      ffi_call_LINUX64(&ecif, -(long) cif->bytes,
		       cif->flags, ecif.rvalue, fn);
      /*@=usedef@*/
      break;
#endif
    default:
      FFI_ASSERT(0);
      break;
    }
}


#ifndef POWERPC64
static void flush_icache(char *, int);

#define MIN_CACHE_LINE_SIZE 8

static void flush_icache(char * addr1, int size)
{
  int i;
  char * addr;
  for (i = 0; i < size; i += MIN_CACHE_LINE_SIZE) {
     addr = addr1 + i;
     __asm__ volatile ("icbi 0,%0;" "dcbf 0,%0;" : : "r"(addr) : "memory");
  }
  addr = addr1 + size - 1;
  __asm__ volatile ("icbi 0,%0;" "dcbf 0,%0;" "sync;" "isync;" : : "r"(addr) : "memory");
}
#endif

ffi_status
ffi_prep_closure (ffi_closure* closure,
		  ffi_cif* cif,
		  void (*fun)(ffi_cif*, void*, void**, void*),
		  void *user_data)
{
#ifdef POWERPC64
  void **tramp = (void **) &closure->tramp[0];

  FFI_ASSERT (cif->abi == FFI_LINUX64);
  /* Copy function address and TOC from ffi_closure_LINUX64.  */
  memcpy (tramp, (char *) ffi_closure_LINUX64, 16);
  tramp[2] = (void *) closure;
#else
  unsigned int *tramp;

  FFI_ASSERT (cif->abi == FFI_GCC_SYSV);

  tramp = (unsigned int *) &closure->tramp[0];
  tramp[0] = 0x7c0802a6;  /*   mflr    r0 */
  tramp[1] = 0x4800000d;  /*   bl      10 <trampoline_initial+0x10> */
  tramp[4] = 0x7d6802a6;  /*   mflr    r11 */
  tramp[5] = 0x7c0803a6;  /*   mtlr    r0 */
  tramp[6] = 0x800b0000;  /*   lwz     r0,0(r11) */
  tramp[7] = 0x816b0004;  /*   lwz     r11,4(r11) */
  tramp[8] = 0x7c0903a6;  /*   mtctr   r0 */
  tramp[9] = 0x4e800420;  /*   bctr */
  *(void **) &tramp[2] = (void *)ffi_closure_SYSV; /* function */
  *(void **) &tramp[3] = (void *)closure;          /* context */

  /* Flush the icache.  */
  flush_icache(&closure->tramp[0],FFI_TRAMPOLINE_SIZE);
#endif

  closure->cif = cif;
  closure->fun = fun;
  closure->user_data = user_data;

  return FFI_OK;
}

typedef union
{
  float f;
  double d;
} ffi_dblfl;

int ffi_closure_helper_SYSV (ffi_closure*, void*, unsigned long*, 
			     ffi_dblfl*, unsigned long*);

/* Basically the trampoline invokes ffi_closure_SYSV, and on 
 * entry, r11 holds the address of the closure.
 * After storing the registers that could possibly contain
 * parameters to be passed into the stack frame and setting
 * up space for a return value, ffi_closure_SYSV invokes the 
 * following helper function to do most of the work
 */

int
ffi_closure_helper_SYSV (ffi_closure* closure, void * rvalue, 
            unsigned long * pgr, ffi_dblfl * pfr, 
            unsigned long * pst)
{
  /* rvalue is the pointer to space for return value in closure assembly */
  /* pgr is the pointer to where r3-r10 are stored in ffi_closure_SYSV */
  /* pfr is the pointer to where f1-f8 are stored in ffi_closure_SYSV  */
  /* pst is the pointer to outgoing parameter stack in original caller */

  void **          avalue;
  ffi_type **      arg_types;
  long             i, avn;
  long             nf;   /* number of floating registers already used */
  long             ng;   /* number of general registers already used */
  ffi_cif *        cif; 
  double           temp; 

  cif = closure->cif;
  avalue = alloca(cif->nargs * sizeof(void *));

  nf = 0;
  ng = 0;

  /* Copy the caller's structure return value address so that the closure
     returns the data directly to the caller.  */
  if (cif->rtype->type == FFI_TYPE_STRUCT)
    {
      rvalue = (void *) *pgr;
      ng++;
      pgr++;
    }

  i = 0;
  avn = cif->nargs;
  arg_types = cif->arg_types;
  
  /* Grab the addresses of the arguments from the stack frame.  */
  while (i < avn)
    {
      switch (arg_types[i]->type)
	{
	case FFI_TYPE_SINT8:
	case FFI_TYPE_UINT8:
	/* there are 8 gpr registers used to pass values */
          if (ng < 8) {
	     avalue[i] = (((char *)pgr)+3);
             ng++;
             pgr++;
          } else {
             avalue[i] = (((char *)pst)+3);
             pst++;
          }
	  break;
           
	case FFI_TYPE_SINT16:
	case FFI_TYPE_UINT16:
	/* there are 8 gpr registers used to pass values */
          if (ng < 8) {
	     avalue[i] = (((char *)pgr)+2);
             ng++;
             pgr++;
          } else {
             avalue[i] = (((char *)pst)+2);
             pst++;
          }
	  break;

	case FFI_TYPE_SINT32:
	case FFI_TYPE_UINT32:
	case FFI_TYPE_POINTER:
	  /* there are 8 gpr registers used to pass values */
          if (ng < 8) {
	     avalue[i] = pgr;
             ng++;
             pgr++;
          } else {
             avalue[i] = pst;
             pst++;
          }
	  break;

	case FFI_TYPE_STRUCT:
	  /* Structs are passed by reference. The address will appear in a 
	     gpr if it is one of the first 8 arguments.  */
          if (ng < 8) {
	     avalue[i] = (void *) *pgr;
             ng++;
             pgr++;
          } else {
             avalue[i] = (void *) *pst;
             pst++;
          }
	  break;

	case FFI_TYPE_SINT64:
	case FFI_TYPE_UINT64:
	  /* passing long long ints are complex, they must
           * be passed in suitable register pairs such as
           * (r3,r4) or (r5,r6) or (r6,r7), or (r7,r8) or (r9,r10)
           * and if the entire pair aren't available then the outgoing
           * parameter stack is used for both but an alignment of 8
           * must will be kept.  So we must either look in pgr
           * or pst to find the correct address for this type
           * of parameter.
           */
           if (ng < 7) {
              if (ng & 0x01) {
		/* skip r4, r6, r8 as starting points */
                  ng++;
                  pgr++;
              }
              avalue[i] = pgr;
              ng+=2;
              pgr+=2;
           } else {
              if (((long)pst) & 4) pst++;
              avalue[i] = pst;
              pst+=2;
           }
           break;

	case FFI_TYPE_FLOAT:
	    /* unfortunately float values are stored as doubles
             * in the ffi_closure_SYSV code (since we don't check
             * the type in that routine).
             */

          /* there are 8 64bit floating point registers */

          if (nf < 8) {
             temp = pfr->d;
             pfr->f = (float)temp;
             avalue[i] = pfr;
             nf++;
             pfr++;
          } else {
	    /* FIXME? here we are really changing the values
             * stored in the original calling routines outgoing
             * parameter stack.  This is probably a really
             * naughty thing to do but...
             */
	     avalue[i] = pst;
             nf++;
             pst+=1;
          }
	  break;

	case FFI_TYPE_DOUBLE:
	  /* On the outgoing stack all values are aligned to 8 */
          /* there are 8 64bit floating point registers */

          if (nf < 8) {
	     avalue[i] = pfr;
             nf++;
             pfr++;
          } else {
	     if (((long)pst) & 4) pst++;
	     avalue[i] = pst;
             nf++;
             pst+=2;
          }
	  break;

	default:
	  FFI_ASSERT(0);
	}

      i++;
    }


  (closure->fun) (cif, rvalue, avalue, closure->user_data);

  /* Tell ffi_closure_SYSV how to perform return type promotions.  */
  return cif->rtype->type;

}

int hidden ffi_closure_helper_LINUX64 (ffi_closure*, void*, unsigned long*,
				       ffi_dblfl*);

int hidden
ffi_closure_helper_LINUX64 (ffi_closure *closure, void *rvalue, 
			    unsigned long *pst, ffi_dblfl *pfr)
{
  /* rvalue is the pointer to space for return value in closure assembly */
  /* pst is the pointer to parameter save area
     (r3-r10 are stored into its first 8 slots by ffi_closure_LINUX64) */
  /* pfr is the pointer to where f1-f13 are stored in ffi_closure_LINUX64 */

  void **avalue;
  ffi_type **arg_types;
  long i, avn;
  ffi_cif *cif; 
  ffi_dblfl *end_pfr = pfr + NUM_FPR_ARG_REGISTERS64;

  cif = closure->cif;
  avalue = alloca (cif->nargs * sizeof (void *));

  /* Copy the caller's structure return value address so that the closure
     returns the data directly to the caller.  */
  if (cif->rtype->type == FFI_TYPE_STRUCT)
    {
      rvalue = (void *) *pst;
      pst++;
    }

  i = 0;
  avn = cif->nargs;
  arg_types = cif->arg_types;
  
  /* Grab the addresses of the arguments from the stack frame.  */
  while (i < avn)
    {
      switch (arg_types[i]->type)
	{
	case FFI_TYPE_SINT8:
	case FFI_TYPE_UINT8:
	  avalue[i] = (char *) pst + 7;
	  pst++;
	  break;
           
	case FFI_TYPE_SINT16:
	case FFI_TYPE_UINT16:
	  avalue[i] = (char *) pst + 6;
	  pst++;
	  break;

	case FFI_TYPE_SINT32:
	case FFI_TYPE_UINT32:
	  avalue[i] = (char *) pst + 4;
	  pst++;
	  break;

	case FFI_TYPE_SINT64:
	case FFI_TYPE_UINT64:
	case FFI_TYPE_POINTER:
	  avalue[i] = pst;
	  pst++;
	  break;

	case FFI_TYPE_STRUCT:
	  /* Structures with size less than eight bytes are passed
	     left-padded.  */
	  if (arg_types[i]->size < 8)
	    avalue[i] = (char *) pst + 8 - arg_types[i]->size;
	  else
	    avalue[i] = pst;
	  pst += (arg_types[i]->size + 7) / 8;
	  break;

	case FFI_TYPE_FLOAT:
	  /* unfortunately float values are stored as doubles
           * in the ffi_closure_LINUX64 code (since we don't check
           * the type in that routine).
           */

          /* there are 13 64bit floating point registers */

          if (pfr < end_pfr)
	    {
	      double temp = pfr->d;
	      pfr->f = (float) temp;
	      avalue[i] = pfr;
	      pfr++;
	    }
	  else
	    avalue[i] = pst;
	  pst++;
	  break;

	case FFI_TYPE_DOUBLE:
	  /* On the outgoing stack all values are aligned to 8 */
          /* there are 13 64bit floating point registers */

	  if (pfr < end_pfr)
	    {
	      avalue[i] = pfr;
	      pfr++;
	    }
	  else
	    avalue[i] = pst;
	  pst++;
	  break;

#if FFI_TYPE_LONGDOUBLE != FFI_TYPE_DOUBLE
	case FFI_TYPE_LONGDOUBLE:
	  if (pfr + 1 < end_pfr)
	    {
	      avalue[i] = pfr;
	      pfr += 2;
	    }
	  else
	    {
	      if (pfr < end_pfr)
		{
		  /* Passed partly in f13 and partly on the stack.
		     Move it all to the stack.  */
		  *pst = *(unsigned long *) pfr;
		  pfr++;
		}
	      avalue[i] = pst;
	    }
	  pst += 2;
	  break;
#endif

	default:
	  FFI_ASSERT(0);
	}

      i++;
    }


  (closure->fun) (cif, rvalue, avalue, closure->user_data);

  /* Tell ffi_closure_LINUX64 how to perform return type promotions.  */
  return cif->rtype->type;
}
