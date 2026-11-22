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
    DataType type;
} Local;

typedef enum {
    TYPE_SCRIPT,
    TYPE_FUNCTION,
    TYPE_METHOD
} FunctionType;

struct Compiler {
    struct Compiler* enclosing;
    ObjFunction* function;
    FunctionType type;
    Local locals[256];
    int localCount;
    int scopeDepth;
};

typedef struct {
    Parser parser;
    Lexer lexer;
    cd_State* vm;
    Compiler* current;
} CompilerState;

void cd_mark_compiler_roots(cd_State* cd) {
    Compiler* compiler = cd->currentCompiler;
    while (compiler != NULL) {
        cd_mark_object(cd, (Obj*)compiler->function);
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

static void init_compiler(CompilerState* cs, Compiler* compiler, FunctionType type, DataType retType) {
    compiler->enclosing = cs->current;
    compiler->function = NULL;
    compiler->type = type;
    compiler->localCount = 0;
    compiler->scopeDepth = 0;
    cs->current = compiler;
    cs->vm->currentCompiler = compiler;
    compiler->function = cd_new_function(cs->vm);
    compiler->function->returnType = retType;

    Local* local = &cs->current->locals[cs->current->localCount++];
    local->depth = 0;
    local->name.start = "";
    local->name.length = 0;
    local->type = TYPE_VOID;
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

static void add_local(CompilerState* cs, Token name, DataType type) {
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

static void add_global_symbol(CompilerState* cs, Token name, DataType type, int arity, DataType* params, DataType ret) {
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
    sym->arity = arity;
    sym->returnType = ret;
    if (params) {
        for (int i = 0; i < arity; i++) sym->paramTypes[i] = params[i];
    }
}

static DataType expression(CompilerState* cs);
static void statement(CompilerState* cs);
static void declaration(CompilerState* cs);
static DataType parse_precedence(CompilerState* cs, Precedence precedence);

static DataType type_from_token(CompilerState* cs, TokenType type) {
    switch (type) {
        case TOKEN_KW_INT: return TYPE_INT;
        case TOKEN_KW_FLOAT: return TYPE_FLOAT;
        case TOKEN_KW_DOUBLE: return TYPE_DOUBLE;
        case TOKEN_KW_BOOL: return TYPE_BOOL;
        case TOKEN_KW_STRING: return TYPE_STRING;
        case TOKEN_KW_VOID: return TYPE_VOID;
        case TOKEN_KW_LIST: {
            if (match(cs, TOKEN_LESS)) {
                while (!check(cs, TOKEN_GREATER) && !check(cs, TOKEN_EOF)) advance(cs);
                consume(cs, TOKEN_GREATER, "Expect '>' after type argument.");
            }
            return TYPE_LIST;
        }
        case TOKEN_KW_MAP: {
            if (match(cs, TOKEN_LESS)) {
                while (!check(cs, TOKEN_GREATER) && !check(cs, TOKEN_EOF)) advance(cs);
                consume(cs, TOKEN_GREATER, "Expect '>' after type arguments.");
            }
            return TYPE_MAP;
        }
        default: return TYPE_ANY;
    }
}

static const char* type_name(DataType type) {
    switch (type) {
        case TYPE_VOID: return "void";
        case TYPE_INT: return "int";
        case TYPE_FLOAT: return "float";
        case TYPE_DOUBLE: return "double";
        case TYPE_BOOL: return "bool";
        case TYPE_STRING: return "string";
        case TYPE_LIST: return "List";
        case TYPE_MAP: return "Map";
        case TYPE_CLASS: return "class";
        case TYPE_ANY: return "any";
    }
    return "unknown";
}

static bool is_float_type(DataType t) {
    return t == TYPE_FLOAT || t == TYPE_DOUBLE;
}

static bool types_compatible(DataType a, DataType b) {
    if (a == TYPE_ANY || b == TYPE_ANY) return true;
    if (a == b) return true;
    if (is_float_type(a) && is_float_type(b)) return true;
    if (a == TYPE_CLASS || b == TYPE_CLASS) return true;
    return false;
}

static void check_type_match(CompilerState* cs, DataType expected, DataType actual, const char* msg) {
    if (!types_compatible(expected, actual)) {
        char buffer[128];
        snprintf(buffer, sizeof(buffer), "%s (expected %s, got %s)", msg, type_name(expected), type_name(actual));
        error(cs, buffer);
    }
}

static DataType parse_variable(CompilerState* cs, Token name, bool canAssign) {
    int arg = resolve_local(cs, cs->current, &name);
    DataType varType = TYPE_ANY;
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
        if (sym != NULL) varType = sym->type;
    }

    if (canAssign && match(cs, TOKEN_EQUAL)) {
        DataType valType = expression(cs);
        check_type_match(cs, varType, valType, "Type mismatch in assignment");
        emit_bytes(cs, setOp, (uint8_t)arg);
        return valType;
    } else if (canAssign && match(cs, TOKEN_PLUS_EQUAL)) {
        emit_bytes(cs, getOp, (uint8_t)arg);
        DataType valType = expression(cs);
        if (varType == TYPE_INT && (valType == TYPE_INT || valType == TYPE_ANY)) {
            emit_byte(cs, OP_ADD);
        } else if (is_float_type(varType) && (is_float_type(valType) || valType == TYPE_INT || valType == TYPE_ANY)) {
            emit_byte(cs, OP_FADD);
        } else if (varType == TYPE_STRING && (valType == TYPE_STRING || valType == TYPE_ANY)) {
            emit_byte(cs, OP_ADD);
        } else {
            error(cs, "Invalid operands for '+='.");
        }
        emit_bytes(cs, setOp, (uint8_t)arg);
        return varType;
    } else if (canAssign && match(cs, TOKEN_MINUS_EQUAL)) {
        emit_bytes(cs, getOp, (uint8_t)arg);
        DataType valType = expression(cs);
        if (varType == TYPE_INT && (valType == TYPE_INT || valType == TYPE_ANY)) {
            emit_byte(cs, OP_SUB);
        } else if (is_float_type(varType) && (is_float_type(valType) || valType == TYPE_INT || valType == TYPE_ANY)) {
            emit_byte(cs, OP_FSUB);
        } else {
            error(cs, "Invalid operands for '-='.");
        }
        emit_bytes(cs, setOp, (uint8_t)arg);
        return varType;
    }

    emit_bytes(cs, getOp, (uint8_t)arg);
    return varType;
}

static DataType call_expr(CompilerState* cs, Token calleeName) {
    int argCount = 0;
    DataType argTypes[16];

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
    DataType retType = TYPE_ANY;

    if (sym != NULL) {
        if (sym->type != TYPE_CLASS && sym->arity != argCount) {
            error(cs, "Incorrect argument count for function.");
        }
        if (sym->type != TYPE_CLASS) {
            for (int i = 0; i < argCount; i++) {
                check_type_match(cs, sym->paramTypes[i], argTypes[i], "Argument type mismatch");
            }
            retType = sym->returnType;
        } else {
            retType = TYPE_CLASS;
        }
    }

    emit_bytes(cs, OP_CALL, (uint8_t)argCount);
    return retType;
}

static DataType primary(CompilerState* cs, bool canAssign) {
    if (match(cs, TOKEN_KW_TRUE)) {
        emit_byte(cs, OP_TRUE);
        return TYPE_BOOL;
    }
    if (match(cs, TOKEN_KW_FALSE)) {
        emit_byte(cs, OP_FALSE);
        return TYPE_BOOL;
    }
    if (match(cs, TOKEN_KW_NULL)) {
        emit_byte(cs, OP_NIL);
        return TYPE_ANY;
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
        return TYPE_INT;
    }
    if (match(cs, TOKEN_FLOAT)) {
        double value = strtod(cs->parser.previous.start, NULL);
        emit_constant(cs, FLOAT_VAL(value));
        return TYPE_DOUBLE;
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
        return TYPE_STRING;
    }
    if (match(cs, TOKEN_LEFT_BRACKET)) {
        int count = 0;
        if (!check(cs, TOKEN_RIGHT_BRACKET)) {
            do {
                expression(cs);
                if (count == 255) {
                    error(cs, "Too many elements in list literal.");
                } else {
                    count++;
                }
            } while (match(cs, TOKEN_COMMA));
        }
        consume(cs, TOKEN_RIGHT_BRACKET, "Expect ']' after list elements.");
        emit_bytes(cs, OP_BUILD_LIST, (uint8_t)count);
        return TYPE_LIST;
    }
    if (match(cs, TOKEN_LEFT_BRACE)) {
        int count = 0;
        if (!check(cs, TOKEN_RIGHT_BRACE)) {
            do {
                expression(cs);
                consume(cs, TOKEN_COLON, "Expect ':' after map key.");
                expression(cs);
                if (count == 255) {
                    error(cs, "Too many entries in map literal.");
                } else {
                    count++;
                }
            } while (match(cs, TOKEN_COMMA));
        }
        consume(cs, TOKEN_RIGHT_BRACE, "Expect '}' after map entries.");
        emit_bytes(cs, OP_BUILD_MAP, (uint8_t)count);
        return TYPE_MAP;
    }
    if (match(cs, TOKEN_LEFT_PAREN)) {
        DataType type = expression(cs);
        consume(cs, TOKEN_RIGHT_PAREN, "Expect ')' after expression.");
        return type;
    }
    if (match(cs, TOKEN_MINUS)) {
        DataType type = parse_precedence(cs, PREC_UNARY);
        if (type == TYPE_INT) {
            emit_byte(cs, OP_NEGATE);
            return TYPE_INT;
        } else if (is_float_type(type)) {
            emit_byte(cs, OP_FNEGATE);
            return type;
        }
        error(cs, "Operand of '-' must be int, float or double.");
        return TYPE_ANY;
    }
    if (match(cs, TOKEN_BANG)) {
        DataType type = parse_precedence(cs, PREC_UNARY);
        check_type_match(cs, TYPE_BOOL, type, "Operand of '!' must be bool");
        emit_byte(cs, OP_NOT);
        return TYPE_BOOL;
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
    return TYPE_ANY;
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

static DataType parse_precedence(CompilerState* cs, Precedence precedence) {
    bool canAssign = precedence <= PREC_ASSIGNMENT;
    DataType leftType = primary(cs, canAssign);

    for (;;) {
        if (match(cs, TOKEN_LEFT_BRACKET)) {
            expression(cs);
            consume(cs, TOKEN_RIGHT_BRACKET, "Expect ']' after index.");
            if (canAssign && match(cs, TOKEN_EQUAL)) {
                DataType valType = expression(cs);
                emit_byte(cs, OP_SET_INDEX);
                leftType = valType;
            } else {
                emit_byte(cs, OP_GET_INDEX);
                leftType = TYPE_ANY;
            }
            continue;
        }

        if (match(cs, TOKEN_DOT)) {
            consume(cs, TOKEN_IDENTIFIER, "Expect property or method name.");
            Token prop = cs->parser.previous;
            uint8_t nameConst = identifier_constant(cs, &prop);

            if (canAssign && match(cs, TOKEN_EQUAL)) {
                DataType valType = expression(cs);
                emit_bytes(cs, OP_SET_PROP, nameConst);
                leftType = valType;
            } else if (match(cs, TOKEN_LEFT_PAREN)) {
                int argCount = 0;
                if (!check(cs, TOKEN_RIGHT_PAREN)) {
                    do {
                        expression(cs);
                        if (argCount == 255) {
                            error(cs, "Can't have more than 255 arguments.");
                        } else {
                            argCount++;
                        }
                    } while (match(cs, TOKEN_COMMA));
                }
                consume(cs, TOKEN_RIGHT_PAREN, "Expect ')' after arguments.");
                emit_byte(cs, OP_INVOKE_PROP);
                emit_byte(cs, nameConst);
                emit_byte(cs, (uint8_t)argCount);
                leftType = TYPE_ANY;
            } else {
                emit_bytes(cs, OP_GET_PROP, nameConst);
                leftType = TYPE_ANY;
            }
            continue;
        }

        if (precedence > get_precedence(cs->parser.current.type)) break;

        advance(cs);
        TokenType op = cs->parser.previous.type;
        Precedence nextPrec = (Precedence)(get_precedence(op) + 1);

        if (op == TOKEN_AND) {
            check_type_match(cs, TYPE_BOOL, leftType, "Left operand of '&&' must be bool");
            int endJump = emit_jump(cs, OP_JUMP_IF_FALSE);
            emit_byte(cs, OP_POP);
            DataType rightType = parse_precedence(cs, PREC_AND);
            check_type_match(cs, TYPE_BOOL, rightType, "Right operand of '&&' must be bool");
            patch_jump(cs, endJump);
            leftType = TYPE_BOOL;
            continue;
        }

        if (op == TOKEN_OR) {
            check_type_match(cs, TYPE_BOOL, leftType, "Left operand of '||' must be bool");
            int elseJump = emit_jump(cs, OP_JUMP_IF_FALSE);
            int endJump = emit_jump(cs, OP_JUMP);
            patch_jump(cs, elseJump);
            emit_byte(cs, OP_POP);
            DataType rightType = parse_precedence(cs, PREC_OR);
            check_type_match(cs, TYPE_BOOL, rightType, "Right operand of '||' must be bool");
            patch_jump(cs, endJump);
            leftType = TYPE_BOOL;
            continue;
        }

        DataType rightType = parse_precedence(cs, nextPrec);

        switch (op) {
            case TOKEN_PLUS:
                if (leftType == TYPE_STRING || rightType == TYPE_STRING) {
                    check_type_match(cs, TYPE_STRING, leftType, "Invalid operands for '+'");
                    check_type_match(cs, TYPE_STRING, rightType, "Invalid operands for '+'");
                    emit_byte(cs, OP_ADD);
                    leftType = TYPE_STRING;
                } else if (is_float_type(leftType) || is_float_type(rightType)) {
                    if ((leftType != TYPE_INT && !is_float_type(leftType) && leftType != TYPE_ANY) ||
                        (rightType != TYPE_INT && !is_float_type(rightType) && rightType != TYPE_ANY)) {
                        error(cs, "Invalid operands for '+'.");
                    }
                    emit_byte(cs, OP_FADD);
                    leftType = TYPE_DOUBLE;
                } else {
                    check_type_match(cs, TYPE_INT, leftType, "Invalid operands for '+'");
                    check_type_match(cs, TYPE_INT, rightType, "Invalid operands for '+'");
                    emit_byte(cs, OP_ADD);
                    leftType = TYPE_INT;
                }
                break;
            case TOKEN_MINUS:
                if (is_float_type(leftType) || is_float_type(rightType)) {
                    if ((leftType != TYPE_INT && !is_float_type(leftType) && leftType != TYPE_ANY) ||
                        (rightType != TYPE_INT && !is_float_type(rightType) && rightType != TYPE_ANY)) {
                        error(cs, "Invalid operands for '-'.");
                    }
                    emit_byte(cs, OP_FSUB);
                    leftType = TYPE_DOUBLE;
                } else {
                    check_type_match(cs, TYPE_INT, leftType, "Invalid operands for '-'");
                    check_type_match(cs, TYPE_INT, rightType, "Invalid operands for '-'");
                    emit_byte(cs, OP_SUB);
                    leftType = TYPE_INT;
                }
                break;
            case TOKEN_STAR:
                if (is_float_type(leftType) || is_float_type(rightType)) {
                    if ((leftType != TYPE_INT && !is_float_type(leftType) && leftType != TYPE_ANY) ||
                        (rightType != TYPE_INT && !is_float_type(rightType) && rightType != TYPE_ANY)) {
                        error(cs, "Invalid operands for '*'.");
                    }
                    emit_byte(cs, OP_FMUL);
                    leftType = TYPE_DOUBLE;
                } else {
                    check_type_match(cs, TYPE_INT, leftType, "Invalid operands for '*'");
                    check_type_match(cs, TYPE_INT, rightType, "Invalid operands for '*'");
                    emit_byte(cs, OP_MUL);
                    leftType = TYPE_INT;
                }
                break;
            case TOKEN_SLASH:
                if (is_float_type(leftType) || is_float_type(rightType)) {
                    if ((leftType != TYPE_INT && !is_float_type(leftType) && leftType != TYPE_ANY) ||
                        (rightType != TYPE_INT && !is_float_type(rightType) && rightType != TYPE_ANY)) {
                        error(cs, "Invalid operands for '/'.");
                    }
                    emit_byte(cs, OP_FDIV);
                    leftType = TYPE_DOUBLE;
                } else {
                    check_type_match(cs, TYPE_INT, leftType, "Invalid operands for '/'");
                    check_type_match(cs, TYPE_INT, rightType, "Invalid operands for '/'");
                    emit_byte(cs, OP_DIV);
                    leftType = TYPE_INT;
                }
                break;
            case TOKEN_PERCENT:
                check_type_match(cs, TYPE_INT, leftType, "Operands of '%' must be int");
                check_type_match(cs, TYPE_INT, rightType, "Operands of '%' must be int");
                emit_byte(cs, OP_MOD);
                leftType = TYPE_INT;
                break;
            case TOKEN_EQUAL_EQUAL:
                emit_byte(cs, OP_EQUAL);
                leftType = TYPE_BOOL;
                break;
            case TOKEN_BANG_EQUAL:
                emit_bytes(cs, OP_EQUAL, OP_NOT);
                leftType = TYPE_BOOL;
                break;
            case TOKEN_GREATER:
                if (is_float_type(leftType) || is_float_type(rightType)) {
                    if ((leftType != TYPE_INT && !is_float_type(leftType) && leftType != TYPE_ANY) ||
                        (rightType != TYPE_INT && !is_float_type(rightType) && rightType != TYPE_ANY)) {
                        error(cs, "Invalid operands for '>'.");
                    }
                    emit_byte(cs, OP_FGREATER);
                } else {
                    check_type_match(cs, TYPE_INT, leftType, "Invalid operands for '>'");
                    check_type_match(cs, TYPE_INT, rightType, "Invalid operands for '>'");
                    emit_byte(cs, OP_GREATER);
                }
                leftType = TYPE_BOOL;
                break;
            case TOKEN_GREATER_EQUAL:
                if (is_float_type(leftType) || is_float_type(rightType)) {
                    if ((leftType != TYPE_INT && !is_float_type(leftType) && leftType != TYPE_ANY) ||
                        (rightType != TYPE_INT && !is_float_type(rightType) && rightType != TYPE_ANY)) {
                        error(cs, "Invalid operands for '>='.");
                    }
                    emit_bytes(cs, OP_FLESS, OP_NOT);
                } else {
                    check_type_match(cs, TYPE_INT, leftType, "Invalid operands for '>='");
                    check_type_match(cs, TYPE_INT, rightType, "Invalid operands for '>='");
                    emit_bytes(cs, OP_LESS, OP_NOT);
                }
                leftType = TYPE_BOOL;
                break;
            case TOKEN_LESS:
                if (is_float_type(leftType) || is_float_type(rightType)) {
                    if ((leftType != TYPE_INT && !is_float_type(leftType) && leftType != TYPE_ANY) ||
                        (rightType != TYPE_INT && !is_float_type(rightType) && rightType != TYPE_ANY)) {
                        error(cs, "Invalid operands for '<'.");
                    }
                    emit_byte(cs, OP_FLESS);
                } else {
                    check_type_match(cs, TYPE_INT, leftType, "Invalid operands for '<'");
                    check_type_match(cs, TYPE_INT, rightType, "Invalid operands for '<'");
                    emit_byte(cs, OP_LESS);
                }
                leftType = TYPE_BOOL;
                break;
            case TOKEN_LESS_EQUAL:
                if (is_float_type(leftType) || is_float_type(rightType)) {
                    if ((leftType != TYPE_INT && !is_float_type(leftType) && leftType != TYPE_ANY) ||
                        (rightType != TYPE_INT && !is_float_type(rightType) && rightType != TYPE_ANY)) {
                        error(cs, "Invalid operands for '<='.");
                    }
                    emit_bytes(cs, OP_FGREATER, OP_NOT);
                } else {
                    check_type_match(cs, TYPE_INT, leftType, "Invalid operands for '<='");
                    check_type_match(cs, TYPE_INT, rightType, "Invalid operands for '<='");
                    emit_bytes(cs, OP_GREATER, OP_NOT);
                }
                leftType = TYPE_BOOL;
                break;
            default:
                return leftType;
        }
    }

    return leftType;
}

static DataType expression(CompilerState* cs) {
    return parse_precedence(cs, PREC_ASSIGNMENT);
}

static void block(CompilerState* cs) {
    while (!check(cs, TOKEN_RIGHT_BRACE) && !check(cs, TOKEN_EOF)) {
        declaration(cs);
    }
    consume(cs, TOKEN_RIGHT_BRACE, "Expect '}' after block.");
}

static void var_declaration(CompilerState* cs, DataType type, Token name) {
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
        DataType valType = expression(cs);
        check_type_match(cs, type, valType, "Type mismatch in variable initialization");
    } else {
        if (type == TYPE_INT) emit_constant(cs, INT_VAL(0));
        else if (is_float_type(type)) emit_constant(cs, FLOAT_VAL(0.0));
        else if (type == TYPE_BOOL) emit_byte(cs, OP_FALSE);
        else emit_byte(cs, OP_NIL);
    }
    consume(cs, TOKEN_SEMICOLON, "Expect ';' after variable declaration.");

    if (cs->current->scopeDepth > 0) {
        cs->current->locals[cs->current->localCount - 1].depth = cs->current->scopeDepth;
    } else {
        uint8_t global = identifier_constant(cs, &name);
        add_global_symbol(cs, name, type, 0, NULL, TYPE_VOID);
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
        check_type_match(cs, cs->current->function->returnType, TYPE_VOID, "Return value required");
        emit_return(cs);
    } else {
        DataType type = expression(cs);
        check_type_match(cs, cs->current->function->returnType, type, "Return type mismatch");
        consume(cs, TOKEN_SEMICOLON, "Expect ';' after return value.");
        emit_byte(cs, OP_RETURN);
    }
}

static void if_statement(CompilerState* cs) {
    bool hasParen = match(cs, TOKEN_LEFT_PAREN);
    DataType condType = expression(cs);
    check_type_match(cs, TYPE_BOOL, condType, "Condition must be bool");
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
    DataType condType = expression(cs);
    check_type_match(cs, TYPE_BOOL, condType, "Condition must be bool");
    if (hasParen) consume(cs, TOKEN_RIGHT_PAREN, "Expect ')' after condition.");

    int exitJump = emit_jump(cs, OP_JUMP_IF_FALSE);
    emit_byte(cs, OP_POP);
    statement(cs);
    emit_loop(cs, loopStart);

    patch_jump(cs, exitJump);
    emit_byte(cs, OP_POP);
}

static void for_statement(CompilerState* cs) {
    begin_scope(cs);
    consume(cs, TOKEN_LEFT_PAREN, "Expect '(' after 'for'.");

    if (match(cs, TOKEN_SEMICOLON)) {
    } else if (match(cs, TOKEN_KW_INT)) {
        consume(cs, TOKEN_IDENTIFIER, "Expect variable name.");
        var_declaration(cs, TYPE_INT, cs->parser.previous);
    } else {
        expression(cs);
        consume(cs, TOKEN_SEMICOLON, "Expect ';'.");
        emit_byte(cs, OP_POP);
    }

    int loopStart = current_chunk(cs)->count;
    int exitJump = -1;
    if (!match(cs, TOKEN_SEMICOLON)) {
        DataType condType = expression(cs);
        check_type_match(cs, TYPE_BOOL, condType, "For condition must be bool");
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

    statement(cs);
    emit_loop(cs, loopStart);

    if (exitJump != -1) {
        patch_jump(cs, exitJump);
        emit_byte(cs, OP_POP);
    }

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
    } else if (match(cs, TOKEN_LEFT_BRACE)) {
        begin_scope(cs);
        block(cs);
        end_scope(cs);
    } else {
        expression_statement(cs);
    }
}

static void function_declaration(CompilerState* cs, DataType retType, Token name) {
    uint8_t global = identifier_constant(cs, &name);

    Compiler compiler;
    init_compiler(cs, &compiler, TYPE_FUNCTION, retType);
    compiler.function->name = cd_copy_string(cs->vm, name.start, name.length);

    begin_scope(cs);
    consume(cs, TOKEN_LEFT_PAREN, "Expect '(' after function name.");

    DataType paramTypes[16];
    int arity = 0;

    if (!check(cs, TOKEN_RIGHT_PAREN)) {
        do {
            TokenType pt = cs->parser.current.type;
            advance(cs);
            DataType ptype = type_from_token(cs, pt);
            consume(cs, TOKEN_IDENTIFIER, "Expect parameter name.");
            Token pname = cs->parser.previous;

            if (arity == 16) {
                error_at_current(cs, "Can't have more than 16 parameters.");
            } else {
                paramTypes[arity] = ptype;
                cs->current->function->paramTypes[arity] = ptype;
                cs->current->function->arity++;
                arity++;
                add_local(cs, pname, ptype);
                cs->current->locals[cs->current->localCount - 1].depth = cs->current->scopeDepth;
            }
        } while (match(cs, TOKEN_COMMA));
    }
    consume(cs, TOKEN_RIGHT_PAREN, "Expect ')' after parameters.");

    add_global_symbol(cs, name, TYPE_ANY, arity, paramTypes, retType);

    if (match(cs, TOKEN_ARROW)) {
        DataType bodyType = expression(cs);
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

static void class_declaration(CompilerState* cs) {
    consume(cs, TOKEN_IDENTIFIER, "Expect class name.");
    Token className = cs->parser.previous;
    uint8_t nameConst = identifier_constant(cs, &className);
    add_global_symbol(cs, className, TYPE_CLASS, 0, NULL, TYPE_CLASS);

    emit_bytes(cs, OP_CLASS, nameConst);
    emit_bytes(cs, OP_DEFINE_GLOBAL, nameConst);

    consume(cs, TOKEN_LEFT_BRACE, "Expect '{' before class body.");

    while (!check(cs, TOKEN_RIGHT_BRACE) && !check(cs, TOKEN_EOF)) {
        TokenType t = cs->parser.current.type;
        DataType retType = TYPE_VOID;
        if (t == TOKEN_KW_INT || t == TOKEN_KW_FLOAT || t == TOKEN_KW_DOUBLE ||
            t == TOKEN_KW_BOOL || t == TOKEN_KW_STRING || t == TOKEN_KW_VOID ||
            t == TOKEN_KW_LIST || t == TOKEN_KW_MAP) {
            advance(cs);
            retType = type_from_token(cs, t);
        }
        consume(cs, TOKEN_IDENTIFIER, "Expect member name.");
        Token memberName = cs->parser.previous;

        if (check(cs, TOKEN_LEFT_PAREN)) {
            emit_bytes(cs, OP_GET_GLOBAL, nameConst);

            Compiler compiler;
            init_compiler(cs, &compiler, TYPE_METHOD, retType);
            cs->current->locals[0].name.start = "this";
            cs->current->locals[0].name.length = 4;
            cs->current->locals[0].type = TYPE_CLASS;
            compiler.function->name = cd_copy_string(cs->vm, memberName.start, memberName.length);
            if (memberName.length == 4 && memcmp(memberName.start, "init", 4) == 0) {
                compiler.function->isInitializer = true;
            }

            begin_scope(cs);
            consume(cs, TOKEN_LEFT_PAREN, "Expect '(' after method name.");
            int arity = 0;
            if (!check(cs, TOKEN_RIGHT_PAREN)) {
                do {
                    TokenType pt = cs->parser.current.type;
                    advance(cs);
                    DataType ptype = type_from_token(cs, pt);
                    consume(cs, TOKEN_IDENTIFIER, "Expect parameter name.");
                    Token pname = cs->parser.previous;
                    if (arity == 16) {
                        error_at_current(cs, "Can't have more than 16 parameters.");
                    } else {
                        compiler.function->paramTypes[arity] = ptype;
                        compiler.function->arity++;
                        arity++;
                        add_local(cs, pname, ptype);
                        cs->current->locals[cs->current->localCount - 1].depth = cs->current->scopeDepth;
                    }
                } while (match(cs, TOKEN_COMMA));
            }
            consume(cs, TOKEN_RIGHT_PAREN, "Expect ')' after parameters.");

            if (match(cs, TOKEN_ARROW)) {
                expression(cs);
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
            uint8_t fieldConst = identifier_constant(cs, &memberName);
            emit_bytes(cs, OP_GET_GLOBAL, nameConst);
            if (match(cs, TOKEN_EQUAL)) {
                DataType valType = expression(cs);
                check_type_match(cs, retType, valType, "Type mismatch in field initializer");
            } else {
                if (retType == TYPE_INT) emit_constant(cs, INT_VAL(0));
                else if (is_float_type(retType)) emit_constant(cs, FLOAT_VAL(0.0));
                else if (retType == TYPE_BOOL) emit_byte(cs, OP_FALSE);
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
    if (t == TOKEN_KW_INT || t == TOKEN_KW_FLOAT || t == TOKEN_KW_DOUBLE ||
        t == TOKEN_KW_BOOL || t == TOKEN_KW_STRING || t == TOKEN_KW_VOID ||
        t == TOKEN_KW_LIST || t == TOKEN_KW_MAP) {
        advance(cs);
        DataType dt = type_from_token(cs, t);
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
            advance(cs);
            advance(cs);
            Token name = cs->parser.previous;
            if (check(cs, TOKEN_LEFT_PAREN)) {
                function_declaration(cs, TYPE_ANY, name);
                return;
            }
            var_declaration(cs, TYPE_ANY, name);
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
    init_compiler(&cs, &compiler, TYPE_SCRIPT, TYPE_VOID);

    advance(&cs);
    while (!match(&cs, TOKEN_EOF)) {
        declaration(&cs);
    }

    ObjFunction* function = end_compiler(&cs);
    return cs.parser.hadError ? NULL : function;
}
