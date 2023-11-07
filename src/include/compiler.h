#ifndef dotk_compiler_h
#define dotk_compiler_h
#include "vm.h"

ObjFunction *compile(const char *source, char *file, bool isRepl);
void markCompilerRoots();

#endif