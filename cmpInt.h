/*
 * cmpInt.h --
 *
 *  Internal header file for the Compiler/Loader package.
 *  This header defines a number of macros that are used by both the writer
 *  and reader package to initialize some static variables. We use macros
 *  because the writer and the reader are two separate packages, and we don't
 *  want to share code between the two.
 *
 * Copyright (c) 1998-2000 Ajuba Solutions
 * Copyright (c) 2002-2014, 2017 ActiveState Software Inc.
 *
 * See the file "license.terms" for information on usage and redistribution
 * of this file, and for a DISCLAIMER OF ALL WARRANTIES.
 *
 * RCS: @(#) $Id: cmpInt.h,v 1.6 2002/12/02 17:42:02 andreas_kupries Exp $
 */

#ifndef _CMPINT_H
#define _CMPINT_H

#include <stdint.h>
#define HAVE_INTPTR_T
#include "tclCompile.h"
#include "tclInt.h"
#include "tclPort.h"

/*
 * Polyfills for missing definitions in Tcl 8
 */
#if TCL_MAJOR_VERSION < 9
#ifdef Tcl_Size
#undef Tcl_Size
typedef int Tcl_Size;
#endif
#define Tcl_GetSizeIntFromObj Tcl_GetIntFromObj
#define Tcl_NewSizeIntObj Tcl_NewIntObj
#define TCL_SIZE_MAX      INT_MAX
#define TCL_SIZE_MODIFIER ""
typedef union Tcl_ObjInternalRep {
    long longValue;
    double doubleValue;
    void *otherValuePtr;
    Tcl_WideInt wideValue;
    struct {
        void *ptr1;
        void *ptr2;
    } twoPtrValue;
    struct {
        void *ptr;
        unsigned long value;
    } ptrAndLongRep;
} Tcl_ObjInternalRep;

static inline int
Tcl_HasInternalRep(Tcl_Obj *objPtr, const Tcl_ObjType *type) {
    return objPtr->typePtr == type;
}
static inline Tcl_ObjInternalRep *
Tcl_FetchInternalRep(Tcl_Obj *objPtr, const Tcl_ObjType *type) {
    return Tcl_HasInternalRep(objPtr, type) ?
        (Tcl_ObjInternalRep *)(&objPtr->internalRep)
        : NULL;
}
static inline void
Tcl_FreeInternalRep(Tcl_Obj *objPtr) {
    if (objPtr->typePtr != NULL) {
	if (objPtr->typePtr->freeIntRepProc != NULL) {
	    objPtr->typePtr->freeIntRepProc(objPtr);
	}
	objPtr->typePtr = NULL;
    }
}
#endif /* TCL_MAJOR_VERSION < 8 */

/*
 * USE_CATCH_WRAPPER controls whether the emitted code has a catch around
 * the call to loader::bceval and code to strip off the additional back trace
 * from the error info
 */
#define USE_CATCH_WRAPPER 0

/*
 * When version numbers change here, must also go into the following files
 * and update the version numbers:
 *
 * src/configure.in
 * src/makefile.bc
 * src/makefile.vc
 *
 * The release level should be  0 for alpha, 1 for beta, and 2 for
 * final/patch.  The release serial value is the number that follows the
 * "a", "b", or "p" in the patch level; for example, if the patch level
 * is 7.6b2, TCL_RELEASE_SERIAL is 2.  It restarts at 1 whenever the
 * release level is changed, except for the final release which is 0
 * (the first patch will start at 1).
 */

#define CMP_MAJOR_VERSION 1
#define CMP_MINOR_VERSION 9
#define CMP_RELEASE_LEVEL 0
#define CMP_RELEASE_SERIAL 0

#define CMP_VERSION "1.9"
#define CMP_PATCH_LEVEL "1.9.0"

/*
 * This macro includes code that emits and reads the location map for a
 * ByteCode struct. The location map is useful only if the source is shipped
 * (which does not happen currently), but we need to populate a location
 * map in the ByteCode because some code in TCL core needs (for example, the
 * code that generates errorInfo uses this map to find the source of the
 * command corresponding to the pc that caused an exception).
 *
 * If EMIT_SRCMAP is 1, both the code arrays and the source arrays from the
 * location map are emitted and extracted.
 * If it is 0, only the code arrays are emitted; the source arrays are
 * generated using the dummy source noSourceCode.
 */
/* #define EMIT_SRCMAP 0 */

/*
 * structure to hold the calculated lengths of the location information
 * arrays for a ByteCode structure
 */

typedef struct LocMapSizes
{
    Tcl_Size codeDeltaSize;  /* size of the codeDeltaStart array */
    Tcl_Size codeLengthSize; /* size of the codeLengthStart array */
    Tcl_Size srcDeltaSize;   /* size of the srcDeltaStart array */
    Tcl_Size srcLengthSize;  /* size of the srcLengthStart array */
} LocMapSizes;

/*
 * Map between ExceptionRangeType enums and type codes
 */

typedef struct ExcRangeMap
{
    ExceptionRangeType type; /* The TCL enum for a given exception range type */
    char name;               /* and its corresponding code */
} ExcRangeMap;

/*
 * An InstLocList structure holds the location in the bytecode of a PUSH
 * instruction.
 * It is used to keep track of a few different things:
 *	- the beginning of a call to "proc"
 *	- instructions that push a given object.
 *
 * This struct is exported for use by the compiler test package, otherwise
 * it could be kept local to the writer.
 */

typedef struct InstLocList
{
    struct InstLocList* next; /* next proc location in the list */
    Tcl_Size bytecodeOffset;  /* offset to the fist byte in the instruction */
    Tcl_Size commandIndex;    /* the command to which this instruction belongs */
} InstLocList;

/*
 * A ProcBodyInfo structure holds the information we need to postprocess a
 * procedure body. If the indices are set to -1, then the step that populated
 * the struct detected that the body should not be compiled.
 *
 * This struct is exported for use by the compiler test package, otherwise it
 * could be kept local to the writer.
 */

typedef struct ProcBodyInfo
{
    Tcl_Size nameIndex;     /* index in the object table of the object
                             * containing the name of the proc */
    Tcl_Size argsIndex;     /* index in the object table of the object
                             * containing the argument list for the proc */
    Tcl_Size bodyOrigIndex; /* the original index in the object table of
                             * the object containing the body of the
                             * procedure */
    Tcl_Size bodyNewIndex;  /* the new index in the object table of the
                             * object containing the body of the procedure.
                             * The index is different from the original if
                             * the object had been shared */
    Tcl_Size procOffset;    /* offset to the location in the bytecodes
                             * where the "proc" string is pushed on the
                             * stack. This is the start of the instruction
                             * group for a proc command execution */
    Tcl_Size bodyOffset;    /* offset to the location in the bytecodes
                             * where this procedure body is pushed on the
                             * stack */
    Tcl_Size commandIndex;  /* the command number for this proc; values
                             * start at 0 for the first command in the
                             * script. */
} ProcBodyInfo;

/*
 * The PostProcessInfo struct holds compilation info used by the compiler to
 * postprocess the compiled proc body. The counters numProcs, numCompiledBodies,
 * and numUnshared are on a compilation by compilation basis (they refer to the
 * current compilation), whereas the counter in the CompilerContext struct
 * defined below are cumulative for all compilations.
 *
 * This struct is exported for use by the compiler test package, otherwise it
 * could be kept local to the writer.
 */

typedef struct PostProcessInfo
{
    struct InstLocList* procs;   /* the list of proc locations */
    Tcl_Size numProcs;           /* how many entries in the list */
    Tcl_HashTable objTable;      /* this hash table is keyed by object
                                  * index and is used to store information
                                  * about references to this object. */
    ProcBodyInfo** infoArrayPtr; /* NULL-terminated array to pointers of
                                  * info structs that are generated for
                                  * each proc at the start of the post
                                  * processing step */
    Tcl_Size numCompiledBodies;  /* total number of procedure bodies that
                                  * were compiled. Not all procedure
                                  * bodies are compiled. */
    Tcl_Size numUnshares;        /* total number of unshares that were
                                  * performed. If 0, then there were no
                                  * shared procedure bodies */
} PostProcessInfo;

/*
 * The CompilerContext struct holds context for use by the compiler code. It
 * contains a pointer to the PostProcessInfo, counters for various statistics,
 * etc... There is one such struct per interpreter.
 *
 * This struct is exported for use by the compiler test package, otherwise it
 * could be kept local to the writer.
 */

typedef struct CompilerContext
{
    PostProcessInfo* ppi;       /* post-processing context for the currently active compilation */
    Tcl_Size numProcs;          /* how many proc commands were seen in the compiled script */
    Tcl_Size numCompiledBodies; /* how many proc bodies were compiled */
    Tcl_Size numUnsharedBodies; /* how many were unshared */
    Tcl_Size numUnshares;       /* how many copies were made when unsharing proc bodies */
} CompilerContext;

/*
 * This is the start of the signature line
 */
#define CMP_SIGNATURE_HEADER "TclPro ByteCode"

/*
 * Default extension for compiled TCL files
 */
#define CMP_TC_EXTENSION ".tbc"

/*
 * Name of the eval command exported by the Loader package
 */
#define CMP_EVAL_COMMAND "bceval"

/*
 * Name of the proc command exported by the Loader package
 */
#define CMP_PROC_COMMAND "bcproc"

/*
 * Name of the writer (compiler) and reader (loader) packages
 */
#define CMP_WRITER_PACKAGE "compiler"
#define CMP_READER_PACKAGE "tbcload"

#if USE_CATCH_WRAPPER
/*
 * Marker string appended by Loader_EvalObjCmd to the errorInfo, for use by
 * the catch code to strip out error info that we don't want.
 */
#define CMP_ERRORINFO_MARKER "----------####----------"
#endif

/*
 * The one-letter codes for various object types.
 * CMP_STRING_TYPE is an uncompressed/unencoded string,
 * CMP_XSTRING_TYPE is compressed/encoded
 */
#define CMP_INT_CODE 'i'
#define CMP_DOUBLE_CODE 'd'
#define CMP_STRING_CODE 's'
#define CMP_XSTRING_CODE 'x'
#define CMP_PROCBODY_CODE 'p'
#define CMP_BOOLEAN_CODE 'b'
#define CMP_BYTECODE_CODE 'c'

/*
 * The one-letter codes for the exception range types
 */
#define CMP_LOOP_EXCEPTION_RANGE 'L'
#define CMP_CATCH_EXCEPTION_RANGE 'C'

/*
 * The one-letter codes for the AuxData types range types
 */
#define CMP_JUMPTABLE_INFO 'J'
#define CMP_DICTUPDATE_INFO 'D'
#define CMP_NEW_FOREACH_INFO 'f'

/*
 * the following set of procedures needs to be wrapped around a DLLEXPORT
 * macro setup, because they are exported by the Tbcload DLL
 */

#ifdef BUILD_tbcload
#undef TCL_STORAGE_CLASS
#define TCL_STORAGE_CLASS DLLEXPORT
#endif

/*
 *---------------------------------------------------------------
 * Procedures exported by the proc body object support, used internally by
 * both compiler and loader.
 * They are defined in the loader DLL, and exported internally to the
 * compiler.
 *---------------------------------------------------------------
 */

EXTERN void ProcBodyCleanupProc(Proc* procPtr);
EXTERN Tcl_Obj* ProcBodyNewObj(Proc* procPtr);
EXTERN void ProcBodyRegisterTypes(void);

/*
 *----------------------------------------------------------------
 * Procedures exported by cmpRead.c and cmpRPkg.c
 *----------------------------------------------------------------
 */

EXTERN int TbcloadInit(Tcl_Interp* interp);

#undef TCL_STORAGE_CLASS
#define TCL_STORAGE_CLASS DLLIMPORT

/*
 *----------------------------------------------------------------
 * Procedures exported for use by the test package
 *----------------------------------------------------------------
 */

EXTERN const char* CmptestGetPackageName();
EXTERN int Cmptest_Init(Tcl_Interp* interp);

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

/* GetContext exported for use by Test package. */
EXTERN CompilerContext* CompilerGetContext(Tcl_Interp* interp);

EXTERN void CompilerInit(Tcl_Interp* interp);

#undef TCL_STORAGE_CLASS
#define TCL_STORAGE_CLASS DLLIMPORT

#endif /* _CMPINT_H */
