#ifndef dotk_optimizer_h
#define dotk_optimizer_h

#include "object.h"

bool optimizeFunction(ObjFunction *function, int level);
bool optimizerValidateFunction(ObjFunction *function, char *errBuf, size_t errCap);

#endif
