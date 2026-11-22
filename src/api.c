#include "../include/chibidart.h"
#include "vm.h"
#include "compiler.h"

cd_State* cd_open(void) {
    cd_State* cd = (cd_State*)malloc(sizeof(cd_State));
    if (!cd) return NULL;
    cd_vm_init(cd);
    return cd;
}

void cd_close(cd_State* cd) {
    if (!cd) return;
    cd_vm_free(cd);
    free(cd);
}

static Value* index_to_slot(cd_State* cd, int idx) {
    if (idx > 0) {
        Value* base = cd->apiBase ? cd->apiBase : cd->stack;
        if (base + idx - 1 < cd->stackTop) {
            return base + idx - 1;
        }
    } else if (idx < 0) {
        if (cd->stackTop + idx >= cd->stack) {
            return cd->stackTop + idx;
        }
    }
    return NULL;
}

int cd_dostring(cd_State* cd, const char* source) {
    ObjFunction* function = cd_compile(cd, source);
    if (function == NULL) return CD_ERR_COMPILE;

    cd_push(cd, OBJ_VAL(function));
    CallFrame* frame = &cd->frames[cd->frameCount++];
    frame->function = function;
    frame->ip = function->chunk.code;
    frame->slots = cd->stackTop - 1;

    return cd_vm_run(cd);
}

static char* read_file(const char* path) {
    FILE* file = fopen(path, "rb");
    if (file == NULL) return NULL;

    fseek(file, 0L, SEEK_END);
    size_t fileSize = ftell(file);
    rewind(file);

    char* buffer = (char*)malloc(fileSize + 1);
    if (buffer == NULL) {
        fclose(file);
        return NULL;
    }

    size_t bytesRead = fread(buffer, sizeof(char), fileSize, file);
    buffer[bytesRead] = '\0';

    fclose(file);
    return buffer;
}

int cd_dofile(cd_State* cd, const char* path) {
    char* source = read_file(path);
    if (source == NULL) {
        fprintf(stderr, "Could not open file \"%s\".\n", path);
        return CD_ERR_RUNTIME;
    }
    int result = cd_dostring(cd, source);
    free(source);
    return result;
}

void cd_pushnil(cd_State* cd) {
    cd_push(cd, NIL_VAL);
}

void cd_pushbool(cd_State* cd, bool b) {
    cd_push(cd, BOOL_VAL(b));
}

void cd_pushint(cd_State* cd, int64_t val) {
    cd_push(cd, INT_VAL(val));
}

void cd_pushstring(cd_State* cd, const char* str) {
    if (str == NULL) {
        cd_pushnil(cd);
        return;
    }
    cd_push(cd, OBJ_VAL(cd_copy_string(cd, str, (int)strlen(str))));
}

void cd_pushlstring(cd_State* cd, const char* str, size_t len) {
    if (str == NULL) {
        cd_pushnil(cd);
        return;
    }
    cd_push(cd, OBJ_VAL(cd_copy_string(cd, str, (int)len)));
}

bool cd_isnil(cd_State* cd, int idx) {
    Value* slot = index_to_slot(cd, idx);
    return slot && IS_NIL(*slot);
}

bool cd_isbool(cd_State* cd, int idx) {
    Value* slot = index_to_slot(cd, idx);
    return slot && IS_BOOL(*slot);
}

bool cd_isint(cd_State* cd, int idx) {
    Value* slot = index_to_slot(cd, idx);
    return slot && IS_INT(*slot);
}

bool cd_isstring(cd_State* cd, int idx) {
    Value* slot = index_to_slot(cd, idx);
    return slot && IS_STRING(*slot);
}

bool cd_tobool(cd_State* cd, int idx) {
    Value* slot = index_to_slot(cd, idx);
    if (!slot) return false;
    if (IS_BOOL(*slot)) return AS_BOOL(*slot);
    return !IS_NIL(*slot);
}

int64_t cd_toint(cd_State* cd, int idx) {
    Value* slot = index_to_slot(cd, idx);
    if (!slot || !IS_INT(*slot)) return 0;
    return AS_INT(*slot);
}

const char* cd_tostring(cd_State* cd, int idx) {
    Value* slot = index_to_slot(cd, idx);
    if (!slot || !IS_STRING(*slot)) return NULL;
    return AS_CSTRING(*slot);
}

int cd_gettop(cd_State* cd) {
    Value* base = cd->apiBase ? cd->apiBase : cd->stack;
    return (int)(cd->stackTop - base);
}

void cd_settop(cd_State* cd, int idx) {
    if (idx >= 0) {
        while (cd_gettop(cd) < idx) cd_pushnil(cd);
        Value* base = cd->apiBase ? cd->apiBase : cd->stack;
        cd->stackTop = base + idx;
    } else {
        cd->stackTop += (idx + 1);
    }
}

void cd_pop(cd_State* cd, int n) {
    cd->stackTop -= n;
}

void cd_getglobal(cd_State* cd, const char* name) {
    ObjString* str = cd_copy_string(cd, name, (int)strlen(name));
    Value val;
    if (table_get(&cd->globals, str, &val)) {
        cd_push(cd, val);
    } else {
        cd_pushnil(cd);
    }
}

void cd_setglobal(cd_State* cd, const char* name) {
    ObjString* str = cd_copy_string(cd, name, (int)strlen(name));
    Value val = cd_pop_val(cd);
    table_set(&cd->globals, str, val);
}

void cd_register(cd_State* cd, const char* name, cd_CFunction fn) {
    ObjString* str = cd_copy_string(cd, name, (int)strlen(name));
    ObjNative* nat = cd_new_native(cd, fn, str, -1);
    table_set(&cd->globals, str, OBJ_VAL(nat));
}

int cd_pcall(cd_State* cd, int nargs, int nresults) {
    Value* calleeSlot = cd->stackTop - nargs - 1;
    Value callee = *calleeSlot;

    if (!IS_OBJ(callee) || !IS_FUNCTION(callee)) {
        return CD_ERR_RUNTIME;
    }

    ObjFunction* function = AS_FUNCTION(callee);
    if (function->arity != nargs) {
        return CD_ERR_RUNTIME;
    }

    CallFrame* frame = &cd->frames[cd->frameCount++];
    frame->function = function;
    frame->ip = function->chunk.code;
    frame->slots = calleeSlot;

    int result = cd_vm_run(cd);
    if (result != CD_OK) return result;

    if (nresults == 0) {
        cd->stackTop = calleeSlot;
    }

    return CD_OK;
}
