#include "compiler.h"
#include "lexer.h"

typedef struct {
    Token current;
    Token previous;
    bool hadError;
    bool panicMode;
} Parser;

typedef enum {
    PREC_NONE,
    PREC_ASSIGNMENT,
    PREC_OR,
    PREC_AND,
    PREC_EQUALITY,
    PREC_COMPARISON,
    PREC_TERM,
    PREC_FACTOR,
    PREC_UNARY,
    PREC_CALL,
    PREC_PRIMARY
} Precedence;

typedef struct {
    Token name;
    int depth;
    TypeInfo type;
} Local;

typedef enum {
    TYPE_SCRIPT,
    TYPE_FUNCTION,
    TYPE_METHOD
} FunctionType;

typedef struct LoopContext {
    struct LoopContext* enclosing;
    int loopStart;
    int scopeDepth;
    int breakJumps[64];
    int breakCount;
} LoopContext;

struct Compiler {
    struct Compiler* enclosing;
    ObjFunction* function;
    FunctionType type;
    Local locals[256];
    int localCount;
    int scopeDepth;
    LoopContext* loop;
    TypeInfo returnTypeInfo;
};

typedef struct {
    Parser parser;
    Lexer lexer;
    cd_State* vm;
    Compiler* current;
} CompilerState;

static TypeInfo make_type(DataType kind) {
    TypeInfo t;
    t.kind = kind;
    t.className = NULL;
    t.elemType = TYPE_ANY;
    t.keyType = TYPE_ANY;
    t.valType = TYPE_ANY;
    return t;
}

static TypeInfo make_class_type(ObjString* className) {
    TypeInfo t = make_type(TYPE_CLASS);
    t.className = className;
    return t;
}

void cd_mark_compiler_roots(cd_State* cd) {
    Compiler* compiler = cd->currentCompiler;
    while (compiler != NULL) {
        cd_mark_object(cd, (Obj*)compiler->function);
        cd_mark_object(cd, (Obj*)compiler->returnTypeInfo.className);
        for (int i = 0; i < compiler->localCount; i++) {
            cd_mark_object(cd, (Obj*)compiler->locals[i].type.className);
        }
        compiler = compiler->enclosing;
    }
}

static void error_at(CompilerState* cs, Token* token, const char* message) {
    if (cs->parser.panicMode) return;
    cs->parser.panicMode = true;
    fprintf(stderr, "[line %d] Error", token->line);

    if (token->type == TOKEN_EOF) {
        fprintf(stderr, " at end");
    } else if (token->type != TOKEN_ERROR) {
        fprintf(stderr, " at '%.*s'", token->length, token->start);
    }

    fprintf(stderr, ": %s\n", message);
    cs->parser.hadError = true;
}

static void error(CompilerState* cs, const char* message) {
    error_at(cs, &cs->parser.previous, message);
}

static void error_at_current(CompilerState* cs, const char* message) {
    error_at(cs, &cs->parser.current, message);
}

static void advance(CompilerState* cs) {
    cs->parser.previous = cs->parser.current;
    for (;;) {
        cs->parser.current = lexer_next(&cs->lexer);
        if (cs->parser.current.type != TOKEN_ERROR) break;
        error_at_current(cs, cs->parser.current.start);
    }
}

static void consume(CompilerState* cs, TokenType type, const char* message) {
    if (cs->parser.current.type == type) {
        advance(cs);
        return;
    }
    error_at_current(cs, message);
}

static bool check(CompilerState* cs, TokenType type) {
    return cs->parser.current.type == type;
}

static bool match(CompilerState* cs, TokenType type) {
    if (!check(cs, type)) return false;
    advance(cs);
    return true;
}

static Chunk* current_chunk(CompilerState* cs) {
    return &cs->current->function->chunk;
}

static void emit_byte(CompilerState* cs, uint8_t byte) {
    chunk_write(current_chunk(cs), byte, cs->parser.previous.line);
}

static void emit_bytes(CompilerState* cs, uint8_t byte1, uint8_t byte2) {
    emit_byte(cs, byte1);
    emit_byte(cs, byte2);
}

static void emit_loop(CompilerState* cs, int loopStart) {
    emit_byte(cs, OP_LOOP);
    int offset = current_chunk(cs)->count - loopStart + 2;
    if (offset > 65535) error(cs, "Loop body too large.");
    emit_byte(cs, (offset >> 8) & 0xff);
    emit_byte(cs, offset & 0xff);
}

static int emit_jump(CompilerState* cs, uint8_t instruction) {
    emit_byte(cs, instruction);
    emit_byte(cs, 0xff);
    emit_byte(cs, 0xff);
    return current_chunk(cs)->count - 2;
}

static void patch_jump(CompilerState* cs, int offset) {
    int jump = current_chunk(cs)->count - offset - 2;
    if (jump > 65535) {
        error(cs, "Too much code to jump over.");
    }
    current_chunk(cs)->code[offset] = (jump >> 8) & 0xff;
    current_chunk(cs)->code[offset + 1] = jump & 0xff;
}

static void emit_return(CompilerState* cs) {
    emit_byte(cs, OP_NIL);
    emit_byte(cs, OP_RETURN);
}

static uint8_t make_constant(CompilerState* cs, Value value) {
    int constant = chunk_add_constant(current_chunk(cs), value);
    if (constant > 255) {
        error(cs, "Too many constants in one chunk.");
        return 0;
    }
    return (uint8_t)constant;
}

static void emit_constant(CompilerState* cs, Value value) {
    emit_bytes(cs, OP_CONSTANT, make_constant(cs, value));
}

static void init_compiler(CompilerState* cs, Compiler* compiler, FunctionType type, TypeInfo retType) {
    compiler->enclosing = cs->current;
    compiler->function = NULL;
    compiler->type = type;
    compiler->localCount = 0;
    compiler->scopeDepth = 0;
    compiler->loop = NULL;
    compiler->returnTypeInfo = retType;
    cs->current = compiler;
    cs->vm->currentCompiler = compiler;
    compiler->function = cd_new_function(cs->vm);
    compiler->function->returnType = retType.kind;

    Local* local = &cs->current->locals[cs->current->localCount++];
    local->depth = 0;
    local->name.start = "";
    local->name.length = 0;
    local->type = make_type(TYPE_VOID);
}

static ObjFunction* end_compiler(CompilerState* cs) {
    emit_return(cs);
    ObjFunction* function = cs->current->function;
    cs->current = cs->current->enclosing;
    cs->vm->currentCompiler = cs->current;
    return function;
}

static void begin_scope(CompilerState* cs) {
    cs->current->scopeDepth++;
}

static void end_scope(CompilerState* cs) {
    cs->current->scopeDepth--;
    while (cs->current->localCount > 0 &&
           cs->current->locals[cs->current->localCount - 1].depth > cs->current->scopeDepth) {
        emit_byte(cs, OP_POP);
        cs->current->localCount--;
    }
}

static bool identifiers_equal(Token* a, Token* b) {
    if (a->length != b->length) return false;
    return memcmp(a->start, b->start, a->length) == 0;
}

static int resolve_local(CompilerState* cs, Compiler* compiler, Token* name) {
    for (int i = compiler->localCount - 1; i >= 0; i--) {
        Local* local = &compiler->locals[i];
        if (identifiers_equal(name, &local->name)) {
            if (local->depth == -1) {
                error(cs, "Can't read local variable in its own initializer.");
            }
            return i;
        }
    }
    return -1;
}

static void add_local(CompilerState* cs, Token name, TypeInfo type) {
    if (cs->current->localCount == 256) {
        error(cs, "Too many local variables in function.");
        return;
    }
    Local* local = &cs->current->locals[cs->current->localCount++];
    local->name = name;
    local->depth = -1;
    local->type = type;
}

static uint8_t identifier_constant(CompilerState* cs, Token* name) {
    return make_constant(cs, OBJ_VAL(cd_copy_string(cs->vm, name->start, name->length)));
}

static ClassInfo* find_class_info_by_str(cd_State* vm, ObjString* name) {
    if (name == NULL) return NULL;
    for (int i = 0; i < vm->classInfoCount; i++) {
        if (vm->classInfos[i].name == name) {
            return &vm->classInfos[i];
        }
    }
    return NULL;
}

static ClassInfo* find_class_info(CompilerState* cs, Token* name) {
    cd_State* vm = cs->vm;
    for (int i = 0; i < vm->classInfoCount; i++) {
        ObjString* cname = vm->classInfos[i].name;
        if (cname->length == name->length &&
            memcmp(cname->chars, name->start, name->length) == 0) {
            return &vm->classInfos[i];
        }
    }
    return NULL;
}

static ClassInfo* add_class_info(CompilerState* cs, Token name) {
    cd_State* vm = cs->vm;
    ClassInfo* existing = find_class_info(cs, &name);
    if (existing != NULL) {
        existing->fieldCount = 0;
        existing->methodCount = 0;
        return existing;
    }
    if (vm->classInfoCount >= vm->classInfoCapacity) {
        int oldCap = vm->classInfoCapacity;
        vm->classInfoCapacity = CD_GROW_CAPACITY(oldCap);
        vm->classInfos = CD_GROW_ARRAY(ClassInfo, vm->classInfos, oldCap, vm->classInfoCapacity);
    }
    ClassInfo* ci = &vm->classInfos[vm->classInfoCount++];
    ci->name = cd_copy_string(vm, name.start, name.length);
    ci->fields = NULL;
    ci->fieldCount = 0;
    ci->fieldCapacity = 0;
    ci->methods = NULL;
    ci->methodCount = 0;
    ci->methodCapacity = 0;
    return ci;
}

static ClassField* find_class_field(ClassInfo* ci, Token* name) {
    if (!ci) return NULL;
    for (int i = 0; i < ci->fieldCount; i++) {
        ObjString* fname = ci->fields[i].name;
        if (fname->length == name->length &&
            memcmp(fname->chars, name->start, name->length) == 0) {
            return &ci->fields[i];
        }
    }
    return NULL;
}

static void add_class_field(CompilerState* cs, ClassInfo* ci, Token name, TypeInfo type) {
    ClassField* existing = find_class_field(ci, &name);
    if (existing != NULL) {
        existing->type = type;
        return;
    }
    if (ci->fieldCount >= ci->fieldCapacity) {
        int oldCap = ci->fieldCapacity;
        ci->fieldCapacity = CD_GROW_CAPACITY(oldCap);
        ci->fields = CD_GROW_ARRAY(ClassField, ci->fields, oldCap, ci->fieldCapacity);
    }
    ClassField* f = &ci->fields[ci->fieldCount++];
    f->name = cd_copy_string(cs->vm, name.start, name.length);
    f->type = type;
}

static ClassMethod* find_class_method(ClassInfo* ci, const char* name, int length) {
    if (!ci) return NULL;
    for (int i = 0; i < ci->methodCount; i++) {
        ObjString* mname = ci->methods[i].name;
        if (mname->length == length &&
            memcmp(mname->chars, name, length) == 0) {
            return &ci->methods[i];
        }
    }
    return NULL;
}

static void add_class_method(CompilerState* cs, ClassInfo* ci, const char* name, int length, int arity, TypeInfo* params, TypeInfo ret) {
    ClassMethod* m = find_class_method(ci, name, length);
    if (m == NULL) {
        if (ci->methodCount >= ci->methodCapacity) {
            int oldCap = ci->methodCapacity;
            ci->methodCapacity = CD_GROW_CAPACITY(oldCap);
            ci->methods = CD_GROW_ARRAY(ClassMethod, ci->methods, oldCap, ci->methodCapacity);
        }
        m = &ci->methods[ci->methodCount++];
        m->name = cd_copy_string(cs->vm, name, length);
    }
    m->arity = arity;
    m->returnType = ret;
    for (int i = 0; i < arity; i++) m->paramTypes[i] = params[i];
}

static GlobalSymbol* find_global_symbol(CompilerState* cs, Token* name) {
    cd_State* vm = cs->vm;
    for (int i = 0; i < vm->globalSymbolCount; i++) {
        ObjString* symName = vm->globalSymbols[i].name;
        if (symName->length == name->length &&
            memcmp(symName->chars, name->start, name->length) == 0) {
            return &vm->globalSymbols[i];
        }
    }
    return NULL;
}

static void add_global_symbol(CompilerState* cs, Token name, TypeInfo type, bool isFunction, int arity, TypeInfo* params, TypeInfo ret) {
    cd_State* vm = cs->vm;
    GlobalSymbol* sym = find_global_symbol(cs, &name);
    if (sym == NULL) {
        if (vm->globalSymbolCount >= vm->globalSymbolCapacity) {
            int oldCap = vm->globalSymbolCapacity;
            vm->globalSymbolCapacity = CD_GROW_CAPACITY(oldCap);
            vm->globalSymbols = CD_GROW_ARRAY(GlobalSymbol, vm->globalSymbols, oldCap, vm->globalSymbolCapacity);
        }
        sym = &vm->globalSymbols[vm->globalSymbolCount++];
        sym->name = cd_copy_string(vm, name.start, name.length);
    }
    sym->type = type;
    sym->isFunction = isFunction;
    sym->arity = arity;
    sym->returnType = ret;
    if (params) {
        for (int i = 0; i < arity; i++) sym->paramTypes[i] = params[i];
    }
}

static TypeInfo expression(CompilerState* cs);
static void statement(CompilerState* cs);
static void declaration(CompilerState* cs);
static TypeInfo parse_precedence(CompilerState* cs, Precedence precedence);

static bool is_builtin_type_token(TokenType type) {
    return type == TOKEN_KW_INT || type == TOKEN_KW_FLOAT || type == TOKEN_KW_DOUBLE ||
           type == TOKEN_KW_BOOL || type == TOKEN_KW_STRING || type == TOKEN_KW_VOID ||
           type == TOKEN_KW_LIST || type == TOKEN_KW_MAP;
}

static DataType simple_type_from_token(TokenType type) {
    switch (type) {
        case TOKEN_KW_INT: return TYPE_INT;
        case TOKEN_KW_FLOAT: return TYPE_FLOAT;
        case TOKEN_KW_DOUBLE: return TYPE_DOUBLE;
        case TOKEN_KW_BOOL: return TYPE_BOOL;
        case TOKEN_KW_STRING: return TYPE_STRING;
        case TOKEN_KW_VOID: return TYPE_VOID;
        case TOKEN_KW_LIST: return TYPE_LIST;
        case TOKEN_KW_MAP: return TYPE_MAP;
        default: return TYPE_ANY;
    }
}

static TypeInfo parse_type(CompilerState* cs) {
    Token tok = cs->parser.current;
    if (is_builtin_type_token(tok.type)) {
        advance(cs);
        if (tok.type == TOKEN_KW_LIST) {
            TypeInfo t = make_type(TYPE_LIST);
            if (match(cs, TOKEN_LESS)) {
                if (is_builtin_type_token(cs->parser.current.type)) {
                    t.elemType = simple_type_from_token(cs->parser.current.type);
                    advance(cs);
                } else if (check(cs, TOKEN_IDENTIFIER)) {
                    if (find_class_info(cs, &cs->parser.current) != NULL) {
                        t.elemType = TYPE_CLASS;
                    } else {
                        error_at_current(cs, "Unknown type argument.");
                    }
                    advance(cs);
                } else {
                    error_at_current(cs, "Expect type argument.");
                }
                consume(cs, TOKEN_GREATER, "Expect '>' after type argument.");
            }
            return t;
        }
        if (tok.type == TOKEN_KW_MAP) {
            TypeInfo t = make_type(TYPE_MAP);
            if (match(cs, TOKEN_LESS)) {
                if (is_builtin_type_token(cs->parser.current.type)) {
                    t.keyType = simple_type_from_token(cs->parser.current.type);
                    advance(cs);
                } else if (check(cs, TOKEN_IDENTIFIER)) {
                    t.keyType = TYPE_CLASS;
                    advance(cs);
                } else {
                    error_at_current(cs, "Expect key type.");
                }
                consume(cs, TOKEN_COMMA, "Expect ',' between Map type arguments.");
                if (is_builtin_type_token(cs->parser.current.type)) {
                    t.valType = simple_type_from_token(cs->parser.current.type);
                    advance(cs);
                } else if (check(cs, TOKEN_IDENTIFIER)) {
                    t.valType = TYPE_CLASS;
                    advance(cs);
                } else {
                    error_at_current(cs, "Expect value type.");
                }
                consume(cs, TOKEN_GREATER, "Expect '>' after type arguments.");
            }
            return t;
        }
        return make_type(simple_type_from_token(tok.type));
    }

    if (tok.type == TOKEN_IDENTIFIER) {
        ClassInfo* ci = find_class_info(cs, &tok);
        if (ci != NULL) {
            advance(cs);
            return make_class_type(ci->name);
        }
        error_at_current(cs, "Unknown type name.");
        advance(cs);
        return make_type(TYPE_ANY);
    }

    error_at_current(cs, "Expect type name.");
    return make_type(TYPE_ANY);
}

static const char* type_name(TypeInfo type) {
    switch (type.kind) {
        case TYPE_VOID: return "void";
        case TYPE_INT: return "int";
        case TYPE_FLOAT: return "float";
        case TYPE_DOUBLE: return "double";
        case TYPE_BOOL: return "bool";
        case TYPE_STRING: return "string";
        case TYPE_LIST: return "List";
        case TYPE_MAP: return "Map";
        case TYPE_CLASS: return type.className ? type.className->chars : "class";
        case TYPE_ANY: return "any";
    }
    return "unknown";
}

static bool is_float_type(DataType t) {
    return t == TYPE_FLOAT || t == TYPE_DOUBLE;
}

static bool scalar_compatible(DataType a, DataType b) {
    if (a == TYPE_ANY || b == TYPE_ANY) return true;
    if (a == b) return true;
    if (is_float_type(a) && is_float_type(b)) return true;
    return false;
}

static bool types_compatible(TypeInfo expected, TypeInfo actual) {
    if (expected.kind == TYPE_ANY || actual.kind == TYPE_ANY) return true;
    if (expected.kind == TYPE_CLASS && actual.kind == TYPE_CLASS) {
        if (expected.className == NULL || actual.className == NULL) return true;
        return expected.className == actual.className;
    }
    if (expected.kind == TYPE_LIST && actual.kind == TYPE_LIST) {
        return scalar_compatible(expected.elemType, actual.elemType);
    }
    if (expected.kind == TYPE_MAP && actual.kind == TYPE_MAP) {
        return scalar_compatible(expected.keyType, actual.keyType) &&
               scalar_compatible(expected.valType, actual.valType);
    }
    if (is_float_type(expected.kind) && is_float_type(actual.kind)) return true;
    return expected.kind == actual.kind;
}

static void check_type_match(CompilerState* cs, TypeInfo expected, TypeInfo actual, const char* msg) {
    if (!types_compatible(expected, actual)) {
        char buffer[160];
        snprintf(buffer, sizeof(buffer), "%s (expected %s, got %s)", msg, type_name(expected), type_name(actual));
        error(cs, buffer);
    }
}

static TypeInfo parse_variable(CompilerState* cs, Token name, bool canAssign) {
    int arg = resolve_local(cs, cs->current, &name);
    TypeInfo varType = make_type(TYPE_ANY);
    uint8_t getOp, setOp;

    if (arg != -1) {
        getOp = OP_GET_LOCAL;
        setOp = OP_SET_LOCAL;
        varType = cs->current->locals[arg].type;
    } else {
        arg = identifier_constant(cs, &name);
        getOp = OP_GET_GLOBAL;
        setOp = OP_SET_GLOBAL;
        GlobalSymbol* sym = find_global_symbol(cs, &name);
        if (sym != NULL) {
            varType = sym->type;
        } else {
            ObjString* str = cd_copy_string(cs->vm, name.start, name.length);
            Value dummy;
            if (!table_get(&cs->vm->globals, str, &dummy)) {
                error(cs, "Undefined variable.");
            }
        }
    }

    if (canAssign && match(cs, TOKEN_EQUAL)) {
        TypeInfo valType = expression(cs);
        check_type_match(cs, varType, valType, "Type mismatch in assignment");
        emit_bytes(cs, setOp, (uint8_t)arg);
        return valType;
    } else if (canAssign && match(cs, TOKEN_PLUS_EQUAL)) {
        emit_bytes(cs, getOp, (uint8_t)arg);
        TypeInfo valType = expression(cs);
        if (varType.kind == TYPE_INT && (valType.kind == TYPE_INT || valType.kind == TYPE_ANY)) {
            emit_byte(cs, OP_ADD);
        } else if (is_float_type(varType.kind) && (is_float_type(valType.kind) || valType.kind == TYPE_INT || valType.kind == TYPE_ANY)) {
            emit_byte(cs, OP_FADD);
        } else if (varType.kind == TYPE_STRING && (valType.kind == TYPE_STRING || valType.kind == TYPE_ANY)) {
            emit_byte(cs, OP_ADD);
        } else {
            error(cs, "Invalid operands for '+='.");
        }
        emit_bytes(cs, setOp, (uint8_t)arg);
        return varType;
    } else if (canAssign && match(cs, TOKEN_MINUS_EQUAL)) {
        emit_bytes(cs, getOp, (uint8_t)arg);
        TypeInfo valType = expression(cs);
        if (varType.kind == TYPE_INT && (valType.kind == TYPE_INT || valType.kind == TYPE_ANY)) {
            emit_byte(cs, OP_SUB);
        } else if (is_float_type(varType.kind) && (is_float_type(valType.kind) || valType.kind == TYPE_INT || valType.kind == TYPE_ANY)) {
            emit_byte(cs, OP_FSUB);
        } else {
            error(cs, "Invalid operands for '-='.");
        }
        emit_bytes(cs, setOp, (uint8_t)arg);
        return varType;
    } else if (canAssign && match(cs, TOKEN_PLUS_PLUS)) {
        if (varType.kind != TYPE_INT && !is_float_type(varType.kind) && varType.kind != TYPE_ANY) {
            error(cs, "Operand of '++' must be numeric.");
        }
        emit_bytes(cs, getOp, (uint8_t)arg);
        emit_bytes(cs, getOp, (uint8_t)arg);
        if (is_float_type(varType.kind)) {
            emit_constant(cs, FLOAT_VAL(1.0));
            emit_byte(cs, OP_FADD);
        } else {
            emit_constant(cs, INT_VAL(1));
            emit_byte(cs, OP_ADD);
        }
        emit_bytes(cs, setOp, (uint8_t)arg);
        emit_byte(cs, OP_POP);
        return varType;
    } else if (canAssign && match(cs, TOKEN_MINUS_MINUS)) {
        if (varType.kind != TYPE_INT && !is_float_type(varType.kind) && varType.kind != TYPE_ANY) {
            error(cs, "Operand of '--' must be numeric.");
        }
        emit_bytes(cs, getOp, (uint8_t)arg);
        emit_bytes(cs, getOp, (uint8_t)arg);
        if (is_float_type(varType.kind)) {
            emit_constant(cs, FLOAT_VAL(1.0));
            emit_byte(cs, OP_FSUB);
        } else {
            emit_constant(cs, INT_VAL(1));
            emit_byte(cs, OP_SUB);
        }
        emit_bytes(cs, setOp, (uint8_t)arg);
        emit_byte(cs, OP_POP);
        return varType;
    }

    emit_bytes(cs, getOp, (uint8_t)arg);
    return varType;
}

static TypeInfo call_expr(CompilerState* cs, Token calleeName) {
    int argCount = 0;
    TypeInfo argTypes[16];

    if (!check(cs, TOKEN_RIGHT_PAREN)) {
        do {
            if (argCount == 16) {
                error(cs, "Can't have more than 16 arguments.");
                expression(cs);
            } else {
                argTypes[argCount++] = expression(cs);
            }
        } while (match(cs, TOKEN_COMMA));
    }
    consume(cs, TOKEN_RIGHT_PAREN, "Expect ')' after arguments.");

    GlobalSymbol* sym = find_global_symbol(cs, &calleeName);
    TypeInfo retType = make_type(TYPE_ANY);

    if (sym != NULL) {
        if (sym->type.kind == TYPE_CLASS) {
            ClassInfo* ci = find_class_info_by_str(cs->vm, sym->type.className);
            ClassMethod* initMethod = find_class_method(ci, "init", 4);
            if (initMethod != NULL) {
                if (initMethod->arity != argCount) {
                    error(cs, "Incorrect argument count for constructor.");
                } else {
                    for (int i = 0; i < argCount; i++) {
                        check_type_match(cs, initMethod->paramTypes[i], argTypes[i], "Constructor argument type mismatch");
                    }
                }
            } else if (argCount != 0) {
                error(cs, "Expected 0 arguments for default constructor.");
            }
            retType = sym->type;
        } else if (sym->isFunction) {
            if (sym->arity >= 0) {
                if (sym->arity != argCount) {
                    error(cs, "Incorrect argument count for function.");
                } else {
                    for (int i = 0; i < argCount; i++) {
                        check_type_match(cs, sym->paramTypes[i], argTypes[i], "Argument type mismatch");
                    }
                }
            }
            retType = sym->returnType;
        } else {
            error(cs, "Cannot call non-function variable.");
        }
    } else {
        ObjString* str = cd_copy_string(cs->vm, calleeName.start, calleeName.length);
        Value dummy;
        if (!table_get(&cs->vm->globals, str, &dummy)) {
            error(cs, "Undefined function.");
        }
    }

    emit_bytes(cs, OP_CALL, (uint8_t)argCount);
    return retType;
}

static TypeInfo primary(CompilerState* cs, bool canAssign) {
    if (match(cs, TOKEN_KW_TRUE)) {
        emit_byte(cs, OP_TRUE);
        return make_type(TYPE_BOOL);
    }
    if (match(cs, TOKEN_KW_FALSE)) {
        emit_byte(cs, OP_FALSE);
        return make_type(TYPE_BOOL);
    }
    if (match(cs, TOKEN_KW_NULL)) {
        emit_byte(cs, OP_NIL);
        return make_type(TYPE_ANY);
    }
    if (match(cs, TOKEN_KW_THIS)) {
        Token thisToken;
        thisToken.start = "this";
        thisToken.length = 4;
        thisToken.line = cs->parser.previous.line;
        return parse_variable(cs, thisToken, false);
    }
    if (match(cs, TOKEN_INT)) {
        int64_t value = strtoll(cs->parser.previous.start, NULL, 10);
        emit_constant(cs, INT_VAL(value));
        return make_type(TYPE_INT);
    }
    if (match(cs, TOKEN_FLOAT)) {
        double value = strtod(cs->parser.previous.start, NULL);
        emit_constant(cs, FLOAT_VAL(value));
        return make_type(TYPE_DOUBLE);
    }
    if (match(cs, TOKEN_STRING)) {
        const char* s = cs->parser.previous.start + 1;
        int len = cs->parser.previous.length - 2;
        char* buf = CD_ALLOC(char, len + 1);
        int j = 0;
        for (int i = 0; i < len; i++) {
            if (s[i] == '\\' && i + 1 < len) {
                i++;
                if (s[i] == 'n') buf[j++] = '\n';
                else if (s[i] == 't') buf[j++] = '\t';
                else if (s[i] == '"') buf[j++] = '"';
                else if (s[i] == '\\') buf[j++] = '\\';
                else buf[j++] = s[i];
            } else {
                buf[j++] = s[i];
            }
        }
        buf[j] = '\0';
        if (j < len) {
            buf = CD_GROW_ARRAY(char, buf, len + 1, j + 1);
        }
        ObjString* str = cd_take_string(cs->vm, buf, j);
        emit_constant(cs, OBJ_VAL(str));
        return make_type(TYPE_STRING);
    }
    if (match(cs, TOKEN_LEFT_BRACKET)) {
        int count = 0;
        DataType elemType = TYPE_ANY;
        bool first = true;
        if (!check(cs, TOKEN_RIGHT_BRACKET)) {
            do {
                TypeInfo et = expression(cs);
                if (first) {
                    elemType = et.kind;
                    first = false;
                } else if (elemType != et.kind) {
                    if (is_float_type(elemType) && is_float_type(et.kind)) {
                        elemType = TYPE_DOUBLE;
                    } else {
                        elemType = TYPE_ANY;
                    }
                }
                if (count == 255) {
                    error(cs, "Too many elements in list literal.");
                } else {
                    count++;
                }
            } while (match(cs, TOKEN_COMMA));
        }
        consume(cs, TOKEN_RIGHT_BRACKET, "Expect ']' after list elements.");
        emit_bytes(cs, OP_BUILD_LIST, (uint8_t)count);
        TypeInfo t = make_type(TYPE_LIST);
        t.elemType = elemType;
        return t;
    }
    if (match(cs, TOKEN_LEFT_BRACE)) {
        int count = 0;
        DataType keyType = TYPE_ANY;
        DataType valType = TYPE_ANY;
        bool first = true;
        if (!check(cs, TOKEN_RIGHT_BRACE)) {
            do {
                TypeInfo kt = expression(cs);
                consume(cs, TOKEN_COLON, "Expect ':' after map key.");
                TypeInfo vt = expression(cs);
                if (first) {
                    keyType = kt.kind;
                    valType = vt.kind;
                    first = false;
                } else {
                    if (keyType != kt.kind) keyType = TYPE_ANY;
                    if (valType != vt.kind) valType = TYPE_ANY;
                }
                if (count == 255) {
                    error(cs, "Too many entries in map literal.");
                } else {
                    count++;
                }
            } while (match(cs, TOKEN_COMMA));
        }
        consume(cs, TOKEN_RIGHT_BRACE, "Expect '}' after map entries.");
        emit_bytes(cs, OP_BUILD_MAP, (uint8_t)count);
        TypeInfo t = make_type(TYPE_MAP);
        t.keyType = keyType;
        t.valType = valType;
        return t;
    }
    if (match(cs, TOKEN_LEFT_PAREN)) {
        TypeInfo type = expression(cs);
        consume(cs, TOKEN_RIGHT_PAREN, "Expect ')' after expression.");
        return type;
    }
    if (match(cs, TOKEN_MINUS)) {
        TypeInfo type = parse_precedence(cs, PREC_UNARY);
        if (type.kind == TYPE_INT) {
            emit_byte(cs, OP_NEGATE);
            return make_type(TYPE_INT);
        } else if (is_float_type(type.kind)) {
            emit_byte(cs, OP_FNEGATE);
            return type;
        }
        error(cs, "Operand of '-' must be int, float or double.");
        return make_type(TYPE_ANY);
    }
    if (match(cs, TOKEN_BANG)) {
        TypeInfo type = parse_precedence(cs, PREC_UNARY);
        check_type_match(cs, make_type(TYPE_BOOL), type, "Operand of '!' must be bool");
        emit_byte(cs, OP_NOT);
        return make_type(TYPE_BOOL);
    }
    if (match(cs, TOKEN_IDENTIFIER)) {
        Token name = cs->parser.previous;
        if (match(cs, TOKEN_LEFT_PAREN)) {
            uint8_t arg = identifier_constant(cs, &name);
            emit_bytes(cs, OP_GET_GLOBAL, arg);
            return call_expr(cs, name);
        }
        return parse_variable(cs, name, canAssign);
    }

    error(cs, "Expect expression.");
    advance(cs);
    return make_type(TYPE_ANY);
}

static Precedence get_precedence(TokenType type) {
    switch (type) {
        case TOKEN_PLUS:
        case TOKEN_MINUS: return PREC_TERM;
        case TOKEN_STAR:
        case TOKEN_SLASH:
        case TOKEN_PERCENT: return PREC_FACTOR;
        case TOKEN_BANG_EQUAL:
        case TOKEN_EQUAL_EQUAL: return PREC_EQUALITY;
        case TOKEN_GREATER:
        case TOKEN_GREATER_EQUAL:
        case TOKEN_LESS:
        case TOKEN_LESS_EQUAL: return PREC_COMPARISON;
        case TOKEN_AND: return PREC_AND;
        case TOKEN_OR: return PREC_OR;
        default: return PREC_NONE;
    }
}

static TypeInfo parse_precedence(CompilerState* cs, Precedence precedence) {
    bool canAssign = precedence <= PREC_ASSIGNMENT;
    TypeInfo leftType = primary(cs, canAssign);

    for (;;) {
        if (match(cs, TOKEN_LEFT_BRACKET)) {
            if (match(cs, TOKEN_COLON)) {
                emit_constant(cs, INT_VAL(0));
                if (check(cs, TOKEN_RIGHT_BRACKET)) {
                    emit_byte(cs, OP_NIL);
                } else {
                    TypeInfo endType = expression(cs);
                    check_type_match(cs, make_type(TYPE_INT), endType, "Slice end must be int");
                }
                consume(cs, TOKEN_RIGHT_BRACKET, "Expect ']' after slice.");
                if (leftType.kind != TYPE_LIST && leftType.kind != TYPE_STRING && leftType.kind != TYPE_ANY) {
                    error(cs, "Can only slice List or string.");
                }
                emit_byte(cs, OP_SLICE);
                continue;
            }

            TypeInfo idxType = expression(cs);
            if (match(cs, TOKEN_COLON)) {
                check_type_match(cs, make_type(TYPE_INT), idxType, "Slice start must be int");
                if (check(cs, TOKEN_RIGHT_BRACKET)) {
                    emit_byte(cs, OP_NIL);
                } else {
                    TypeInfo endType = expression(cs);
                    check_type_match(cs, make_type(TYPE_INT), endType, "Slice end must be int");
                }
                consume(cs, TOKEN_RIGHT_BRACKET, "Expect ']' after slice.");
                if (leftType.kind != TYPE_LIST && leftType.kind != TYPE_STRING && leftType.kind != TYPE_ANY) {
                    error(cs, "Can only slice List or string.");
                }
                emit_byte(cs, OP_SLICE);
                continue;
            }

            consume(cs, TOKEN_RIGHT_BRACKET, "Expect ']' after index.");
            if (leftType.kind == TYPE_LIST || leftType.kind == TYPE_STRING) {
                check_type_match(cs, make_type(TYPE_INT), idxType, "Index must be int");
            } else if (leftType.kind == TYPE_MAP) {
                check_type_match(cs, make_type(leftType.keyType), idxType, "Map key type mismatch");
            } else if (leftType.kind != TYPE_ANY) {
                error(cs, "Cannot index non-collection value.");
            }

            if (canAssign && match(cs, TOKEN_EQUAL)) {
                if (leftType.kind == TYPE_STRING) {
                    error(cs, "Strings are immutable.");
                }
                TypeInfo valType = expression(cs);
                if (leftType.kind == TYPE_LIST) {
                    check_type_match(cs, make_type(leftType.elemType), valType, "List element type mismatch");
                } else if (leftType.kind == TYPE_MAP) {
                    check_type_match(cs, make_type(leftType.valType), valType, "Map value type mismatch");
                }
                emit_byte(cs, OP_SET_INDEX);
                leftType = valType;
            } else {
                emit_byte(cs, OP_GET_INDEX);
                if (leftType.kind == TYPE_LIST) leftType = make_type(leftType.elemType);
                else if (leftType.kind == TYPE_MAP) leftType = make_type(leftType.valType);
                else if (leftType.kind == TYPE_STRING) leftType = make_type(TYPE_STRING);
                else leftType = make_type(TYPE_ANY);
            }
            continue;
        }

        if (match(cs, TOKEN_DOT)) {
            consume(cs, TOKEN_IDENTIFIER, "Expect property or method name.");
            Token prop = cs->parser.previous;
            uint8_t nameConst = identifier_constant(cs, &prop);

            if (canAssign && match(cs, TOKEN_EQUAL)) {
                TypeInfo valType = expression(cs);
                if (leftType.kind == TYPE_CLASS && leftType.className != NULL) {
                    ClassInfo* ci = find_class_info_by_str(cs->vm, leftType.className);
                    ClassField* field = find_class_field(ci, &prop);
                    if (field == NULL) {
                        error(cs, "Undefined field on class.");
                    } else {
                        check_type_match(cs, field->type, valType, "Type mismatch in field assignment");
                    }
                } else if (leftType.kind != TYPE_ANY) {
                    error(cs, "Only class instances have assignable fields.");
                }
                emit_bytes(cs, OP_SET_PROP, nameConst);
                leftType = valType;
            } else if (match(cs, TOKEN_LEFT_PAREN)) {
                int argCount = 0;
                TypeInfo argTypes[16];
                if (!check(cs, TOKEN_RIGHT_PAREN)) {
                    do {
                        TypeInfo at = expression(cs);
                        if (argCount < 16) argTypes[argCount] = at;
                        if (argCount == 255) {
                            error(cs, "Can't have more than 255 arguments.");
                        } else {
                            argCount++;
                        }
                    } while (match(cs, TOKEN_COMMA));
                }
                consume(cs, TOKEN_RIGHT_PAREN, "Expect ')' after arguments.");

                TypeInfo retType = make_type(TYPE_ANY);
                if (leftType.kind == TYPE_CLASS && leftType.className != NULL) {
                    ClassInfo* ci = find_class_info_by_str(cs->vm, leftType.className);
                    ClassMethod* m = find_class_method(ci, prop.start, prop.length);
                    if (m == NULL) {
                        error(cs, "Undefined method on class.");
                    } else {
                        if (m->arity != argCount) {
                            error(cs, "Incorrect argument count for method.");
                        } else {
                            for (int i = 0; i < argCount && i < 16; i++) {
                                check_type_match(cs, m->paramTypes[i], argTypes[i], "Method argument type mismatch");
                            }
                        }
                        retType = m->returnType;
                    }
                } else if (leftType.kind == TYPE_LIST) {
                    if ((prop.length == 3 && memcmp(prop.start, "add", 3) == 0) ||
                        (prop.length == 4 && memcmp(prop.start, "push", 4) == 0)) {
                        if (argCount != 1) error(cs, "Expected 1 argument.");
                        else check_type_match(cs, make_type(leftType.elemType), argTypes[0], "List element type mismatch");
                        retType = make_type(leftType.elemType);
                    } else if (prop.length == 3 && memcmp(prop.start, "pop", 3) == 0) {
                        if (argCount != 0) error(cs, "Expected 0 arguments.");
                        retType = make_type(leftType.elemType);
                    } else {
                        error(cs, "Unknown List method.");
                    }
                } else if (leftType.kind == TYPE_MAP) {
                    if ((prop.length == 3 && memcmp(prop.start, "has", 3) == 0) ||
                        (prop.length == 6 && memcmp(prop.start, "remove", 6) == 0)) {
                        if (argCount != 1) error(cs, "Expected 1 argument.");
                        else check_type_match(cs, make_type(leftType.keyType), argTypes[0], "Map key type mismatch");
                        retType = make_type(TYPE_BOOL);
                    } else {
                        error(cs, "Unknown Map method.");
                    }
                } else if (leftType.kind != TYPE_ANY) {
                    error(cs, "Cannot call method on primitive type.");
                }

                emit_byte(cs, OP_INVOKE_PROP);
                emit_byte(cs, nameConst);
                emit_byte(cs, (uint8_t)argCount);
                leftType = retType;
            } else {
                TypeInfo propType = make_type(TYPE_ANY);
                if (leftType.kind == TYPE_CLASS && leftType.className != NULL) {
                    ClassInfo* ci = find_class_info_by_str(cs->vm, leftType.className);
                    ClassField* field = find_class_field(ci, &prop);
                    if (field != NULL) {
                        propType = field->type;
                    } else {
                        ClassMethod* m = find_class_method(ci, prop.start, prop.length);
                        if (m == NULL) {
                            error(cs, "Undefined property on class.");
                        }
                    }
                } else if (leftType.kind == TYPE_LIST || leftType.kind == TYPE_MAP || leftType.kind == TYPE_STRING) {
                    if (prop.length == 6 && memcmp(prop.start, "length", 6) == 0) {
                        propType = make_type(TYPE_INT);
                    } else {
                        error(cs, "Unknown property.");
                    }
                } else if (leftType.kind != TYPE_ANY) {
                    error(cs, "Cannot access property on primitive type.");
                }
                emit_bytes(cs, OP_GET_PROP, nameConst);
                leftType = propType;
            }
            continue;
        }

        if (precedence > get_precedence(cs->parser.current.type)) break;

        advance(cs);
        TokenType op = cs->parser.previous.type;
        Precedence nextPrec = (Precedence)(get_precedence(op) + 1);

        if (op == TOKEN_AND) {
            check_type_match(cs, make_type(TYPE_BOOL), leftType, "Left operand of '&&' must be bool");
            int endJump = emit_jump(cs, OP_JUMP_IF_FALSE);
            emit_byte(cs, OP_POP);
            TypeInfo rightType = parse_precedence(cs, PREC_AND);
            check_type_match(cs, make_type(TYPE_BOOL), rightType, "Right operand of '&&' must be bool");
            patch_jump(cs, endJump);
            leftType = make_type(TYPE_BOOL);
            continue;
        }

        if (op == TOKEN_OR) {
            check_type_match(cs, make_type(TYPE_BOOL), leftType, "Left operand of '||' must be bool");
            int elseJump = emit_jump(cs, OP_JUMP_IF_FALSE);
            int endJump = emit_jump(cs, OP_JUMP);
            patch_jump(cs, elseJump);
            emit_byte(cs, OP_POP);
            TypeInfo rightType = parse_precedence(cs, PREC_OR);
            check_type_match(cs, make_type(TYPE_BOOL), rightType, "Right operand of '||' must be bool");
            patch_jump(cs, endJump);
            leftType = make_type(TYPE_BOOL);
            continue;
        }

        TypeInfo rightType = parse_precedence(cs, nextPrec);

        switch (op) {
            case TOKEN_PLUS:
                if (leftType.kind == TYPE_STRING || rightType.kind == TYPE_STRING) {
                    check_type_match(cs, make_type(TYPE_STRING), leftType, "Invalid operands for '+'");
                    check_type_match(cs, make_type(TYPE_STRING), rightType, "Invalid operands for '+'");
                    emit_byte(cs, OP_ADD);
                    leftType = make_type(TYPE_STRING);
                } else if (is_float_type(leftType.kind) || is_float_type(rightType.kind)) {
                    if ((leftType.kind != TYPE_INT && !is_float_type(leftType.kind) && leftType.kind != TYPE_ANY) ||
                        (rightType.kind != TYPE_INT && !is_float_type(rightType.kind) && rightType.kind != TYPE_ANY)) {
                        error(cs, "Invalid operands for '+'.");
                    }
                    emit_byte(cs, OP_FADD);
                    leftType = make_type(TYPE_DOUBLE);
                } else {
                    check_type_match(cs, make_type(TYPE_INT), leftType, "Invalid operands for '+'");
                    check_type_match(cs, make_type(TYPE_INT), rightType, "Invalid operands for '+'");
                    emit_byte(cs, OP_ADD);
                    leftType = make_type(TYPE_INT);
                }
                break;
            case TOKEN_MINUS:
                if (is_float_type(leftType.kind) || is_float_type(rightType.kind)) {
                    if ((leftType.kind != TYPE_INT && !is_float_type(leftType.kind) && leftType.kind != TYPE_ANY) ||
                        (rightType.kind != TYPE_INT && !is_float_type(rightType.kind) && rightType.kind != TYPE_ANY)) {
                        error(cs, "Invalid operands for '-'.");
                    }
                    emit_byte(cs, OP_FSUB);
                    leftType = make_type(TYPE_DOUBLE);
                } else {
                    check_type_match(cs, make_type(TYPE_INT), leftType, "Invalid operands for '-'");
                    check_type_match(cs, make_type(TYPE_INT), rightType, "Invalid operands for '-'");
                    emit_byte(cs, OP_SUB);
                    leftType = make_type(TYPE_INT);
                }
                break;
            case TOKEN_STAR:
                if (is_float_type(leftType.kind) || is_float_type(rightType.kind)) {
                    if ((leftType.kind != TYPE_INT && !is_float_type(leftType.kind) && leftType.kind != TYPE_ANY) ||
                        (rightType.kind != TYPE_INT && !is_float_type(rightType.kind) && rightType.kind != TYPE_ANY)) {
                        error(cs, "Invalid operands for '*'.");
                    }
                    emit_byte(cs, OP_FMUL);
                    leftType = make_type(TYPE_DOUBLE);
                } else {
                    check_type_match(cs, make_type(TYPE_INT), leftType, "Invalid operands for '*'");
                    check_type_match(cs, make_type(TYPE_INT), rightType, "Invalid operands for '*'");
                    emit_byte(cs, OP_MUL);
                    leftType = make_type(TYPE_INT);
                }
                break;
            case TOKEN_SLASH:
                if (is_float_type(leftType.kind) || is_float_type(rightType.kind)) {
                    if ((leftType.kind != TYPE_INT && !is_float_type(leftType.kind) && leftType.kind != TYPE_ANY) ||
                        (rightType.kind != TYPE_INT && !is_float_type(rightType.kind) && rightType.kind != TYPE_ANY)) {
                        error(cs, "Invalid operands for '/'.");
                    }
                    emit_byte(cs, OP_FDIV);
                    leftType = make_type(TYPE_DOUBLE);
                } else {
                    check_type_match(cs, make_type(TYPE_INT), leftType, "Invalid operands for '/'");
                    check_type_match(cs, make_type(TYPE_INT), rightType, "Invalid operands for '/'");
                    emit_byte(cs, OP_DIV);
                    leftType = make_type(TYPE_INT);
                }
                break;
            case TOKEN_PERCENT:
                check_type_match(cs, make_type(TYPE_INT), leftType, "Operands of '%' must be int");
                check_type_match(cs, make_type(TYPE_INT), rightType, "Operands of '%' must be int");
                emit_byte(cs, OP_MOD);
                leftType = make_type(TYPE_INT);
                break;
            case TOKEN_EQUAL_EQUAL:
                emit_byte(cs, OP_EQUAL);
                leftType = make_type(TYPE_BOOL);
                break;
            case TOKEN_BANG_EQUAL:
                emit_bytes(cs, OP_EQUAL, OP_NOT);
                leftType = make_type(TYPE_BOOL);
                break;
            case TOKEN_GREATER:
                if (is_float_type(leftType.kind) || is_float_type(rightType.kind)) {
                    if ((leftType.kind != TYPE_INT && !is_float_type(leftType.kind) && leftType.kind != TYPE_ANY) ||
                        (rightType.kind != TYPE_INT && !is_float_type(rightType.kind) && rightType.kind != TYPE_ANY)) {
                        error(cs, "Invalid operands for '>'.");
                    }
                    emit_byte(cs, OP_FGREATER);
                } else {
                    check_type_match(cs, make_type(TYPE_INT), leftType, "Invalid operands for '>'");
                    check_type_match(cs, make_type(TYPE_INT), rightType, "Invalid operands for '>'");
                    emit_byte(cs, OP_GREATER);
                }
                leftType = make_type(TYPE_BOOL);
                break;
            case TOKEN_GREATER_EQUAL:
                if (is_float_type(leftType.kind) || is_float_type(rightType.kind)) {
                    if ((leftType.kind != TYPE_INT && !is_float_type(leftType.kind) && leftType.kind != TYPE_ANY) ||
                        (rightType.kind != TYPE_INT && !is_float_type(rightType.kind) && rightType.kind != TYPE_ANY)) {
                        error(cs, "Invalid operands for '>='.");
                    }
                    emit_bytes(cs, OP_FLESS, OP_NOT);
                } else {
                    check_type_match(cs, make_type(TYPE_INT), leftType, "Invalid operands for '>='");
                    check_type_match(cs, make_type(TYPE_INT), rightType, "Invalid operands for '>='");
                    emit_bytes(cs, OP_LESS, OP_NOT);
                }
                leftType = make_type(TYPE_BOOL);
                break;
            case TOKEN_LESS:
                if (is_float_type(leftType.kind) || is_float_type(rightType.kind)) {
                    if ((leftType.kind != TYPE_INT && !is_float_type(leftType.kind) && leftType.kind != TYPE_ANY) ||
                        (rightType.kind != TYPE_INT && !is_float_type(rightType.kind) && rightType.kind != TYPE_ANY)) {
                        error(cs, "Invalid operands for '<'.");
                    }
                    emit_byte(cs, OP_FLESS);
                } else {
                    check_type_match(cs, make_type(TYPE_INT), leftType, "Invalid operands for '<'");
                    check_type_match(cs, make_type(TYPE_INT), rightType, "Invalid operands for '<'");
                    emit_byte(cs, OP_LESS);
                }
                leftType = make_type(TYPE_BOOL);
                break;
            case TOKEN_LESS_EQUAL:
                if (is_float_type(leftType.kind) || is_float_type(rightType.kind)) {
                    if ((leftType.kind != TYPE_INT && !is_float_type(leftType.kind) && leftType.kind != TYPE_ANY) ||
                        (rightType.kind != TYPE_INT && !is_float_type(rightType.kind) && rightType.kind != TYPE_ANY)) {
                        error(cs, "Invalid operands for '<='.");
                    }
                    emit_bytes(cs, OP_FGREATER, OP_NOT);
                } else {
                    check_type_match(cs, make_type(TYPE_INT), leftType, "Invalid operands for '<='");
                    check_type_match(cs, make_type(TYPE_INT), rightType, "Invalid operands for '<='");
                    emit_bytes(cs, OP_GREATER, OP_NOT);
                }
                leftType = make_type(TYPE_BOOL);
                break;
            default:
                return leftType;
        }
    }

    return leftType;
}

static TypeInfo expression(CompilerState* cs) {
    return parse_precedence(cs, PREC_ASSIGNMENT);
}

static void block(CompilerState* cs) {
    while (!check(cs, TOKEN_RIGHT_BRACE) && !check(cs, TOKEN_EOF)) {
        declaration(cs);
    }
    consume(cs, TOKEN_RIGHT_BRACE, "Expect '}' after block.");
}

static void var_declaration(CompilerState* cs, TypeInfo type, Token name) {
    if (type.kind == TYPE_VOID) {
        error(cs, "Variable cannot have type void.");
    }

    if (cs->current->scopeDepth > 0) {
        for (int i = cs->current->localCount - 1; i >= 0; i--) {
            Local* local = &cs->current->locals[i];
            if (local->depth != -1 && local->depth < cs->current->scopeDepth) break;
            if (identifiers_equal(&name, &local->name)) {
                error(cs, "Already a variable with this name in this scope.");
            }
        }
        add_local(cs, name, type);
    }

    if (match(cs, TOKEN_EQUAL)) {
        TypeInfo valType = expression(cs);
        check_type_match(cs, type, valType, "Type mismatch in variable initialization");
    } else {
        if (type.kind == TYPE_INT) emit_constant(cs, INT_VAL(0));
        else if (is_float_type(type.kind)) emit_constant(cs, FLOAT_VAL(0.0));
        else if (type.kind == TYPE_BOOL) emit_byte(cs, OP_FALSE);
        else emit_byte(cs, OP_NIL);
    }
    consume(cs, TOKEN_SEMICOLON, "Expect ';' after variable declaration.");

    if (cs->current->scopeDepth > 0) {
        cs->current->locals[cs->current->localCount - 1].depth = cs->current->scopeDepth;
    } else {
        uint8_t global = identifier_constant(cs, &name);
        add_global_symbol(cs, name, type, false, 0, NULL, make_type(TYPE_VOID));
        emit_bytes(cs, OP_DEFINE_GLOBAL, global);
    }
}

static void print_statement(CompilerState* cs) {
    consume(cs, TOKEN_LEFT_PAREN, "Expect '(' after print.");
    expression(cs);
    consume(cs, TOKEN_RIGHT_PAREN, "Expect ')' after value.");
    consume(cs, TOKEN_SEMICOLON, "Expect ';' after statement.");
    emit_byte(cs, OP_PRINT);
}

static void return_statement(CompilerState* cs) {
    if (cs->current->type == TYPE_SCRIPT) {
        error(cs, "Can't return from top-level code.");
    }

    if (match(cs, TOKEN_SEMICOLON)) {
        check_type_match(cs, cs->current->returnTypeInfo, make_type(TYPE_VOID), "Return value required");
        emit_return(cs);
    } else {
        TypeInfo type = expression(cs);
        check_type_match(cs, cs->current->returnTypeInfo, type, "Return type mismatch");
        consume(cs, TOKEN_SEMICOLON, "Expect ';' after return value.");
        emit_byte(cs, OP_RETURN);
    }
}

static void pop_locals_to_depth(CompilerState* cs, int targetDepth) {
    for (int i = cs->current->localCount - 1; i >= 0; i--) {
        if (cs->current->locals[i].depth <= targetDepth) break;
        emit_byte(cs, OP_POP);
    }
}

static void break_statement(CompilerState* cs) {
    if (cs->current->loop == NULL) {
        error(cs, "Can't use 'break' outside of a loop.");
        consume(cs, TOKEN_SEMICOLON, "Expect ';' after 'break'.");
        return;
    }
    consume(cs, TOKEN_SEMICOLON, "Expect ';' after 'break'.");
    LoopContext* loop = cs->current->loop;
    pop_locals_to_depth(cs, loop->scopeDepth);
    if (loop->breakCount >= 64) {
        error(cs, "Too many 'break' statements in loop.");
        return;
    }
    loop->breakJumps[loop->breakCount++] = emit_jump(cs, OP_JUMP);
}

static void continue_statement(CompilerState* cs) {
    if (cs->current->loop == NULL) {
        error(cs, "Can't use 'continue' outside of a loop.");
        consume(cs, TOKEN_SEMICOLON, "Expect ';' after 'continue'.");
        return;
    }
    consume(cs, TOKEN_SEMICOLON, "Expect ';' after 'continue'.");
    LoopContext* loop = cs->current->loop;
    pop_locals_to_depth(cs, loop->scopeDepth);
    emit_loop(cs, loop->loopStart);
}

static void if_statement(CompilerState* cs) {
    bool hasParen = match(cs, TOKEN_LEFT_PAREN);
    TypeInfo condType = expression(cs);
    check_type_match(cs, make_type(TYPE_BOOL), condType, "Condition must be bool");
    if (hasParen) consume(cs, TOKEN_RIGHT_PAREN, "Expect ')' after condition.");

    int thenJump = emit_jump(cs, OP_JUMP_IF_FALSE);
    emit_byte(cs, OP_POP);
    statement(cs);

    int elseJump = emit_jump(cs, OP_JUMP);
    patch_jump(cs, thenJump);
    emit_byte(cs, OP_POP);

    if (match(cs, TOKEN_KW_ELSE)) statement(cs);
    patch_jump(cs, elseJump);
}

static void while_statement(CompilerState* cs) {
    int loopStart = current_chunk(cs)->count;
    bool hasParen = match(cs, TOKEN_LEFT_PAREN);
    TypeInfo condType = expression(cs);
    check_type_match(cs, make_type(TYPE_BOOL), condType, "Condition must be bool");
    if (hasParen) consume(cs, TOKEN_RIGHT_PAREN, "Expect ')' after condition.");

    int exitJump = emit_jump(cs, OP_JUMP_IF_FALSE);
    emit_byte(cs, OP_POP);

    LoopContext loop;
    loop.enclosing = cs->current->loop;
    loop.loopStart = loopStart;
    loop.scopeDepth = cs->current->scopeDepth;
    loop.breakCount = 0;
    cs->current->loop = &loop;

    statement(cs);
    emit_loop(cs, loopStart);

    patch_jump(cs, exitJump);
    emit_byte(cs, OP_POP);

    for (int i = 0; i < loop.breakCount; i++) {
        patch_jump(cs, loop.breakJumps[i]);
    }
    cs->current->loop = loop.enclosing;
}

static void for_statement(CompilerState* cs) {
    begin_scope(cs);
    consume(cs, TOKEN_LEFT_PAREN, "Expect '(' after 'for'.");

    if (match(cs, TOKEN_SEMICOLON)) {
    } else if (match(cs, TOKEN_KW_INT)) {
        consume(cs, TOKEN_IDENTIFIER, "Expect variable name.");
        var_declaration(cs, make_type(TYPE_INT), cs->parser.previous);
    } else if (match(cs, TOKEN_KW_FLOAT)) {
        consume(cs, TOKEN_IDENTIFIER, "Expect variable name.");
        var_declaration(cs, make_type(TYPE_FLOAT), cs->parser.previous);
    } else if (match(cs, TOKEN_KW_DOUBLE)) {
        consume(cs, TOKEN_IDENTIFIER, "Expect variable name.");
        var_declaration(cs, make_type(TYPE_DOUBLE), cs->parser.previous);
    } else {
        expression(cs);
        consume(cs, TOKEN_SEMICOLON, "Expect ';'.");
        emit_byte(cs, OP_POP);
    }

    int loopStart = current_chunk(cs)->count;
    int exitJump = -1;
    if (!match(cs, TOKEN_SEMICOLON)) {
        TypeInfo condType = expression(cs);
        check_type_match(cs, make_type(TYPE_BOOL), condType, "For condition must be bool");
        consume(cs, TOKEN_SEMICOLON, "Expect ';' after loop condition.");

        exitJump = emit_jump(cs, OP_JUMP_IF_FALSE);
        emit_byte(cs, OP_POP);
    }

    if (!match(cs, TOKEN_RIGHT_PAREN)) {
        int bodyJump = emit_jump(cs, OP_JUMP);
        int incrementStart = current_chunk(cs)->count;
        expression(cs);
        emit_byte(cs, OP_POP);
        consume(cs, TOKEN_RIGHT_PAREN, "Expect ')' after for clauses.");

        emit_loop(cs, loopStart);
        loopStart = incrementStart;
        patch_jump(cs, bodyJump);
    }

    LoopContext loop;
    loop.enclosing = cs->current->loop;
    loop.loopStart = loopStart;
    loop.scopeDepth = cs->current->scopeDepth;
    loop.breakCount = 0;
    cs->current->loop = &loop;

    statement(cs);
    emit_loop(cs, loopStart);

    if (exitJump != -1) {
        patch_jump(cs, exitJump);
        emit_byte(cs, OP_POP);
    }

    for (int i = 0; i < loop.breakCount; i++) {
        patch_jump(cs, loop.breakJumps[i]);
    }
    cs->current->loop = loop.enclosing;

    end_scope(cs);
}

static void expression_statement(CompilerState* cs) {
    expression(cs);
    consume(cs, TOKEN_SEMICOLON, "Expect ';' after expression.");
    emit_byte(cs, OP_POP);
}

static void statement(CompilerState* cs) {
    if (match(cs, TOKEN_KW_PRINT)) {
        print_statement(cs);
    } else if (match(cs, TOKEN_KW_IF)) {
        if_statement(cs);
    } else if (match(cs, TOKEN_KW_RETURN)) {
        return_statement(cs);
    } else if (match(cs, TOKEN_KW_WHILE)) {
        while_statement(cs);
    } else if (match(cs, TOKEN_KW_FOR)) {
        for_statement(cs);
    } else if (match(cs, TOKEN_KW_BREAK)) {
        break_statement(cs);
    } else if (match(cs, TOKEN_KW_CONTINUE)) {
        continue_statement(cs);
    } else if (match(cs, TOKEN_LEFT_BRACE)) {
        begin_scope(cs);
        block(cs);
        end_scope(cs);
    } else {
        expression_statement(cs);
    }
}

static void function_declaration(CompilerState* cs, TypeInfo retType, Token name) {
    uint8_t global = identifier_constant(cs, &name);

    Compiler compiler;
    init_compiler(cs, &compiler, TYPE_FUNCTION, retType);
    compiler.function->name = cd_copy_string(cs->vm, name.start, name.length);

    begin_scope(cs);
    consume(cs, TOKEN_LEFT_PAREN, "Expect '(' after function name.");

    TypeInfo paramTypes[16];
    int arity = 0;

    if (!check(cs, TOKEN_RIGHT_PAREN)) {
        do {
            TypeInfo ptype = parse_type(cs);
            consume(cs, TOKEN_IDENTIFIER, "Expect parameter name.");
            Token pname = cs->parser.previous;

            if (arity == 16) {
                error_at_current(cs, "Can't have more than 16 parameters.");
            } else {
                paramTypes[arity] = ptype;
                cs->current->function->paramTypes[arity] = ptype.kind;
                cs->current->function->arity++;
                arity++;
                add_local(cs, pname, ptype);
                cs->current->locals[cs->current->localCount - 1].depth = cs->current->scopeDepth;
            }
        } while (match(cs, TOKEN_COMMA));
    }
    consume(cs, TOKEN_RIGHT_PAREN, "Expect ')' after parameters.");

    add_global_symbol(cs, name, make_type(TYPE_ANY), true, arity, paramTypes, retType);

    if (match(cs, TOKEN_ARROW)) {
        TypeInfo bodyType = expression(cs);
        check_type_match(cs, retType, bodyType, "Return type mismatch in arrow function");
        consume(cs, TOKEN_SEMICOLON, "Expect ';' after arrow function body.");
        emit_byte(cs, OP_RETURN);
    } else {
        consume(cs, TOKEN_LEFT_BRACE, "Expect '{' before function body.");
        block(cs);
    }

    ObjFunction* function = end_compiler(cs);
    emit_bytes(cs, OP_CONSTANT, make_constant(cs, OBJ_VAL(function)));
    emit_bytes(cs, OP_DEFINE_GLOBAL, global);
}

static void compile_constructor(CompilerState* cs, ClassInfo* ci, Token className, uint8_t nameConst) {
    emit_bytes(cs, OP_GET_GLOBAL, nameConst);

    Compiler compiler;
    init_compiler(cs, &compiler, TYPE_METHOD, make_type(TYPE_VOID));
    cs->current->locals[0].name.start = "this";
    cs->current->locals[0].name.length = 4;
    cs->current->locals[0].type = make_class_type(ci->name);
    compiler.function->name = cd_copy_string(cs->vm, "init", 4);
    compiler.function->isInitializer = true;

    begin_scope(cs);
    consume(cs, TOKEN_LEFT_PAREN, "Expect '(' after constructor name.");

    TypeInfo paramTypes[16];
    Token initFields[16];
    int initFieldSlots[16];
    int initFieldCount = 0;
    int arity = 0;

    if (!check(cs, TOKEN_RIGHT_PAREN)) {
        do {
            if (match(cs, TOKEN_KW_THIS)) {
                consume(cs, TOKEN_DOT, "Expect '.' after 'this'.");
                consume(cs, TOKEN_IDENTIFIER, "Expect field name.");
                Token fname = cs->parser.previous;
                ClassField* field = find_class_field(ci, &fname);
                TypeInfo ftype = make_type(TYPE_ANY);
                if (field == NULL) {
                    error(cs, "Unknown field in constructor parameter.");
                } else {
                    ftype = field->type;
                }
                if (arity == 16) {
                    error_at_current(cs, "Can't have more than 16 parameters.");
                } else {
                    paramTypes[arity] = ftype;
                    compiler.function->paramTypes[arity] = ftype.kind;
                    compiler.function->arity++;
                    arity++;
                    add_local(cs, fname, ftype);
                    int slot = cs->current->localCount - 1;
                    cs->current->locals[slot].depth = cs->current->scopeDepth;
                    initFields[initFieldCount] = fname;
                    initFieldSlots[initFieldCount] = slot;
                    initFieldCount++;
                }
            } else {
                TypeInfo ptype = parse_type(cs);
                consume(cs, TOKEN_IDENTIFIER, "Expect parameter name.");
                Token pname = cs->parser.previous;
                if (arity == 16) {
                    error_at_current(cs, "Can't have more than 16 parameters.");
                } else {
                    paramTypes[arity] = ptype;
                    compiler.function->paramTypes[arity] = ptype.kind;
                    compiler.function->arity++;
                    arity++;
                    add_local(cs, pname, ptype);
                    cs->current->locals[cs->current->localCount - 1].depth = cs->current->scopeDepth;
                }
            }
        } while (match(cs, TOKEN_COMMA));
    }
    consume(cs, TOKEN_RIGHT_PAREN, "Expect ')' after constructor parameters.");

    add_class_method(cs, ci, "init", 4, arity, paramTypes, make_class_type(ci->name));

    for (int i = 0; i < initFieldCount; i++) {
        uint8_t fieldConst = identifier_constant(cs, &initFields[i]);
        emit_bytes(cs, OP_GET_LOCAL, 0);
        emit_bytes(cs, OP_GET_LOCAL, (uint8_t)initFieldSlots[i]);
        emit_bytes(cs, OP_SET_PROP, fieldConst);
        emit_byte(cs, OP_POP);
    }

    if (!match(cs, TOKEN_SEMICOLON)) {
        consume(cs, TOKEN_LEFT_BRACE, "Expect '{' or ';' after constructor.");
        block(cs);
    }

    (void)className;
    ObjFunction* methodFn = end_compiler(cs);
    emit_bytes(cs, OP_CONSTANT, make_constant(cs, OBJ_VAL(methodFn)));
    Token initTok;
    initTok.start = "init";
    initTok.length = 4;
    initTok.line = cs->parser.previous.line;
    uint8_t methodConst = identifier_constant(cs, &initTok);
    emit_bytes(cs, OP_METHOD, methodConst);
    emit_byte(cs, OP_POP);
}

static void class_declaration(CompilerState* cs) {
    consume(cs, TOKEN_IDENTIFIER, "Expect class name.");
    Token className = cs->parser.previous;
    uint8_t nameConst = identifier_constant(cs, &className);
    ClassInfo* ci = add_class_info(cs, className);
    TypeInfo classType = make_class_type(ci->name);
    add_global_symbol(cs, className, classType, false, 0, NULL, classType);

    emit_bytes(cs, OP_CLASS, nameConst);
    emit_bytes(cs, OP_DEFINE_GLOBAL, nameConst);

    consume(cs, TOKEN_LEFT_BRACE, "Expect '{' before class body.");

    while (!check(cs, TOKEN_RIGHT_BRACE) && !check(cs, TOKEN_EOF)) {
        if (check(cs, TOKEN_IDENTIFIER) && identifiers_equal(&cs->parser.current, &className)) {
            Lexer saveLexer = cs->lexer;
            Token nextTok = lexer_next(&saveLexer);
            if (nextTok.type == TOKEN_LEFT_PAREN) {
                advance(cs);
                compile_constructor(cs, ci, className, nameConst);
                continue;
            }
        }

        TypeInfo memberType = parse_type(cs);
        consume(cs, TOKEN_IDENTIFIER, "Expect member name.");
        Token memberName = cs->parser.previous;

        if (check(cs, TOKEN_LEFT_PAREN)) {
            emit_bytes(cs, OP_GET_GLOBAL, nameConst);

            Compiler compiler;
            init_compiler(cs, &compiler, TYPE_METHOD, memberType);
            cs->current->locals[0].name.start = "this";
            cs->current->locals[0].name.length = 4;
            cs->current->locals[0].type = classType;
            compiler.function->name = cd_copy_string(cs->vm, memberName.start, memberName.length);
            bool isInit = (memberName.length == 4 && memcmp(memberName.start, "init", 4) == 0);
            if (isInit) {
                compiler.function->isInitializer = true;
            }

            begin_scope(cs);
            consume(cs, TOKEN_LEFT_PAREN, "Expect '(' after method name.");
            TypeInfo paramTypes[16];
            int arity = 0;
            if (!check(cs, TOKEN_RIGHT_PAREN)) {
                do {
                    TypeInfo ptype = parse_type(cs);
                    consume(cs, TOKEN_IDENTIFIER, "Expect parameter name.");
                    Token pname = cs->parser.previous;
                    if (arity == 16) {
                        error_at_current(cs, "Can't have more than 16 parameters.");
                    } else {
                        paramTypes[arity] = ptype;
                        compiler.function->paramTypes[arity] = ptype.kind;
                        compiler.function->arity++;
                        arity++;
                        add_local(cs, pname, ptype);
                        cs->current->locals[cs->current->localCount - 1].depth = cs->current->scopeDepth;
                    }
                } while (match(cs, TOKEN_COMMA));
            }
            consume(cs, TOKEN_RIGHT_PAREN, "Expect ')' after parameters.");

            TypeInfo regRet = isInit ? classType : memberType;
            add_class_method(cs, ci, memberName.start, memberName.length, arity, paramTypes, regRet);

            if (match(cs, TOKEN_ARROW)) {
                TypeInfo bodyType = expression(cs);
                check_type_match(cs, memberType, bodyType, "Return type mismatch in arrow method");
                consume(cs, TOKEN_SEMICOLON, "Expect ';' after arrow method.");
                emit_byte(cs, OP_RETURN);
            } else {
                consume(cs, TOKEN_LEFT_BRACE, "Expect '{' before method body.");
                block(cs);
            }

            ObjFunction* methodFn = end_compiler(cs);
            emit_bytes(cs, OP_CONSTANT, make_constant(cs, OBJ_VAL(methodFn)));
            uint8_t methodConst = identifier_constant(cs, &memberName);
            emit_bytes(cs, OP_METHOD, methodConst);
            emit_byte(cs, OP_POP);
        } else {
            if (memberType.kind == TYPE_VOID) {
                error(cs, "Field cannot have type void.");
            }
            add_class_field(cs, ci, memberName, memberType);
            uint8_t fieldConst = identifier_constant(cs, &memberName);
            emit_bytes(cs, OP_GET_GLOBAL, nameConst);
            if (match(cs, TOKEN_EQUAL)) {
                TypeInfo valType = expression(cs);
                check_type_match(cs, memberType, valType, "Type mismatch in field initializer");
            } else {
                if (memberType.kind == TYPE_INT) emit_constant(cs, INT_VAL(0));
                else if (is_float_type(memberType.kind)) emit_constant(cs, FLOAT_VAL(0.0));
                else if (memberType.kind == TYPE_BOOL) emit_byte(cs, OP_FALSE);
                else emit_byte(cs, OP_NIL);
            }
            emit_bytes(cs, OP_CLASS_FIELD, fieldConst);
            consume(cs, TOKEN_SEMICOLON, "Expect ';' after field declaration.");
        }
    }

    consume(cs, TOKEN_RIGHT_BRACE, "Expect '}' after class body.");
}

static void declaration(CompilerState* cs) {
    if (match(cs, TOKEN_KW_CLASS)) {
        class_declaration(cs);
        return;
    }

    TokenType t = cs->parser.current.type;
    if (is_builtin_type_token(t)) {
        TypeInfo dt = parse_type(cs);
        consume(cs, TOKEN_IDENTIFIER, "Expect identifier.");
        Token name = cs->parser.previous;
        if (check(cs, TOKEN_LEFT_PAREN)) {
            function_declaration(cs, dt, name);
            return;
        }
        var_declaration(cs, dt, name);
        return;
    }

    if (t == TOKEN_IDENTIFIER) {
        Lexer saveLexer = cs->lexer;
        Token nextTok = lexer_next(&saveLexer);
        if (nextTok.type == TOKEN_IDENTIFIER) {
            TypeInfo dt = parse_type(cs);
            consume(cs, TOKEN_IDENTIFIER, "Expect identifier.");
            Token name = cs->parser.previous;
            if (check(cs, TOKEN_LEFT_PAREN)) {
                function_declaration(cs, dt, name);
                return;
            }
            var_declaration(cs, dt, name);
            return;
        }
    }

    statement(cs);
}

ObjFunction* cd_compile(cd_State* cd, const char* source) {
    CompilerState cs;
    cs.vm = cd;
    cs.current = NULL;
    cs.parser.hadError = false;
    cs.parser.panicMode = false;
    lexer_init(&cs.lexer, source);

    Compiler compiler;
    init_compiler(&cs, &compiler, TYPE_SCRIPT, make_type(TYPE_VOID));

    advance(&cs);
    while (!match(&cs, TOKEN_EOF)) {
        declaration(&cs);
    }

    ObjFunction* function = end_compiler(&cs);
    return cs.parser.hadError ? NULL : function;
}
