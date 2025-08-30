/*
 * cmpWPkg.c --
 *
 *  Compiler package initialization (writer).
 *  Tcl 9 modernization:
 *    - Provide package "tclcompiler" while installing commands under "::compiler"
 *    - No string eval; idempotent namespace handling
 *    - Const-correct tables; modern ObjCmd signatures (void*, Tcl_Size)
 *    - Safe string assembly via Tcl_DString
 */

#include <string.h>
#include "cmpInt.h"
#include "cmpWrite.h"

/* Package identity (TEA) – THIS is what we 'provide' */
static char packageName[] = PACKAGE_NAME;       /* "tclcompiler" */
static char packageVersion[] = PACKAGE_VERSION; /* e.g. "1.9.0" */

/* Public namespace where commands live */
static const char nsName[] = CMP_WRITER_PACKAGE; /* "compiler" */

/* Default error variable/value for missing loader */
static char errorVariable[] = LOADER_ERROR_VARIABLE;
static char errorMessage[] = LOADER_ERROR_MESSAGE;

/* Tables */
typedef struct VarTable
{
    const char* varName;
    const char* varValue;
} VarTable;

typedef struct CmdTable
{
    const char* cmdName;   /* unqualified subcommand */
    Tcl_ObjCmdProc *proc;  /* implementation */
    int exportIt;          /* nonzero => export */
} CmdTable;

/* Local command (modern signature) */
static int Compiler_GetTclVerObjCmd(void* dummy, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]);

/* Variables & commands installed by this package */
static const VarTable variables[] = {{errorVariable, errorMessage}, {NULL, NULL}};

static const CmdTable commands[] = {{"compile", Compiler_CompileObjCmd, 1},
                                    {"getBytecodeExtension", Compiler_GetBytecodeExtensionObjCmd, 1},
                                    {"getTclVer", Compiler_GetTclVerObjCmd, 1},
                                    {NULL, NULL, 0}};

/* --- helpers --- */
static Tcl_Namespace* GetOrCreateNamespace(Tcl_Interp* interp, const char* name)
{
    Tcl_Namespace* ns = Tcl_FindNamespace(interp, name, NULL, TCL_GLOBAL_ONLY);
    if (!ns)
    {
        ns = Tcl_CreateNamespace(interp, name, NULL, NULL);
    }
    return ns;
}

static int RegisterCommand(Tcl_Interp* interp, const char* ns, const CmdTable* cmd)
{
    Tcl_Namespace* nsp = GetOrCreateNamespace(interp, ns);
    if (!nsp)
        return TCL_ERROR;

    Tcl_DString fq;
    Tcl_DStringInit(&fq);
    Tcl_DStringAppend(&fq, ns, -1);
    Tcl_DStringAppend(&fq, "::", 2);
    Tcl_DStringAppend(&fq, cmd->cmdName, -1);

    Tcl_CreateObjCommand(interp, Tcl_DStringValue(&fq), cmd->proc, NULL, NULL);
    Tcl_DStringFree(&fq);

    if (cmd->exportIt)
    {
        if (Tcl_Export(interp, nsp, cmd->cmdName, 0) != TCL_OK)
            return TCL_ERROR;
    }
    return TCL_OK;
}

static int RegisterVariable(Tcl_Interp* interp, const char* ns, const VarTable* var)
{
    if (!GetOrCreateNamespace(interp, ns))
        return TCL_ERROR;

    Tcl_DString fq;
    Tcl_DStringInit(&fq);
    Tcl_DStringAppend(&fq, ns, -1);
    Tcl_DStringAppend(&fq, "::", 2);
    Tcl_DStringAppend(&fq, var->varName, -1);

    Tcl_Obj* val = Tcl_NewStringObj(var->varValue, -1);
    Tcl_IncrRefCount(val);
    /* No TCL_GLOBAL_ONLY here: name is fully-qualified, let Tcl resolve to ::ns::var */
    if (Tcl_SetVar2Ex(interp, Tcl_DStringValue(&fq), NULL, val, 0) == NULL)
    {
        Tcl_DecrRefCount(val);
        Tcl_DStringFree(&fq);
        return TCL_ERROR;
    }
    Tcl_DecrRefCount(val);
    Tcl_DStringFree(&fq);
    return TCL_OK;
}

/* --- public init(s) --- */

int Tclcompiler_Init(Tcl_Interp* interp)
{
#ifdef USE_TCL_STUBS
    if (!Tcl_InitStubs(interp, TCL_VERSION, 1))
    {
        return TCL_ERROR;
    }
#else
    if (Tcl_PkgRequire(interp, "Tcl", TCL_VERSION, 1) == NULL)
    {
        return TCL_ERROR;
    }
#endif

    /* Initialize writer core */
    CompilerInit(interp); /* (signature in cmpWrite.h; returns void in original) */

    /* Install commands into ::compiler */
    for (const CmdTable* ct = &commands[0]; ct->cmdName; ct++)
    {
        if (RegisterCommand(interp, nsName, ct) != TCL_OK)
        {
            return TCL_ERROR;
        }
    }

    /* Install variables into ::compiler */
    for (const VarTable* vt = &variables[0]; vt->varName; vt++)
    {
        if (RegisterVariable(interp, nsName, vt) != TCL_OK)
        {
            return TCL_ERROR;
        }
    }

    /* Provide the package under its TEA/package name: "tclcompiler" */
    return Tcl_PkgProvide(interp, packageName, packageVersion);
}

/* Safe init: same surface */
int Tclcompiler_SafeInit(Tcl_Interp* interp)
{
    return Tclcompiler_Init(interp);
}

/* --- simple utility subcommand: ::compiler::getTclVer --- */
static int Compiler_GetTclVerObjCmd(void* dummy, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[])
{
    (void)dummy;
    (void)objc;
    (void)objv;
    Tcl_SetObjResult(interp, Tcl_NewStringObj(TCL_VERSION, -1));
    return TCL_OK;
}

/* For tests/tools */
const char* CompilerGetPackageName(void)
{
    return packageName;
}
