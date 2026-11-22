#ifndef CD_CHUNK_H
#define CD_CHUNK_H

#include "common.h"
#include "value.h"

typedef enum {
    OP_CONSTANT,
    OP_NIL,
    OP_TRUE,
    OP_FALSE,
    OP_POP,
    OP_GET_LOCAL,
    OP_SET_LOCAL,
    OP_GET_GLOBAL,
    OP_SET_GLOBAL,
    OP_DEFINE_GLOBAL,
    OP_EQUAL,
    OP_GREATER,
    OP_LESS,
    OP_ADD,
    OP_SUB,
    OP_MUL,
    OP_DIV,
    OP_MOD,
    OP_FADD,
    OP_FSUB,
    OP_FMUL,
    OP_FDIV,
    OP_FGREATER,
    OP_FLESS,
    OP_FNEGATE,
    OP_NOT,
    OP_NEGATE,
    OP_PRINT,
    OP_JUMP,
    OP_JUMP_IF_FALSE,
    OP_LOOP,
    OP_CALL,
    OP_RETURN,
    OP_BUILD_LIST,
    OP_BUILD_MAP,
    OP_GET_INDEX,
    OP_SET_INDEX,
    OP_GET_PROP,
    OP_SET_PROP,
    OP_INVOKE_PROP,
    OP_CLASS,
    OP_METHOD,
    OP_CLASS_FIELD
} OpCode;

typedef struct {
    int count;
    int capacity;
    uint8_t* code;
    int* lines;
    ValueArray constants;
} Chunk;

struct ObjFunction {
    Obj obj;
    int arity;
    int paramTypes[16];
    int returnType;
    bool isInitializer;
    Chunk chunk;
    ObjString* name;
};

void chunk_init(Chunk* chunk);
void chunk_free(Chunk* chunk);
void chunk_write(Chunk* chunk, uint8_t byte, int line);
int chunk_add_constant(Chunk* chunk, Value value);

#endif
