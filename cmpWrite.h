/*
 * cmpWrite.h --
 *
 *  Declarations of the interfaces exported by the Compiler package.
 *
 * Copyright (c) 1998 by Scriptics Corporation.
 *
 *  Copyright (c) 2018 ActiveState Software Inc.
 *  Released under the BSD-3 license. See LICENSE file for details.
 *
 * RCS: @(#) $Id: cmpWrite.h,v 1.2 2002/12/02 17:44:01 andreas_kupries Exp $
 */

#ifndef _CMPWRITE
#define _CMPWRITE

#include "tcl.h"

/*
 * The default error message displayed when we cannot find a Loader package:
 * variable name in the Compiler package, and default value.
 */

#define LOADER_ERROR_VARIABLE "LoaderError"
#define LOADER_ERROR_MESSAGE "The bytecode loader is not available or does not support the correct version"

/*
 *----------------------------------------------------------------
 * Procedures exported by cmpWrite.c and cmpWPkg.c
 *----------------------------------------------------------------
 */

#undef TCL_STORAGE_CLASS
#ifdef BUILD_tclcompiler
#define TCL_STORAGE_CLASS DLLEXPORT
#else
#define TCL_STORAGE_CLASS DLLIMPORT
#endif

EXTERN Tcl_ObjCmdProc Compiler_CompileObjCmd;
EXTERN int Compiler_CompileFile(Tcl_Interp* interp, char* inFilePtr, char* outFilePtr, char* preamblePtr);
EXTERN int Compiler_CompileObj(Tcl_Interp* interp, Tcl_Obj* objPtr);
EXTERN Tcl_ObjCmdProc Compiler_GetBytecodeExtensionObjCmd;

EXTERN const char* CompilerGetPackageName(void);
EXTERN int Compiler_Init(Tcl_Interp* interp);

/*
 * Declarations for functions defined in this file.
 */

EXTERN int Tclcompiler_Init(Tcl_Interp* interp);

#undef TCL_STORAGE_CLASS
#define TCL_STORAGE_CLASS DLLIMPORT

#endif /* _CMPWRITE */
