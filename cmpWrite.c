/*
 * cmpWrite.c --
 *
 *  This file contains code that generates compiled scripts from script
 *  files. It implements the "compile" command in the "Compiler" package.
 *
 * Copyright (c) 1998 by Scriptics Corporation.
 * Copyright (c) 2010 ActiveSTate Software Inc.
 *
 *  Copyright (c) 2018 ActiveState Software Inc.
 *  Released under the BSD-3 license. See LICENSE file for details.
 *
 * RCS: @(#) $Id: cmpWrite.c,v 1.7 2005/03/19 00:44:00 hobbs Exp $
 */

#include "cmpWrite.h"
#include "cmpInt.h"

/*
 * A ProcInfo structure is used to store temporary information about the
 * current proc command implementation.
 */
typedef struct ProcInfo
{
    Command* procCmdPtr;
    CompileProc* savedCompileProc;
} ProcInfo;

/*
 * A ObjRefInfo structure holds the information on references to an object
 * in the compile environment's object table.
 */
typedef struct ObjRefInfo
{
    Tcl_Size numReferences;     /* how many times this object is used as an
                                 * operand to opcodes. If this number is
                                 * greater than 1, then we assume that this
                                 * object is shared and therefore it needs to
                                 * be duplicated */
    Tcl_Size numProcReferences; /* how many times this object is pushed on the
                                 * stack as the body in a proc call.
                                 * If this number is greated than 1, then
                                 * this object is shared and needs to be
                                 * duplicated. Note that numReferences does
                                 * include this count */
    Tcl_Size numUnshares;       /* how many copies of the object have been
                                 * made so far. Used by UnshareProcBodies to
                                 * track unsharing */
} ObjRefInfo;

/*
 * This struct holds the encoding context for a run of EmitByteSequence
 */
#define ENCODED_BUFFER_SIZE 72

typedef struct A85EncodeContext
{
    Tcl_Channel target; /* the target channel; when the encoding buffer is full, it is written out to it */
    char* basePtr;      /* base of the encoding buffer */
    char* curPtr;       /* current available position in the encoding buffer */
    char* endPtr;       /* one past the last available position in the buffer; when curPtr == endPtr, the buffer is full */
    char separator;     /* written to the target channel after each flush of the encode buffer */
    char encBuffer[ENCODED_BUFFER_SIZE]; /* the encoding buffer */
} A85EncodeContext;

/*
 * Mask for rwx flags in struct stat's st_mode
 */
#ifndef ACCESSPERMS
#define ACCESSPERMS 0777
#endif

/*
 * The key for the associative data is the name of the package
 */
#define CMP_ASSOC_KEY CMP_WRITER_PACKAGE

/*
 * The format version number for this file.
 */
static int formatVersion = 3;

/*
 * This is the start of the signature line
 */
static char signatureHeader[] = CMP_SIGNATURE_HEADER;

/*
 * Default extension for compiled TCL files
 */

static char tcExtension[] = CMP_TC_EXTENSION;

/*
 * The following variables make up the pieces of the script preamble
 */
#if USE_CATCH_WRAPPER
static char preambleFormat[] = "\
if {[catch {package require %s %s} err] == 1} {\n\
    return -code error \"[info script]: %s -- $err\"\n\
}\n\
if {[catch {%s::%s {\
";
#else
static char preambleFormat[] = "\
if {[catch {package require %s %s} err] == 1} {\n\
    return -code error \"[info script]: %s -- $err\"\n\
}\n\
%s::%s {\
";
#endif

static char errorMessage[] = LOADER_ERROR_MESSAGE;
static char errorVariable[] = LOADER_ERROR_VARIABLE;
static char evalCommand[] = CMP_EVAL_COMMAND;
static char loaderName[] = CMP_READER_PACKAGE;
static char loaderVersion[] = TBCLOAD_VERSION;
static char procCommand[] = CMP_PROC_COMMAND;

/*
 * The following variables make up the pieces of the script postamble
 */

#if USE_CATCH_WRAPPER
static char postambleFormat[] = "\
}} err] == 1} {\n\
    set ei $::errorInfo\n\
    set ec $::errorCode\n\
    regexp {(.*)%s} $ei dum ei\n\
    error $err $ei $ec\n\
}\
";
static char errorInfoMarker[] = CMP_ERRORINFO_MARKER;
#else
static char postambleFormat[] = "}";
#endif

/*
 * Map between ExceptionRangeType enums and type codes.
 * This map must be kept consistent with the equivalent one in cmpRead.c.
 */
static ExcRangeMap excRangeMap[] = {
    {LOOP_EXCEPTION_RANGE, CMP_LOOP_EXCEPTION_RANGE}, {CATCH_EXCEPTION_RANGE, CMP_CATCH_EXCEPTION_RANGE}, {0, '\0'}};

/*
 * The list of VAR_ flag values to check when emitting. The order is
 * is important an must be kept consistent with the equivalent list in
 * cmpRead.c
 */
static int varFlagsList[] = {
    /*
     * For 8.5+, keep the same size for compat with 8.4 written bytecodes,
     * but ignore all but VAR_ARGUMENT and VAR_TEMPORARY.
     */
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    VAR_ARGUMENT,
    VAR_TEMPORARY,
    0 /* VAR_RESOLVED is always mapped as 0 */
};
static int varFlagsListSize = sizeof(varFlagsList) / sizeof(varFlagsList[0]);

/*
 * We use a modified encoding scheme which avoids the Tcl special characters
 * ", $, {, }, [, ], and \.
 * Because of this, we need to use a table instead of generating the character
 * codes arithmetically.
 * (for hilit: ")
 */
#define EN(c) encodeMap[(c)]

static char encodeMap[] = {
    '!',  /*  0: ! */
    'v',  /*  1: was ", is now v (and this is for hilit:") */
    '#',  /*  2: # */
    'w',  /*  3: was $, is now w */
    '%',  /*  4: % */
    '&',  /*  5: & */
    '\'', /*  6: ' */
    '(',  /*  7: ( */
    ')',  /*  8: ) */
    '*',  /*  9: * */
    '+',  /* 10: + */
    ',',  /* 11: , */
    '-',  /* 12: - */
    '.',  /* 13: . */
    '/',  /* 14: / */
    '0',  /* 15: 0 */
    '1',  /* 16: 1 */
    '2',  /* 17: 2 */
    '3',  /* 18: 3 */
    '4',  /* 19: 4 */
    '5',  /* 20: 5 */
    '6',  /* 21: 6 */
    '7',  /* 22: 7 */
    '8',  /* 23: 8 */
    '9',  /* 24: 9 */
    ':',  /* 25: : */
    ';',  /* 26: ; */
    '<',  /* 27: < */
    '=',  /* 28: = */
    '>',  /* 29: > */
    '?',  /* 30: ? */
    '@',  /* 31: @ */
    'A',  /* 32: A */
    'B',  /* 33: B */
    'C',  /* 34: C */
    'D',  /* 35: D */
    'E',  /* 36: E */
    'F',  /* 37: F */
    'G',  /* 38: G */
    'H',  /* 39: H */
    'I',  /* 40: I */
    'J',  /* 41: J */
    'K',  /* 42: K */
    'L',  /* 43: L */
    'M',  /* 44: M */
    'N',  /* 45: N */
    'O',  /* 46: O */
    'P',  /* 47: P */
    'Q',  /* 48: Q */
    'R',  /* 49: R */
    'S',  /* 50: S */
    'T',  /* 51: T */
    'U',  /* 52: U */
    'V',  /* 53: V */
    'W',  /* 54: W */
    'X',  /* 55: X */
    'Y',  /* 56: Y */
    'Z',  /* 57: Z */
    'x',  /* 58: was [, is now x */
    'y',  /* 59: was \, is now y */
    '|',  /* 60: was ], is now | */
    '^',  /* 61: ^ */
    '_',  /* 62: _ */
    '`',  /* 63: ` */
    'a',  /* 64: a */
    'b',  /* 65: b */
    'c',  /* 66: c */
    'd',  /* 67: d */
    'e',  /* 68: e */
    'f',  /* 69: f */
    'g',  /* 70: g */
    'h',  /* 71: h */
    'i',  /* 72: i */
    'j',  /* 73: j */
    'k',  /* 74: k */
    'l',  /* 75: l */
    'm',  /* 76: m */
    'n',  /* 77: n */
    'o',  /* 78: o */
    'p',  /* 79: p */
    'q',  /* 80: q */
    'r',  /* 81: r */
    's',  /* 82: s */
    't',  /* 83: t */
    'u'   /* 84: u */
};

/*
 * These Tcl_ObjType pointers are initialized the first time that the package
 * is loaded; we do it this way because the actual object types are not
 * exported by the TCL DLL, and therefore if we use the address of the
 * standard types we get an undefined symbol at link time.
 */
static const Tcl_ObjType* cmpProcBodyType = 0;
static const Tcl_ObjType* cmpByteCodeType = 0;
static const Tcl_ObjType* cmpDoubleType = 0;
static const Tcl_ObjType* cmpIntType = 0;

/*
 * Same thing for AuxDataTypes.
 */
static const AuxDataType* cmpJumptableInfoType = 0;
static const AuxDataType* cmpDictUpdateInfoType = 0;
static const AuxDataType* cmpNewForeachInfoType = 0;

static int didLoadTypes = 0;

/*
 * Format for the name of the dummy command used to compile procedure bodies,
 * and counter used to generate unique names.
 */
static char dummyCommandName[] = "$$compiler$$dummy%d";
static int dummyCommandCounter = 1;

/*
 * Prototypes for procedures defined later in this file:
 */
static int A85EmitChar(Tcl_Interp* interp, int toEmit, A85EncodeContext* ctxPtr);
static int A85EncodeBytes(Tcl_Interp* interp, unsigned char* bytesPtr, Tcl_Size numBytes, A85EncodeContext* ctxPtr);
static int A85Flush(Tcl_Interp* interp, A85EncodeContext* ctxPtr);
static void A85InitEncodeContext(Tcl_Channel target, int separator, A85EncodeContext* ctxPtr);
static void AppendInstLocList(Tcl_Interp* interp, CompileEnv* envPtr);
static Tcl_Size CalculateLocArrayLength(unsigned char* bytes, Tcl_Size numCommands);
static void CalculateLocMapSizes(ByteCode* codePtr, LocMapSizes* sizes);
static void CleanObjRefInfoTable(PostProcessInfo* locInfoPtr);
static void CleanCompilerContext(void* clientData, Tcl_Interp* interp);
static int CompileObject(Tcl_Interp* interp, Tcl_Obj* objPtr);
static int CompileOneProcBody(Tcl_Interp* interp, ProcBodyInfo* infoPtr, CompilerContext* ctxPtr, CompileEnv* compEnvPtr);
static int CompileProcBodies(Tcl_Interp* interp, CompileEnv* compEnvPtr);
static void CreateProcBodyInfoArray(PostProcessInfo* locInfoPtr, CompileEnv* compEnvPtr, ProcBodyInfo*** arrayPtrPtr);
static PostProcessInfo* CreatePostProcessInfo(void);
static InstLocList* CreateInstLocList(CompileEnv* envPtr);
static void CmpDeleteProc(void* clientData);
static Tcl_ObjCmdProc DummyObjInterpProc;
static int EmitAuxDataArray(Tcl_Interp* interp, ByteCode* codePtr, Tcl_Channel chan);
static int EmitByteCode(Tcl_Interp* interp, ByteCode* codePtr, Tcl_Channel chan);
static int EmitByteSequence(Tcl_Interp* interp, unsigned char* bytesPtr, Tcl_Size length, Tcl_Channel chan);
static int EmitChar(Tcl_Interp* interp, int value, int separator, Tcl_Channel chan);
static int EmitCompiledLocal(Tcl_Interp* interp, CompiledLocal* localPtr, Tcl_Channel chan);
static int EmitCompiledObject(Tcl_Interp* interp, Tcl_Obj* objPtr, Tcl_Channel chan);
static int EmitExcRangeArray(Tcl_Interp* interp, ByteCode* codePtr, Tcl_Channel chan);
static int EmitJumptableInfo(Tcl_Interp* interp, JumptableInfo* infoPtr, Tcl_Channel chan);
static int EmitDictUpdateInfo(Tcl_Interp* interp, DictUpdateInfo* infoPtr, Tcl_Channel chan);
static int EmitNewForeachInfo(Tcl_Interp* interp, ForeachInfo* infoPtr, Tcl_Channel chan);
static int EmitTclSize(Tcl_Interp* interp, Tcl_Size value, int separator, Tcl_Channel chan);
static int EmitObjArray(Tcl_Interp* interp, ByteCode* codePtr, Tcl_Channel chan);
static int EmitObject(Tcl_Interp* interp, Tcl_Obj* objPtr, Tcl_Channel chan);
static int EmitProcBody(Tcl_Interp* interp, Proc* procPtr, Tcl_Channel chan);
static int EmitScriptPostamble(Tcl_Interp* interp, Tcl_Channel chan);
static int EmitScriptPreamble(Tcl_Interp* interp, Tcl_Channel chan);
static int EmitSignature(Tcl_Interp* interp, Tcl_Channel chan);
static int EmitString(Tcl_Interp* interp, char* src, Tcl_Size length, int separator, Tcl_Channel chan);
static void FreeProcBodyInfoArray(PostProcessInfo* infoPtr);
static void FreePostProcessInfo(PostProcessInfo* infoPtr);
static Tcl_Size GetSharedIndex(unsigned char* pc);
static void InitCompilerContext(Tcl_Interp* interp);
static void InitTypes(void);
static void LoadObjRefInfoTable(PostProcessInfo* locInfoPtr, CompileEnv* compEnvPtr);
static void LoadProcBodyInfo(InstLocList* locInfoPtr, CompileEnv* compEnvPtr, ProcBodyInfo* infoPtr);
static int LocalProcCompileProc(Tcl_Interp* interp, Tcl_Parse* parsePtr, Command* cmdPtr, struct CompileEnv* compEnvPtr);
static char NameFromExcRange(ExceptionRangeType type);
static int PostProcessCompile(Tcl_Interp* interp, struct CompileEnv* compEnvPtr, void* clientData);
static void PrependResult(Tcl_Interp* interp, char* msgPtr);
static void ReleaseCompilerContext(Tcl_Interp* interp);
static int ReplacePushIndex(Tcl_Size commandIndex, unsigned char* pc, Tcl_Size newIndex, CompileEnv* compEnvPtr);
static void ShiftByteCodes(Tcl_Size commandIndex, Tcl_Size startOffset, Tcl_Size shiftCount, CompileEnv* compEnvPtr);
static int UnshareObject(Tcl_Size origIndex, CompileEnv* compEnvPtr);
static void UnshareProcBodies(Tcl_Interp* interp, CompilerContext* ctxPtr, CompileEnv* compEnvPtr);
static void UpdateByteCodes(PostProcessInfo* infoPtr, CompileEnv* compEnvPtr);
#ifdef DEBUG_REWRITE
static void FormatInstruction(CompileEnv* compEnvPtr, unsigned char* pc);
#endif

/*
 *----------------------------------------------------------------------
 *
 * Compiler_CompileObjCmd --
 *
 *  Read in a file containing a Tcl script, and compile it. The resulting
 *  ByteCode structure is then written out to the file specified in the
 *  second argument. If the second argument is not given, the output file
 *  will have the same root as the input, with extension ".tbc".
 *
 *  Call format:
 *    compiler::compile ?-preamble value? inputFile ?outputFile?
 *  The -preamble flag specifies a chunk of code to be prepended to the
 *  generated compiled script.
 *
 * Results:
 *  Returns a standard TCL result code.
 *
 * Side effects:
 *  None.
 *
 *----------------------------------------------------------------------
 */

int Compiler_CompileObjCmd(void* dummy, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[])
{
    static char argsMsg[] = "?-preamble value? inputFileName ?outputFileName?";

    char* inFilePtr;
    char* outFilePtr = NULL;
    char* preamblePtr = NULL;
    int fileIndex = 1;
    int argCount = 2;
    Tcl_Size len;

    Tcl_ResetResult(interp);

    if (objc < 2)
    {
        Tcl_WrongNumArgs(interp, 1, objv, argsMsg);
        return TCL_ERROR;
    }

    if (strcmp(Tcl_GetString(objv[1]), "-preamble") == 0)
    {
        if (objc < 3)
        {
            Tcl_SetObjResult(interp, Tcl_NewStringObj("missing value for the -preamble flag", -1));
            return TCL_ERROR;
        }
        preamblePtr = Tcl_GetString(objv[2]);
        fileIndex = 3;
        argCount = 4;
    }

    if (objc < argCount)
    {
        Tcl_WrongNumArgs(interp, 1, objv, argsMsg);
        return TCL_ERROR;
    }

    /*
     * THESE FAIL IF THE OBJECT'S STRING REP CONTAINS A NULL.
     */

    inFilePtr = Tcl_GetStringFromObj(objv[fileIndex], &len);

    if (objc > argCount)
    {
        outFilePtr = Tcl_GetStringFromObj(objv[fileIndex + 1], &len);
    }

    return Compiler_CompileFile(interp, inFilePtr, outFilePtr, preamblePtr);
}

/*
 *----------------------------------------------------------------------
 *
 * Compiler_GetBytecodeExtensionObjCmd --
 *
 *  Returns the default extension used for bytecode compiled files.
 *
 *  Call format:
 *    compiler::getBytecodeExtension
 *
 * Results:
 *  Returns a standard TCL result code.
 *
 * Side effects:
 *  None.
 *
 *----------------------------------------------------------------------
 */

int Compiler_GetBytecodeExtensionObjCmd(void* dummy, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[])
{
    Tcl_Obj* objPtr = Tcl_NewStringObj(tcExtension, -1);
    Tcl_SetObjResult(interp, objPtr);
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * Compiler_CompileFile --
 *
 *  Read in a file containing a Tcl script, and compile it. The resulting
 *  ByteCode structure is then written out to the file specified in the
 *  second argument. If the second argument is NULL, the output file will
 *  have the same root as the input, and extension ".tbc".
 *  The preamblePtr argument specifies a preamble to be written out to the
 *  output file before the body of the compiled script is emitted; this can
 *  be used to customize the generated script. A value of NULL means no
 *  preamble is to be emitted.
 *
 *  Tilde expansion and conversion to native format will be done for both
 *  file names.
 *
 *  Currently overwrites the input file if input and output file are the
 *  same. Does this need to be fixed?
 *
 * Results:
 *  Returns a standard TCL result code.
 *
 * Side effects:
 *  None.
 *
 *----------------------------------------------------------------------
 */

int Compiler_CompileFile(Tcl_Interp* interp, char* inFilePtr, char* outFilePtr, char* preamblePtr)
{
    Interp* iPtr = (Interp*)interp;
    Tcl_DString inBuffer, outBuffer;
    char* nativeInName;
    char* nativeOutName;
    Tcl_Channel chan;
    int result;
    struct stat statBuf;
    unsigned short fileMode;
    Tcl_Obj* cmdObjPtr;
    LiteralTable glt; /* Save buffer for global literals */

    Tcl_ResetResult(interp);

    Tcl_DStringInit(&inBuffer);
    Tcl_DStringInit(&outBuffer);

    nativeInName = Tcl_TranslateFileName(interp, inFilePtr, &inBuffer);
    if (nativeInName == NULL)
    {
        goto error;
    }

    if (outFilePtr == NULL)
    {
        nativeOutName = nativeInName;
        Tcl_DStringAppend(&outBuffer, nativeOutName, -1);
    }
    else
    {
        nativeOutName = Tcl_TranslateFileName(interp, outFilePtr, &outBuffer);
        if (nativeOutName == NULL)
        {
            goto error;
        }
    }

    /*
     * If Tcl_TranslateFileName didn't already copy the file names, do it
     * here.  This way we don't depend on fileName staying constant
     * throughout the execution of the script (e.g., what if it happens
     * to point to a Tcl variable that the script could change?).
     * This part came from Tcl_EvalFile, not sure it is needed here, the
     * compiler should not affect the variable.
     */

    if (nativeInName != Tcl_DStringValue(&inBuffer))
    {
        Tcl_DStringSetLength(&inBuffer, 0);
        Tcl_DStringAppend(&inBuffer, nativeInName, -1);
        nativeInName = Tcl_DStringValue(&inBuffer);
    }

    if (nativeOutName != Tcl_DStringValue(&outBuffer))
    {
        Tcl_DStringSetLength(&outBuffer, 0);
        Tcl_DStringAppend(&outBuffer, nativeOutName, -1);
        nativeOutName = Tcl_DStringValue(&outBuffer);
    }

    /*
     * If the outFilePtr argument was a NULL, then we must replace the
     * extension for its current value, because its current value is inFilePtr.
     */
    if (outFilePtr == NULL)
    {
        const char* extension = TclGetExtension(nativeOutName);
        if (extension != NULL)
        {
            Tcl_DStringSetLength(&outBuffer, (Tcl_DStringLength(&outBuffer) - strlen(extension)));
        }
        Tcl_DStringAppend(&outBuffer, tcExtension, -1);
        nativeOutName = Tcl_DStringValue(&outBuffer);
    }

    if (stat(nativeInName, &statBuf) == -1)
    {
        Tcl_SetErrno(errno);
        Tcl_SetObjResult(interp, Tcl_ObjPrintf("couldn't read file \"%s\": %s", inFilePtr, Tcl_PosixError(interp)));
        goto error;
    }
    fileMode = statBuf.st_mode & ACCESSPERMS;

    chan = Tcl_OpenFileChannel(interp, nativeInName, "r", 0644);
    if (chan == (Tcl_Channel)NULL)
    {
        Tcl_ResetResult(interp);
        Tcl_SetObjResult(interp, Tcl_ObjPrintf("couldn't read file \"%s\": %s", inFilePtr, Tcl_PosixError(interp)));
        goto error;
    }
    cmdObjPtr = Tcl_NewObj();
    result = Tcl_ReadChars(chan, cmdObjPtr, -1, 0);
    if (result < 0)
    {
        Tcl_Close(interp, chan);
        Tcl_SetObjResult(interp, Tcl_ObjPrintf("couldn't read file \"%s\": %s", inFilePtr, Tcl_PosixError(interp)));
        goto error;
    }
    if (Tcl_Close(interp, chan) != TCL_OK)
    {
        goto error;
    }

    /*
     * Saving state of interpreter literals, then reinitializing
     * for compiler. Prevents interference between application
     * running the compiler and compiler itself.
     */

    memcpy(&glt, &iPtr->literalTable, sizeof(LiteralTable));

    /* Inlined copy of "TclInitLiteralTable (&iPtr->literalTable);"
     * This function is not in the stub table of Tcl, not even in
     * the internal one. This causes link problems.
     */

#define REBUILD_MULTIPLIER 3

    iPtr->literalTable.buckets = iPtr->literalTable.staticBuckets;
    iPtr->literalTable.staticBuckets[0] = iPtr->literalTable.staticBuckets[1] = 0;
    iPtr->literalTable.staticBuckets[2] = iPtr->literalTable.staticBuckets[3] = 0;
    iPtr->literalTable.numBuckets = TCL_SMALL_HASH_TABLE;
    iPtr->literalTable.numEntries = 0;
    iPtr->literalTable.rebuildSize = TCL_SMALL_HASH_TABLE * REBUILD_MULTIPLIER;
    iPtr->literalTable.mask = 3;

    Tcl_IncrRefCount(cmdObjPtr);
    result = Compiler_CompileObj(interp, cmdObjPtr);
    if (result == TCL_RETURN)
    {
        result = TclUpdateReturnInfo(iPtr);
    }
    else if (result == TCL_ERROR)
    {
        char msg[200];

        /*
         * Record information telling where the error occurred.
         */

        sprintf(msg, "\n    (file \"%.150s\" line %d)", inFilePtr, Tcl_GetErrorLine(interp));
        Tcl_AppendObjToErrorInfo(interp, Tcl_NewStringObj(msg, -1));
    }
    else
    {
        chan = Tcl_OpenFileChannel(interp, nativeOutName, "w", fileMode);
        if (chan == (Tcl_Channel)NULL)
        {
            Tcl_ResetResult(interp);
            Tcl_SetObjResult(interp,
                             Tcl_ObjPrintf("couldn't create output file \"%s\": %s", nativeOutName, Tcl_PosixError(interp)));
            result = TCL_ERROR;
        }
        else
        {
            result = TCL_OK;
            if (preamblePtr)
            {
                result = EmitString(interp, preamblePtr, -1, '\n', chan);
            }
            if (result == TCL_OK)
            {
                result = EmitCompiledObject(interp, cmdObjPtr, chan);
            }
            if (Tcl_Close(interp, chan) != TCL_OK)
            {
                Tcl_SetObjResult(interp, Tcl_ObjPrintf("error closing bytecode stream: %s", Tcl_PosixError(interp)));
                result = TCL_ERROR;
            }
        }
    }
    if (result != TCL_ERROR)
    {
        /*
         * If an error was returned, the previous internal rep may
         * already be freed, and this can cause crash conditions.
         * [AS Bug 20078]
         */
        Tcl_DecrRefCount(cmdObjPtr);
    }

    /*
     * Restore interpreter literals from save buffer. Can't delete the
     * transient table, causes crashes.
     */

    /* ** TclDeleteLiteralTable (interp,&iPtr->literalTable); ** */
    memcpy(&iPtr->literalTable, &glt, sizeof(LiteralTable));

    Tcl_DStringFree(&inBuffer);
    Tcl_DStringFree(&outBuffer);

    return result;

error:
    Tcl_DStringFree(&inBuffer);
    Tcl_DStringFree(&outBuffer);

    return TCL_ERROR;
}

/*
 *----------------------------------------------------------------------
 *
 * Compiler_CompileObj --
 *
 *  Compile Tcl commands stored in a Tcl object. These commands are
 *  compiled into bytecodes.
 *
 *  Scarfed from Tcl_EvalObj, where we run the compilation step but not
 *  the execution step.
 *
 * Results:
 *  The return value is one of the return codes defined in tcl.h
 *  (such as TCL_OK), and the interpreter's result contains a value
 *  to supplement the return code.
 *
 * Side effects:
 *  The object is converted to a ByteCode object that holds the bytecode
 *  instructions for the commands.
 *
 *----------------------------------------------------------------------
 */

int Compiler_CompileObj(Tcl_Interp* interp, Tcl_Obj* objPtr)
{
    Interp* iPtr = (Interp*)interp;
    int result = TCL_OK;

    /*
     * If the interpreter has been deleted, return an error.
     */

    if (iPtr->flags & DELETED)
    {
        Tcl_ResetResult(interp);
        Tcl_AppendToObj(Tcl_GetObjResult(interp), "attempt to call compile in deleted interpreter", -1);
        Tcl_SetErrorCode(interp, "COMPILER", "COMPILE", "attempt to call compile in deleted interpreter", (char*)NULL);
        return TCL_ERROR;
    }

    /*
     * We force a recompilation even though this object may already be
     * compiled. However, we do not attempt to recompile an object that had
     * been generated from a compiled script.
     */
    const Tcl_ObjInternalRep* ir = Tcl_FetchInternalRep(objPtr, cmpByteCodeType);
    if (ir)
    {
        ByteCode* codePtr = (ByteCode*)ir->twoPtrValue.ptr1;
        if (codePtr->flags & TCL_BYTECODE_PRECOMPILED)
            return TCL_OK;
        Tcl_FreeInternalRep(objPtr);
        ir = NULL;
    }
    if (!ir)
    {
        Tcl_SetErrorLine(interp, 1);
        if (CompileObject(interp, objPtr) != TCL_OK)
            return TCL_ERROR;
        if (Tcl_ConvertToType(interp, objPtr, cmpByteCodeType) != TCL_OK)
            return TCL_ERROR;
    }

    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * EmitCompiledObject --
 *
 *  Emits the contents of a ByteCode structure to a Tcl_Channel to generate
 *  a TCL "object file".
 *  There are three parts to the object file:
 *   - a header containing information about the ByteCode structure.
 *   - the dump of the bytecodes
 *   - the dump of the support arrays; this includes the dump of all the
 *     objects used by the byte code itself.
 *
 * Results:
 *  Returns a TCL result code.
 *
 * Side effects:
 *  Appends an error message to the TCL error.
 *
 *----------------------------------------------------------------------
 */

static int EmitCompiledObject(Tcl_Interp* interp, Tcl_Obj* objPtr, Tcl_Channel chan)
{
    if ((EmitScriptPreamble(interp, chan) != TCL_OK) || (EmitSignature(interp, chan) != TCL_OK))
    {
        return TCL_ERROR;
    }

    if (EmitByteCode(interp, (ByteCode*)objPtr->internalRep.otherValuePtr, chan) != TCL_OK)
    {
        PrependResult(interp, "error writing bytecode stream: ");
        return TCL_ERROR;
    }

    if (EmitScriptPostamble(interp, chan) != TCL_OK)
    {
        return TCL_ERROR;
    }

    if (Tcl_Flush(chan) != TCL_OK)
    {
        Tcl_SetObjResult(interp, Tcl_ObjPrintf("error flushing bytecode stream: Tcl_Flush: %s", Tcl_PosixError(interp)));
        return TCL_ERROR;
    }

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * EmitByteCode --
 *
 *  Emits the contents of a ByteCode structure to a Tcl_Channel.
 *  There are three parts to the dumped information:
 *   - a header containing information about the ByteCode structure.
 *   - the dump of the bytecodes
 *   - the dump of the support arrays; this includes the dump of all the
 *     objects used by the byte code itself.
 *
 * Results:
 *  Returns TCL_OK on success, TCL_ERROR on failure.
 *
 * Side effects:
 *  None.
 *
 *----------------------------------------------------------------------
 */

static int EmitByteCode(Tcl_Interp* interp, ByteCode* codePtr, Tcl_Channel chan)
{
    LocMapSizes locMapSizes;

    /*
     * Emit the sizes of the various components of the ByteCode struct,
     * so that the size can be recalculated at read time.
     * These fields are currently written out as 0 always:
     * numSrcChars
     */

    CalculateLocMapSizes(codePtr, &locMapSizes);

    if ((EmitTclSize(interp, codePtr->numCommands, ' ', chan) != TCL_OK) ||
        (EmitTclSize(interp, 0, ' ', chan) != TCL_OK) /* numSrcChars */
        || (EmitTclSize(interp, codePtr->numCodeBytes, ' ', chan) != TCL_OK) ||
        (EmitTclSize(interp, codePtr->numLitObjects, ' ', chan) != TCL_OK) ||
        (EmitTclSize(interp, codePtr->numExceptRanges, ' ', chan) != TCL_OK) ||
        (EmitTclSize(interp, codePtr->numAuxDataItems, ' ', chan) != TCL_OK) ||
        (EmitTclSize(interp, codePtr->numCmdLocBytes, ' ', chan) != TCL_OK) ||
        (EmitTclSize(interp, codePtr->maxExceptDepth, ' ', chan) != TCL_OK) ||
        (EmitTclSize(interp, codePtr->maxStackDepth, ' ', chan) != TCL_OK))
    {
        return TCL_ERROR;
    }

#if EMIT_SRCMAP
    if ((EmitTclSize(interp, locMapSizes.codeDeltaSize, ' ', chan) != TCL_OK) ||
        (EmitTclSize(interp, locMapSizes.codeLengthSize, ' ', chan) != TCL_OK) ||
        (EmitTclSize(interp, locMapSizes.srcDeltaSize, ' ', chan) != TCL_OK) ||
        (EmitTclSize(interp, locMapSizes.srcLengthSize, '\n', chan) != TCL_OK))
    {
        return TCL_ERROR;
    }
#else
    if ((EmitTclSize(interp, locMapSizes.codeDeltaSize, ' ', chan) != TCL_OK) ||
        (EmitTclSize(interp, locMapSizes.codeLengthSize, ' ', chan) != TCL_OK) ||
        (EmitTclSize(interp, -1, ' ', chan) != TCL_OK) || (EmitTclSize(interp, -1, '\n', chan) != TCL_OK))
    {
        return TCL_ERROR;
    }
#endif

    /*
     * The byte code dumps
     */

    if (EmitByteSequence(interp, codePtr->codeStart, codePtr->numCodeBytes, chan) != TCL_OK)
    {
        return TCL_ERROR;
    }

    if ((EmitByteSequence(interp, codePtr->codeDeltaStart, locMapSizes.codeDeltaSize, chan) != TCL_OK) ||
        (EmitByteSequence(interp, codePtr->codeLengthStart, locMapSizes.codeLengthSize, chan) != TCL_OK))
    {
        return TCL_ERROR;
    }
#if EMIT_SRCMAP
    if ((EmitByteSequence(interp, codePtr->srcDeltaStart, locMapSizes.srcDeltaSize, chan) != TCL_OK) ||
        (EmitByteSequence(interp, codePtr->srcLengthStart, locMapSizes.srcLengthSize, chan) != TCL_OK))
    {
        return TCL_ERROR;
    }
#endif

    /*
     * the support arrays
     */

    if ((EmitObjArray(interp, codePtr, chan) != TCL_OK) || (EmitExcRangeArray(interp, codePtr, chan) != TCL_OK) ||
        (EmitAuxDataArray(interp, codePtr, chan) != TCL_OK))
    {
        return TCL_ERROR;
    }

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * EmitChar --
 *
 *  Emits a character value to a Tcl_Channel.
 *  The separator argument specifies a character to be emitted after the
 *  integer.
 *
 * Results:
 *  Returns TCL_OK on success, TCL_ERROR on failure.
 *
 * Side effects:
 *  None.
 *
 *----------------------------------------------------------------------
 */

static int EmitChar(Tcl_Interp* interp, int value, int separator, Tcl_Channel chan)
{
    char buf[2];

    buf[0] = value;
    buf[1] = separator;
    if (Tcl_Write(chan, buf, 2) < 0)
    {
        Tcl_SetObjResult(interp, Tcl_ObjPrintf("Tcl_Write: %s", Tcl_PosixError(interp)));
        return TCL_ERROR;
    }

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * EmitTclSize --
 *
 *  Emits an integer value to a Tcl_Channel.
 *  The separator argument specifies a character to be emitted after the
 *  integer.
 *
 * Results:
 *  Returns TCL_OK on success, TCL_ERROR on failure.
 *
 * Side effects:
 *  None.
 *
 *----------------------------------------------------------------------
 */

static int EmitTclSize(Tcl_Interp* interp, Tcl_Size value, int separator, Tcl_Channel chan)
{
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "%" TCL_SIZE_MODIFIER "d%c", value, (char)separator);
    if (n < 0 || Tcl_Write(chan, buf, n) < 0)
    {
        Tcl_SetObjResult(interp, Tcl_ObjPrintf("Tcl_WriteObj: %s", Tcl_PosixError(interp)));
        return TCL_ERROR;
    }
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * EmitString --
 *
 *  Emits a string value to a Tcl_Channel.
 *  If the length is passed as -1, it is calculated with strlen.
 *  The separator argument specifies a character to be emitted after the
 *  string.
 *
 * Results:
 *  Returns TCL_OK on success, TCL_ERROR on failure.
 *
 * Side effects:
 *  None.
 *
 *----------------------------------------------------------------------
 */

static int EmitString(Tcl_Interp* interp, char* src, Tcl_Size length, int separator, Tcl_Channel chan)
{
    if (length < 0)
    {
        length = strlen(src);
    }

    if ((length > 0) && (Tcl_Write(chan, src, length) < 0))
    {
        Tcl_SetObjResult(interp, Tcl_ObjPrintf("Tcl_Write: %s", Tcl_PosixError(interp)));
        return TCL_ERROR;
    }

    const char c = (char)separator;
    if (Tcl_Write(chan, &c, 1) < 0)
    {
        Tcl_SetObjResult(interp, Tcl_ObjPrintf("Tcl_Write: %s", Tcl_PosixError(interp)));
        return TCL_ERROR;
    }

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * EmitByteSequence --
 *
 *  Emits an array of bytes to a Tcl_Channel, in ASCII85.
 *  This procedure encodes its input with a modified version of the ASCII85
 *  encode filter.
 *  There are two differences from the standard ASCII85 algorithm:
 *  - the encoding characters are obtained from a table rather than
 *    being generated arithmetically. This is done because we want to
 *    avoid using Tcl special characters.
 *  - the order in which bytes in a 4-tuple are encoded is the opposite
 *    of the standard order. This lets us drop ! bytes in the encoded
 *    5-tuple, which buys us better encoding with short strings.
 *  As a consequence, this encoder is not general purpose; only a similarly
 *  specialized decoder can extract the bytes back.
 *
 *  The format is a line containing the byte count, then lines each containing
 *  72 ASCII characters, or fewer for the last line.
 *
 * Results:
 *  Returns TCL_OK on success, TCL_ERROR on failure.
 *
 * Side effects:
 *  None.
 *
 *----------------------------------------------------------------------
 */

static int EmitByteSequence(Tcl_Interp* interp, unsigned char* bytesPtr, Tcl_Size length, Tcl_Channel chan)
{
    A85EncodeContext encodeCtx;
    unsigned char bytes[4];
    Tcl_Size numBytes = 0;

    if (EmitTclSize(interp, length, '\n', chan) != TCL_OK)
    {
        return TCL_ERROR;
    }

    A85InitEncodeContext(chan, '\n', &encodeCtx);

    while (length > 0)
    {
        bytes[numBytes] = *bytesPtr;

        if (numBytes == 3)
        {
            if (A85EncodeBytes(interp, bytes, 4, &encodeCtx) != TCL_OK)
            {
                return TCL_ERROR;
            }
            numBytes = -1;
        }

        numBytes += 1;
        bytesPtr += 1;
        length -= 1;
    }

    if ((numBytes != 0) && (A85EncodeBytes(interp, bytes, numBytes, &encodeCtx) != TCL_OK))
    {
        return TCL_ERROR;
    }

    return A85Flush(interp, &encodeCtx);
}

/*
 *----------------------------------------------------------------------
 *
 * CalculateLocMapSizes --
 *
 *  Calculates the lengths of the location map arrays in a ByteCode
 *  struct and places them in the LocMapSizes argument.
 *  Note that, although it could use pointer arithmetic for all but the
 *  last one (because the arrays are contiguous), we elect to do it by
 *  scanning the arrays in all cases.
 *
 * Results:
 *  None.
 *
 * Side effects:
 *  Fills in the fields of the LocMapSizes struct.
 *
 *----------------------------------------------------------------------
 */
static void CalculateLocMapSizes(ByteCode* codePtr, LocMapSizes* sizes)
{
    /*
     * EMIL for all but the last, we could do sanity checking by using
     * pointer arithmetic.
     */

    sizes->codeDeltaSize = CalculateLocArrayLength(codePtr->codeDeltaStart, codePtr->numCommands);
    sizes->codeLengthSize = CalculateLocArrayLength(codePtr->codeLengthStart, codePtr->numCommands);
    sizes->srcDeltaSize = CalculateLocArrayLength(codePtr->srcDeltaStart, codePtr->numCommands);
    sizes->srcLengthSize = CalculateLocArrayLength(codePtr->srcLengthStart, codePtr->numCommands);
}

/*
 *----------------------------------------------------------------------
 *
 * CalculateLocArrayLength --
 *
 *  Calculates the length of the given location array; numCommands is the
 *  number of commands in the ByteCode structure.
 *
 * Results:
 *  Returns the length, in bytes, of the array.
 *
 * Side effects:
 *  None.
 *
 *----------------------------------------------------------------------
 */

static Tcl_Size CalculateLocArrayLength(unsigned char* bytes, Tcl_Size numCommands)
{
    int i, length = 0;

    /*
     * array is encoded as either a single byte, or a four-byte sequence
     * preceded by the 0xff tag
     */

    for (i = 0; i < numCommands; i++)
    {
        if (*bytes == 0xff)
        {
            length += 5;
            bytes += 5;
        }
        else
        {
            length += 1;
            bytes += 1;
        }
    }

    return length;
}

/*
 *----------------------------------------------------------------------
 *
 * EmitObjArray --
 *
 *  Emits the object array for a ByteCode struct to a Tcl_Channel.
 *
 * Results:
 *  Returns TCL_OK on success, TCL_ERROR on failure.
 *
 * Side effects:
 *  None.
 *
 *----------------------------------------------------------------------
 */

static int EmitObjArray(Tcl_Interp* interp, ByteCode* codePtr, Tcl_Channel chan)
{
    int result;
    Tcl_Size i, numLitObjects = codePtr->numLitObjects;
    Tcl_Obj** objArrayPtr = &codePtr->objArrayPtr[0];

    if (EmitTclSize(interp, numLitObjects, '\n', chan) != TCL_OK)
    {
        return TCL_ERROR;
    }

    for (i = 0; i < numLitObjects; i++)
    {
        result = EmitObject(interp, objArrayPtr[i], chan);
        if (result != TCL_OK)
        {
            return result;
        }
    }

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * EmitObject --
 *
 *  Emits a Tcl_Obj to a Tcl_Channel.
 *
 * Results:
 *  Returns TCL_OK on success, TCL_ERROR on failure.
 *
 * Side effects:
 *  None.
 *
 *----------------------------------------------------------------------
 */

static int EmitObject(Tcl_Interp* interp, Tcl_Obj* objPtr, Tcl_Channel chan)
{
    const Tcl_ObjType* objTypePtr;
    char* objBytes;
    Tcl_Size objLength;
    char typeCode = CMP_STRING_CODE;
    int emitCount = 1;

    objTypePtr = objPtr->typePtr;
    objBytes = Tcl_GetStringFromObj(objPtr, &objLength);
    if (!objBytes)
    {
        objBytes = "";
        objLength = 0;
    }

    if (objTypePtr == cmpIntType)
    {
        typeCode = CMP_INT_CODE;
        emitCount = 0;
    }
    else if (objTypePtr == cmpDoubleType)
    {
        typeCode = CMP_DOUBLE_CODE;
        emitCount = 0;
    }
    else if (objTypePtr == cmpByteCodeType)
    {
        if (EmitChar(interp, CMP_BYTECODE_CODE, '\n', chan) != TCL_OK)
        {
            return TCL_ERROR;
        }
        return EmitByteCode(interp, (ByteCode*)objPtr->internalRep.otherValuePtr, chan);
    }
    else if (objTypePtr == cmpProcBodyType)
    {
        if (EmitChar(interp, CMP_PROCBODY_CODE, '\n', chan) != TCL_OK)
        {
            return TCL_ERROR;
        }
        return EmitProcBody(interp, (Proc*)objPtr->internalRep.otherValuePtr, chan);
    }
    else
    {
        if (EmitChar(interp, CMP_XSTRING_CODE, '\n', chan) != TCL_OK)
        {
            return TCL_ERROR;
        }
        return EmitByteSequence(interp, (unsigned char*)objBytes, objLength, chan);
    }
    if (EmitChar(interp, typeCode, '\n', chan) != TCL_OK)
    {
        return TCL_ERROR;
    }
    if (emitCount && (EmitTclSize(interp, objLength, '\n', chan) != TCL_OK))
    {
        return TCL_ERROR;
    }
    return EmitString(interp, objBytes, objLength, '\n', chan);
}

/*
 *----------------------------------------------------------------------
 *
 * EmitExcRangeArray --
 *
 *  Emits the exception range array for a ByteCode struct to a Tcl_Channel.
 *
 * Results:
 *  Returns TCL_OK on success, TCL_ERROR on failure.
 *
 * Side effects:
 *  None.
 *
 *----------------------------------------------------------------------
 */

static int EmitExcRangeArray(Tcl_Interp* interp, ByteCode* codePtr, Tcl_Channel chan)
{
    Tcl_Size i, numExceptRanges = codePtr->numExceptRanges;
    ExceptionRange* excArrayPtr = codePtr->exceptArrayPtr;
    char excName;

    if (EmitTclSize(interp, numExceptRanges, '\n', chan) != TCL_OK)
    {
        return TCL_ERROR;
    }

    for (i = 0; i < numExceptRanges; i++)
    {
        excName = NameFromExcRange(excArrayPtr->type);
        if (excName == '\0')
        {
            return -1;
        }

        if ((EmitChar(interp, excName, ' ', chan) != TCL_OK) ||
            (EmitTclSize(interp, excArrayPtr->nestingLevel, ' ', chan) != TCL_OK) ||
            (EmitTclSize(interp, excArrayPtr->codeOffset, ' ', chan) != TCL_OK) ||
            (EmitTclSize(interp, excArrayPtr->numCodeBytes, ' ', chan) != TCL_OK) ||
            (EmitTclSize(interp, excArrayPtr->breakOffset, ' ', chan) != TCL_OK) ||
            (EmitTclSize(interp, excArrayPtr->continueOffset, ' ', chan) != TCL_OK) ||
            (EmitTclSize(interp, excArrayPtr->catchOffset, '\n', chan) != TCL_OK))
        {
            return TCL_ERROR;
        }

        excArrayPtr += 1;
    }

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * EmitAuxDataArray --
 *
 *  Emits the AuxData array for a ByteCode struct to a Tcl_Channel.
 *
 * Results:
 *  Returns TCL_OK on success, TCL_ERROR on failure.
 *
 * Side effects:
 *  None.
 *
 *----------------------------------------------------------------------
 */

static int EmitAuxDataArray(Tcl_Interp* interp, ByteCode* codePtr, Tcl_Channel chan)
{
    int result;
    Tcl_Size i, numAuxDataItems = codePtr->numAuxDataItems;
    AuxData* auxDataPtr = codePtr->auxDataArrayPtr;
    const AuxDataType* typePtr;

    if (EmitTclSize(interp, numAuxDataItems, '\n', chan) != TCL_OK)
    {
        return TCL_ERROR;
    }

    for (i = 0; i < numAuxDataItems; i++)
    {
        /*
         * write out the type, then switch based on the AuxData type
         */
        typePtr = auxDataPtr->type;
        if (typePtr == cmpJumptableInfoType)
        {
            result = EmitChar(interp, CMP_JUMPTABLE_INFO, '\n', chan);
            if (result != TCL_OK)
            {
                return result;
            }
            result = EmitJumptableInfo(interp, (JumptableInfo*)auxDataPtr->clientData, chan);
            if (result != TCL_OK)
            {
                return result;
            }
        }
        else if (typePtr == cmpDictUpdateInfoType)
        {
            result = EmitChar(interp, CMP_DICTUPDATE_INFO, '\n', chan);
            if (result != TCL_OK)
            {
                return result;
            }
            result = EmitDictUpdateInfo(interp, (DictUpdateInfo*)auxDataPtr->clientData, chan);
            if (result != TCL_OK)
            {
                return result;
            }
        }
        else if (typePtr == cmpNewForeachInfoType)
        {
            result = EmitChar(interp, CMP_NEW_FOREACH_INFO, '\n', chan);
            if (result != TCL_OK)
            {
                return result;
            }
            result = EmitNewForeachInfo(interp, (ForeachInfo*)auxDataPtr->clientData, chan);
            if (result != TCL_OK)
            {
                return result;
            }
        }
        else
        {
            Tcl_Panic("EmitAuxDataArray: unknown AuxType \"%s\"", typePtr->name);
        }
        auxDataPtr += 1;
    }
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * EmitSignature --
 *
 *  Emit a signature string to be used in the header of a compiled TCL
 *  script. The signature is used to mark the file as a compiled TCL script.
 *
 * Results:
 *  Returns TCL_OK on success, TCL_ERROR on error.
 *
 * Side effects:
 *  None.
 *
 *----------------------------------------------------------------------
 */

static int EmitSignature(Tcl_Interp* interp, Tcl_Channel chan)
{
    if ((EmitString(interp, signatureHeader, -1, ' ', chan) != TCL_OK) ||
        (EmitTclSize(interp, formatVersion, ' ', chan) != TCL_OK) || (EmitString(interp, PACKAGE_VERSION, -1, ' ', chan) != TCL_OK) ||
        (EmitString(interp, TCL_VERSION, -1, '\n', chan) != TCL_OK))
    {
        PrependResult(interp, "error writing signature: ");
        return TCL_ERROR;
    }
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * NameFromExcRange --
 *
 *  Given an ExceptionRangeType enum, return the corresponding name.
 *
 * Results:
 *  Returns the name if it was successful at converting the enum value to
 *  a name, '\0' otherwise.
 *
 * Side effects:
 *  None.
 *
 *----------------------------------------------------------------------
 */

static char NameFromExcRange(ExceptionRangeType type)
{
    const ExcRangeMap* mapPtr;

    for (mapPtr = &excRangeMap[0]; mapPtr->name != 0; mapPtr++)
    {
        if (mapPtr->type == type)
        {
            return mapPtr->name;
        }
    }

    return '\0';
}

/*
 *----------------------------------------------------------------------
 *
 * InitTypes --
 *
 *  Uses Tcl_GetObjType to load pointers to known object types into static
 *  variables, which can then be used instead of the known objects themselves.
 *
 * Results:
 *  None.
 *
 * Side effects:
 *  None.
 *
 *----------------------------------------------------------------------
 */

static void InitTypes()
{
    if (didLoadTypes == 0)
    {
        cmpProcBodyType = Tcl_GetObjType("procbody");
        if (!cmpProcBodyType)
        {
            Tcl_Panic("InitTypes: failed to find the procbody type");
        }

        cmpByteCodeType = Tcl_GetObjType("bytecode");
        if (!cmpByteCodeType)
        {
            Tcl_Panic("InitTypes: failed to find the bytecode type");
        }

        cmpDoubleType = Tcl_GetObjType("double");
        if (!cmpByteCodeType)
        {
            Tcl_Panic("InitTypes: failed to find the double type");
        }

        Tcl_Obj* obj = Tcl_NewIntObj(0);
        cmpIntType = obj->typePtr;
        Tcl_DecrRefCount(obj);

        cmpJumptableInfoType = TclGetAuxDataType("JumptableInfo");
        if (!cmpJumptableInfoType)
        {
            Tcl_Panic("InitTypes: failed to find the JumptableInfo AuxData type");
        }
        cmpDictUpdateInfoType = TclGetAuxDataType("DictUpdateInfo");
        if (!cmpDictUpdateInfoType)
        {
            Tcl_Panic("InitTypes: failed to find the DictUpdateInfo AuxData type");
        }
        cmpNewForeachInfoType = TclGetAuxDataType("NewForeachInfo");
        if (!cmpNewForeachInfoType)
        {
            Tcl_Panic("InitTypes: failed to find the NewForeachInfo AuxData type");
        }
        didLoadTypes = 1;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * PrependResult --
 *
 *  Prepends the string in msgPtr to the current result value. Used by error
 *  reporting to augment a generic error string.
 *
 * Results:
 *  None.
 *
 * Side effects:
 *  None.
 *
 *----------------------------------------------------------------------
 */

static void PrependResult(Tcl_Interp* interp, char* msgPtr)
{
    Tcl_DString buf;
    Tcl_Obj* resultPtr = Tcl_GetObjResult(interp);

    Tcl_DStringInit(&buf);
    Tcl_DStringAppend(&buf, msgPtr, -1);
    Tcl_DStringAppend(&buf, resultPtr->bytes, resultPtr->length);

    resultPtr = Tcl_NewStringObj(Tcl_DStringValue(&buf), Tcl_DStringLength(&buf));
    Tcl_SetObjResult(interp, resultPtr);
}

/*
 *----------------------------------------------------------------------
 *
 * EmitScriptPreamble --
 *
 *  Emit the preamble for the compiled script. Writes out the TCL boilerplate
 *  that requires the loader package and evals the bytecodes.
 *
 * Results:
 *  Returns TCL_OK on success, TCL_ERROR on error.
 *
 * Side effects:
 *  None.
 *
 *----------------------------------------------------------------------
 */

static int EmitScriptPreamble(Tcl_Interp* interp, Tcl_Channel chan)
{
    char buf[256];
    char* errMsgPtr;
    int result = TCL_OK;
    Tcl_Obj* errObjPtr = 0;

    /*
     * Extract the loader error message from the package itself, and if not
     * present use the default value. This lets us modify the error message
     * in a script.
     */

    sprintf(buf, "variable %s; set %s", errorVariable, errorVariable);
    Tcl_Obj* script = Tcl_NewStringObj(buf, -1);
    Tcl_IncrRefCount(script);
    int _rc = Tcl_EvalObjEx(interp, script, 0);
    Tcl_DecrRefCount(script);
    if (_rc != TCL_OK)
    {
        errMsgPtr = errorMessage;
    }
    else
    {
        Tcl_Obj* errObjPtr = Tcl_GetObjResult(interp);
        Tcl_IncrRefCount(errObjPtr);
        errMsgPtr = errObjPtr->bytes;
    }

    sprintf(buf, preambleFormat, loaderName, loaderVersion, errMsgPtr, loaderName, evalCommand);
    if (EmitString(interp, buf, -1, '\n', chan) != TCL_OK)
    {
        PrependResult(interp, "error writing script preamble: ");
        result = TCL_ERROR;
    }

    if (errObjPtr)
    {
        Tcl_DecrRefCount(errObjPtr);
    }
    Tcl_ResetResult(interp);

    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * EmitScriptPostamble --
 *
 *  Emit the postamble for the compiled script. Writes out the TCL boilerplate
 *  that requires the loader package and evals the bytecodes.
 *
 * Results:
 *  Returns TCL_OK on success, TCL_ERROR on error.
 *
 * Side effects:
 *  None.
 *
 *----------------------------------------------------------------------
 */

static int EmitScriptPostamble(Tcl_Interp* interp, Tcl_Channel chan)
{
    char buf[256];

#if USE_CATCH_WRAPPER
    sprintf(buf, postambleFormat, errorInfoMarker);
#else
    strcpy(buf, postambleFormat);
#endif
    if (EmitString(interp, buf, -1, '\n', chan) != TCL_OK)
    {
        PrependResult(interp, "error writing script postamble: ");
        return TCL_ERROR;
    }
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * LocalProcCompileProc --
 *
 *  This procedure is registered as the CompileProc for the "proc" command,
 *  and is used to make a record of where in the bytecodes the calls to
 *  "proc" have been emitted. This information is used in a post-processing
 *  pass to compile the procedure bodies.
 *
 * Results:
 *  Returns TCL_OUT_LINE_COMPILE, to make the compiler create non-inline code.
 *
 * Side effects:
 *  None.
 *
 *----------------------------------------------------------------------
 */

static int LocalProcCompileProc(Tcl_Interp* interp, Tcl_Parse* parsePtr, Command* cmdPtr, struct CompileEnv* compEnvPtr)
{
    AppendInstLocList(interp, compEnvPtr);
    return TCL_ERROR;
}

/*
 *----------------------------------------------------------------------
 *
 * CompilerInit --
 *
 *  Initializes the internal structures used by the Compiler package.
 *  Must be called before the public interfaces to the Compiler package;
 *  this is done typically in the package registration proc (Compiler_Init).
 *
 * Results:
 *  None.
 *
 * Side effects:
 *  Initializes a number of internal data structures used by the Compiler
 *  implementation.
 *
 *----------------------------------------------------------------------
 */

void CompilerInit(Tcl_Interp* interp)
{
    CompilerContext* ctxPtr;

    /*
     * Initialize the local copies of pointers to some built-in object types.
     * We need to do it because the built-in types are not exported by the
     * windows Tcl DLL.
     */

    InitTypes();

    /*
     * Create the compiler context structure and attach it to the interp
     */

    ctxPtr = (CompilerContext*)Tcl_Alloc(sizeof(CompilerContext));
    Tcl_SetAssocData(interp, CMP_ASSOC_KEY, CleanCompilerContext, (void*)ctxPtr);
    ctxPtr->ppi = (PostProcessInfo*)NULL;
    ctxPtr->numProcs = 0;
    ctxPtr->numCompiledBodies = 0;
    ctxPtr->numUnsharedBodies = 0;
    ctxPtr->numUnshares = 0;
}

/*
 *----------------------------------------------------------------------
 *
 * CleanCompilerContext --
 *
 *  Cleans up the compiler context.
 *  Frees the post-processing info if any is present, then frees the context
 *  struct itself.
 *
 * Results:
 *  None.
 *
 * Side effects:
 *  See above.
 *
 *----------------------------------------------------------------------
 */

static void CleanCompilerContext(void* clientData, Tcl_Interp* interp)
{
    CompilerContext* ctxPtr = (CompilerContext*)clientData;

    FreePostProcessInfo(ctxPtr->ppi);
    Tcl_Free((char*)ctxPtr);
}

/*
 *----------------------------------------------------------------------
 *
 * CompilerGetContext --
 *
 *  Returns a pointer to the CompilerContext struct for the given interp.
 *
 * Results:
 *  See above.
 *
 * Side effects:
 *  None.
 *
 *----------------------------------------------------------------------
 */

CompilerContext* CompilerGetContext(Tcl_Interp* interp)
{
    CompilerContext* ctxPtr = (CompilerContext*)Tcl_GetAssocData(interp, CMP_ASSOC_KEY, NULL);

    if (!ctxPtr)
    {
        Tcl_Panic("unregistered compiler context!");
    }

    return ctxPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * InitCompilerContext --
 *
 *  Initializes the compiler context for a given interpreter.
 *
 * Results:
 *  None.
 *
 * Side effects:
 *  Allocates a new PostProcessInfo, and frees the old one.
 *
 *----------------------------------------------------------------------
 */

static void InitCompilerContext(Tcl_Interp* interp)
{
    CompilerContext* ctxPtr = CompilerGetContext(interp);

    FreePostProcessInfo(ctxPtr->ppi);
    ctxPtr->ppi = CreatePostProcessInfo();
    ctxPtr->numProcs = 0;
    ctxPtr->numCompiledBodies = 0;
    ctxPtr->numUnsharedBodies = 0;
    ctxPtr->numUnshares = 0;
}

/*
 *----------------------------------------------------------------------
 *
 * ReleaseCompilerContext --
 *
 *  Frees the post-processing info associated with the compiler context.
 *
 * Results:
 *  None.
 *
 * Side effects:
 *  None.
 *
 *----------------------------------------------------------------------
 */

static void ReleaseCompilerContext(Tcl_Interp* interp)
{
    CompilerContext* ctxPtr = CompilerGetContext(interp);

    FreePostProcessInfo(ctxPtr->ppi);
    ctxPtr->ppi = (PostProcessInfo*)NULL;
}

/*
 *----------------------------------------------------------------------
 *
 * CreateInstLocList --
 *
 *  Creates a InstLocList struct.
 *
 * Results:
 *  Returns a Tcl_Alloc'ed InstLocList, initialized with the next field set to
 *  0, and the bytecodeOffset to the given value..
 *
 * Side effects:
 *  None.
 *
 *----------------------------------------------------------------------
 */

static InstLocList* CreateInstLocList(CompileEnv* envPtr)
{
    InstLocList* listPtr = (InstLocList*)Tcl_Alloc(sizeof(struct InstLocList));
    listPtr->next = (InstLocList*)NULL;
    listPtr->bytecodeOffset = envPtr->codeNext - envPtr->codeStart;
    listPtr->commandIndex = envPtr->numCommands - 1;
    if ((listPtr->bytecodeOffset >= 9) && (INST_START_CMD == *(envPtr->codeNext - 9)))
    {
        /*
         * Tcl 8.5 core. Did emit an INST_START_CMD instruction. This
         * instruction goes away again due to us forcing the outline
         * compile in our caller (LocalProcCompileProc), so we have to
         * adjust the remembered offset. Irrelevant for the first
         * command (offset 0).
         *
         * 9 = 1byte ISC opcode + 2 4byte ISC operands.
         */
        listPtr->bytecodeOffset -= 9;
    }
    return listPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * CreatePostProcessInfo --
 *
 *  Creates a PostProcessInfo struct.
 *
 * Results:
 *  Returns a Tcl_Alloc'ed InstLocList, initialized with the list field set to
 *  0, and the numProcs field set to 0.
 *
 * Side effects:
 *  None.
 *
 *----------------------------------------------------------------------
 */

static PostProcessInfo* CreatePostProcessInfo()
{
    PostProcessInfo* infoPtr = (PostProcessInfo*)Tcl_Alloc(sizeof(PostProcessInfo));
    infoPtr->procs = (InstLocList*)NULL;
    infoPtr->numProcs = 0;
    Tcl_InitHashTable(&infoPtr->objTable, TCL_ONE_WORD_KEYS);
    infoPtr->infoArrayPtr = (ProcBodyInfo**)NULL;
    infoPtr->numUnshares = 0;
    infoPtr->numCompiledBodies = 0;

    return infoPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * FreePostProcessInfo --
 *
 *  Frees all entries in the proc location list, then the info itself.
 *
 * Results:
 *  None.
 *
 * Side effects:
 *  None.
 *
 *----------------------------------------------------------------------
 */

static void FreePostProcessInfo(PostProcessInfo* infoPtr)
{
    if (infoPtr)
    {
        InstLocList* nextPtr;
        InstLocList* listPtr;

        for (listPtr = infoPtr->procs; listPtr; listPtr = nextPtr)
        {
            nextPtr = listPtr->next;
            Tcl_Free((char*)listPtr);
        }

        FreeProcBodyInfoArray(infoPtr);

        Tcl_DeleteHashTable(&infoPtr->objTable);

        Tcl_Free((char*)infoPtr);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * AppendInstLocList --
 *
 *  Appends the given bytecode offset to the proc location list for a given
 *  interpreter.
 *
 * Results:
 *  None.
 *
 * Side effects:
 *  None.
 *
 *----------------------------------------------------------------------
 */

static void AppendInstLocList(Tcl_Interp* interp, CompileEnv* compEnvPtr)
{
    CompilerContext* ctxPtr = CompilerGetContext(interp);
    PostProcessInfo* infoPtr = ctxPtr->ppi;
    InstLocList* newPtr = CreateInstLocList(compEnvPtr);
    InstLocList* listPtr = infoPtr->procs;

    if (listPtr)
    {
        while (listPtr->next)
        {
            listPtr = listPtr->next;
        }
        listPtr->next = newPtr;
    }
    else
    {
        infoPtr->procs = newPtr;
    }

    infoPtr->numProcs += 1;
    ctxPtr->numProcs += 1;
}

/*
 *-----------------------------------------------------------------------
 *
 * CompileObject --
 *
 *  Lifted from SetByteCodeFromAny in tclCompile.c, does pretty much the
 *  same thing, except that it postprocesses the compiled string to also
 *  compile any procedure bodies. It needs to have access to the compile
 *  environment, because it may have to add objects to the list that was
 *  created by the compiler.
 *
 *  The rest of this comment block comes from the SetByteCodeFromAny header.
 *
 *  Part of the bytecode Tcl object type implementation. Attempts to
 *  generate an byte code internal form for the Tcl object "objPtr" by
 *  compiling its string representation.
 *
 * Results:
 *  The return value is a standard Tcl object result. If an error occurs
 *  during compilation, an error message is left in the interpreter's
 *  result unless "interp" is NULL.
 *
 * Side effects:
 *  Frees the old internal representation. If no error occurs, then the
 *  compiled code is stored as "objPtr"s bytecode representation.
 *  Also, if debugging, initializes the "tcl_traceCompile" Tcl variable
 *  used to trace compilations.
 *
 *----------------------------------------------------------------------
 */

static int CompileObject(Tcl_Interp* interp, Tcl_Obj* objPtr)
{
    int result;
    ProcInfo info;

    /*
     * Before starting the compile, temporarily override the Command struct
     * for the "proc" command to use our CompileProc. This lets us trap
     * calls to "proc" during compilation, from which we can compile the
     * procedure bodies
     */

    info.procCmdPtr = (Command*)Tcl_FindCommand(interp, "proc", (Tcl_Namespace*)NULL, 0);

    if (info.procCmdPtr)
    {
        /*
         * For the time being, we don't need to make sure that this is really
         * the builtin "proc" command because we are running the compiler from
         * our own script in our own executable, and therefore nobody has
         * had a chance to redefine "proc"
         */

        /*
         * EMIL we need to save the current value somewhere where
         * LocalProcCompileProc can find it and, if it is not 0,
         * call it.
         * Probably a global hash table keyed by interpreter
         */

        info.savedCompileProc = info.procCmdPtr->compileProc;
        info.procCmdPtr->compileProc = LocalProcCompileProc;
    }

    /*
     * Initialize the compiler context struct. This includes the proc location
     * list for this interpreter; this list will be populated by the local
     * compile proc and used later to compile the procedure bodies
     */

    InitCompilerContext(interp);

    result = TclSetByteCodeFromAny(interp, objPtr, PostProcessCompile, (void*)&info);

    /*
     * Restore the "proc" command compile procedure.  This may be unnecessary
     * since PostProcessCompile will normally restore the function, but in
     * error cases it may never be called.
     */

    if (info.procCmdPtr)
    {
        info.procCmdPtr->compileProc = info.savedCompileProc;
    }

    ReleaseCompilerContext(interp);

    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * PostProcessCompile --
 *
 *  Runs the postprocessing step on a compilation environment.
 *
 * Results:
 *  A standard TCL error code.
 *
 * Side effects:
 *  The various postprocessing steps may have side effects.
 *
 *----------------------------------------------------------------------
 */

static int PostProcessCompile(Tcl_Interp* interp, struct CompileEnv* compEnvPtr, void* clientData)
{
    int result;
    ProcInfo* infoPtr = (ProcInfo*)clientData;

    /*
     * restore the original compile proc for "proc" before we postprocess
     * the compiled environment. If we don't, and there are procedures that
     * call "proc" in their bodies, the numProc field in the post
     * process info gets corrupted (it is bumped up, which makes it
     * inconsistent with the number of process info struct stored).
     */

    if (infoPtr->procCmdPtr)
    {
        infoPtr->procCmdPtr->compileProc = infoPtr->savedCompileProc;
    }

    /*
     * Only postprocessing so far is the compilation of procedure bodies
     */

    result = CompileProcBodies(interp, compEnvPtr);
    if (result != TCL_OK)
    {
        return result;
    }

    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * CompileProcBodies --
 *
 *  Compiles the procedure bodies present in a compilation environment.
 *
 * Results:
 *  A standard TCL error code.
 *
 * Side effects:
 *  Compiles procedure bodies if any; the objects will have an internal
 *  bytecode representation. Additionally, if the procedure bodies were
 *  shared objects, it creates a new copy of the body object in order to
 *  make them unshared. In this case, it will modify both the bytecodes and
 *  the object table.
 *  May set the TCL result object on error.
 *
 *----------------------------------------------------------------------
 */

static int CompileProcBodies(Tcl_Interp* interp, CompileEnv* compEnvPtr)
{
    CompilerContext* ctxPtr = CompilerGetContext(interp);
    PostProcessInfo* infoPtr = ctxPtr->ppi;
    ProcBodyInfo** infoArrayPtr;
    int result = TCL_OK;
    Tcl_Size i;

    if (!infoPtr)
    {
        Tcl_Panic("CompileProcBodies: no postprocess info for interpreter");
        return TCL_ERROR;
    }

    if (infoPtr->numProcs < 1)
    {
        return TCL_OK;
    }

    CreateProcBodyInfoArray(infoPtr, compEnvPtr, &infoArrayPtr);
    LoadObjRefInfoTable(infoPtr, compEnvPtr);

    /*
     * Before compiling, check for shared objects and, if there are any,
     * copy them to new objects.
     */

    UnshareProcBodies(interp, ctxPtr, compEnvPtr);

    /*
     * Compile the procedure bodies.
     */

    infoPtr->numCompiledBodies = 0;
    for (i = 0; i < infoPtr->numProcs; i++)
    {
        if (infoArrayPtr[i]->bodyNewIndex != -1)
        {
            result = CompileOneProcBody(interp, infoArrayPtr[i], ctxPtr, compEnvPtr);
            if (result != TCL_OK)
            {
                return result;
            }
            infoPtr->numCompiledBodies++;
        }
    }

    /*
     * If some procedure bodies have been compiled, we need to modify the
     * bytecodes and related data structures
     */

    UpdateByteCodes(infoPtr, compEnvPtr);

    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * CreateProcBodyInfoArray --
 *
 *  Allocate and populate an array of ProcBodyInfo structs.
 *  This is a NULL-terminated array of pointers to ProcBodyInfo, each
 *  initialized from the corresponding procedure body.
 *  The allocated array is placed in the infoArrayPtr field of locInfoPtr,
 *  and is owned by it; it will be released when locInfoPtr is freed.
 *
 * Results:
 *  None.
 *
 * Side effects:
 *  Tcl_Panics on error.
 *
 *----------------------------------------------------------------------
 */

static void CreateProcBodyInfoArray(PostProcessInfo* locInfoPtr, CompileEnv* compEnvPtr, ProcBodyInfo*** arrayPtrPtr)
{
    Tcl_Size allocSize, arraySize;
    Tcl_Size i, numProcs = locInfoPtr->numProcs;
    char* allocPtr;
    ProcBodyInfo** infoAryPtr;
    ProcBodyInfo* infoPtr;
    InstLocList* locPtr;

    FreeProcBodyInfoArray(locInfoPtr);

    /*
     * allocate a single area, used for both the array of pointers and the
     * info structs
     */

    arraySize = (numProcs + 1) * sizeof(ProcBodyInfo*);
    arraySize += TCL_ALIGN(arraySize); /* align the info array */
    allocSize = arraySize + (numProcs * sizeof(ProcBodyInfo));
    allocPtr = Tcl_Alloc(allocSize);

    locInfoPtr->infoArrayPtr = (ProcBodyInfo**)allocPtr;
    infoAryPtr = locInfoPtr->infoArrayPtr;
    for (i = 0; i <= numProcs; i++)
    {
        *infoAryPtr = (ProcBodyInfo*)NULL;
        infoAryPtr += 1;
    }

    /*
     * get the object indexes for the body and args objects,
     * and the push offsets
     */

    infoPtr = (ProcBodyInfo*)(allocPtr + arraySize);
    infoAryPtr = locInfoPtr->infoArrayPtr;
    locPtr = locInfoPtr->procs;
    for (i = 0; i < numProcs; i++)
    {
        *infoAryPtr = infoPtr;
        LoadProcBodyInfo(locPtr, compEnvPtr, infoPtr);
        infoAryPtr += 1;
        infoPtr += 1;
        locPtr = locPtr->next;
    }

    if (arrayPtrPtr != NULL)
    {
        *arrayPtrPtr = locInfoPtr->infoArrayPtr;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * FreeProcBodyInfoArray --
 *
 *  Frees the array of ProcBodyInfo structs in the PostProcessInfo struct.
 *
 * Results:
 *  None.
 *
 * Side effects:
 *  None.
 *
 *----------------------------------------------------------------------
 */

static void FreeProcBodyInfoArray(PostProcessInfo* infoPtr)
{
    if (infoPtr->infoArrayPtr)
    {
        Tcl_Free((char*)infoPtr->infoArrayPtr);
    }
    infoPtr->infoArrayPtr = (ProcBodyInfo**)NULL;
}

/*
 *----------------------------------------------------------------------
 *
 * LoadProcBodyInfo --
 *
 *  Populate a ProcBodyInfo struct.  This function determines if a
 *  procedure body should be compiled or not; see the comments in
 *  the body, below, for a description of the algorithm.  A
 *  procedure body that should not be compiled has its
 *  bodyNewIndex field set to -1.
 *
 * Results:
 *  None.
 *
 * Side effects:
 *  None.
 *
 *---------------------------------------------------------------------- */

static void LoadProcBodyInfo(InstLocList* locInfoPtr, CompileEnv* compEnvPtr, ProcBodyInfo* infoPtr)
{
    unsigned char* pc = compEnvPtr->codeStart + locInfoPtr->bytecodeOffset;

    /*
     * Here is where we scan the bytecodes and figure out where the args
     * and the procedure body are put on the stack.
     *
     * Currently, we only detect the simplest (and most common) case, where
     * all arguments to proc are pushed as literals, like this:
     *      proc a { a1 a2 } { return [list $a1 $a2] }
     * This corresponds to an expected sequence of commands like this:
     *      PUSH, PUSH, PUSH, PUSH, INVOKE_STK1
     * where PUSH can be either PUSH1 or PUSH4, but INVOKE_STK1 only is
     * expected (because there are only 4 arguments).
     * The operand of the first PUSH is the index to the "proc" string,
     * for the second it is the procedure name, for the third the argument
     * list, and for the fourth the procedure body.
     *
     * If the args or the body are not pushed as literals, then the PUSH
     * should be followed by different opcodes; for example, something like
     * this
     *      set body { return [list $a1 $a2] }
     *      proc a { a1 a2 } $b
     * generates a sequence like (PUSH "b"; LOAD) when the body is loaded,
     * and we shouldn't compile "b" (of course!) and neither the object
     * loaded as "b" (because it may not be a literal).
     *
     * The tough part is how do we detect stuff like this:
     *      proc $procName { a1 a2 ] { return [list $a1 $a2] }
     */

    infoPtr->commandIndex = locInfoPtr->commandIndex;
    infoPtr->procOffset = (pc - compEnvPtr->codeStart);
    infoPtr->nameIndex = -1;
    infoPtr->argsIndex = -1;
    infoPtr->bodyOrigIndex = -1;
    infoPtr->bodyNewIndex = -1;
    infoPtr->bodyOffset = -1; /* Bugzilla 89467 */

    /*
     * Skip the "proc" string
     */

    switch (*pc)
    {
        case INST_PUSH1:
            pc += 2;
            break;

        case INST_PUSH4:
            pc += 5;
            break;

        default:
            return;
    }

    /*
     * get the index of the proc name
     */

    switch (*pc)
    {
        case INST_PUSH1:
            infoPtr->nameIndex = TclGetUInt1AtPtr(pc + 1);
            pc += 2;
            break;

        case INST_PUSH4:
            infoPtr->nameIndex = TclGetUInt4AtPtr(pc + 1);
            pc += 5;
            break;

        default:
            return;
    }

    /*
     * get the index of the argument list
     */

    switch (*pc)
    {
        case INST_PUSH1:
            infoPtr->argsIndex = TclGetUInt1AtPtr(pc + 1);
            pc += 2;
            break;

        case INST_PUSH4:
            infoPtr->argsIndex = TclGetUInt4AtPtr(pc + 1);
            pc += 5;
            break;

        default:
            return;
    }

    /*
     * get the index of the procedure body, and save the offset to the
     * push instruction.
     */

    infoPtr->bodyOffset = (pc - compEnvPtr->codeStart);
    switch (*pc)
    {
        case INST_PUSH1:
            infoPtr->bodyOrigIndex = TclGetUInt1AtPtr(pc + 1);
            pc += 2;
            break;

        case INST_PUSH4:
            infoPtr->bodyOrigIndex = TclGetUInt4AtPtr(pc + 1);
            pc += 5;
            break;

        default:
            return;
    }

    /*
     * (****)
     */

    infoPtr->bodyNewIndex = infoPtr->bodyOrigIndex;

    /*
     * finally, check if the following instruction is a INVOKE_STK1 with
     * argument 4
     */

    if ((*pc != INST_INVOKE_STK1) || (TclGetUInt1AtPtr(pc + 1) != 4))
    {
        infoPtr->nameIndex = -1;
        infoPtr->argsIndex = -1;
        infoPtr->bodyOrigIndex = -1;
        infoPtr->bodyNewIndex = -1;
    }

    return;
}

/*
 *----------------------------------------------------------------------
 *
 * LoadObjRefInfoTable --
 *
 *  Load the object reference table.
 *
 * Results:
 *  None.
 *
 * Side effects:
 *  Populates the objTable field of the locInfoPtr struct.
 *
 *----------------------------------------------------------------------
 */

static void LoadObjRefInfoTable(PostProcessInfo* locInfoPtr, CompileEnv* compEnvPtr)
{
    Tcl_HashTable* objTablePtr;
    ProcBodyInfo** infoAryPtr;
    ProcBodyInfo* infoPtr;
    Tcl_HashEntry* entryPtr;
    int isNew;
    Tcl_Size objIndex;
    ObjRefInfo* refInfoPtr;
    InstructionDesc* opCodesTablePtr;
    unsigned char* pc;

    CleanObjRefInfoTable(locInfoPtr);

    /*
     * count how many references to this object as a procedure body
     */

    objTablePtr = &locInfoPtr->objTable;
    for (infoAryPtr = locInfoPtr->infoArrayPtr; *infoAryPtr; infoAryPtr++)
    {
        infoPtr = *infoAryPtr;
        entryPtr = Tcl_CreateHashEntry(objTablePtr, (char*)INT2PTR(infoPtr->bodyOrigIndex), &isNew);
        if (isNew)
        {
            refInfoPtr = (ObjRefInfo*)Tcl_Alloc(sizeof(ObjRefInfo));
            refInfoPtr->numReferences = 0;
            refInfoPtr->numProcReferences = 0;
            refInfoPtr->numUnshares = 0;

            Tcl_SetHashValue(entryPtr, (void*)refInfoPtr);
        }
        else
        {
            refInfoPtr = (ObjRefInfo*)Tcl_GetHashValue(entryPtr);
        }

        refInfoPtr->numProcReferences += 1;
    }

    /*
     * now scan the bytecodes and count the references from the bytecodes.
     * Note that this count includes references from the PUSH instructions
     * in the "proc" call.
     */

    opCodesTablePtr = (InstructionDesc*)TclGetInstructionTable();
    for (pc = compEnvPtr->codeStart; pc < compEnvPtr->codeNext;)
    {
        objIndex = GetSharedIndex(pc);
        if (objIndex >= 0)
        {
            entryPtr = Tcl_FindHashEntry(objTablePtr, (char*)INT2PTR(objIndex));
            if (entryPtr)
            {
                /*
                 * this is a reference to a known procedure body
                 */

                refInfoPtr = (ObjRefInfo*)Tcl_GetHashValue(entryPtr);
                refInfoPtr->numReferences += 1;
            }
        }

        pc += opCodesTablePtr[*pc].numBytes;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * CleanObjRefInfoTable --
 *
 *  Releases all entries in the object reference table.
 *
 * Results:
 *  None.
 *
 * Side effects:
 *  See above.
 *
 *----------------------------------------------------------------------
 */

static void CleanObjRefInfoTable(PostProcessInfo* locInfoPtr)
{
    Tcl_HashSearch iterCtx; /* the iteration context */
    Tcl_HashEntry* entryPtr;
    ObjRefInfo* refInfoPtr;

    for (entryPtr = Tcl_FirstHashEntry(&locInfoPtr->objTable, &iterCtx); entryPtr; entryPtr = Tcl_NextHashEntry(&iterCtx))
    {
        refInfoPtr = (ObjRefInfo*)Tcl_GetHashValue(entryPtr);
        Tcl_Free((char*)refInfoPtr);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * CompileOneProcBody --
 *
 *  Compiles a procedure body.
 *  Much of this code is derived from Tcl_ProcObjCmd and TclObjInterpProc.
 *
 * Results:
 *  A standard TCL error code.
 *
 * Side effects:
 *  If the call succeeds, the objects will have an internal bytecode
 *  representation. Additionally, if the object was shared, a new copy is
 *  created in order to make it unshared; in this case, the new object is
 *  added to the object table and the new index field in infoPtr is updated.
 *  May set the TCL result object on error.
 *
 *----------------------------------------------------------------------
 */

static int CompileOneProcBody(Tcl_Interp* interp, ProcBodyInfo* infoPtr, CompilerContext* ctxPtr, CompileEnv* compEnvPtr)
{
    Interp* iPtr = (Interp*)interp;
    Proc* procPtr = (Proc*)NULL;
    Tcl_Command cmd = (Tcl_Command)NULL;

    CompiledLocal* localPtr;
    Proc* saveProcPtr;
    Tcl_Obj* bodyPtr;
    Tcl_Obj* procObjPtr;
    Tcl_Size numArgs, i;
    char* fullName;
    char cmdNameBuf[64];
    const char** argArray = NULL;
    const char* p;
    int result = TCL_OK;

    if (infoPtr->bodyNewIndex == -1)
    {
        return TCL_OK;
    }

    /*
     * Here we get the current namespace. We have to do it differently
     * than Tcl_ProcObjCmd does, because the current namespace is NOT the one
     * that is current at the time this proc is executed (we are compiling the
     * file).
     *
     * For the time being, the full name is the one that was pushed.
     */

    fullName = Tcl_GetString(compEnvPtr->literalArrayPtr[infoPtr->nameIndex].objPtr);

    /*
     * the newIndex is the one of the unshared object, so there is no need
     * to do any shared checks here
     */

    bodyPtr = compEnvPtr->literalArrayPtr[infoPtr->bodyNewIndex].objPtr;

    /*
     * Create and initialize a Proc structure for the procedure. Note that
     * we initialize its cmdPtr field below after we've created the command
     * for the procedure. HOWEVER, differently from Tcl_ProcObjCmd, the
     * command we create is just a dummy for use by the compiler code; it will
     * be deleted after the compilation.
     * We increment the ref count of the procedure's body object since there
     * will be a reference to it in the Proc structure.
     */

    Tcl_IncrRefCount(bodyPtr);

    procPtr = (Proc*)Tcl_Alloc(sizeof(Proc));
    procPtr->iPtr = iPtr;
    procPtr->refCount = 1;
    procPtr->bodyPtr = bodyPtr;
    procPtr->firstLocalPtr = NULL;
    procPtr->lastLocalPtr = NULL;

    /*
     * Break up the argument list into argument specifiers, then process
     * each argument specifier.
     */

    result = Tcl_SplitList(interp, Tcl_GetString(compEnvPtr->literalArrayPtr[infoPtr->argsIndex].objPtr), &numArgs, &argArray);
    if (result != TCL_OK)
    {
        Tcl_DecrRefCount(bodyPtr);
        Tcl_Free((char*)procPtr);
        return result;
    }

    procPtr->numArgs = numArgs;
    procPtr->numCompiledLocals = numArgs;

    for (i = 0; i < numArgs; i++)
    {
        Tcl_Size fieldCount;
        Tcl_Size nameLength, valueLength;
        const char** fieldValues;

        /*
         * Now divide the specifier up into name and default.
         */

        result = Tcl_SplitList(interp, argArray[i], &fieldCount, &fieldValues);
        if (result != TCL_OK)
        {
            result = TCL_ERROR;
            break;
        }
        if (fieldCount > 2)
        {
            Tcl_Free((char*)fieldValues);
            Tcl_AppendStringsToObj(Tcl_GetObjResult(interp),
                                   "compilation of procedure \"",
                                   fullName,
                                   "\" failed: too many fields in argument specifier \"",
                                   argArray[i],
                                   "\"",
                                   (char*)NULL);
            result = TCL_ERROR;
            break;
        }

        if ((fieldCount == 0) || (*fieldValues[0] == 0))
        {
            Tcl_Free((char*)fieldValues);
            Tcl_AppendStringsToObj(Tcl_GetObjResult(interp),
                                   "compilation of procedure \"",
                                   fullName,
                                   "\" failed: argument with no name",
                                   (char*)NULL);
            result = TCL_ERROR;
            break;
        }

        nameLength = strlen(fieldValues[0]);
        if (fieldCount == 2)
        {
            valueLength = strlen(fieldValues[1]);
        }
        else
        {
            valueLength = 0;
        }

        /*
         * Check that the formal parameter name is a scalar.
         */

        p = fieldValues[0];
        while (*p != '\0')
        {
            if (*p == '(')
            {
                const char* q = p;
                do
                {
                    q++;
                } while (*q != '\0');
                q--;
                if (*q == ')')
                { /* we have an array element */
                    Tcl_AppendStringsToObj(Tcl_GetObjResult(interp),
                                           "compilation of procedure \"",
                                           fullName,
                                           "\" failed: formal parameter \"",
                                           fieldValues[0],
                                           "\" is an array element",
                                           (char*)NULL);
                    Tcl_Free((char*)fieldValues);
                    Tcl_DecrRefCount(bodyPtr);
                    Tcl_Free((char*)procPtr);
                    return result;
                }
            }
            p++;
        }

        /*
         * Allocate an entry in the runtime procedure frame's array of local
         * variables for the argument.
         */
        localPtr = (CompiledLocal*)Tcl_Alloc(offsetof(CompiledLocal, name) + 1U + nameLength);

        if (procPtr->firstLocalPtr == NULL)
        {
            procPtr->firstLocalPtr = procPtr->lastLocalPtr = localPtr;
        }
        else
        {
            procPtr->lastLocalPtr->nextPtr = localPtr;
            procPtr->lastLocalPtr = localPtr;
        }
        localPtr->nextPtr = NULL;
        localPtr->nameLength = nameLength;
        localPtr->frameIndex = i;
        localPtr->flags = VAR_ARGUMENT;
        localPtr->resolveInfo = NULL;

        if (fieldCount == 2)
        {
            localPtr->defValuePtr = Tcl_NewStringObj(fieldValues[1], valueLength);
            Tcl_IncrRefCount(localPtr->defValuePtr);
        }
        else
        {
            localPtr->defValuePtr = NULL;
        }
        strcpy(localPtr->name, fieldValues[0]);

        Tcl_Free((char*)fieldValues);
    }

    if (result != TCL_OK)
    {
        for (localPtr = procPtr->firstLocalPtr; localPtr;)
        {
            CompiledLocal* next = localPtr->nextPtr;
            if (localPtr->defValuePtr)
                Tcl_DecrRefCount(localPtr->defValuePtr);
            Tcl_Free((void*)localPtr);
            localPtr = next;
        }
        Tcl_DecrRefCount(bodyPtr);
        Tcl_Free((char*)procPtr);
        Tcl_Free((char*)argArray);
        return result;
    }

    /*
     * Now create a command for the procedure. This will initially be in
     * the current namespace unless the procedure's name included namespace
     * qualifiers. To create the new command in the right namespace, we
     * generate a fully qualified name for it.
     */

    /*
     * Make sure that the temporary name is not already used
     */

    do
    {
        sprintf(cmdNameBuf, dummyCommandName, dummyCommandCounter);
        cmd = Tcl_FindCommand(interp, dummyCommandName, (Tcl_Namespace*)NULL, TCL_GLOBAL_ONLY);
        dummyCommandCounter += 1;
    } while (cmd != (Tcl_Command)NULL);

    cmd = Tcl_CreateObjCommand(interp, cmdNameBuf, DummyObjInterpProc, (void*)procPtr, CmpDeleteProc);

    if (cmd == (Tcl_Command)NULL)
    {
        for (localPtr = procPtr->firstLocalPtr; localPtr;)
        {
            CompiledLocal* next = localPtr->nextPtr;
            if (localPtr->defValuePtr)
                Tcl_DecrRefCount(localPtr->defValuePtr);
            Tcl_Free((char*)localPtr);
            localPtr = next;
        }
        Tcl_DecrRefCount(bodyPtr);
        Tcl_Free((char*)procPtr);
        Tcl_Free((char*)argArray);
        return TCL_ERROR;
    }

    /*
     * Now initialize the new procedure's cmdPtr field. This will be used
     * later when the procedure is called to determine what namespace the
     * procedure will run in. This will be different than the current
     * namespace if the proc was renamed into a different namespace.
     */

    procPtr->cmdPtr = (Command*)cmd;

    /*
     * At this stage, we are ready to compile the procedure body.
     * Much of this code is derived from TclObjInterpProc.
     *
     * We force a recompilation of the body, even if the body is already
     * of bytecode type.
     */

    if (bodyPtr->typePtr)
    {
        bodyPtr->typePtr->freeIntRepProc(bodyPtr);
        bodyPtr->typePtr = (Tcl_ObjType*)NULL;
    }

    saveProcPtr = iPtr->compiledProcPtr;
    iPtr->compiledProcPtr = procPtr;
    result = cmpByteCodeType->setFromAnyProc(interp, bodyPtr);
    iPtr->compiledProcPtr = saveProcPtr;

    if (result != TCL_OK)
    {
        if (result == TCL_ERROR)
        {
            char buf[100];
            Tcl_Size numChars;
            char* ellipsis;

            /*
             * Prepend the procedure name to the error object
             */

            sprintf(buf, "compilation of procedure \"%s\" failed: ", fullName);
            PrependResult(interp, buf);

            numChars = strlen(fullName);
            ellipsis = "";
            if (numChars > 50)
            {
                numChars = 50;
                ellipsis = "...";
            }
            sprintf(buf,
                    "\n    (compiling body of proc \"%.*s%s\", line %d)",
                    (int)numChars,
                    fullName,
                    ellipsis,
                    Tcl_GetErrorLine(interp));
            Tcl_AppendObjToErrorInfo(interp, Tcl_NewStringObj(buf, -1));
        }
        Tcl_DeleteCommandFromToken(interp, cmd);
        Tcl_Free((char*)argArray);
        return result;
    }

    ctxPtr->numCompiledBodies += 1;

    /*
     * Now that we have compiled the procedure, create a new TCL object
     * containing both the bytecodes and the info stored in the Proc struct.
     * this info is usually generated at run time during the compilation of
     * the procedure body, but of course this won't be possible in our case
     * (the body is already compiled). So, we use this strategy:
     *   1. compile the body, then save the relevant parts of Proc into a
     *      procbody object. This object replaces the proc body object
     *      in the object table.
     *   2. tweak the name of the command to call from "proc" to
     *      "loader::bcproc". This is our version of "proc" that knows how
     *      to reconstruct the Proc struct from the TclProProcBody object.
     */

    procObjPtr = TclNewProcBodyObj(procPtr);
    Tcl_IncrRefCount(procObjPtr);
    compEnvPtr->literalArrayPtr[infoPtr->bodyNewIndex].objPtr = procObjPtr;
    Tcl_DecrRefCount(bodyPtr);
    Tcl_Free((char*)argArray);
    if (cmd)
    {
        Tcl_DeleteCommandFromToken(interp, cmd);
    }
    else
    {
        TclProcCleanupProc(procPtr);
    }

    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * DummyObjInterpProc --
 *
 *  Dummy, used to supply a nonzero value to the dummyCommand object.
 *
 * Results:
 *  Returns TCL_OK.
 *
 * Side effects:
 *  None.
 *
 *----------------------------------------------------------------------
 */

static int DummyObjInterpProc(void* clientData, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[])
{
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * CmpDeleteProc --
 *
 *  From ProcDeleteProc in tclProc.c; the rest of the header is the one for
 *  ProcDeleteProc.
 *
 *  This procedure is invoked just before a command procedure is
 *  removed from an interpreter.  Its job is to release all the
 *  resources allocated to the procedure.
 *
 * Results:
 *  None.
 *
 * Side effects:
 *  Memory gets freed, unless the procedure is actively being
 *  executed.  In this case the cleanup is delayed until the
 *  last call to the current procedure completes.
 *
 *----------------------------------------------------------------------
 */

static void CmpDeleteProc(void* clientData)
{
    Proc* procPtr = (Proc*)clientData;

    procPtr->refCount--;
    if (procPtr->refCount <= 0)
    {
        TclProcCleanupProc(procPtr);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * UnshareProcBodies --
 *
 *  If any of the procedure bodies are shared, create duplicate objects so
 *  that they are no longer shared. The index to the new object is stored in
 *  the info structs for later use by the compilation procedure.
 *
 * Results:
 *  None.
 *
 * Side effects:
 *  May add objects to the object table for the compilation environment.
 *  Tcl_Panics on error.
 *
 *----------------------------------------------------------------------
 */

static void UnshareProcBodies(Tcl_Interp* interp, CompilerContext* ctxPtr, CompileEnv* compEnvPtr)
{
    PostProcessInfo* infoPtr = ctxPtr->ppi;
    ProcBodyInfo** infoArrayPtr;
    ProcBodyInfo* bodyInfoPtr;
    Tcl_Size origIndex;
    Tcl_HashTable* objTablePtr;
    Tcl_HashEntry* entryPtr;
    ObjRefInfo* refInfoPtr;

    infoPtr->numUnshares = 0;

    if (infoPtr->numProcs < 1)
    {
        return;
    }

    objTablePtr = &infoPtr->objTable;
    for (infoArrayPtr = infoPtr->infoArrayPtr; *infoArrayPtr; infoArrayPtr++)
    {
        bodyInfoPtr = *infoArrayPtr;
        origIndex = bodyInfoPtr->bodyOrigIndex;
        if (origIndex != -1)
        {
            entryPtr = Tcl_FindHashEntry(objTablePtr, (char*)INT2PTR(origIndex));
            if (!entryPtr)
            {
                Tcl_Panic("UnshareProcBodies: no ObjRefInfo entry in objTable!");
            }
            refInfoPtr = (ObjRefInfo*)Tcl_GetHashValue(entryPtr);

            if (refInfoPtr->numReferences < 2)
            {
                /*
                 * Not a shared object, but we still need to remove
                 * the object from the literal hash table so it
                 * doesn't show up as a local literal without a global.
                 */

                TclHideLiteral(interp, compEnvPtr, bodyInfoPtr->bodyNewIndex);

                continue;
            }

            /*
             * If the only sharing is among procedure bodies, then we can
             * copy N - 1 objects, and compile one in place.
             * But if at least one other entity is sharing, then we must
             * unshare all the procedure bodies.
             */

            if ((refInfoPtr->numReferences == refInfoPtr->numProcReferences) && (refInfoPtr->numUnshares < 1))
            {
                /*
                 * do not copy the first occurrence, just remove it from
                 * the global and local literal hash tables
                 */

                TclHideLiteral(interp, compEnvPtr, bodyInfoPtr->bodyNewIndex);
                refInfoPtr->numUnshares = 1;
            }
            else
            {
                /* (xxxx) */

                bodyInfoPtr->bodyNewIndex = UnshareObject(origIndex, compEnvPtr);
                refInfoPtr->numUnshares += 1;
                infoPtr->numUnshares += 1;
                ctxPtr->numUnshares += 1;
            }

            if (refInfoPtr->numUnshares == 1)
            {
                ctxPtr->numUnsharedBodies += 1;
            }
        }
    }
}

/*
 *----------------------------------------------------------------------
 *
 * UnshareObject --
 *
 *  Creates a copy of an object, and adds it to the object table for the
 *  compilation environment.
 *
 * Results:
 *  Returns the index to the newly created object.
 *
 * Side effects:
 *  None.
 *
 *----------------------------------------------------------------------
 */

static int UnshareObject(Tcl_Size origIndex, CompileEnv* compEnvPtr)
{
    return TclAddLiteralObj(compEnvPtr, Tcl_DuplicateObj(compEnvPtr->literalArrayPtr[origIndex].objPtr), NULL);
}

/*
 *----------------------------------------------------------------------
 *
 * UpdateByteCodes --
 *
 *  If any of the procedure bodies have been compiled, or have been unshared
 *  (if they are unshared then they also were compiled), we need to modify the
 *  bytecodes so that the correct object index is pushed on the stack.
 *  There are two cases where we need to modify the bytecodes:
 *    1. A procedure body that has been compiled is stored as a TclProBodyType
 *       Tcl_Obj; this is an object that contains the ByteCode, and also some
 *       fields from the Proc struct, which need to be loaded at runtime
 *       (they cannot be regenerated at runtime because the body is already
 *       compiled, and the compiler does that work at runtime). In this case,
 *       we need to push the name of the loader package's bcproc command,
 *       which is a modified version of proc which knows how to handle the
 *       TclProBodyType object.
 *    2. If the body has been unshared, we have to make sure the the correct
 *       index in the object table is used; the original one refers to the
 *       shared object.
 *
 * Results:
 *  None.
 *
 * Side effects:
 *  May modify the bytecodes, possibly reallocating the bytecode array; may
 *  also modify support data structures in the compilation environment.
 *  Tcl_Panics on error.
 *
 *----------------------------------------------------------------------
 */

static void UpdateByteCodes(PostProcessInfo* infoPtr, CompileEnv* compEnvPtr)
{
    ProcBodyInfo** infoArrayPtr;
    ProcBodyInfo* bodyInfoPtr;
    Tcl_Size newIndex;
    unsigned char* pc;
    Tcl_Size offset, delta;
    Tcl_Size procNameObjIndex;
    Tcl_Obj* objPtr;

    if (infoPtr->numCompiledBodies == 0)
    {
        return;
    }

    /*
     * Some bodies were compiled: create a new string object containing the
     * name of the loader package's proc command, add it to the object table,
     * and use its index as the operand to the first PUSH instruction.
     */

    objPtr = Tcl_NewObj();
    Tcl_IncrRefCount(objPtr);
    Tcl_AppendStringsToObj(objPtr, loaderName, "::", procCommand, NULL);
    procNameObjIndex = TclAddLiteralObj(compEnvPtr, objPtr, NULL);
    Tcl_DecrRefCount(objPtr);

#ifdef DEBUG_REWRITE
    fprintf(stdout, "tbcload::bcproc @ %ld\n", procNameObjIndex);
    fflush(stdout);
#endif
    if (procNameObjIndex >= 255)
    {
        /*
         * This literal index signals that all the primary INST_PUSH
         * instructions (for the proc command name) will be rewritten from
         * push1 to push4, growing the bytecode (by 3 bytes per compiled
         * procedure). This means that any JUMP instructions around procedure
         * definitions require updates to their offsets to avoid having them
         * jumping into the middle of an instruction. Assuming that there are
         * jump instructions.
         *
         * The easiest way of doing that is to not only record where all jump
         * instructions are, but to rewrite them all to jump4. Otherwise we
         * would have to do some iterative rewriting where changing the offset
         * of a jump makes it larger, forcing more jump instructions to be
         * rewritten. It is likely easier to simply convert them all to the
         * large form.
         *
         * First we have to scan the bytecode and check if there are jump
         * instructions. If not then there is no need to run the complex jump
         * compensation code and the regular rewrite below (at %%%%) is fine.
         *
         * If jump instructions are present we have to scan a second time,
         * recording where all instructions are, and how much they are shifted
         * by instruction expansion, applied to _ALL_ jump and push
         * instructions. Then we scan a third time and build the expanded
         * bytecode, compensating for the shift in all jump instructions. This
         * includes updating all the auxiliary structures recording
         * instruction offsets and size.
         *
         * At last the expanded bytecode goes into the regular rewrite at
         * (%%%%). There no expansion will happen any longer, as all the
         * relevant instructions are already in their push4 forms, and any
         * jump offsets are corrected already as well.
         */

        int jumps = 0;
        InstructionDesc* opCodesTablePtr = (InstructionDesc*)TclGetInstructionTable();

        /*
         * Scan for jumps.
         */

        for (pc = compEnvPtr->codeStart; pc < compEnvPtr->codeNext;)
        {
            if ((*pc >= INST_JUMP1) && (*pc <= INST_JUMP_FALSE4))
            {
                jumps++;
            }
            pc += opCodesTablePtr[*pc].numBytes;
        }

        if (jumps)
        {
            Tcl_Size codesize = compEnvPtr->codeNext - compEnvPtr->codeStart;
            Tcl_Size* delta = (Tcl_Size*)Tcl_Alloc(codesize * sizeof(Tcl_Size));
            Tcl_Size offset = 0;

#ifdef DEBUG_REWRITE
            fprintf(stderr, "=== BEFORE START ===\n");
            {
                Tcl_Size i, numExceptRanges = compEnvPtr->exceptArrayNext;
                ExceptionRange* excPtr = compEnvPtr->exceptArrayPtr;
                for (i = 0; i < numExceptRanges; i++)
                {
                    switch (excPtr->type)
                    {
                        case CATCH_EXCEPTION_RANGE:
                            fprintf(stderr,
                                    "EC [%8ld] @%4ld /%4ld : %4ld\n",
                                    i,
                                    excPtr->codeOffset,
                                    excPtr->numCodeBytes,
                                    excPtr->catchOffset);
                            break;
                        case LOOP_EXCEPTION_RANGE:
                            fprintf(stderr,
                                    "EL [%8ld] @%4ld /%4ld : %4ld %4ld\n",
                                    i,
                                    excPtr->codeOffset,
                                    excPtr->numCodeBytes,
                                    excPtr->breakOffset,
                                    excPtr->continueOffset);
                            break;
                        default:
                            fprintf(stderr, "E? [%8ld] @%4ld /%4ld\n", i, excPtr->codeOffset, excPtr->numCodeBytes);
                    }
                    excPtr += 1;
                }
            }
            for (pc = compEnvPtr->codeStart; pc < compEnvPtr->codeNext;)
            {
                FormatInstruction(compEnvPtr, pc);
                pc += opCodesTablePtr[*pc].numBytes;
            }
            fprintf(stderr, "=== BEFORE END =====\n");
            fflush(stderr);
#endif
            /*
             * Compute per-instruction shift offsets under the assumption that
             * all jump1 and push1 instructions are expanded. The value of
             * 'offset' after this loop is the total amount of expansion
             * required. This may be nothing if all instructions are already
             * in *4 form. In that case we can skip the expansion-rewrite.
             */

            for (pc = compEnvPtr->codeStart; pc < compEnvPtr->codeNext;)
            {
                delta[pc - compEnvPtr->codeStart] = offset;
                switch (*pc)
                {
                    case INST_JUMP1:
                    case INST_JUMP_TRUE1:
                    case INST_JUMP_FALSE1:
                    case INST_PUSH1:
                        offset += 3;
                        break;
                    default:
                        break;
                }
                pc += opCodesTablePtr[*pc].numBytes;
            }

            if (offset)
            {
                /*
                 * We use a helper array for the expanded bytecode to avoid
                 * lots of shifting. We basically copy instructions from the
                 * original array over to the expanded one, expanding them as
                 * we go, and when we are done we expand the compilation
                 * environment proper and copy things back.
                 */

                Tcl_Size newcodesize = codesize + offset;
                unsigned char* newbc = (unsigned char*)Tcl_Alloc(newcodesize * sizeof(unsigned char));
                unsigned char* pcnew = newbc;
                Tcl_Size isize;

                for (pc = compEnvPtr->codeStart; pc < compEnvPtr->codeNext;)
                {
                    isize = opCodesTablePtr[*pc].numBytes;
#ifdef DEBUG_REWRITE
                    fprintf(stderr,
                            "[%8ld] d%4ld s%2ld %s\n",
                            pc - compEnvPtr->codeStart,
                            delta[(pc - compEnvPtr->codeStart)],
                            isize,
                            opCodesTablePtr[*pc].name);
                    fflush(stderr);
#endif
                    switch (*pc)
                    {
                        case INST_JUMP1:
                        case INST_JUMP_TRUE1:
                        case INST_JUMP_FALSE1:
                        {
                            /*
                             * These instructions expand to *4 form, and may have
                             * to change their jump offset to compensate for
                             * differences in shift for this instruction and at
                             * the jump destination.
                             */

                            int jmpdelta = TclGetInt1AtPtr(pc + 1);
                            int jmpshift = delta[(pc - compEnvPtr->codeStart)];
                            int dstshift = delta[(pc - compEnvPtr->codeStart) + jmpdelta];

                            if (jmpshift != dstshift)
                            {
#ifdef DEBUG_REWRITE
                                fprintf(stderr,
                                        "           JUMP1 change %4d (%4d/%4d) by %4d, now %4d\n",
                                        jmpdelta,
                                        jmpshift,
                                        dstshift,
                                        dstshift - jmpshift,
                                        jmpdelta + (dstshift - jmpshift));
                                fflush(stderr);
#endif
                                jmpdelta += (dstshift - jmpshift);
                            }

                            /*
                             * Instruction change!
                             * HACK :: Assumes that the *1 and *4 forms
                             * are paired, with *4 one higher than *1.
                             * See tclCompile.h
                             */
                            TclUpdateInstInt4AtPc((*pc) + 1, jmpdelta, pcnew);
                            pcnew += 5;
                        };
                        break;
                        case INST_JUMP4:
                        case INST_JUMP_TRUE4:
                        case INST_JUMP_FALSE4:
                        {
                            /*
                             * While these instructions do not expand we still may
                             * have to change their jump offset to compensate for
                             * differences in shift for this instruction and at
                             * the jump destination. If there is no difference no
                             * change is needed. Otherwise the jump offset has to
                             * be modified.
                             */

                            int jmpdelta = TclGetInt4AtPtr(pc + 1);
                            int jmpshift = delta[(pc - compEnvPtr->codeStart)];
                            int dstshift = delta[(pc - compEnvPtr->codeStart) + jmpdelta];

                            if (jmpshift != dstshift)
                            {
#ifdef DEBUG_REWRITE
                                fprintf(stderr,
                                        "           JUMP4 change %4d (%4d/%4d) by %4d, now %4d\n",
                                        jmpdelta,
                                        jmpshift,
                                        dstshift,
                                        dstshift - jmpshift,
                                        jmpdelta + (dstshift - jmpshift));
                                fflush(stderr);
#endif
                                jmpdelta += (dstshift - jmpshift);

                                TclUpdateInstInt4AtPc((*pc), jmpdelta, pcnew);
                                pcnew += 5;
                            }
                            else
                                goto copy;
                        };
                        break;
                        case INST_PUSH1:
                        {
                            /*
                             * All push1 instructions expand to push4. This code
                             * copied from ReplacePushIndex, except that growing
                             * is not necessary at this point.
                             */

                            int literal = TclGetUInt1AtPtr(pc + 1);

                            TclUpdateInstInt4AtPc(INST_PUSH4, literal, pcnew);
                            pcnew += 5;
                        };
                        break;
                        default:
                        {
                        copy:
                            /*
                             * All other instruction are copied as they are
                             */
                            memcpy(pcnew, pc, isize);
                            pcnew += isize;
                        };
                        break;
                    }
                    pc += isize;
                }

                /*
                 * At last copy the expanded byte code sequence back into the
                 * compile environment and fix the auxiliary data structures.
                 */

                while ((compEnvPtr->codeStart + newcodesize) > compEnvPtr->codeEnd)
                {
                    TclExpandCodeArray(compEnvPtr);
                }

                memcpy(compEnvPtr->codeStart, newbc, newcodesize);
                compEnvPtr->codeNext = compEnvPtr->codeStart + newcodesize;
                Tcl_Free((char*)newbc);

                /*
                 * Fix the auxiliary data structures containing instruction
                 * sizes and offsets.
                 */

                /*
                 * Fix command location array. We have it easier because we
                 * know for each place in the old code how much it was shifted
                 * (-> delta array).
                 */

                {
                    Tcl_Size i;
                    CmdLocation* locPtr;

                    for (i = 0; i < compEnvPtr->numCommands; i++)
                    {
                        locPtr = &compEnvPtr->cmdMapPtr[i];
                        locPtr->codeOffset += delta[locPtr->codeOffset];
                        locPtr->numCodeBytes = opCodesTablePtr[*(compEnvPtr->codeStart + locPtr->codeOffset)].numBytes;
                    }
                }

                /*
                 * Fix exception ranges. See also ShiftByteCodes. We have it
                 * easier because we know for each place in the old code how
                 * much it was shifted (-> delta array).
                 */

                {
                    Tcl_Size i, numExceptRanges = compEnvPtr->exceptArrayNext;
                    ExceptionRange* excPtr = compEnvPtr->exceptArrayPtr;

                    for (i = 0; i < numExceptRanges; i++)
                    {
                        excPtr->numCodeBytes += delta[excPtr->codeOffset + excPtr->numCodeBytes];
                        excPtr->codeOffset += delta[excPtr->codeOffset];

                        switch (excPtr->type)
                        {
                            case CATCH_EXCEPTION_RANGE:
                                excPtr->catchOffset += delta[excPtr->catchOffset];
                                break;
                            case LOOP_EXCEPTION_RANGE:
                                excPtr->breakOffset += delta[excPtr->breakOffset];
                                excPtr->continueOffset += delta[excPtr->continueOffset];
                                break;
                        }

                        excPtr += 1;
                    }
                }

                /*
                 * Fix the local infoPtr->infoArrayPtr structures where we
                 * recorded the locations of the compiled proc commands.
                 */

                {
                    for (infoArrayPtr = infoPtr->infoArrayPtr; *infoArrayPtr; infoArrayPtr++)
                    {
                        bodyInfoPtr = *infoArrayPtr;
                        bodyInfoPtr->procOffset += delta[bodyInfoPtr->procOffset];
                        bodyInfoPtr->bodyOffset += delta[bodyInfoPtr->bodyOffset];
                    }
                }
            }

            Tcl_Free((char*)delta);

#ifdef DEBUG_REWRITE
            fprintf(stderr, "=== AFTER_ START ===\n");
            {
                Tcl_Size i, numExceptRanges = compEnvPtr->exceptArrayNext;
                ExceptionRange* excPtr = compEnvPtr->exceptArrayPtr;
                for (i = 0; i < numExceptRanges; i++)
                {
                    switch (excPtr->type)
                    {
                        case CATCH_EXCEPTION_RANGE:
                            fprintf(stderr,
                                    "EC [%8ld] @%4ld /%4ld : %4ld\n",
                                    i,
                                    excPtr->codeOffset,
                                    excPtr->numCodeBytes,
                                    excPtr->catchOffset);
                            break;
                        case LOOP_EXCEPTION_RANGE:
                            fprintf(stderr,
                                    "EL [%8ld] @%4ld /%4ld : %4ld %4ld\n",
                                    i,
                                    excPtr->codeOffset,
                                    excPtr->numCodeBytes,
                                    excPtr->breakOffset,
                                    excPtr->continueOffset);
                            break;
                        default:
                            fprintf(stderr, "E? [%8ld] @%4ld /%4ld\n", i, excPtr->codeOffset, excPtr->numCodeBytes);
                    }
                    excPtr += 1;
                }
            }
            for (pc = compEnvPtr->codeStart; pc < compEnvPtr->codeNext;)
            {
                FormatInstruction(compEnvPtr, pc);
                pc += opCodesTablePtr[*pc].numBytes;
            }
            fprintf(stderr, "=== AFTER_ END =====\n");
            fflush(stderr);
#endif
        }
    }

    /*
     * (%%%%)
     * offset is the sum of all shifts we have done; it is used to correct
     * the original offsets as saved in the ProcBodyInfo structs so that
     * they correspond to the new bytecodes.
     * Note that this assumes that the ProcBodyInfo structs are ordered,
     * which they are because they are built in the compile proc.
     */

    offset = 0;

    for (infoArrayPtr = infoPtr->infoArrayPtr; *infoArrayPtr; infoArrayPtr++)
    {
        bodyInfoPtr = *infoArrayPtr;
        newIndex = bodyInfoPtr->bodyNewIndex;

        /*
         * correct the offsets to the new bytecodes
         */

        bodyInfoPtr->procOffset += offset;
        bodyInfoPtr->bodyOffset += offset;

        if (newIndex != -1)
        {
            /*
             * Replace the index for the command name object. This is done for
             * all compiled procedure bodies
             */

            pc = compEnvPtr->codeStart + bodyInfoPtr->procOffset;
            delta = ReplacePushIndex(bodyInfoPtr->commandIndex, pc, procNameObjIndex, compEnvPtr);
            offset += delta;
            bodyInfoPtr->bodyOffset += delta;

            if (newIndex != bodyInfoPtr->bodyOrigIndex)
            {
                /*
                 * replace the index of the body with the unshared index
                 */

                pc = compEnvPtr->codeStart + bodyInfoPtr->bodyOffset;
                delta = ReplacePushIndex(bodyInfoPtr->commandIndex, pc, newIndex, compEnvPtr);

                /*
                 * According to (****) the newIndex is the original index,
                 * thus this replacement should not require growth. Ah. But
                 * (xxxx) in UnshareProcBodies allows differently. Therefore,
                 * don't panic! (You have a towel with you, don't you ? ;)
                 */

                offset += delta;
            }
        }
    }
}

/*
 *----------------------------------------------------------------------
 *
 * ReplacePushIndex --
 *
 *  Replaces the operand to a PUSH operation with the new index value.
 *
 * Results:
 *  Returns the number of bytes by which the bytecodes were shifted in
 *  order to make room for the new operand.
 *
 * Side effects:
 *  May modify the bytecodes, possibly reallocating the bytecode array; may
 *  also modify support data structures in the compilation environment.
 *  Tcl_Panics on error.
 *
 *----------------------------------------------------------------------
 */

static int ReplacePushIndex(Tcl_Size commandIndex, unsigned char* pc, Tcl_Size newIndex, CompileEnv* compEnvPtr)
{
    Tcl_Size offset = 0;

    switch (*pc)
    {
        case INST_PUSH1:
            if (newIndex < 255)
            {
                pc += 1;
                *pc = (unsigned char)newIndex;
            }
            else
            {
                int savedOffset = pc - compEnvPtr->codeStart;
                ShiftByteCodes(commandIndex, savedOffset, 3, compEnvPtr);
                pc = compEnvPtr->codeStart + savedOffset;
                *pc++ = INST_PUSH4;
                *pc++ = (unsigned char)((unsigned int)newIndex >> 24);
                *pc++ = (unsigned char)((unsigned int)newIndex >> 16);
                *pc++ = (unsigned char)((unsigned int)newIndex >> 8);
                *pc++ = (unsigned char)((unsigned int)newIndex);

                /*
                 * we shifted everything right by 3 bytes
                 */

                offset += 3;
            }
            break;

        case INST_PUSH4:
            /*
             * Because a 4 byte PUSH supports a single byte, we don't
             * bother shrinking the bytecodes, just fit the new
             * index in. It is unlikely that this will happen anyway, because
             * we add new objects at the end of the object array anyway.
             */

            pc += 1;
            *pc++ = (unsigned char)((unsigned int)newIndex >> 24);
            *pc++ = (unsigned char)((unsigned int)newIndex >> 16);
            *pc++ = (unsigned char)((unsigned int)newIndex >> 8);
            *pc++ = (unsigned char)((unsigned int)newIndex);
            break;

        default:
            Tcl_Panic("ReplacePushIndex: expected a push opcode");
            break;
    }

    return offset;
}

/*
 *----------------------------------------------------------------------
 *
 * ShiftByteCodes --
 *
 *  Moves all bytecodes past a given offset to the right by 'shiftCount'
 *  bytes. This opens up 'shiftCount' bytes in the bytecodes at startOffset.
 *
 * Results:
 *  None.
 *
 * Side effects:
 *  May grow the bytecode array. After the shift, it also modifies the
 *  various data structures in the compile environment so that they are
 *  corrected for the shift amount.
 *
 *----------------------------------------------------------------------
 */

static void ShiftByteCodes(Tcl_Size commandIndex, Tcl_Size startOffset, Tcl_Size shiftCount, CompileEnv* compEnvPtr)
{
    unsigned char *fromPtr, *toPtr;
    Tcl_Size currBytes, numCmds, i;
    CmdLocation* locPtr;
    Tcl_Size numExceptRanges, start, end;
    ExceptionRange* excPtr;

    /*
     * Grow the array if necessary
     */

    if ((compEnvPtr->codeNext + shiftCount) > compEnvPtr->codeEnd)
    {
        TclExpandCodeArray(compEnvPtr);
    }

    /*
     * the bytes from 0 to (startOffset-1) are fine where they are.
     * The others need to be moved up by shiftCount bytes.
     * Use memmove, which guarantees that the copy is correct even with
     * overlapping source and destination.
     */

    fromPtr = compEnvPtr->codeStart + startOffset;
    toPtr = fromPtr + shiftCount;
    currBytes = compEnvPtr->codeNext - compEnvPtr->codeStart;
    memmove(toPtr, fromPtr, currBytes - startOffset);

    /*
     * Now we need to fix up the data structures.
     *
     * First the command location arrays.
     * We only need to modify entries past the commandIndex.
     * The entry at commandIndex needs to have its size bumped.
     */

    compEnvPtr->codeNext += shiftCount;

    locPtr = &compEnvPtr->cmdMapPtr[commandIndex];
    locPtr->numCodeBytes += shiftCount;
    locPtr += 1;

    numCmds = compEnvPtr->numCommands;
    for (i = commandIndex + 1; i < numCmds; i++)
    {
        locPtr->codeOffset += shiftCount;
        locPtr += 1;
    }

    /*
     * Now the exception ranges.
     * We need to slide their offsets, so that the range covers the same
     * sequence of bytecodes as before the shift.
     *
     * For catch ranges, we also need to slide the catchOffset
     * For loop ranges, we need to slide the break and continue Offsets
     */

    numExceptRanges = compEnvPtr->exceptArrayNext;
    excPtr = compEnvPtr->exceptArrayPtr;
    for (i = 0; i < numExceptRanges; i++)
    {
        start = excPtr->codeOffset;
        if (start > startOffset)
        {
            excPtr->codeOffset += shiftCount;
        }
        else
        {
            end = start + excPtr->numCodeBytes;
            if (end > startOffset)
            {
                /*
                 * the starting offset for the bytecodes shift was inside the
                 * range, so in this case we don't bump the code offset,
                 * but we do bump the number of bytes in the range
                 */

                excPtr->numCodeBytes += shiftCount;
            }
        }

        switch (excPtr->type)
        {
            case CATCH_EXCEPTION_RANGE:
                if (excPtr->catchOffset > startOffset)
                {
                    excPtr->catchOffset += shiftCount;
                }
                break;

            case LOOP_EXCEPTION_RANGE:
                if (excPtr->breakOffset > startOffset)
                {
                    excPtr->breakOffset += shiftCount;
                }
                if (excPtr->continueOffset > startOffset)
                {
                    excPtr->continueOffset += shiftCount;
                }
                break;
        }

        excPtr += 1;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * GetSharedIndex --
 *
 *  Looks at the bytecode instruction at *pc, if the instruction is one
 *  that makes a reference to an object in the object table extract the
 *  index operand and return it.
 *
 * Results:
 *  Returns an index into the object table, or -1 if this instruction is
 *  not on the list of instructions that make index references.
 *
 * Side effects:
 *  None.
 *
 *----------------------------------------------------------------------
 */

static Tcl_Size GetSharedIndex(unsigned char* pc)
{
    unsigned int objIndex;

    switch (*pc)
    {
        case INST_PUSH1:
            objIndex = TclGetUInt1AtPtr(pc + 1);
            break;

        case INST_PUSH4:
            objIndex = TclGetUInt4AtPtr(pc + 1);
            break;

        default:
            return -1;
    }

    return (Tcl_Size)objIndex;
}

/*
 *----------------------------------------------------------------------
 *
 * EmitNewForeachInfo --
 *
 *  Emits a ForeachInfo struct as used by the 8.6.2 bytecode to a Tcl_Channel.
 *
 * Results:
 *  Returns a TCL error code.
 *
 * Side effects:
 *  Sets the TCL result on error.
 *
 *----------------------------------------------------------------------
 */

static int EmitNewForeachInfo(Tcl_Interp* interp, ForeachInfo* infoPtr, Tcl_Channel chan)
{
    Tcl_Size i, j, lastEntry, result;
    Tcl_Size* varIndexesPtr;
    char separator;
    ForeachVarList* varListPtr;

    result = EmitTclSize(interp, infoPtr->numLists, ' ', chan);
    if (result != TCL_OK)
    {
        return result;
    }
    /*
     * The new bytecodes handling foreach do not use firstValueTemp.
     * Dropped from saved bytecode.
     */
    result = EmitTclSize(interp, infoPtr->loopCtTemp, '\n', chan);
    if (result != TCL_OK)
    {
        return result;
    }

    for (i = 0; i < infoPtr->numLists; i++)
    {
        varListPtr = infoPtr->varLists[i];

        result = EmitTclSize(interp, varListPtr->numVars, '\n', chan);
        if (result != TCL_OK)
        {
            return result;
        }

        varIndexesPtr = varListPtr->varIndexes;
        separator = ' ';
        lastEntry = varListPtr->numVars - 1;
        for (j = 0; j <= lastEntry; j++)
        {
            if (j == lastEntry)
            {
                separator = '\n';
            }

            result = EmitTclSize(interp, *varIndexesPtr, separator, chan);
            if (result != TCL_OK)
            {
                return result;
            }

            varIndexesPtr += 1;
        }
    }

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * EmitJumptableInfo --
 *
 *  Emits a JumptableInfo struct to a Tcl_Channel.
 *
 * Results:
 *  Returns a TCL error code.
 *
 * Side effects:
 *  Sets the TCL result on error.
 *
 *----------------------------------------------------------------------
 */

static int EmitJumptableInfo(Tcl_Interp* interp, JumptableInfo* infoPtr, Tcl_Channel chan)
{
    int result;
    Tcl_Size numJmp;
    Tcl_HashSearch jmpHashSearch;
    Tcl_HashEntry* jmpHashEntry;
    char* key;

    numJmp = 0;

    /* get number of entries */
    jmpHashEntry = Tcl_FirstHashEntry(&infoPtr->hashTable, &jmpHashSearch);
    while (jmpHashEntry)
    {
        numJmp++;
        jmpHashEntry = Tcl_NextHashEntry(&jmpHashSearch);
    }

    result = EmitTclSize(interp, numJmp, '\n', chan);
    if (result != TCL_OK)
    {
        return result;
    }

    jmpHashEntry = Tcl_FirstHashEntry(&infoPtr->hashTable, &jmpHashSearch);
    while (jmpHashEntry)
    {
        result = EmitTclSize(interp, PTR2INT(Tcl_GetHashValue(jmpHashEntry)), '\n', chan);
        if (result != TCL_OK)
        {
            return result;
        }

        key = Tcl_GetHashKey(&infoPtr->hashTable, jmpHashEntry);

        result = EmitByteSequence(interp, (unsigned char*)key, strlen(key), chan);
        if (result != TCL_OK)
        {
            return result;
        }
        jmpHashEntry = Tcl_NextHashEntry(&jmpHashSearch);
    }
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * EmitDictUpdateInfo --
 *
 *  Emits a DictUpdateInfo struct to a Tcl_Channel.
 *
 * Results:
 *  Returns a TCL error code.
 *
 * Side effects:
 *  Sets the TCL result on error.
 *
 *----------------------------------------------------------------------
 */

static int EmitDictUpdateInfo(Tcl_Interp* interp, DictUpdateInfo* infoPtr, Tcl_Channel chan)
{
    int result;
    Tcl_Size i;

    result = EmitTclSize(interp, infoPtr->length, '\n', chan);
    if (result != TCL_OK)
    {
        return result;
    }

    for (i = 0; i < infoPtr->length; i++)
    {
        result = EmitTclSize(interp, infoPtr->varIndices[i], '\n', chan);
        if (result != TCL_OK)
        {
            return result;
        }
    }

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * EmitProcBody --
 *
 *  Emits the contents of a Proc structure to a Tcl_Channel.
 *  There are two parts to the dumped information:
 *   - the dump of the ByteCode structure.
 *   - the dump of the additional Proc struct values.
 *
 * Results:
 *  Returns TCL_OK on success, TCL_ERROR on failure.
 *
 * Side effects:
 *  None.
 *
 *----------------------------------------------------------------------
 */

static int EmitProcBody(Tcl_Interp* interp, Proc* procPtr, Tcl_Channel chan)
{
    int result;
    Tcl_Obj* bodyPtr = procPtr->bodyPtr;
    CompiledLocal* localPtr;

    if (bodyPtr->typePtr != cmpByteCodeType)
    {
        Tcl_Panic("EmitProcBody: body is not compiled");
    }

    /*
     * Emit the ByteCode associated with this proc body
     */

    result = EmitByteCode(interp, (ByteCode*)bodyPtr->internalRep.otherValuePtr, chan);
    if (result != TCL_OK)
    {
        return result;
    }

    /*
     * Now the additional Proc fields
     */

    if ((EmitTclSize(interp, procPtr->numArgs, ' ', chan) != TCL_OK) ||
        (EmitTclSize(interp, procPtr->numCompiledLocals, '\n', chan) != TCL_OK))
    {
        return TCL_ERROR;
    }

    for (localPtr = procPtr->firstLocalPtr; localPtr; localPtr = localPtr->nextPtr)
    {
        result = EmitCompiledLocal(interp, localPtr, chan);
        if (result != TCL_OK)
        {
            return result;
        }
    }

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * EmitCompiledLocal --
 *
 *  Emits a CompiledLocal struct to the TclChannel chan.
 *
 * Results:
 *  Returns a TCL result code.
 *
 * Side effects:
 *  May set the TCL result on error.
 *
 *----------------------------------------------------------------------
 */

static int EmitCompiledLocal(Tcl_Interp* interp, CompiledLocal* localPtr, Tcl_Channel chan)
{
    int hasDef = (localPtr->defValuePtr) ? 1 : 0;
    int i, flags;
    unsigned int bit, mask;

    /*
     * First the name.
     */

    if (EmitByteSequence(interp, (unsigned char*)localPtr->name, localPtr->nameLength, chan) != TCL_OK)
    {
        return TCL_ERROR;
    }

    /*
     * The flags are mapped to a bit sequence and written as an int.
     * This step lets us filter out some flags.
     */

    bit = 1;
    mask = 0;
    flags = localPtr->flags;
    for (i = 0; i < varFlagsListSize; i++)
    {
        if (flags & varFlagsList[i])
        {
            mask |= bit;
        }
        bit <<= 1;
    }

    /*
     * emit the control fields in a single line, except for nameLength
     * which was emitted with the name.
     */
    if ((EmitTclSize(interp, localPtr->frameIndex, ' ', chan) != TCL_OK) || (EmitTclSize(interp, hasDef, ' ', chan) != TCL_OK) ||
        (EmitTclSize(interp, mask, '\n', chan) != TCL_OK))
    {
        return TCL_ERROR;
    }

    /*
     * the default value if any
     */

    if (hasDef && (EmitObject(interp, localPtr->defValuePtr, chan) != TCL_OK))
    {
        return TCL_ERROR;
    }

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * A85InitEncodeContext --
 *
 *  Initialize an A85EncodeContext struct.
 *
 * Results:
 *  None.
 *
 * Side effects:
 *  initializes the fields of the A85ContextStruct with appropriate values.
 *
 *----------------------------------------------------------------------
 */

static void A85InitEncodeContext(Tcl_Channel target, int separator, A85EncodeContext* ctxPtr)
{
    ctxPtr->target = target;
    ctxPtr->basePtr = &ctxPtr->encBuffer[0];
    ctxPtr->curPtr = ctxPtr->basePtr;
    ctxPtr->endPtr = ctxPtr->curPtr + ENCODED_BUFFER_SIZE;
    ctxPtr->separator = separator;
}

/*
 *----------------------------------------------------------------------
 *
 * A85EncodeBytes --
 *
 *  Encodes a N byte sequence using our modified ASCII85 filter. Typically
 *  N is 4, except that the final tuple may specify that fewer than 4 bytes
 *  are to be encoded. If 'numBytes' < 4, the 'bytesPtr' array is zero padded
 *  to 4.
 *
 * Results:
 *  Returns a Tcl standard result code.
 *
 * Side effects:
 *  Modifies the eror result in the interpreter in case of error.
 *
 *----------------------------------------------------------------------
 */

static int A85EncodeBytes(Tcl_Interp* interp, unsigned char* bytesPtr, Tcl_Size numBytes, A85EncodeContext* ctxPtr)
{
    unsigned long word = 0;
    Tcl_Size i;
    char toEmit[5];

    for (i = numBytes; i < 4; i++)
    {
        bytesPtr[i] = 0;
    }

    for (i = 3; i >= 0; i--)
    {
        word <<= 8;
        word |= bytesPtr[i];
    }

    if (word == 0)
    {
        A85EmitChar(interp, 'z', ctxPtr);
    }
    else
    {
        /*
         * We emit from least significant to most significant char, so that
         * the 0 chars from an incomplete 4-tuple are the last ones in the
         * sequence and can be omitted (for the last 4-tuple in the array).
         */

        for (i = 0; i < 5; i++)
        {
            toEmit[i] = EN(word % 85UL);
            word /= 85UL;
        }

        /*
         * Emit only 'numBytes+1' chars, since the extra ones are all '!'
         * and can therefore be reconstructed by the decoder (if we know the
         * number of bytes that were encoded).
         */

        for (i = 0; i <= numBytes; i++)
        {
            A85EmitChar(interp, toEmit[i], ctxPtr);
        }
    }

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * A85EmitChar --
 *
 *  Emits a character.
 *
 * Results:
 *  Returns a Tcl standard result code.
 *
 * Side effects:
 *  Modifies the error result in the interpreter in case of error.
 *
 *----------------------------------------------------------------------
 */

static int A85EmitChar(Tcl_Interp* interp, int toEmit, A85EncodeContext* ctxPtr)
{
    *(ctxPtr->curPtr) = toEmit;
    ctxPtr->curPtr += 1;

    if (ctxPtr->curPtr >= ctxPtr->endPtr)
    {
        return A85Flush(interp, ctxPtr);
    }

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * A85Flush --
 *
 *  Flush the encoded buffer.
 *
 * Results:
 *  Returns a Tcl standard result code.
 *
 * Side effects:
 *  Modifies the error result in the interpreter in case of error.
 *
 *----------------------------------------------------------------------
 */

static int A85Flush(Tcl_Interp* interp, A85EncodeContext* ctxPtr)
{
    int toWrite = ctxPtr->curPtr - ctxPtr->basePtr;

    if (Tcl_Write(ctxPtr->target, ctxPtr->basePtr, toWrite) < 0)
    {
        Tcl_SetObjResult(interp, Tcl_ObjPrintf("Tcl_Write: %s", Tcl_PosixError(interp)));
        return TCL_ERROR;
    }

    ctxPtr->curPtr = ctxPtr->basePtr;

    if (ctxPtr->separator != '\0')
    {
        if (Tcl_Write(ctxPtr->target, &ctxPtr->separator, 1) < 0)
        {
            Tcl_SetObjResult(interp, Tcl_ObjPrintf("Tcl_Write: %s", Tcl_PosixError(interp)));
            return TCL_ERROR;
        }
    }

    return TCL_OK;
}

#ifdef DEBUG_REWRITE
/*
 * Snarfed from tclCompile.c and modified for our environment.
 * Prints directly to stderr.
 */
static void PrintSource(const char* stringPtr, int maxChars)
{
    const char* p;
    int i = 0;

    if (stringPtr == NULL)
    {
        fprintf(stderr, "%s", "\"\"");
        return;
    }

    fprintf(stderr, "%s", "\"");
    p = stringPtr;
    for (; (*p != '\0') && (i < maxChars); p++, i++)
    {
        switch (*p)
        {
            case '"':
                fprintf(stderr, "%s", "\\\"");
                continue;
            case '\f':
                fprintf(stderr, "%s", "\\f");
                continue;
            case '\n':
                fprintf(stderr, "%s", "\\n");
                continue;
            case '\r':
                fprintf(stderr, "%s", "\\r");
                continue;
            case '\t':
                fprintf(stderr, "%s", "\\t");
                continue;
            case '\v':
                fprintf(stderr, "%s", "\\v");
                continue;
            default:
                fprintf(stderr, "%c", *p);
                continue;
        }
    }
    fprintf(stderr, "%s", "\"");
}

static void FormatInstruction(CompileEnv* compEnvPtr, unsigned char* pc)
{
    unsigned char opCode = *pc;
    InstructionDesc* opCodesTablePtr = (InstructionDesc*)TclGetInstructionTable();
    InstructionDesc* instDesc = &opCodesTablePtr[opCode];
    unsigned char* codeStart = compEnvPtr->codeStart;
    unsigned pcOffset = pc - codeStart;
    int opnd = 0, i, numBytes = 1;
    char suffixBuffer[128]; /* Additional info to print after main opcode
                             * and immediates. */
    char* suffixSrc = NULL;
    Tcl_Obj* suffixObj = NULL;

    suffixBuffer[0] = '\0';
    fprintf(stderr, "(%u) %s ", pcOffset, instDesc->name);
    for (i = 0; i < instDesc->numOperands; i++)
    {
        switch (instDesc->opTypes[i])
        {
            case OPERAND_INT1:
                opnd = TclGetInt1AtPtr(pc + numBytes);
                numBytes++;
                if (opCode == INST_JUMP1 || opCode == INST_JUMP_TRUE1 || opCode == INST_JUMP_FALSE1)
                {
                    sprintf(suffixBuffer, "pc %u", pcOffset + opnd);
                }
                fprintf(stderr, "%+d ", opnd);
                break;
            case OPERAND_INT4:
                opnd = TclGetInt4AtPtr(pc + numBytes);
                numBytes += 4;
                if (opCode == INST_JUMP4 || opCode == INST_JUMP_TRUE4 || opCode == INST_JUMP_FALSE4)
                {
                    sprintf(suffixBuffer, "pc %u", pcOffset + opnd);
                }
                else if (opCode == INST_START_CMD)
                {
                    sprintf(suffixBuffer, "next cmd at pc %u", pcOffset + opnd);
                }
                fprintf(stderr, "%+d ", opnd);
                break;
            case OPERAND_UINT1:
                opnd = TclGetUInt1AtPtr(pc + numBytes);
                numBytes++;
                if (opCode == INST_PUSH1)
                {
                    suffixObj = compEnvPtr->literalArrayPtr[opnd].objPtr;
                }
                fprintf(stderr, "%u ", (unsigned)opnd);
                break;
            case OPERAND_AUX4:
            case OPERAND_UINT4:
                opnd = TclGetUInt4AtPtr(pc + numBytes);
                numBytes += 4;
                if (opCode == INST_PUSH4)
                {
                    suffixObj = compEnvPtr->literalArrayPtr[opnd].objPtr;
                }
                else if (opCode == INST_START_CMD && opnd != 1)
                {
                    sprintf(suffixBuffer + strlen(suffixBuffer), ", %u cmds start here", opnd);
                }
                fprintf(stderr, "%u ", (unsigned)opnd);
                break;
            case OPERAND_IDX4:
                opnd = TclGetInt4AtPtr(pc + numBytes);
                numBytes += 4;
                if (opnd >= -1)
                {
                    fprintf(stderr, "%d ", opnd);
                }
                else if (opnd == -2)
                {
                    fprintf(stderr, "end ");
                }
                else
                {
                    fprintf(stderr, "end-%d ", -2 - opnd);
                }
                break;
            case OPERAND_LVT1:
                opnd = TclGetUInt1AtPtr(pc + numBytes);
                numBytes++;
                goto printLVTindex;
            case OPERAND_LVT4:
                opnd = TclGetUInt4AtPtr(pc + numBytes);
                numBytes += 4;
            printLVTindex:
                fprintf(stderr, "%%v%u ", (unsigned)opnd);
                break;
            case OPERAND_NONE:
            default:
                break;
        }
    }
    if (suffixObj)
    {
        char* bytes;
        Tcl_Size length;

        fprintf(stderr, "\t# ");
        bytes = Tcl_GetStringFromObj(compEnvPtr->literalArrayPtr[opnd].objPtr, &length);

        PrintSource(bytes, (length < 40 ? length : 40));
    }
    else if (suffixBuffer[0])
    {
        fprintf(stderr, "\t# %s", suffixBuffer);
        if (suffixSrc)
        {
            PrintSource(suffixSrc, 40);
        }
    }
    fprintf(stderr, "\n");
}
#endif
/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * End:
 */
