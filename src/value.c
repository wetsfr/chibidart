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
        case VAL_FLOAT:
            printf("%g", AS_FLOAT(value));
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
                case OBJ_LIST: {
                    ObjList* list = AS_LIST(value);
                    printf("[");
                    for (int i = 0; i < list->count; i++) {
                        value_print(list->items[i]);
                        if (i + 1 < list->count) printf(", ");
                    }
                    printf("]");
                    break;
                }
                case OBJ_MAP: {
                    ObjMap* map = AS_MAP(value);
                    printf("{");
                    int printed = 0;
                    for (int i = 0; i < map->capacity; i++) {
                        if (map->entries[i].state != MAP_OCCUPIED) continue;
                        if (printed > 0) printf(", ");
                        value_print(map->entries[i].key);
                        printf(": ");
                        value_print(map->entries[i].value);
                        printed++;
                    }
                    printf("}");
                    break;
                }
                case OBJ_CLASS: {
                    ObjClass* klass = AS_CLASS(value);
                    printf("<class %s>", klass->name ? klass->name->chars : "class");
                    break;
                }
                case OBJ_INSTANCE: {
                    ObjInstance* inst = AS_INSTANCE(value);
                    printf("<%s instance>", inst->klass->name ? inst->klass->name->chars : "instance");
                    break;
                }
                case OBJ_BOUND_METHOD: {
                    ObjBoundMethod* bm = AS_BOUND_METHOD(value);
                    printf("<method %s>", bm->method->name ? bm->method->name->chars : "anon");
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
        case VAL_FLOAT: return AS_FLOAT(a) == AS_FLOAT(b);
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

static uint32_t hash_u64(uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return (uint32_t)(x ^ (x >> 32));
}

static uint32_t hash_value(Value v) {
    switch (v.type) {
        case VAL_NIL: return 0x9e3779b9u;
        case VAL_BOOL: return AS_BOOL(v) ? 0x12345678u : 0x87654321u;
        case VAL_INT: return hash_u64((uint64_t)AS_INT(v));
        case VAL_FLOAT: {
            double d = AS_FLOAT(v);
            if (d == 0.0) d = 0.0;
            uint64_t bits = 0;
            memcpy(&bits, &d, sizeof(bits));
            return hash_u64(bits);
        }
        case VAL_OBJ:
            if (IS_STRING(v)) return AS_STRING(v)->hash;
            return hash_u64((uint64_t)(uintptr_t)AS_OBJ(v));
    }
    return 0;
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

void table_add_all(Table* from, Table* to) {
    for (int i = 0; i < from->capacity; i++) {
        Entry* entry = &from->entries[i];
        if (entry->key != NULL) {
            table_set(to, entry->key, entry->value);
        }
    }
}

void table_remove_white(Table* table) {
    for (int i = 0; i < table->capacity; i++) {
        Entry* entry = &table->entries[i];
        if (entry->key != NULL && !entry->key->obj.isMarked) {
            table_delete(table, entry->key);
        }
    }
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
    if (cd->bytesAllocated > cd->nextGC) {
        cd_collect_garbage(cd);
    }
    Obj* object = (Obj*)cd_reallocate(NULL, 0, size);
    object->type = type;
    object->isMarked = false;
    object->next = cd->objects;
    cd->objects = object;
    cd->bytesAllocated += size;
    return object;
}

static ObjString* allocate_string(cd_State* cd, char* chars, int length, uint32_t hash) {
    ObjString* string = (ObjString*)allocate_object(cd, sizeof(ObjString), OBJ_STRING);
    string->length = length;
    string->hash = hash;
    string->chars = chars;
    cd->bytesAllocated += (size_t)(length + 1);
    cd_push(cd, OBJ_VAL(string));
    table_set(&cd->strings, string, NIL_VAL);
    cd_pop_val(cd);
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
    function->isInitializer = false;
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

ObjList* cd_new_list(cd_State* cd) {
    ObjList* list = (ObjList*)allocate_object(cd, sizeof(ObjList), OBJ_LIST);
    list->count = 0;
    list->capacity = 0;
    list->items = NULL;
    return list;
}

ObjMap* cd_new_map(cd_State* cd) {
    ObjMap* map = (ObjMap*)allocate_object(cd, sizeof(ObjMap), OBJ_MAP);
    map->count = 0;
    map->used = 0;
    map->capacity = 0;
    map->entries = NULL;
    return map;
}

ObjClass* cd_new_class(cd_State* cd, ObjString* name) {
    ObjClass* klass = (ObjClass*)allocate_object(cd, sizeof(ObjClass), OBJ_CLASS);
    klass->name = name;
    table_init(&klass->methods);
    table_init(&klass->defaults);
    return klass;
}

ObjInstance* cd_new_instance(cd_State* cd, ObjClass* klass) {
    ObjInstance* instance = (ObjInstance*)allocate_object(cd, sizeof(ObjInstance), OBJ_INSTANCE);
    instance->klass = klass;
    table_init(&instance->fields);
    table_add_all(&klass->defaults, &instance->fields);
    return instance;
}

ObjBoundMethod* cd_new_bound_method(cd_State* cd, Value receiver, ObjFunction* method) {
    ObjBoundMethod* bound = (ObjBoundMethod*)allocate_object(cd, sizeof(ObjBoundMethod), OBJ_BOUND_METHOD);
    bound->receiver = receiver;
    bound->method = method;
    return bound;
}

void list_append(ObjList* list, Value value) {
    if (list->capacity < list->count + 1) {
        int oldCap = list->capacity;
        list->capacity = CD_GROW_CAPACITY(oldCap);
        list->items = CD_GROW_ARRAY(Value, list->items, oldCap, list->capacity);
    }
    list->items[list->count++] = value;
}

Value list_get(ObjList* list, int64_t index) {
    if (index < 0 || index >= (int64_t)list->count) return NIL_VAL;
    return list->items[index];
}

void list_set(ObjList* list, int64_t index, Value value) {
    if (index >= 0 && index < (int64_t)list->count) {
        list->items[index] = value;
    }
}

static MapPair* map_find_entry(MapPair* entries, int capacity, Value key) {
    uint32_t index = hash_value(key) & (capacity - 1);
    MapPair* tombstone = NULL;

    for (;;) {
        MapPair* entry = &entries[index];
        if (entry->state == MAP_EMPTY) {
            return tombstone != NULL ? tombstone : entry;
        } else if (entry->state == MAP_TOMBSTONE) {
            if (tombstone == NULL) tombstone = entry;
        } else if (value_equal(entry->key, key)) {
            return entry;
        }
        index = (index + 1) & (capacity - 1);
    }
}

static void map_adjust_capacity(ObjMap* map, int capacity) {
    MapPair* entries = CD_ALLOC(MapPair, capacity);
    for (int i = 0; i < capacity; i++) {
        entries[i].key = NIL_VAL;
        entries[i].value = NIL_VAL;
        entries[i].state = MAP_EMPTY;
    }

    map->count = 0;
    map->used = 0;
    for (int i = 0; i < map->capacity; i++) {
        MapPair* entry = &map->entries[i];
        if (entry->state != MAP_OCCUPIED) continue;
        MapPair* dest = map_find_entry(entries, capacity, entry->key);
        dest->key = entry->key;
        dest->value = entry->value;
        dest->state = MAP_OCCUPIED;
        map->count++;
        map->used++;
    }

    CD_FREE_ARRAY(MapPair, map->entries, map->capacity);
    map->entries = entries;
    map->capacity = capacity;
}

bool map_get(ObjMap* map, Value key, Value* out) {
    if (map->count == 0) return false;
    MapPair* entry = map_find_entry(map->entries, map->capacity, key);
    if (entry->state != MAP_OCCUPIED) return false;
    if (out) *out = entry->value;
    return true;
}

void map_set(ObjMap* map, Value key, Value value) {
    if (map->used + 1 > map->capacity * 0.75) {
        int capacity = CD_GROW_CAPACITY(map->capacity);
        map_adjust_capacity(map, capacity);
    }

    MapPair* entry = map_find_entry(map->entries, map->capacity, key);
    if (entry->state == MAP_EMPTY) map->used++;
    if (entry->state != MAP_OCCUPIED) map->count++;

    entry->key = key;
    entry->value = value;
    entry->state = MAP_OCCUPIED;
}

bool map_delete(ObjMap* map, Value key) {
    if (map->count == 0) return false;
    MapPair* entry = map_find_entry(map->entries, map->capacity, key);
    if (entry->state != MAP_OCCUPIED) return false;
    entry->key = NIL_VAL;
    entry->value = NIL_VAL;
    entry->state = MAP_TOMBSTONE;
    map->count--;
    return true;
}

void cd_free_object(cd_State* cd, Obj* object) {
    switch (object->type) {
        case OBJ_STRING: {
            ObjString* string = (ObjString*)object;
            if (cd) cd->bytesAllocated -= sizeof(ObjString) + (size_t)(string->length + 1);
            CD_FREE_ARRAY(char, string->chars, string->length + 1);
            CD_FREE(ObjString, object);
            break;
        }
        case OBJ_FUNCTION: {
            ObjFunction* function = (ObjFunction*)object;
            if (cd) cd->bytesAllocated -= sizeof(ObjFunction);
            chunk_free(&function->chunk);
            CD_FREE(ObjFunction, object);
            break;
        }
        case OBJ_NATIVE: {
            if (cd) cd->bytesAllocated -= sizeof(ObjNative);
            CD_FREE(ObjNative, object);
            break;
        }
        case OBJ_LIST: {
            ObjList* list = (ObjList*)object;
            if (cd) cd->bytesAllocated -= sizeof(ObjList);
            CD_FREE_ARRAY(Value, list->items, list->capacity);
            CD_FREE(ObjList, object);
            break;
        }
        case OBJ_MAP: {
            ObjMap* map = (ObjMap*)object;
            if (cd) cd->bytesAllocated -= sizeof(ObjMap);
            CD_FREE_ARRAY(MapPair, map->entries, map->capacity);
            CD_FREE(ObjMap, object);
            break;
        }
        case OBJ_CLASS: {
            ObjClass* klass = (ObjClass*)object;
            if (cd) cd->bytesAllocated -= sizeof(ObjClass);
            table_free(&klass->methods);
            table_free(&klass->defaults);
            CD_FREE(ObjClass, object);
            break;
        }
        case OBJ_INSTANCE: {
            ObjInstance* instance = (ObjInstance*)object;
            if (cd) cd->bytesAllocated -= sizeof(ObjInstance);
            table_free(&instance->fields);
            CD_FREE(ObjInstance, object);
            break;
        }
        case OBJ_BOUND_METHOD: {
            if (cd) cd->bytesAllocated -= sizeof(ObjBoundMethod);
            CD_FREE(ObjBoundMethod, object);
            break;
        }
    }
}

void cd_mark_object(cd_State* cd, Obj* object) {
    if (object == NULL || object->isMarked) return;
    object->isMarked = true;

    if (cd->grayCapacity < cd->grayCount + 1) {
        cd->grayCapacity = CD_GROW_CAPACITY(cd->grayCapacity);
        cd->grayStack = (Obj**)realloc(cd->grayStack, sizeof(Obj*) * cd->grayCapacity);
        if (cd->grayStack == NULL) exit(1);
    }
    cd->grayStack[cd->grayCount++] = object;
}

void cd_mark_value(cd_State* cd, Value value) {
    if (IS_OBJ(value)) cd_mark_object(cd, AS_OBJ(value));
}

static void mark_array(cd_State* cd, ValueArray* array) {
    for (int i = 0; i < array->count; i++) {
        cd_mark_value(cd, array->values[i]);
    }
}

static void mark_table(cd_State* cd, Table* table) {
    for (int i = 0; i < table->capacity; i++) {
        Entry* entry = &table->entries[i];
        if (entry->key != NULL) {
            cd_mark_object(cd, (Obj*)entry->key);
            cd_mark_value(cd, entry->value);
        }
    }
}

static void blacken_object(cd_State* cd, Obj* object) {
    switch (object->type) {
        case OBJ_NATIVE: {
            ObjNative* native = (ObjNative*)object;
            cd_mark_object(cd, (Obj*)native->name);
            break;
        }
        case OBJ_STRING:
            break;
        case OBJ_FUNCTION: {
            ObjFunction* function = (ObjFunction*)object;
            cd_mark_object(cd, (Obj*)function->name);
            mark_array(cd, &function->chunk.constants);
            break;
        }
        case OBJ_LIST: {
            ObjList* list = (ObjList*)object;
            for (int i = 0; i < list->count; i++) {
                cd_mark_value(cd, list->items[i]);
            }
            break;
        }
        case OBJ_MAP: {
            ObjMap* map = (ObjMap*)object;
            for (int i = 0; i < map->capacity; i++) {
                if (map->entries[i].state == MAP_OCCUPIED) {
                    cd_mark_value(cd, map->entries[i].key);
                    cd_mark_value(cd, map->entries[i].value);
                }
            }
            break;
        }
        case OBJ_CLASS: {
            ObjClass* klass = (ObjClass*)object;
            cd_mark_object(cd, (Obj*)klass->name);
            mark_table(cd, &klass->methods);
            mark_table(cd, &klass->defaults);
            break;
        }
        case OBJ_INSTANCE: {
            ObjInstance* instance = (ObjInstance*)object;
            cd_mark_object(cd, (Obj*)instance->klass);
            mark_table(cd, &instance->fields);
            break;
        }
        case OBJ_BOUND_METHOD: {
            ObjBoundMethod* bound = (ObjBoundMethod*)object;
            cd_mark_value(cd, bound->receiver);
            cd_mark_object(cd, (Obj*)bound->method);
            break;
        }
    }
}

static void mark_roots(cd_State* cd) {
    for (Value* slot = cd->stack; slot < cd->stackTop; slot++) {
        cd_mark_value(cd, *slot);
    }
    for (int i = 0; i < cd->frameCount; i++) {
        cd_mark_object(cd, (Obj*)cd->frames[i].function);
    }
    mark_table(cd, &cd->globals);
    for (int i = 0; i < cd->globalSymbolCount; i++) {
        cd_mark_object(cd, (Obj*)cd->globalSymbols[i].name);
    }
    cd_mark_compiler_roots(cd);
}

static void trace_references(cd_State* cd) {
    while (cd->grayCount > 0) {
        Obj* object = cd->grayStack[--cd->grayCount];
        blacken_object(cd, object);
    }
}

static void sweep(cd_State* cd) {
    Obj* previous = NULL;
    Obj* object = cd->objects;
    while (object != NULL) {
        if (object->isMarked) {
            object->isMarked = false;
            previous = object;
            object = object->next;
        } else {
            Obj* unreached = object;
            object = object->next;
            if (previous != NULL) {
                previous->next = object;
            } else {
                cd->objects = object;
            }
            cd_free_object(cd, unreached);
        }
    }
}

void cd_collect_garbage(cd_State* cd) {
    mark_roots(cd);
    trace_references(cd);
    table_remove_white(&cd->strings);
    sweep(cd);
    cd->nextGC = cd->bytesAllocated * 2;
    if (cd->nextGC < 1024 * 1024) cd->nextGC = 1024 * 1024;
}
