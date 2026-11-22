#ifndef CD_VM_H
#define CD_VM_H

#include "common.h"
#include "value.h"
#include "chunk.h"

#define CD_FRAMES_MAX 64
#define CD_STACK_MAX (CD_FRAMES_MAX * 256)

typedef struct {
    ObjFunction* function;
    uint8_t* ip;
    Value* slots;
} CallFrame;

struct cd_State {
    CallFrame frames[CD_FRAMES_MAX];
    int frameCount;
    Value stack[CD_STACK_MAX];
    Value* stackTop;
    Value* apiBase;
    Table globals;
    Table strings;
    Obj* objects;
};

void cd_vm_init(cd_State* cd);
void cd_vm_free(cd_State* cd);
int cd_vm_run(cd_State* cd);

void cd_push(cd_State* cd, Value value);
Value cd_pop_val(cd_State* cd);
Value cd_peek(cd_State* cd, int distance);
void cd_runtime_error(cd_State* cd, const char* format, ...);

#endif
