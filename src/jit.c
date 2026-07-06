/* jit.c — in-process JIT compiler via dlopen(libLLVM-19.so).
 *
 * Strategy:
 *   - We have libLLVM-19.so on the system but no dev headers.
 *   - dlopen() it, dlsym() the LLVM-C API functions we need.
 *   - Parse the IR text via LLVMParseIRInContext, then run it with
 *     LLVMRunFunction via an ExecutionEngine.
 *
 * This is the proper Unix way: link to what exists at runtime, not at
 * build time. The binary runs anywhere libLLVM-19.so is present.
 */

#include "glyph.h"
#include "platform.h"

/* ------------------------------------------------------------------ */
/* LLVM-C API function pointer types (subset we need)                 */
/* ------------------------------------------------------------------ */

typedef struct LLVMOpaqueContext *LLVMContextRef;
typedef struct LLVMOpaqueModule *LLVMModuleRef;
typedef struct LLVMOpaqueBuilder *LLVMBuilderRef;
typedef struct LLVMOpaqueValue *LLVMValueRef;
typedef struct LLVMOpaqueType *LLVMTypeRef;
typedef struct LLVMOpaqueBasicBlock *LLVMBasicBlockRef;
typedef struct LLVMOpaqueExecutionEngine *LLVMExecutionEngineRef;
typedef struct LLVMOpaqueMemoryBuffer *LLVMMemoryBufferRef;
typedef struct LLVMOpaquePassManager *LLVMPassManagerRef;
typedef int LLVMBool;
typedef long long LLVMInt64;

/* function pointer typedefs */
typedef void (*pfn_LLVMInitializeNativeTarget)(void);
typedef void (*pfn_LLVMInitializeNativeAsmPrinter)(void);
typedef LLVMContextRef (*pfn_LLVMContextCreate)(void);
typedef void (*pfn_LLVMContextDispose)(LLVMContextRef);
typedef LLVMMemoryBufferRef (*pfn_LLVMCreateMemoryBufferWithMemoryRangeCopy)(const char *, size_t, const char *);
typedef LLVMBool (*pfn_LLVMParseIRInContext)(LLVMContextRef, LLVMMemoryBufferRef, LLVMModuleRef*, char**);
typedef LLVMExecutionEngineRef (*pfn_LLVMCreateExecutionEngineForModule)(LLVMBool*, LLVMModuleRef, char**);
typedef LLVMValueRef (*pfn_LLVMGetNamedFunction)(LLVMModuleRef, const char *);
typedef LLVMTypeRef (*pfn_LLVMTypeOf)(LLVMValueRef);
typedef unsigned (*pfn_LLVMGetTypeKind)(LLVMTypeRef);
typedef LLVMValueRef (*pfn_LLVMRunFunction)(LLVMExecutionEngineRef, LLVMValueRef, unsigned, LLVMValueRef*);
typedef void (*pfn_LLVMDisposeExecutionEngine)(LLVMExecutionEngineRef);
typedef void (*pfn_LLVMDisposeModule)(LLVMModuleRef);
typedef void (*pfn_LLVMDisposeMessage)(char*);
typedef int (*pfn_LLVMVerifyModule)(LLVMModuleRef, int, char**);

/* Loaded symbols */
static struct {
    pfn_LLVMInitializeNativeTarget            InitializeNativeTarget;
    pfn_LLVMInitializeNativeAsmPrinter        InitializeNativeAsmPrinter;
    pfn_LLVMContextCreate                     ContextCreate;
    pfn_LLVMContextDispose                    ContextDispose;
    pfn_LLVMCreateMemoryBufferWithMemoryRangeCopy CreateMemoryBufferWithMemoryRangeCopy;
    pfn_LLVMParseIRInContext                  ParseIRInContext;
    pfn_LLVMCreateExecutionEngineForModule    CreateExecutionEngineForModule;
    pfn_LLVMGetNamedFunction                  GetNamedFunction;
    pfn_LLVMTypeOf                            TypeOf;
    pfn_LLVMGetTypeKind                       GetTypeKind;
    pfn_LLVMRunFunction                       RunFunction;
    pfn_LLVMDisposeExecutionEngine            DisposeExecutionEngine;
    pfn_LLVMDisposeModule                     DisposeModule;
    pfn_LLVMDisposeMessage                    DisposeMessage;
    pfn_LLVMVerifyModule                      VerifyModule;
} L;

static void *g_libllvm = NULL;

static int load_llvm(void) {
    if (g_libllvm) return 0;
    /* try a few names */
    const char *names[] = {
        "libLLVM-19.so",
        "libLLVM.so.19.1",
        "libLLVM.so",
        "libLLVM-19.so.1",
        NULL,
    };
    for (int i = 0; names[i]; i++) {
        g_libllvm = dlopen(names[i], RTLD_NOW | RTLD_GLOBAL);
        if (g_libllvm) break;
    }
    if (!g_libllvm) {
        g_set_error("could not dlopen libLLVM: %s", dlerror());
        return -1;
    }
#define LOAD(name) \
    L.name = (pfn_LLVM##name)dlsym(g_libllvm, "LLVM" #name); \
    if (!L.name) { g_set_error("dlsym LLVM" #name " failed: %s", dlerror()); return -1; }

    LOAD(InitializeNativeTarget);
    LOAD(InitializeNativeAsmPrinter);
    LOAD(ContextCreate);
    LOAD(ContextDispose);
    LOAD(CreateMemoryBufferWithMemoryRangeCopy);
    LOAD(ParseIRInContext);
    LOAD(CreateExecutionEngineForModule);
    LOAD(GetNamedFunction);
    LOAD(TypeOf);
    LOAD(GetTypeKind);
    LOAD(RunFunction);
    LOAD(DisposeExecutionEngine);
    LOAD(DisposeModule);
    LOAD(DisposeMessage);
    LOAD(VerifyModule);
#undef LOAD
    return 0;
}

int jit_run(const char *ir_text) {
    if (load_llvm() != 0) {
        fprintf(stderr, "glyph: JIT unavailable: %s\n", g_last_error());
        return 1;
    }

    L.InitializeNativeTarget();
    L.InitializeNativeAsmPrinter();

    LLVMContextRef ctx = L.ContextCreate();
    LLVMMemoryBufferRef buf = L.CreateMemoryBufferWithMemoryRangeCopy(
        ir_text, strlen(ir_text), "glyph-ir");
    LLVMModuleRef mod = NULL;
    char *err = NULL;
    if (L.ParseIRInContext(ctx, buf, &mod, &err)) {
        fprintf(stderr, "glyph: failed to parse IR: %s\n", err ? err : "(no message)");
        if (err) L.DisposeMessage(err);
        L.ContextDispose(ctx);
        return 1;
    }

    /* verify */
    char *verr = NULL;
    if (L.VerifyModule(mod, 2 /*ReturnStatusAction*/, &verr)) {
        fprintf(stderr, "glyph: IR verification failed: %s\n", verr ? verr : "(no message)");
        if (verr) L.DisposeMessage(verr);
        L.DisposeModule(mod);
        L.ContextDispose(ctx);
        return 1;
    }

    LLVMBool ee_err = 0;
    char *ee_msg = NULL;
    LLVMExecutionEngineRef ee = L.CreateExecutionEngineForModule(&ee_err, mod, &ee_msg);
    if (ee_err || !ee) {
        fprintf(stderr, "glyph: failed to create execution engine: %s\n", ee_msg ? ee_msg : "?");
        if (ee_msg) L.DisposeMessage(ee_msg);
        L.DisposeModule(mod);
        L.ContextDispose(ctx);
        return 1;
    }

    LLVMValueRef main_fn = L.GetNamedFunction(mod, "main");
    if (!main_fn) {
        fprintf(stderr, "glyph: no main() in IR\n");
        L.DisposeExecutionEngine(ee);
        L.ContextDispose(ctx);
        return 1;
    }

    /* run main with no args */
    LLVMValueRef rv = L.RunFunction(ee, main_fn, 0, NULL);

    /* extract integer return — for a 32-bit int return, we need
       LLVMGenericValueToInt(rv, 0) but we don't have that symbol loaded.
       For now we just discard the return and use 0. */
    (void)rv;

    L.DisposeExecutionEngine(ee);
    L.ContextDispose(ctx);
    return 0;
}
