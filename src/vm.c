#include "vm.h"

void cd_vm_init(cd_State* cd) {
    cd->frameCount = 0;
    cd->stackTop = cd->stack;
    cd->apiBase = NULL;
    cd->objects = NULL;
    table_init(&cd->globals);
    table_init(&cd->strings);
}

void cd_vm_free(cd_State* cd) {
    table_free(&cd->globals);
    table_free(&cd->strings);

    Obj* object = cd->objects;
    while (object != NULL) {
        Obj* next = object->next;
        cd_free_object(object);
        object = next;
    }
}

void cd_push(cd_State* cd, Value value) {
    *cd->stackTop = value;
    cd->stackTop++;
}

Value cd_pop_val(cd_State* cd) {
    cd->stackTop--;
    return *cd->stackTop;
}

Value cd_peek(cd_State* cd, int distance) {
    return cd->stackTop[-1 - distance];
}

void cd_runtime_error(cd_State* cd, const char* format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputs("\n", stderr);

    for (int i = cd->frameCount - 1; i >= 0; i--) {
        CallFrame* frame = &cd->frames[i];
        ObjFunction* function = frame->function;
        size_t instruction = frame->ip - function->chunk.code - 1;
        int line = function->chunk.lines[instruction];
        fprintf(stderr, "[line %d] in ", line);
        if (function->name == NULL) {
            fprintf(stderr, "script\n");
        } else {
            fprintf(stderr, "%s()\n", function->name->chars);
        }
    }
}

static bool call(cd_State* cd, ObjFunction* function, int argCount) {
    if (argCount != function->arity) {
        cd_runtime_error(cd, "Expected %d arguments but got %d.", function->arity, argCount);
        return false;
    }

    if (cd->frameCount == CD_FRAMES_MAX) {
        cd_runtime_error(cd, "Stack overflow.");
        return false;
    }

    CallFrame* frame = &cd->frames[cd->frameCount++];
    frame->function = function;
    frame->ip = function->chunk.code;
    frame->slots = cd->stackTop - argCount - 1;
    return true;
}

static bool call_value(cd_State* cd, Value callee, int argCount) {
    if (IS_OBJ(callee)) {
        switch (AS_OBJ(callee)->type) {
            case OBJ_FUNCTION:
                return call(cd, AS_FUNCTION(callee), argCount);
            case OBJ_CLASS: {
                ObjClass* klass = AS_CLASS(callee);
                ObjInstance* instance = cd_new_instance(cd, klass);
                *(cd->stackTop - argCount - 1) = OBJ_VAL(instance);
                Value initMethod;
                if (table_get(&klass->methods, cd_copy_string(cd, "init", 4), &initMethod)) {
                    return call(cd, AS_FUNCTION(initMethod), argCount);
                } else if (argCount != 0) {
                    cd_runtime_error(cd, "Expected 0 arguments but got %d.", argCount);
                    return false;
                }
                return true;
            }
            case OBJ_BOUND_METHOD: {
                ObjBoundMethod* bound = AS_BOUND_METHOD(callee);
                *(cd->stackTop - argCount - 1) = bound->receiver;
                return call(cd, bound->method, argCount);
            }
            case OBJ_NATIVE: {
                ObjNative* native = AS_NATIVE(callee);
                Value* prevBase = cd->apiBase;
                cd->apiBase = cd->stackTop - argCount;
                int result = native->function(cd);
                cd->apiBase = prevBase;
                Value resVal = NIL_VAL;
                if (result > 0) {
                    resVal = cd_pop_val(cd);
                }
                cd->stackTop -= (argCount + 1);
                cd_push(cd, resVal);
                return true;
            }
            default:
                break;
        }
    }
    cd_runtime_error(cd, "Can only call functions.");
    return false;
}

static void concatenate(cd_State* cd) {
    ObjString* b = AS_STRING(cd_pop_val(cd));
    ObjString* a = AS_STRING(cd_pop_val(cd));

    int length = a->length + b->length;
    char* chars = CD_ALLOC(char, length + 1);
    memcpy(chars, a->chars, a->length);
    memcpy(chars + a->length, b->chars, b->length);
    chars[length] = '\0';

    ObjString* result = cd_take_string(cd, chars, length);
    cd_push(cd, OBJ_VAL(result));
}

int cd_vm_run(cd_State* cd) {
    CallFrame* frame = &cd->frames[cd->frameCount - 1];

#define READ_BYTE() (*frame->ip++)
#define READ_SHORT() (frame->ip += 2, (uint16_t)((frame->ip[-2] << 8) | frame->ip[-1]))
#define READ_CONSTANT() (frame->function->chunk.constants.values[READ_BYTE()])
#define READ_STRING() AS_STRING(READ_CONSTANT())
#define BINARY_INT_OP(op) \
    do { \
        int64_t b = AS_INT(cd_pop_val(cd)); \
        int64_t a = AS_INT(cd_pop_val(cd)); \
        cd_push(cd, INT_VAL(a op b)); \
    } while (false)
#define BINARY_FLOAT_OP(op) \
    do { \
        double b = AS_FLOAT(cd_pop_val(cd)); \
        double a = AS_FLOAT(cd_pop_val(cd)); \
        cd_push(cd, FLOAT_VAL(a op b)); \
    } while (false)

    for (;;) {
        uint8_t instruction = READ_BYTE();
        switch (instruction) {
            case OP_CONSTANT: {
                Value constant = READ_CONSTANT();
                cd_push(cd, constant);
                break;
            }
            case OP_NIL:   cd_push(cd, NIL_VAL); break;
            case OP_TRUE:  cd_push(cd, BOOL_VAL(true)); break;
            case OP_FALSE: cd_push(cd, BOOL_VAL(false)); break;
            case OP_POP:   cd_pop_val(cd); break;

            case OP_GET_LOCAL: {
                uint8_t slot = READ_BYTE();
                cd_push(cd, frame->slots[slot]);
                break;
            }
            case OP_SET_LOCAL: {
                uint8_t slot = READ_BYTE();
                frame->slots[slot] = cd_peek(cd, 0);
                break;
            }
            case OP_GET_GLOBAL: {
                ObjString* name = READ_STRING();
                Value value;
                if (!table_get(&cd->globals, name, &value)) {
                    cd_runtime_error(cd, "Undefined variable '%s'.", name->chars);
                    return CD_ERR_RUNTIME;
                }
                cd_push(cd, value);
                break;
            }
            case OP_DEFINE_GLOBAL: {
                ObjString* name = READ_STRING();
                table_set(&cd->globals, name, cd_peek(cd, 0));
                cd_pop_val(cd);
                break;
            }
            case OP_SET_GLOBAL: {
                ObjString* name = READ_STRING();
                if (table_set(&cd->globals, name, cd_peek(cd, 0))) {
                    table_delete(&cd->globals, name);
                    cd_runtime_error(cd, "Undefined variable '%s'.", name->chars);
                    return CD_ERR_RUNTIME;
                }
                break;
            }
            case OP_EQUAL: {
                Value b = cd_pop_val(cd);
                Value a = cd_pop_val(cd);
                cd_push(cd, BOOL_VAL(value_equal(a, b)));
                break;
            }
            case OP_GREATER: {
                int64_t b = AS_INT(cd_pop_val(cd));
                int64_t a = AS_INT(cd_pop_val(cd));
                cd_push(cd, BOOL_VAL(a > b));
                break;
            }
            case OP_LESS: {
                int64_t b = AS_INT(cd_pop_val(cd));
                int64_t a = AS_INT(cd_pop_val(cd));
                cd_push(cd, BOOL_VAL(a < b));
                break;
            }
            case OP_ADD: {
                if (IS_STRING(cd_peek(cd, 0)) && IS_STRING(cd_peek(cd, 1))) {
                    concatenate(cd);
                } else if (IS_INT(cd_peek(cd, 0)) && IS_INT(cd_peek(cd, 1))) {
                    BINARY_INT_OP(+);
                } else {
                    cd_runtime_error(cd, "Operands must be two integers or two strings.");
                    return CD_ERR_RUNTIME;
                }
                break;
            }
            case OP_SUB: BINARY_INT_OP(-); break;
            case OP_MUL: BINARY_INT_OP(*); break;
            case OP_DIV: {
                int64_t b = AS_INT(cd_pop_val(cd));
                int64_t a = AS_INT(cd_pop_val(cd));
                if (b == 0) {
                    cd_runtime_error(cd, "Division by zero.");
                    return CD_ERR_RUNTIME;
                }
                cd_push(cd, INT_VAL(a / b));
                break;
            }
            case OP_MOD: {
                int64_t b = AS_INT(cd_pop_val(cd));
                int64_t a = AS_INT(cd_pop_val(cd));
                if (b == 0) {
                    cd_runtime_error(cd, "Modulo by zero.");
                    return CD_ERR_RUNTIME;
                }
                cd_push(cd, INT_VAL(a % b));
                break;
            }
            case OP_FADD: BINARY_FLOAT_OP(+); break;
            case OP_FSUB: BINARY_FLOAT_OP(-); break;
            case OP_FMUL: BINARY_FLOAT_OP(*); break;
            case OP_FDIV: BINARY_FLOAT_OP(/); break;
            case OP_FGREATER: {
                double b = AS_FLOAT(cd_pop_val(cd));
                double a = AS_FLOAT(cd_pop_val(cd));
                cd_push(cd, BOOL_VAL(a > b));
                break;
            }
            case OP_FLESS: {
                double b = AS_FLOAT(cd_pop_val(cd));
                double a = AS_FLOAT(cd_pop_val(cd));
                cd_push(cd, BOOL_VAL(a < b));
                break;
            }
            case OP_FNEGATE:
                cd_push(cd, FLOAT_VAL(-AS_FLOAT(cd_pop_val(cd))));
                break;
            case OP_NOT:
                cd_push(cd, BOOL_VAL(!AS_BOOL(cd_pop_val(cd))));
                break;
            case OP_NEGATE:
                cd_push(cd, INT_VAL(-AS_INT(cd_pop_val(cd))));
                break;
            case OP_PRINT: {
                value_print(cd_pop_val(cd));
                printf("\n");
                break;
            }
            case OP_JUMP: {
                uint16_t offset = READ_SHORT();
                frame->ip += offset;
                break;
            }
            case OP_JUMP_IF_FALSE: {
                uint16_t offset = READ_SHORT();
                if (!AS_BOOL(cd_peek(cd, 0))) frame->ip += offset;
                break;
            }
            case OP_LOOP: {
                uint16_t offset = READ_SHORT();
                frame->ip -= offset;
                break;
            }
            case OP_CALL: {
                int argCount = READ_BYTE();
                if (!call_value(cd, cd_peek(cd, argCount), argCount)) {
                    return CD_ERR_RUNTIME;
                }
                frame = &cd->frames[cd->frameCount - 1];
                break;
            }
            case OP_RETURN: {
                Value result = cd_pop_val(cd);
                cd->frameCount--;
                if (cd->frameCount == 0) {
                    cd_pop_val(cd);
                    return CD_OK;
                }
                Value returnVal = result;
                if (frame->function->name && strcmp(frame->function->name->chars, "init") == 0) {
                    returnVal = frame->slots[0];
                }
                cd->stackTop = frame->slots;
                cd_push(cd, returnVal);
                frame = &cd->frames[cd->frameCount - 1];
                break;
            }
            case OP_BUILD_LIST: {
                uint8_t count = READ_BYTE();
                ObjList* list = cd_new_list(cd);
                for (int i = count - 1; i >= 0; i--) {
                    list_append(list, cd_peek(cd, i));
                }
                cd->stackTop -= count;
                cd_push(cd, OBJ_VAL(list));
                break;
            }
            case OP_BUILD_MAP: {
                uint8_t count = READ_BYTE();
                ObjMap* map = cd_new_map(cd);
                for (int i = count - 1; i >= 0; i--) {
                    Value k = cd_peek(cd, i * 2 + 1);
                    Value v = cd_peek(cd, i * 2);
                    map_set(map, k, v);
                }
                cd->stackTop -= count * 2;
                cd_push(cd, OBJ_VAL(map));
                break;
            }
            case OP_GET_INDEX: {
                Value index = cd_pop_val(cd);
                Value target = cd_pop_val(cd);
                if (IS_LIST(target)) {
                    if (!IS_INT(index)) {
                        cd_runtime_error(cd, "List index must be integer.");
                        return CD_ERR_RUNTIME;
                    }
                    cd_push(cd, list_get(AS_LIST(target), (int)AS_INT(index)));
                } else if (IS_MAP(target)) {
                    Value res = NIL_VAL;
                    map_get(AS_MAP(target), index, &res);
                    cd_push(cd, res);
                } else if (IS_STRING(target)) {
                    if (!IS_INT(index)) {
                        cd_runtime_error(cd, "String index must be integer.");
                        return CD_ERR_RUNTIME;
                    }
                    ObjString* s = AS_STRING(target);
                    int idx = (int)AS_INT(index);
                    if (idx < 0 || idx >= s->length) {
                        cd_push(cd, OBJ_VAL(cd_copy_string(cd, "", 0)));
                    } else {
                        char buf[2] = { s->chars[idx], '\0' };
                        cd_push(cd, OBJ_VAL(cd_copy_string(cd, buf, 1)));
                    }
                } else {
                    cd_runtime_error(cd, "Cannot index non-collection value.");
                    return CD_ERR_RUNTIME;
                }
                break;
            }
            case OP_SET_INDEX: {
                Value val = cd_pop_val(cd);
                Value index = cd_pop_val(cd);
                Value target = cd_pop_val(cd);
                if (IS_LIST(target)) {
                    if (!IS_INT(index)) {
                        cd_runtime_error(cd, "List index must be integer.");
                        return CD_ERR_RUNTIME;
                    }
                    list_set(AS_LIST(target), (int)AS_INT(index), val);
                } else if (IS_MAP(target)) {
                    map_set(AS_MAP(target), index, val);
                } else {
                    cd_runtime_error(cd, "Cannot set index on non-collection.");
                    return CD_ERR_RUNTIME;
                }
                cd_push(cd, val);
                break;
            }
            case OP_CLASS: {
                ObjString* name = READ_STRING();
                ObjClass* klass = cd_new_class(cd, name);
                cd_push(cd, OBJ_VAL(klass));
                break;
            }
            case OP_METHOD: {
                ObjString* name = READ_STRING();
                Value methodVal = cd_peek(cd, 0);
                ObjClass* klass = AS_CLASS(cd_peek(cd, 1));
                table_set(&klass->methods, name, methodVal);
                cd_pop_val(cd);
                break;
            }
            case OP_GET_PROP: {
                ObjString* prop = READ_STRING();
                Value target = cd_pop_val(cd);
                if (IS_INSTANCE(target)) {
                    ObjInstance* instance = AS_INSTANCE(target);
                    Value val;
                    if (table_get(&instance->fields, prop, &val)) {
                        cd_push(cd, val);
                        break;
                    }
                    if (table_get(&instance->klass->methods, prop, &val)) {
                        ObjBoundMethod* bound = cd_new_bound_method(cd, target, AS_FUNCTION(val));
                        cd_push(cd, OBJ_VAL(bound));
                        break;
                    }
                    cd_runtime_error(cd, "Undefined property '%s'.", prop->chars);
                    return CD_ERR_RUNTIME;
                }
                if (strcmp(prop->chars, "length") == 0) {
                    if (IS_LIST(target)) cd_push(cd, INT_VAL(AS_LIST(target)->count));
                    else if (IS_MAP(target)) cd_push(cd, INT_VAL(AS_MAP(target)->count));
                    else if (IS_STRING(target)) cd_push(cd, INT_VAL(AS_STRING(target)->length));
                    else cd_push(cd, INT_VAL(0));
                } else {
                    cd_runtime_error(cd, "Unknown property '%s'.", prop->chars);
                    return CD_ERR_RUNTIME;
                }
                break;
            }
            case OP_SET_PROP: {
                ObjString* prop = READ_STRING();
                Value val = cd_pop_val(cd);
                Value target = cd_pop_val(cd);
                if (!IS_INSTANCE(target)) {
                    cd_runtime_error(cd, "Only instances have fields.");
                    return CD_ERR_RUNTIME;
                }
                table_set(&AS_INSTANCE(target)->fields, prop, val);
                cd_push(cd, val);
                break;
            }
            case OP_INVOKE_PROP: {
                ObjString* method = READ_STRING();
                uint8_t argCount = READ_BYTE();
                Value target = cd_peek(cd, argCount);
                if (IS_INSTANCE(target)) {
                    ObjInstance* instance = AS_INSTANCE(target);
                    Value methodVal;
                    if (!table_get(&instance->klass->methods, method, &methodVal)) {
                        cd_runtime_error(cd, "Undefined method '%s'.", method->chars);
                        return CD_ERR_RUNTIME;
                    }
                    if (!call(cd, AS_FUNCTION(methodVal), argCount)) {
                        return CD_ERR_RUNTIME;
                    }
                    frame = &cd->frames[cd->frameCount - 1];
                    break;
                }
                if (IS_LIST(target)) {
                    ObjList* list = AS_LIST(target);
                    if (strcmp(method->chars, "add") == 0 || strcmp(method->chars, "push") == 0) {
                        Value arg = cd_peek(cd, 0);
                        list_append(list, arg);
                        cd->stackTop -= (argCount + 1);
                        cd_push(cd, arg);
                    } else if (strcmp(method->chars, "pop") == 0) {
                        Value res = list->count > 0 ? list->items[--list->count] : NIL_VAL;
                        cd->stackTop -= (argCount + 1);
                        cd_push(cd, res);
                    } else {
                        cd_runtime_error(cd, "Unknown list method '%s'.", method->chars);
                        return CD_ERR_RUNTIME;
                    }
                } else if (IS_MAP(target)) {
                    ObjMap* map = AS_MAP(target);
                    if (strcmp(method->chars, "has") == 0) {
                        Value key = cd_peek(cd, 0);
                        bool exists = map_get(map, key, NULL);
                        cd->stackTop -= (argCount + 1);
                        cd_push(cd, BOOL_VAL(exists));
                    } else if (strcmp(method->chars, "remove") == 0) {
                        Value key = cd_peek(cd, 0);
                        bool removed = map_delete(map, key);
                        cd->stackTop -= (argCount + 1);
                        cd_push(cd, BOOL_VAL(removed));
                    } else {
                        cd_runtime_error(cd, "Unknown map method '%s'.", method->chars);
                        return CD_ERR_RUNTIME;
                    }
                } else {
                    cd_runtime_error(cd, "Cannot call method '%s' on non-object.", method->chars);
                    return CD_ERR_RUNTIME;
                }
                break;
            }
        }
    }

#undef READ_BYTE
#undef READ_SHORT
#undef READ_CONSTANT
#undef READ_STRING
#undef BINARY_INT_OP
#undef BINARY_FLOAT_OP
}
