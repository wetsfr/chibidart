#ifndef CD_COMPILER_H
#define CD_COMPILER_H

#include "vm.h"

ObjFunction* cd_compile(cd_State* cd, const char* source);

#endif
