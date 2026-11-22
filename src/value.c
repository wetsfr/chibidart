#include "value.h"
#include "chunk.h"
#include "vm.h"

void* cd_reallocate(void* ptr, size_t oldSize, size_t newSize) {
    (void)oldSize;
    if (newSize == 0) {
        free(ptr);
        return NULL;
    }
    void* result = realloc(ptr, newSize);
    if (!result) exit(1);
    return result;
}

void value_array_init(ValueArray* array) {
    array->values = NULL;
    array->capacity = 0;
    array->count = 0;
}

void value_array_write(ValueArray* array, Value value) {
    if (array->capacity < array->count + 1) {
        int oldCapacity = array->capacity;
        array->capacity = CD_GROW_CAPACITY(oldCapacity);
        array->values = CD_GROW_ARRAY(Value, array->values, oldCapacity, array->capacity);
    }
    array->values[array->count++] = value;
}

void value_array_free(ValueArray* array) {
    CD_FREE_ARRAY(Value, array->values, array->capacity);
    value_array_init(array);
}

void value_print(Value value) {
    switch (value.type) {
        case VAL_NIL:
            printf("null");
            break;
        case VAL_BOOL:
            printf(AS_BOOL(value) ? "true" : "false");
            break;
        case VAL_INT:
            printf("%lld", (long long)AS_INT(value));
            break;
        case VAL_OBJ:
            switch (AS_OBJ(value)->type) {
                case OBJ_STRING:
                    printf("%s", AS_CSTRING(value));
                    break;
                case OBJ_FUNCTION: {
                    ObjFunction* fn = AS_FUNCTION(value);
                    if (fn->name) printf("<fn %s>", fn->name->chars);
                    else printf("<script>");
                    break;
                }
                case OBJ_NATIVE: {
                    ObjNative* nat = AS_NATIVE(value);
                    if (nat->name) printf("<native %s>", nat->name->chars);
                    else printf("<native>");
                    break;
                }
            }
            break;
    }
}

bool value_equal(Value a, Value b) {
    if (a.type != b.type) return false;
    switch (a.type) {
        case VAL_NIL: return true;
        case VAL_BOOL: return AS_BOOL(a) == AS_BOOL(b);
        case VAL_INT: return AS_INT(a) == AS_INT(b);
        case VAL_OBJ: return AS_OBJ(a) == AS_OBJ(b);
        default: return false;
    }
}

static uint32_t hash_string(const char* key, int length) {
    uint32_t hash = 2166136261u;
    for (int i = 0; i < length; i++) {
        hash ^= (uint8_t)key[i];
        hash *= 16777619;
    }
    return hash;
}

void table_init(Table* table) {
    table->count = 0;
    table->capacity = 0;
    table->entries = NULL;
}

void table_free(Table* table) {
    CD_FREE_ARRAY(Entry, table->entries, table->capacity);
    table_init(table);
}

static Entry* find_entry(Entry* entries, int capacity, ObjString* key) {
    uint32_t index = key->hash & (capacity - 1);
    Entry* tombstone = NULL;

    for (;;) {
        Entry* entry = &entries[index];
        if (entry->key == NULL) {
            if (IS_NIL(entry->value)) {
                return tombstone != NULL ? tombstone : entry;
            } else {
                if (tombstone == NULL) tombstone = entry;
            }
        } else if (entry->key == key) {
            return entry;
        }
        index = (index + 1) & (capacity - 1);
    }
}

static void table_adjust_capacity(Table* table, int capacity) {
    Entry* entries = CD_ALLOC(Entry, capacity);
    for (int i = 0; i < capacity; i++) {
        entries[i].key = NULL;
        entries[i].value = NIL_VAL;
    }

    table->count = 0;
    for (int i = 0; i < table->capacity; i++) {
        Entry* entry = &table->entries[i];
        if (entry->key == NULL) continue;
        Entry* dest = find_entry(entries, capacity, entry->key);
        dest->key = entry->key;
        dest->value = entry->value;
        table->count++;
    }

    CD_FREE_ARRAY(Entry, table->entries, table->capacity);
    table->entries = entries;
    table->capacity = capacity;
}

bool table_get(Table* table, ObjString* key, Value* value) {
    if (table->count == 0) return false;
    Entry* entry = find_entry(table->entries, table->capacity, key);
    if (entry->key == NULL) return false;
    *value = entry->value;
    return true;
}

bool table_set(Table* table, ObjString* key, Value value) {
    if (table->count + 1 > table->capacity * 0.75) {
        int capacity = CD_GROW_CAPACITY(table->capacity);
        table_adjust_capacity(table, capacity);
    }

    Entry* entry = find_entry(table->entries, table->capacity, key);
    bool isNewKey = entry->key == NULL;
    if (isNewKey && IS_NIL(entry->value)) table->count++;

    entry->key = key;
    entry->value = value;
    return isNewKey;
}

bool table_delete(Table* table, ObjString* key) {
    if (table->count == 0) return false;
    Entry* entry = find_entry(table->entries, table->capacity, key);
    if (entry->key == NULL) return false;
    entry->key = NULL;
    entry->value = BOOL_VAL(true);
    return true;
}

ObjString* table_find_string(Table* table, const char* chars, int length, uint32_t hash) {
    if (table->count == 0) return NULL;
    uint32_t index = hash & (table->capacity - 1);
    for (;;) {
        Entry* entry = &table->entries[index];
        if (entry->key == NULL) {
            if (IS_NIL(entry->value)) return NULL;
        } else if (entry->key->length == length &&
                   entry->key->hash == hash &&
                   memcmp(entry->key->chars, chars, length) == 0) {
            return entry->key;
        }
        index = (index + 1) & (table->capacity - 1);
    }
}

static Obj* allocate_object(cd_State* cd, size_t size, ObjType type) {
    Obj* object = (Obj*)cd_reallocate(NULL, 0, size);
    object->type = type;
    object->next = cd->objects;
    cd->objects = object;
    return object;
}

static ObjString* allocate_string(cd_State* cd, char* chars, int length, uint32_t hash) {
    ObjString* string = (ObjString*)allocate_object(cd, sizeof(ObjString), OBJ_STRING);
    string->length = length;
    string->hash = hash;
    string->chars = chars;
    table_set(&cd->strings, string, NIL_VAL);
    return string;
}

ObjString* cd_copy_string(cd_State* cd, const char* chars, int length) {
    uint32_t hash = hash_string(chars, length);
    ObjString* interned = table_find_string(&cd->strings, chars, length, hash);
    if (interned != NULL) return interned;

    char* heapChars = CD_ALLOC(char, length + 1);
    memcpy(heapChars, chars, length);
    heapChars[length] = '\0';
    return allocate_string(cd, heapChars, length, hash);
}

ObjString* cd_take_string(cd_State* cd, char* chars, int length) {
    uint32_t hash = hash_string(chars, length);
    ObjString* interned = table_find_string(&cd->strings, chars, length, hash);
    if (interned != NULL) {
        CD_FREE_ARRAY(char, chars, length + 1);
        return interned;
    }
    return allocate_string(cd, chars, length, hash);
}

ObjFunction* cd_new_function(cd_State* cd) {
    ObjFunction* function = (ObjFunction*)allocate_object(cd, sizeof(ObjFunction), OBJ_FUNCTION);
    function->arity = 0;
    function->returnType = TYPE_VOID;
    for (int i = 0; i < 16; i++) function->paramTypes[i] = TYPE_VOID;
    function->name = NULL;
    chunk_init(&function->chunk);
    return function;
}

ObjNative* cd_new_native(cd_State* cd, cd_CFunction function, ObjString* name, int arity) {
    ObjNative* native = (ObjNative*)allocate_object(cd, sizeof(ObjNative), OBJ_NATIVE);
    native->function = function;
    native->name = name;
    native->arity = arity;
    return native;
}

void cd_free_object(Obj* object) {
    switch (object->type) {
        case OBJ_STRING: {
            ObjString* string = (ObjString*)object;
            CD_FREE_ARRAY(char, string->chars, string->length + 1);
            CD_FREE(ObjString, object);
            break;
        }
        case OBJ_FUNCTION: {
            ObjFunction* function = (ObjFunction*)object;
            chunk_free(&function->chunk);
            CD_FREE(ObjFunction, object);
            break;
        }
        case OBJ_NATIVE: {
            CD_FREE(ObjNative, object);
            break;
        }
    }
}
