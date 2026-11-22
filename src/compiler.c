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
    TYPE_FUNCTION
} FunctionType;

typedef struct {
    Token name;
    DataType type;
    int arity;
    DataType paramTypes[16];
    DataType returnType;
} GlobalSymbol;

typedef struct Compiler {
    struct Compiler* enclosing;
    ObjFunction* function;
    FunctionType type;
    Local locals[256];
    int localCount;
    int scopeDepth;
} Compiler;

typedef struct {
    GlobalSymbol symbols[256];
    int count;
} GlobalTable;

static Parser parser;
static Lexer lexer;
static cd_State* currentVm;
static Compiler* current = NULL;
static GlobalTable globals;

static void error_at(Token* token, const char* message) {
    if (parser.panicMode) return;
    parser.panicMode = true;
    fprintf(stderr, "[line %d] Error", token->line);

    if (token->type == TOKEN_EOF) {
        fprintf(stderr, " at end");
    } else if (token->type != TOKEN_ERROR) {
        fprintf(stderr, " at '%.*s'", token->length, token->start);
    }

    fprintf(stderr, ": %s\n", message);
    parser.hadError = true;
}

static void error(const char* message) {
    error_at(&parser.previous, message);
}

static void error_at_current(const char* message) {
    error_at(&parser.current, message);
}

static void advance(void) {
    parser.previous = parser.current;
    for (;;) {
        parser.current = lexer_next(&lexer);
        if (parser.current.type != TOKEN_ERROR) break;
        error_at_current(parser.current.start);
    }
}

static void consume(TokenType type, const char* message) {
    if (parser.current.type == type) {
        advance();
        return;
    }
    error_at_current(message);
}

static bool check(TokenType type) {
    return parser.current.type == type;
}

static bool match(TokenType type) {
    if (!check(type)) return false;
    advance();
    return true;
}

static Chunk* current_chunk(void) {
    return &current->function->chunk;
}

static void emit_byte(uint8_t byte) {
    chunk_write(current_chunk(), byte, parser.previous.line);
}

static void emit_bytes(uint8_t byte1, uint8_t byte2) {
    emit_byte(byte1);
    emit_byte(byte2);
}

static void emit_loop(int loopStart) {
    emit_byte(OP_LOOP);
    int offset = current_chunk()->count - loopStart + 2;
    if (offset > 65535) error("Loop body too large.");
    emit_byte((offset >> 8) & 0xff);
    emit_byte(offset & 0xff);
}

static int emit_jump(uint8_t instruction) {
    emit_byte(instruction);
    emit_byte(0xff);
    emit_byte(0xff);
    return current_chunk()->count - 2;
}

static void patch_jump(int offset) {
    int jump = current_chunk()->count - offset - 2;
    if (jump > 65535) {
        error("Too much code to jump over.");
    }
    current_chunk()->code[offset] = (jump >> 8) & 0xff;
    current_chunk()->code[offset + 1] = jump & 0xff;
}

static void emit_return(void) {
    emit_byte(OP_NIL);
    emit_byte(OP_RETURN);
}

static uint8_t make_constant(Value value) {
    int constant = chunk_add_constant(current_chunk(), value);
    if (constant > 255) {
        error("Too many constants in one chunk.");
        return 0;
    }
    return (uint8_t)constant;
}

static void emit_constant(Value value) {
    emit_bytes(OP_CONSTANT, make_constant(value));
}

static void init_compiler(Compiler* compiler, FunctionType type, DataType retType) {
    compiler->enclosing = current;
    compiler->function = NULL;
    compiler->type = type;
    compiler->localCount = 0;
    compiler->scopeDepth = 0;
    compiler->function = cd_new_function(currentVm);
    compiler->function->returnType = retType;
    current = compiler;

    Local* local = &current->locals[current->localCount++];
    local->depth = 0;
    local->name.start = "";
    local->name.length = 0;
    local->type = TYPE_VOID;
}

static ObjFunction* end_compiler(void) {
    emit_return();
    ObjFunction* function = current->function;
    current = current->enclosing;
    return function;
}

static void begin_scope(void) {
    current->scopeDepth++;
}

static void end_scope(void) {
    current->scopeDepth--;
    while (current->localCount > 0 &&
           current->locals[current->localCount - 1].depth > current->scopeDepth) {
        emit_byte(OP_POP);
        current->localCount--;
    }
}

static bool identifiers_equal(Token* a, Token* b) {
    if (a->length != b->length) return false;
    return memcmp(a->start, b->start, a->length) == 0;
}

static int resolve_local(Compiler* compiler, Token* name) {
    for (int i = compiler->localCount - 1; i >= 0; i--) {
        Local* local = &compiler->locals[i];
        if (identifiers_equal(name, &local->name)) {
            if (local->depth == -1) {
                error("Can't read local variable in its own initializer.");
            }
            return i;
        }
    }
    return -1;
}

static void add_local(Token name, DataType type) {
    if (current->localCount == 256) {
        error("Too many local variables in function.");
        return;
    }
    Local* local = &current->locals[current->localCount++];
    local->name = name;
    local->depth = -1;
    local->type = type;
}

static uint8_t identifier_constant(Token* name) {
    return make_constant(OBJ_VAL(cd_copy_string(currentVm, name->start, name->length)));
}

static GlobalSymbol* find_global_symbol(Token* name) {
    for (int i = 0; i < globals.count; i++) {
        if (identifiers_equal(name, &globals.symbols[i].name)) {
            return &globals.symbols[i];
        }
    }
    return NULL;
}

static void add_global_symbol(Token name, DataType type, int arity, DataType* params, DataType ret) {
    GlobalSymbol* sym = find_global_symbol(&name);
    if (sym == NULL) {
        sym = &globals.symbols[globals.count++];
    }
    sym->name = name;
    sym->type = type;
    sym->arity = arity;
    sym->returnType = ret;
    if (params) {
        for (int i = 0; i < arity; i++) sym->paramTypes[i] = params[i];
    }
}

static DataType expression(void);
static void statement(void);
static void declaration(void);
static DataType parse_precedence(Precedence precedence);

static DataType type_from_token(TokenType type) {
    switch (type) {
        case TOKEN_KW_INT: return TYPE_INT;
        case TOKEN_KW_BOOL: return TYPE_BOOL;
        case TOKEN_KW_STRING: return TYPE_STRING;
        case TOKEN_KW_VOID: return TYPE_VOID;
        default: return TYPE_ANY;
    }
}

static const char* type_name(DataType type) {
    switch (type) {
        case TYPE_VOID: return "void";
        case TYPE_INT: return "int";
        case TYPE_BOOL: return "bool";
        case TYPE_STRING: return "string";
        case TYPE_ANY: return "any";
    }
    return "unknown";
}

static void check_type_match(DataType expected, DataType actual, const char* msg) {
    if (expected == TYPE_ANY || actual == TYPE_ANY) return;
    if (expected != actual) {
        char buffer[128];
        snprintf(buffer, sizeof(buffer), "%s (expected %s, got %s)", msg, type_name(expected), type_name(actual));
        error(buffer);
    }
}

static DataType parse_variable(Token name, bool canAssign) {
    int arg = resolve_local(current, &name);
    DataType varType = TYPE_ANY;
    uint8_t getOp, setOp;

    if (arg != -1) {
        getOp = OP_GET_LOCAL;
        setOp = OP_SET_LOCAL;
        varType = current->locals[arg].type;
    } else {
        arg = identifier_constant(&name);
        getOp = OP_GET_GLOBAL;
        setOp = OP_SET_GLOBAL;
        GlobalSymbol* sym = find_global_symbol(&name);
        if (sym != NULL) varType = sym->type;
    }

    if (canAssign && match(TOKEN_EQUAL)) {
        DataType valType = expression();
        check_type_match(varType, valType, "Type mismatch in assignment");
        emit_bytes(setOp, (uint8_t)arg);
        return valType;
    } else if (canAssign && match(TOKEN_PLUS_EQUAL)) {
        emit_bytes(getOp, (uint8_t)arg);
        DataType valType = expression();
        if (varType == TYPE_INT && valType == TYPE_INT) {
            emit_byte(OP_ADD);
        } else if (varType == TYPE_STRING && valType == TYPE_STRING) {
            emit_byte(OP_ADD);
        } else {
            error("Invalid operands for '+='.");
        }
        emit_bytes(setOp, (uint8_t)arg);
        return varType;
    } else if (canAssign && match(TOKEN_MINUS_EQUAL)) {
        emit_bytes(getOp, (uint8_t)arg);
        DataType valType = expression();
        check_type_match(TYPE_INT, varType, "'-=' requires int variable");
        check_type_match(TYPE_INT, valType, "'-=' requires int value");
        emit_byte(OP_SUB);
        emit_bytes(setOp, (uint8_t)arg);
        return TYPE_INT;
    }

    emit_bytes(getOp, (uint8_t)arg);
    return varType;
}

static DataType call_expr(Token calleeName) {
    int argCount = 0;
    DataType argTypes[16];

    if (!check(TOKEN_RIGHT_PAREN)) {
        do {
            if (argCount == 16) error("Can't have more than 16 arguments.");
            argTypes[argCount++] = expression();
        } while (match(TOKEN_COMMA));
    }
    consume(TOKEN_RIGHT_PAREN, "Expect ')' after arguments.");

    GlobalSymbol* sym = find_global_symbol(&calleeName);
    DataType retType = TYPE_ANY;

    if (sym != NULL) {
        if (sym->arity != argCount) {
            error("Incorrect argument count for function.");
        }
        for (int i = 0; i < argCount; i++) {
            check_type_match(sym->paramTypes[i], argTypes[i], "Argument type mismatch");
        }
        retType = sym->returnType;
    }

    emit_bytes(OP_CALL, (uint8_t)argCount);
    return retType;
}

static DataType primary(bool canAssign) {
    if (match(TOKEN_KW_TRUE)) {
        emit_byte(OP_TRUE);
        return TYPE_BOOL;
    }
    if (match(TOKEN_KW_FALSE)) {
        emit_byte(OP_FALSE);
        return TYPE_BOOL;
    }
    if (match(TOKEN_KW_NULL)) {
        emit_byte(OP_NIL);
        return TYPE_ANY;
    }
    if (match(TOKEN_INT)) {
        int64_t value = strtoll(parser.previous.start, NULL, 10);
        emit_constant(INT_VAL(value));
        return TYPE_INT;
    }
    if (match(TOKEN_STRING)) {
        const char* s = parser.previous.start + 1;
        int len = parser.previous.length - 2;
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
        ObjString* str = cd_take_string(currentVm, buf, j);
        emit_constant(OBJ_VAL(str));
        return TYPE_STRING;
    }
    if (match(TOKEN_LEFT_PAREN)) {
        DataType type = expression();
        consume(TOKEN_RIGHT_PAREN, "Expect ')' after expression.");
        return type;
    }
    if (match(TOKEN_MINUS)) {
        DataType type = parse_precedence(PREC_UNARY);
        check_type_match(TYPE_INT, type, "Operand of '-' must be int");
        emit_byte(OP_NEGATE);
        return TYPE_INT;
    }
    if (match(TOKEN_BANG)) {
        DataType type = parse_precedence(PREC_UNARY);
        check_type_match(TYPE_BOOL, type, "Operand of '!' must be bool");
        emit_byte(OP_NOT);
        return TYPE_BOOL;
    }
    if (match(TOKEN_IDENTIFIER)) {
        Token name = parser.previous;
        if (match(TOKEN_LEFT_PAREN)) {
            uint8_t arg = identifier_constant(&name);
            emit_bytes(OP_GET_GLOBAL, arg);
            return call_expr(name);
        }
        return parse_variable(name, canAssign);
    }

    error("Expect expression.");
    advance();
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

static DataType parse_precedence(Precedence precedence) {
    bool canAssign = precedence <= PREC_ASSIGNMENT;
    DataType leftType = primary(canAssign);

    while (precedence <= get_precedence(parser.current.type)) {
        advance();
        TokenType op = parser.previous.type;
        Precedence nextPrec = (Precedence)(get_precedence(op) + 1);

        if (op == TOKEN_AND) {
            check_type_match(TYPE_BOOL, leftType, "Left operand of '&&' must be bool");
            int endJump = emit_jump(OP_JUMP_IF_FALSE);
            emit_byte(OP_POP);
            DataType rightType = parse_precedence(PREC_AND);
            check_type_match(TYPE_BOOL, rightType, "Right operand of '&&' must be bool");
            patch_jump(endJump);
            leftType = TYPE_BOOL;
            continue;
        }

        if (op == TOKEN_OR) {
            check_type_match(TYPE_BOOL, leftType, "Left operand of '||' must be bool");
            int elseJump = emit_jump(OP_JUMP_IF_FALSE);
            int endJump = emit_jump(OP_JUMP);
            patch_jump(elseJump);
            emit_byte(OP_POP);
            DataType rightType = parse_precedence(PREC_OR);
            check_type_match(TYPE_BOOL, rightType, "Right operand of '||' must be bool");
            patch_jump(endJump);
            leftType = TYPE_BOOL;
            continue;
        }

        DataType rightType = parse_precedence(nextPrec);

        switch (op) {
            case TOKEN_PLUS:
                if (leftType == TYPE_INT && rightType == TYPE_INT) {
                    emit_byte(OP_ADD);
                    leftType = TYPE_INT;
                } else if (leftType == TYPE_STRING && rightType == TYPE_STRING) {
                    emit_byte(OP_ADD);
                    leftType = TYPE_STRING;
                } else {
                    error("Operands of '+' must be both ints or both strings.");
                }
                break;
            case TOKEN_MINUS:
                check_type_match(TYPE_INT, leftType, "Left operand of '-' must be int");
                check_type_match(TYPE_INT, rightType, "Right operand of '-' must be int");
                emit_byte(OP_SUB);
                leftType = TYPE_INT;
                break;
            case TOKEN_STAR:
                check_type_match(TYPE_INT, leftType, "Left operand of '*' must be int");
                check_type_match(TYPE_INT, rightType, "Right operand of '*' must be int");
                emit_byte(OP_MUL);
                leftType = TYPE_INT;
                break;
            case TOKEN_SLASH:
                check_type_match(TYPE_INT, leftType, "Left operand of '/' must be int");
                check_type_match(TYPE_INT, rightType, "Right operand of '/' must be int");
                emit_byte(OP_DIV);
                leftType = TYPE_INT;
                break;
            case TOKEN_PERCENT:
                check_type_match(TYPE_INT, leftType, "Left operand of '%' must be int");
                check_type_match(TYPE_INT, rightType, "Right operand of '%' must be int");
                emit_byte(OP_MOD);
                leftType = TYPE_INT;
                break;
            case TOKEN_EQUAL_EQUAL:
                check_type_match(leftType, rightType, "Operands of '==' must have matching types");
                emit_byte(OP_EQUAL);
                leftType = TYPE_BOOL;
                break;
            case TOKEN_BANG_EQUAL:
                check_type_match(leftType, rightType, "Operands of '!=' must have matching types");
                emit_bytes(OP_EQUAL, OP_NOT);
                leftType = TYPE_BOOL;
                break;
            case TOKEN_GREATER:
                check_type_match(TYPE_INT, leftType, "Left operand of '>' must be int");
                check_type_match(TYPE_INT, rightType, "Right operand of '>' must be int");
                emit_byte(OP_GREATER);
                leftType = TYPE_BOOL;
                break;
            case TOKEN_GREATER_EQUAL:
                check_type_match(TYPE_INT, leftType, "Left operand of '>=' must be int");
                check_type_match(TYPE_INT, rightType, "Right operand of '>=' must be int");
                emit_bytes(OP_LESS, OP_NOT);
                leftType = TYPE_BOOL;
                break;
            case TOKEN_LESS:
                check_type_match(TYPE_INT, leftType, "Left operand of '<' must be int");
                check_type_match(TYPE_INT, rightType, "Right operand of '<' must be int");
                emit_byte(OP_LESS);
                leftType = TYPE_BOOL;
                break;
            case TOKEN_LESS_EQUAL:
                check_type_match(TYPE_INT, leftType, "Left operand of '<=' must be int");
                check_type_match(TYPE_INT, rightType, "Right operand of '<=' must be int");
                emit_bytes(OP_GREATER, OP_NOT);
                leftType = TYPE_BOOL;
                break;
            default:
                return leftType;
        }
    }

    return leftType;
}

static DataType expression(void) {
    return parse_precedence(PREC_ASSIGNMENT);
}

static void block(void) {
    while (!check(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF)) {
        declaration();
    }
    consume(TOKEN_RIGHT_BRACE, "Expect '}' after block.");
}

static void var_declaration(DataType type, Token name) {
    if (current->scopeDepth > 0) {
        for (int i = current->localCount - 1; i >= 0; i--) {
            Local* local = &current->locals[i];
            if (local->depth != -1 && local->depth < current->scopeDepth) break;
            if (identifiers_equal(&name, &local->name)) {
                error("Already a variable with this name in this scope.");
            }
        }
        add_local(name, type);
    }

    if (match(TOKEN_EQUAL)) {
        DataType valType = expression();
        check_type_match(type, valType, "Type mismatch in variable initialization");
    } else {
        if (type == TYPE_INT) emit_constant(INT_VAL(0));
        else if (type == TYPE_BOOL) emit_byte(OP_FALSE);
        else emit_byte(OP_NIL);
    }
    consume(TOKEN_SEMICOLON, "Expect ';' after variable declaration.");

    if (current->scopeDepth > 0) {
        current->locals[current->localCount - 1].depth = current->scopeDepth;
    } else {
        uint8_t global = identifier_constant(&name);
        add_global_symbol(name, type, 0, NULL, TYPE_VOID);
        emit_bytes(OP_DEFINE_GLOBAL, global);
    }
}

static void print_statement(void) {
    consume(TOKEN_LEFT_PAREN, "Expect '(' after print.");
    expression();
    consume(TOKEN_RIGHT_PAREN, "Expect ')' after value.");
    consume(TOKEN_SEMICOLON, "Expect ';' after statement.");
    emit_byte(OP_PRINT);
}

static void return_statement(void) {
    if (current->type == TYPE_SCRIPT) {
        error("Can't return from top-level code.");
    }

    if (match(TOKEN_SEMICOLON)) {
        check_type_match(current->function->returnType, TYPE_VOID, "Return value required");
        emit_return();
    } else {
        DataType type = expression();
        check_type_match(current->function->returnType, type, "Return type mismatch");
        consume(TOKEN_SEMICOLON, "Expect ';' after return value.");
        emit_byte(OP_RETURN);
    }
}

static void if_statement(void) {
    bool hasParen = match(TOKEN_LEFT_PAREN);
    DataType condType = expression();
    check_type_match(TYPE_BOOL, condType, "Condition must be bool");
    if (hasParen) consume(TOKEN_RIGHT_PAREN, "Expect ')' after condition.");

    int thenJump = emit_jump(OP_JUMP_IF_FALSE);
    emit_byte(OP_POP);
    statement();

    int elseJump = emit_jump(OP_JUMP);
    patch_jump(thenJump);
    emit_byte(OP_POP);

    if (match(TOKEN_KW_ELSE)) statement();
    patch_jump(elseJump);
}

static void while_statement(void) {
    int loopStart = current_chunk()->count;
    bool hasParen = match(TOKEN_LEFT_PAREN);
    DataType condType = expression();
    check_type_match(TYPE_BOOL, condType, "Condition must be bool");
    if (hasParen) consume(TOKEN_RIGHT_PAREN, "Expect ')' after condition.");

    int exitJump = emit_jump(OP_JUMP_IF_FALSE);
    emit_byte(OP_POP);
    statement();
    emit_loop(loopStart);

    patch_jump(exitJump);
    emit_byte(OP_POP);
}

static void for_statement(void) {
    begin_scope();
    consume(TOKEN_LEFT_PAREN, "Expect '(' after 'for'.");

    if (match(TOKEN_SEMICOLON)) {
    } else if (match(TOKEN_KW_INT)) {
        consume(TOKEN_IDENTIFIER, "Expect variable name.");
        var_declaration(TYPE_INT, parser.previous);
    } else {
        expression();
        consume(TOKEN_SEMICOLON, "Expect ';'.");
        emit_byte(OP_POP);
    }

    int loopStart = current_chunk()->count;
    int exitJump = -1;
    if (!match(TOKEN_SEMICOLON)) {
        DataType condType = expression();
        check_type_match(TYPE_BOOL, condType, "For condition must be bool");
        consume(TOKEN_SEMICOLON, "Expect ';' after loop condition.");

        exitJump = emit_jump(OP_JUMP_IF_FALSE);
        emit_byte(OP_POP);
    }

    if (!match(TOKEN_RIGHT_PAREN)) {
        int bodyJump = emit_jump(OP_JUMP);
        int incrementStart = current_chunk()->count;
        expression();
        emit_byte(OP_POP);
        consume(TOKEN_RIGHT_PAREN, "Expect ')' after for clauses.");

        emit_loop(loopStart);
        loopStart = incrementStart;
        patch_jump(bodyJump);
    }

    statement();
    emit_loop(loopStart);

    if (exitJump != -1) {
        patch_jump(exitJump);
        emit_byte(OP_POP);
    }

    end_scope();
}

static void expression_statement(void) {
    expression();
    consume(TOKEN_SEMICOLON, "Expect ';' after expression.");
    emit_byte(OP_POP);
}

static void statement(void) {
    if (match(TOKEN_KW_PRINT)) {
        print_statement();
    } else if (match(TOKEN_KW_IF)) {
        if_statement();
    } else if (match(TOKEN_KW_RETURN)) {
        return_statement();
    } else if (match(TOKEN_KW_WHILE)) {
        while_statement();
    } else if (match(TOKEN_KW_FOR)) {
        for_statement();
    } else if (match(TOKEN_LEFT_BRACE)) {
        begin_scope();
        block();
        end_scope();
    } else {
        expression_statement();
    }
}

static void function_declaration(DataType retType, Token name) {
    uint8_t global = identifier_constant(&name);

    Compiler compiler;
    init_compiler(&compiler, TYPE_FUNCTION, retType);
    compiler.function->name = cd_copy_string(currentVm, name.start, name.length);

    begin_scope();
    consume(TOKEN_LEFT_PAREN, "Expect '(' after function name.");

    DataType paramTypes[16];
    int arity = 0;

    if (!check(TOKEN_RIGHT_PAREN)) {
        do {
            if (current->function->arity == 16) {
                error_at_current("Can't have more than 16 parameters.");
            }
            TokenType pt = parser.current.type;
            if (pt != TOKEN_KW_INT && pt != TOKEN_KW_BOOL && pt != TOKEN_KW_STRING) {
                error_at_current("Expect parameter type.");
            }
            advance();
            DataType ptype = type_from_token(pt);
            consume(TOKEN_IDENTIFIER, "Expect parameter name.");
            Token pname = parser.previous;

            paramTypes[arity] = ptype;
            current->function->paramTypes[arity] = ptype;
            current->function->arity++;
            arity++;

            add_local(pname, ptype);
            current->locals[current->localCount - 1].depth = current->scopeDepth;
        } while (match(TOKEN_COMMA));
    }
    consume(TOKEN_RIGHT_PAREN, "Expect ')' after parameters.");

    add_global_symbol(name, TYPE_ANY, arity, paramTypes, retType);

    if (match(TOKEN_ARROW)) {
        DataType bodyType = expression();
        check_type_match(retType, bodyType, "Return type mismatch in arrow function");
        consume(TOKEN_SEMICOLON, "Expect ';' after arrow function body.");
        emit_byte(OP_RETURN);
    } else {
        consume(TOKEN_LEFT_BRACE, "Expect '{' before function body.");
        block();
    }

    ObjFunction* function = end_compiler();
    emit_bytes(OP_CONSTANT, make_constant(OBJ_VAL(function)));
    emit_bytes(OP_DEFINE_GLOBAL, global);
}

static void declaration(void) {
    TokenType t = parser.current.type;
    if (t == TOKEN_KW_INT || t == TOKEN_KW_BOOL || t == TOKEN_KW_STRING || t == TOKEN_KW_VOID) {
        advance();
        DataType dt = type_from_token(t);
        consume(TOKEN_IDENTIFIER, "Expect identifier.");
        Token name = parser.previous;
        if (check(TOKEN_LEFT_PAREN)) {
            function_declaration(dt, name);
            return;
        }
        var_declaration(dt, name);
        return;
    }
    statement();
}

ObjFunction* cd_compile(cd_State* cd, const char* source) {
    currentVm = cd;
    lexer_init(&lexer, source);
    parser.hadError = false;
    parser.panicMode = false;
    globals.count = 0;

    Compiler compiler;
    init_compiler(&compiler, TYPE_SCRIPT, TYPE_VOID);

    advance();
    while (!match(TOKEN_EOF)) {
        declaration();
    }

    ObjFunction* function = end_compiler();
    return parser.hadError ? NULL : function;
}
