#ifndef CD_VM_H
#define CD_VM_H

#include "common.h"
#include "value.h"
#include "chunk.h"

#define CD_FRAMES_MAX 64
#define CD_STACK_MAX (CD_FRAMES_MAX * 256)

typedef struct Compiler Compiler;

typedef struct {
    ObjString* name;
    TypeInfo type;
} ClassField;

typedef struct {
    ObjString* name;
    int arity;
    TypeInfo paramTypes[16];
    TypeInfo returnType;
} ClassMethod;

typedef struct {
    ObjString* name;
    ClassField* fields;
    int fieldCount;
    int fieldCapacity;
    ClassMethod* methods;
    int methodCount;
    int methodCapacity;
} ClassInfo;

typedef struct {
    ObjString* name;
    TypeInfo type;
    int arity;
    TypeInfo paramTypes[16];
    TypeInfo returnType;
    bool isFunction;
} GlobalSymbol;

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
    size_t bytesAllocated;
    size_t nextGC;
    Obj** grayStack;
    int grayCount;
    int grayCapacity;
    Compiler* currentCompiler;
    GlobalSymbol* globalSymbols;
    int globalSymbolCount;
    int globalSymbolCapacity;
    ClassInfo* classInfos;
    int classInfoCount;
    int classInfoCapacity;
};

void cd_vm_init(cd_State* cd);
void cd_vm_free(cd_State* cd);
int cd_vm_run(cd_State* cd);

void cd_push(cd_State* cd, Value value);
Value cd_pop_val(cd_State* cd);
Value cd_peek(cd_State* cd, int distance);
void cd_runtime_error(cd_State* cd, const char* format, ...);
void cd_mark_compiler_roots(cd_State* cd);

#endif
