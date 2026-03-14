#ifndef dotk_native_exec_h
#define dotk_native_exec_h

#include "object.h"
#include <stdio.h>

bool nativeExecIsSupported(void);
bool nativeTryExecuteFunction(ObjFunction *function, Value *resultOut);
bool nativeDumpFunctionIr(ObjFunction *function, FILE *out);
bool nativeAotWriteExecutable(ObjFunction *function,
                              const char *outPath,
                              bool keepTempSource,
                              char *keptSourcePathOut,
                              size_t keptSourcePathOutCap,
                              char *errBuf,
                              size_t errBufCap);

#endif