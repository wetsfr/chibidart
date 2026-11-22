#ifndef CD_VALUE_H
#define CD_VALUE_H

#include "common.h"
#include "../include/chibidart.h"

typedef struct Obj Obj;
typedef struct ObjString ObjString;
typedef struct ObjFunction ObjFunction;
typedef struct ObjNative ObjNative;
typedef struct cd_State cd_State;

typedef enum {
    TYPE_VOID,
    TYPE_INT,
    TYPE_FLOAT,
    TYPE_DOUBLE,
    TYPE_BOOL,
    TYPE_STRING,
    TYPE_ANY
} DataType;

typedef enum {
    VAL_NIL,
    VAL_BOOL,
    VAL_INT,
    VAL_FLOAT,
    VAL_OBJ
} ValueType;

typedef struct {
    ValueType type;
    union {
        bool boolean;
        int64_t integer;
        double number;
        Obj* obj;
    } as;
} Value;

#define IS_NIL(v) ((v).type == VAL_NIL)
#define IS_BOOL(v) ((v).type == VAL_BOOL)
#define IS_INT(v) ((v).type == VAL_INT)
#define IS_FLOAT(v) ((v).type == VAL_FLOAT)
#define IS_OBJ(v) ((v).type == VAL_OBJ)

#define AS_BOOL(v) ((v).as.boolean)
#define AS_INT(v) ((v).as.integer)
#define AS_FLOAT(v) ((v).as.number)
#define AS_OBJ(v) ((v).as.obj)

#define NIL_VAL ((Value){VAL_NIL, {.integer = 0}})
#define BOOL_VAL(b) ((Value){VAL_BOOL, {.boolean = (b)}})
#define INT_VAL(i) ((Value){VAL_INT, {.integer = (i)}})
#define FLOAT_VAL(f) ((Value){VAL_FLOAT, {.number = (f)}})
#define OBJ_VAL(o) ((Value){VAL_OBJ, {.obj = (Obj*)(o)}})

typedef enum {
    OBJ_STRING,
    OBJ_FUNCTION,
    OBJ_NATIVE
} ObjType;

struct Obj {
    ObjType type;
    struct Obj* next;
};

struct ObjString {
    Obj obj;
    int length;
    uint32_t hash;
    char* chars;
};

struct ObjNative {
    Obj obj;
    cd_CFunction function;
    ObjString* name;
    int arity;
};

static inline bool cd_is_obj_type(Value v, ObjType type) {
    return IS_OBJ(v) && AS_OBJ(v)->type == type;
}

#define IS_STRING(v) cd_is_obj_type(v, OBJ_STRING)
#define IS_FUNCTION(v) cd_is_obj_type(v, OBJ_FUNCTION)
#define IS_NATIVE(v) cd_is_obj_type(v, OBJ_NATIVE)

#define AS_STRING(v) ((ObjString*)AS_OBJ(v))
#define AS_CSTRING(v) (((ObjString*)AS_OBJ(v))->chars)
#define AS_FUNCTION(v) ((ObjFunction*)AS_OBJ(v))
#define AS_NATIVE(v) ((ObjNative*)AS_OBJ(v))

typedef struct {
    int capacity;
    int count;
    Value* values;
} ValueArray;

typedef struct {
    ObjString* key;
    Value value;
} Entry;

typedef struct {
    int count;
    int capacity;
    Entry* entries;
} Table;

void value_array_init(ValueArray* array);
void value_array_write(ValueArray* array, Value value);
void value_array_free(ValueArray* array);
void value_print(Value value);
bool value_equal(Value a, Value b);

void table_init(Table* table);
void table_free(Table* table);
bool table_get(Table* table, ObjString* key, Value* value);
bool table_set(Table* table, ObjString* key, Value value);
bool table_delete(Table* table, ObjString* key);
ObjString* table_find_string(Table* table, const char* chars, int length, uint32_t hash);

ObjString* cd_copy_string(cd_State* cd, const char* chars, int length);
ObjString* cd_take_string(cd_State* cd, char* chars, int length);
ObjFunction* cd_new_function(cd_State* cd);
ObjNative* cd_new_native(cd_State* cd, cd_CFunction function, ObjString* name, int arity);
void cd_free_object(Obj* object);

#endif
