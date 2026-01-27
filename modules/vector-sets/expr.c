/* Filtering of objects based on simple expressions.
 * This powers the FILTER option of Vector Sets, but it is otherwise
 * general code to be used when we want to tell if a given object (with fields)
 * passes or fails a given test for scalars, strings, ...
 *
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 * Originally authored by: Salvatore Sanfilippo.
 */

#ifdef TEST_MAIN
#define RedisModule_Alloc malloc
#define RedisModule_Realloc realloc
#define RedisModule_Free free
#define RedisModule_Strdup strdup
#define RedisModule_Assert assert
#define _DEFAULT_SOURCE
#define _USE_MATH_DEFINES
#include <assert.h>
#include <math.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <math.h>
#include <string.h>

#define EXPR_TOKEN_EOF 0
#define EXPR_TOKEN_NUM 1
#define EXPR_TOKEN_STR 2
#define EXPR_TOKEN_TUPLE 3
#define EXPR_TOKEN_SELECTOR 4
#define EXPR_TOKEN_OP 5
#define EXPR_TOKEN_NULL 6

#define EXPR_OP_OPAREN 0  /* ( */
#define EXPR_OP_CPAREN 1  /* ) */
#define EXPR_OP_NOT    2  /* ! */
#define EXPR_OP_POW    3  /* ** */
#define EXPR_OP_MULT   4  /* * */
#define EXPR_OP_DIV    5  /* / */
#define EXPR_OP_MOD    6  /* % */
#define EXPR_OP_SUM    7  /* + */
#define EXPR_OP_DIFF   8  /* - */
#define EXPR_OP_GT     9  /* > */
#define EXPR_OP_GTE    10 /* >= */
#define EXPR_OP_LT     11 /* < */
#define EXPR_OP_LTE    12 /* <= */
#define EXPR_OP_EQ     13 /* == */
#define EXPR_OP_NEQ    14 /* != */
#define EXPR_OP_IN     15 /* in */
#define EXPR_OP_AND    16 /* and */
#define EXPR_OP_OR     17 /* or */

/* This structure represents a token in our expression. It's either
 * literals like 4, "foo", or operators like "+", "-", "and", or
 * json selectors, that start with a dot: ".age", ".properties.somearray[1]" */
typedef struct exprtoken {
    int refcount;           // Reference counting for memory reclaiming.
    int token_type;         // Token type of the just parsed token.
    int offset;             // Chars offset in expression.
    union {
        double num;         // Value for EXPR_TOKEN_NUM.
        struct {
            char *start;    // String pointer for EXPR_TOKEN_STR / SELECTOR.
            size_t len;     // String len for EXPR_TOKEN_STR / SELECTOR.
            char *heapstr;  // True if we have a private allocation for this
                            // string. When possible, it just references to the
                            // string expression we compiled, exprstate->expr.
        } str;
        int opcode;         // Opcode ID for EXPR_TOKEN_OP.
        struct {
            struct exprtoken **ele;
            size_t len;
        } tuple;            // Tuples are like [1, 2, 3] for "in" operator.
    };
} exprtoken;

/* Simple stack of expr tokens. This is used both to represent the stack
 * of values and the stack of operands during VM execution. */
typedef struct exprstack {
    exprtoken **items;
    int numitems;
    int allocsize;
} exprstack;

typedef struct exprstate {
    char *expr;             /* Expression string to compile. Note that
                             * expression token strings point directly to this
                             * string. */
    char *p;                // Current position inside 'expr', while parsing.

    // Virtual machine state.
    exprstack values_stack;
    exprstack ops_stack;    // Operator stack used during compilation.
    exprstack tokens;       // Expression processed into a sequence of tokens.
    exprstack program;      // Expression compiled into opcodes and values.
} exprstate;

/* Valid operators. */
struct {
    char *opname;
    int oplen;
    int opcode;
    int precedence;
    int arity;
} ExprOptable[] = {
    {"(",   1,  EXPR_OP_OPAREN,  7, 0},
    {")",   1,  EXPR_OP_CPAREN,  7, 0},
    {"!",   1,  EXPR_OP_NOT,     6, 1},
    {"not", 3,  EXPR_OP_NOT,     6, 1},
    {"**",  2,  EXPR_OP_POW,     5, 2},
    {"*",   1,  EXPR_OP_MULT,    4, 2},
    {"/",   1,  EXPR_OP_DIV,     4, 2},
    {"%",   1,  EXPR_OP_MOD,     4, 2},
    {"+",   1,  EXPR_OP_SUM,     3, 2},
    {"-",   1,  EXPR_OP_DIFF,    3, 2},
    {">",   1,  EXPR_OP_GT,      2, 2},
    {">=",  2,  EXPR_OP_GTE,     2, 2},
    {"<",   1,  EXPR_OP_LT,      2, 2},
    {"<=",  2,  EXPR_OP_LTE,     2, 2},
    {"==",  2,  EXPR_OP_EQ,      2, 2},
    {"!=",  2,  EXPR_OP_NEQ,     2, 2},
    {"in",  2,  EXPR_OP_IN,      2, 2},
    {"and", 3,  EXPR_OP_AND,     1, 2},
    {"&&",  2,  EXPR_OP_AND,     1, 2},
    {"or",  2,  EXPR_OP_OR,      0, 2},
    {"||",  2,  EXPR_OP_OR,      0, 2},
    {NULL,  0,  0,               0, 0}   // Terminator.
};

#define EXPR_OP_SPECIALCHARS "+-*%/!()<>=|&"
#define EXPR_SELECTOR_SPECIALCHARS "_-"

/* ================================ Expr token ============================== */

/* Return an heap allocated token of the specified type, setting the
 * reference count to 1. */
exprtoken *exprNewToken(int type) {
    exprtoken *t = RedisModule_Alloc(sizeof(exprtoken));
    memset(t,0,sizeof(*t));
    t->token_type = type;
    t->refcount = 1;
    return t;
}

/* Generic free token function, can be used to free stack allocated
 * objects (in this case the pointer itself will not be freed) or
 * heap allocated objects. See the wrappers below. */
void exprTokenRelease(exprtoken *t) {
    if (t == NULL) return;

    RedisModule_Assert(t->refcount > 0); // Catch double free & more.
    t->refcount--;
    if (t->refcount > 0) return;

    // We reached refcount 0: free the object.
    if (t->token_type == EXPR_TOKEN_STR) {
        if (t->str.heapstr != NULL) RedisModule_Free(t->str.heapstr);
    } else if (t->token_type == EXPR_TOKEN_TUPLE) {
        for (size_t j = 0; j < t->tuple.len; j++)
            exprTokenRelease(t->tuple.ele[j]);
        if (t->tuple.ele) RedisModule_Free(t->tuple.ele);
    }
    RedisModule_Free(t);
}

void exprTokenRetain(exprtoken *t) {
    t->refcount++;
}

/* ============================== Stack handling ============================ */

#include <stdlib.h>
#include <string.h>

#define EXPR_STACK_INITIAL_SIZE 16

/* Initialize a new expression stack. */
void exprStackInit(exprstack *stack) {
    stack->items = RedisModule_Alloc(sizeof(exprtoken*) * EXPR_STACK_INITIAL_SIZE);
    stack->numitems = 0;
    stack->allocsize = EXPR_STACK_INITIAL_SIZE;
}

/* Push a token pointer onto the stack. Does not increment the refcount
 * of the token: it is up to the caller doing this. */
void exprStackPush(exprstack *stack, exprtoken *token) {
    /* Check if we need to grow the stack. */
    if (stack->numitems == stack->allocsize) {
        size_t newsize = stack->allocsize * 2;
        exprtoken **newitems =
            RedisModule_Realloc(stack->items, sizeof(exprtoken*) * newsize);
        stack->items = newitems;
        stack->allocsize = newsize;
    }
    stack->items[stack->numitems] = token;
    stack->numitems++;
}

/* Pop a token pointer from the stack. Return NULL if the stack is
 * empty. Does NOT recrement the refcount of the token, it's up to the
 * caller to do so, as the new owner of the reference. */
exprtoken *exprStackPop(exprstack *stack) {
    if (stack->numitems == 0) return NULL;
    stack->numitems--;
    return stack->items[stack->numitems];
}

/* Just return the last element pushed, without consuming it nor altering
 * the reference count. */
exprtoken *exprStackPeek(exprstack *stack) {
    if (stack->numitems == 0) return NULL;
    return stack->items[stack->numitems-1];
}

/* Free the stack structure state, including the items it contains, that are
 * assumed to be heap allocated. The passed pointer itself is not freed. */
void exprStackFree(exprstack *stack) {
    for (int j = 0; j < stack->numitems; j++)
        exprTokenRelease(stack->items[j]);
    RedisModule_Free(stack->items);
}

/* Just reset the stack removing all the items, but leaving it in a state
 * that makes it still usable for new elements. */
void exprStackReset(exprstack *stack) {
    for (int j = 0; j < stack->numitems; j++)
        exprTokenRelease(stack->items[j]);
    stack->numitems = 0;
}

/* =========================== Expression compilation ======================= */

void exprConsumeSpaces(exprstate *es) {
    while(es->p[0] && isspace(es->p[0])) es->p++;
}

/* Parse an operator or a literal (just "null" currently).
 * When parsing operators, the function will try to match the longest match
 * in the operators table. */
exprtoken *exprParseOperatorOrLiteral(exprstate *es) {
    exprtoken *t = exprNewToken(EXPR_TOKEN_OP);
    char *start = es->p;

    while(es->p[0] &&
          (isalpha(es->p[0]) ||
           strchr(EXPR_OP_SPECIALCHARS,es->p[0]) != NULL))
    {
        es->p++;
    }

    int matchlen = es->p - start;
    int bestlen = 0;
    int j;

    // Check if it's a literal.
    if (matchlen == 4 && !memcmp("null",start,4)) {
        t->token_type = EXPR_TOKEN_NULL;
        return t;
    }

    // Find the longest matching operator.
    for (j = 0; ExprOptable[j].opname != NULL; j++) {
        if (ExprOptable[j].oplen > matchlen) continue;
        if (memcmp(ExprOptable[j].opname, start, ExprOptable[j].oplen) != 0)
        {
            continue;
        }
        if (ExprOptable[j].oplen > bestlen) {
            t->opcode = ExprOptable[j].opcode;
            bestlen = ExprOptable[j].oplen;
        }
    }
    if (bestlen == 0) {
        exprTokenRelease(t);
        return NULL;
    } else {
        es->p = start + bestlen;
    }
    return t;
}

// Valid selector charset.
static int is_selector_char(int c) {
    return (isalpha(c) ||
            isdigit(c) ||
            strchr(EXPR_SELECTOR_SPECIALCHARS,c) != NULL);
}

/* Parse selectors, they start with a dot and can have alphanumerical
 * or few special chars. */
exprtoken *exprParseSelector(exprstate *es) {
    exprtoken *t = exprNewToken(EXPR_TOKEN_SELECTOR);
    es->p++; // Skip dot.
    char *start = es->p;

    while(es->p[0] && is_selector_char(es->p[0])) es->p++;
    int matchlen = es->p - start;
    t->str.start = start;
    t->str.len = matchlen;
    return t;
}

exprtoken *exprParseNumber(exprstate *es) {
    exprtoken *t = exprNewToken(EXPR_TOKEN_NUM);
    char num[256];
    int idx = 0;
    while(isdigit(es->p[0]) || es->p[0] == '.' || es->p[0] == 'e' ||
          es->p[0] == 'E' || (idx == 0 && es->p[0] == '-'))
    {
        if (idx >= (int)sizeof(num)-1) {
            exprTokenRelease(t);
            return NULL;
        }
        num[idx++] = es->p[0];
        es->p++;
    }
    num[idx] = 0;

    char *endptr;
    t->num = strtod(num, &endptr);
    if (*endptr != '\0') {
        exprTokenRelease(t);
        return NULL;
    }
    return t;
}

exprtoken *exprParseString(exprstate *es) {
    char quote = es->p[0];  /* Store the quote type (' or "). */
    es->p++;                /* Skip opening quote. */

    exprtoken *t = exprNewToken(EXPR_TOKEN_STR);
    t->str.start = es->p;

    while(es->p[0] != '\0') {
        if (es->p[0] == '\\' && es->p[1] != '\0') {
            es->p += 2; // Skip escaped char.
            continue;
        }
        if (es->p[0] == quote) {
            t->str.len = es->p - t->str.start;
            es->p++; // Skip closing quote.
            return t;
        }
        es->p++;
    }
    /* If we reach here, string was not terminated. */
    exprTokenRelease(t);
    return NULL;
}

/* Parse a tuple of the form [1, "foo", 42]. No nested tuples are
 * supported. This type is useful mostly to be used with the "IN"
 * operator. */
exprtoken *exprParseTuple(exprstate *es) {
    exprtoken *t = exprNewToken(EXPR_TOKEN_TUPLE);
    t->tuple.ele = NULL;
    t->tuple.len = 0;
    es->p++; /* Skip opening '['. */

    size_t allocated = 0;
    while(1) {
        exprConsumeSpaces(es);

        /* Check for empty tuple or end. */
        if (es->p[0] == ']') {
            es->p++;
            break;
        }

        /* Grow tuple array if needed. */
        if (t->tuple.len == allocated) {
            size_t newsize = allocated == 0 ? 4 : allocated * 2;
            exprtoken **newele = RedisModule_Realloc(t->tuple.ele,
                sizeof(exprtoken*) * newsize);
            t->tuple.ele = newele;
            allocated = newsize;
        }

        /* Parse tuple element. */
        exprtoken *ele = NULL;
        if (isdigit(es->p[0]) || es->p[0] == '-') {
            ele = exprParseNumber(es);
        } else if (es->p[0] == '"' || es->p[0] == '\'') {
            ele = exprParseString(es);
        } else {
            exprTokenRelease(t);
            return NULL;
        }

        /* Error parsing number/string? */
        if (ele == NULL) {
            exprTokenRelease(t);
            return NULL;
        }

        /* Store element if no error was detected. */
        t->tuple.ele[t->tuple.len] = ele;
        t->tuple.len++;

        /* Check for next element. */
        exprConsumeSpaces(es);
        if (es->p[0] == ']') {
            es->p++;
            break;
        }
        if (es->p[0] != ',') {
            exprTokenRelease(t);
            return NULL;
        }
        es->p++; /* Skip comma. */
    }
    return t;
}

/* Deallocate the object returned by exprCompile(). */
void exprFree(exprstate *es) {
    if (es == NULL) return;

    /* Free the original expression string. */
    if (es->expr) RedisModule_Free(es->expr);

    /* Free all stacks. */
    exprStackFree(&es->values_stack);
    exprStackFree(&es->ops_stack);
    exprStackFree(&es->tokens);
    exprStackFree(&es->program);

    /* Free the state object itself. */
    RedisModule_Free(es);
}

/* Split the provided expression into a stack of tokens. Returns
 * 0 on success, 1 on error. */
int exprTokenize(exprstate *es, int *errpos) {
    /* Main parsing loop. */
    while(1) {
        exprConsumeSpaces(es);

        /* Set a flag to see if we can consider the - part of the
         * number, or an operator. */
        int minus_is_number = 0; // By default is an operator.

        exprtoken *last = exprStackPeek(&es->tokens);
        if (last == NULL) {
            /* If we are at the start of an expression, the minus is
             * considered a number. */
            minus_is_number = 1;
        } else if (last->token_type == EXPR_TOKEN_OP &&
                   last->opcode != EXPR_OP_CPAREN)
        {
            /* Also, if the previous token was an operator, the minus
             * is considered a number, unless the previous operator is
             * a closing parens. In such case it's like (...) -5, or alike
             * and we want to emit an operator. */
            minus_is_number = 1;
        }

        /* Parse based on the current character. */
        exprtoken *current = NULL;
        if (*es->p == '\0') {
            current = exprNewToken(EXPR_TOKEN_EOF);
        } else if (isdigit(*es->p) ||
                  (minus_is_number && *es->p == '-' && isdigit(es->p[1])))
        {
            current = exprParseNumber(es);
        } else if (*es->p == '"' || *es->p == '\'') {
            current = exprParseString(es);
        } else if (*es->p == '.' && is_selector_char(es->p[1])) {
            current = exprParseSelector(es);
        } else if (*es->p == '[') {
            current = exprParseTuple(es);
        } else if (isalpha(*es->p) || strchr(EXPR_OP_SPECIALCHARS, *es->p)) {
            current = exprParseOperatorOrLiteral(es);
        }

        if (current == NULL) {
            if (errpos) *errpos = es->p - es->expr;
            return 1; // Syntax Error.
        }

        /* Push the current token to tokens stack. */
        exprStackPush(&es->tokens, current);
        if (current->token_type == EXPR_TOKEN_EOF) break;
    }
    return 0;
}

/* Helper function to get operator precedence from the operator table. */
int exprGetOpPrecedence(int opcode) {
    for (int i = 0; ExprOptable[i].opname != NULL; i++) {
        if (ExprOptable[i].opcode == opcode)
            return ExprOptable[i].precedence;
    }
    return -1;
}

/* Helper function to get operator arity from the operator table. */
int exprGetOpArity(int opcode) {
    for (int i = 0; ExprOptable[i].opname != NULL; i++) {
        if (ExprOptable[i].opcode == opcode)
            return ExprOptable[i].arity;
    }
    return -1;
}

/* Process an operator during compilation. Returns 0 on success, 1 on error.
 * This function will retain a reference of the operator 'op' in case it
 * is pushed on the operators stack. */
int exprProcessOperator(exprstate *es, exprtoken *op, int *stack_items, int *errpos) {
    if (op->opcode == EXPR_OP_OPAREN) {
	// This is just a marker for us. Do nothing.
        exprStackPush(&es->ops_stack, op);
        exprTokenRetain(op);
        return 0;
    }

    if (op->opcode == EXPR_OP_CPAREN) {
        /* Process operators until we find the matching opening parenthesis. */
        while (1) {
            exprtoken *top_op = exprStackPop(&es->ops_stack);
            if (top_op == NULL) {
                if (errpos) *errpos = op->offset;
                return 1;
            }

            if (top_op->opcode == EXPR_OP_OPAREN) {
                /* Open parethesis found. Our work finished. */
                exprTokenRelease(top_op);
                return 0;
            }

            int arity = exprGetOpArity(top_op->opcode);
            if (*stack_items < arity) {
                exprTokenRelease(top_op);
                if (errpos) *errpos = top_op->offset;
                return 1;
            }

            /* Move the operator on the program stack. */
            exprStackPush(&es->program, top_op);
            *stack_items = *stack_items - arity + 1;
        }
    }

    int curr_prec = exprGetOpPrecedence(op->opcode);

    /* Process operators with higher or equal precedence. */
    while (1) {
        exprtoken *top_op = exprStackPeek(&es->ops_stack);
        if (top_op == NULL || top_op->opcode == EXPR_OP_OPAREN) break;

        int top_prec = exprGetOpPrecedence(top_op->opcode);
        if (top_prec < curr_prec) break;
        /* Special case for **: only pop if precedence is strictly higher
         * so that the operator is right associative, that is:
         * 2 ** 3 ** 2 is evaluated as 2 ** (3 ** 2) == 512 instead
         * of (2 ** 3) ** 2 == 64. */
        if (op->opcode == EXPR_OP_POW && top_prec <= curr_prec) break;

        /* Pop and add to program. */
        top_op = exprStackPop(&es->ops_stack);
        int arity = exprGetOpArity(top_op->opcode);
        if (*stack_items < arity) {
            exprTokenRelease(top_op);
            if (errpos) *errpos = top_op->offset;
            return 1;
        }

        /* Move to the program stack. */
        exprStackPush(&es->program, top_op);
        *stack_items = *stack_items - arity + 1;
    }

    /* Push current operator. */
    exprStackPush(&es->ops_stack, op);
    exprTokenRetain(op);
    return 0;
}

/* Compile the expression into a set of push-value and exec-operator
 * that exprRun() can execute. The function returns an expstate object
 * that can be used for execution of the program. On error, NULL
 * is returned, and optionally the position of the error into the
 * expression is returned by reference. */
exprstate *exprCompile(char *expr, int *errpos) {
    /* Initialize expression state. */
    exprstate *es = RedisModule_Alloc(sizeof(exprstate));
    es->expr = RedisModule_Strdup(expr);
    es->p = es->expr;

    /* Initialize all stacks. */
    exprStackInit(&es->values_stack);
    exprStackInit(&es->ops_stack);
    exprStackInit(&es->tokens);
    exprStackInit(&es->program);

    /* Tokenization. */
    if (exprTokenize(es, errpos)) {
        exprFree(es);
        return NULL;
    }

    /* Compile the expression into a sequence of operations. */
    int stack_items = 0;  // Track # of items that would be on the stack
                         // during execution. This way we can detect arity
                         // issues at compile time.

    /* Process each token. */
    for (int i = 0; i < es->tokens.numitems; i++) {
        exprtoken *token = es->tokens.items[i];

        if (token->token_type == EXPR_TOKEN_EOF) break;

        /* Handle values (numbers, strings, selectors, null). */
        if (token->token_type == EXPR_TOKEN_NUM ||
            token->token_type == EXPR_TOKEN_STR ||
            token->token_type == EXPR_TOKEN_TUPLE ||
            token->token_type == EXPR_TOKEN_SELECTOR ||
            token->token_type == EXPR_TOKEN_NULL)
        {
            exprStackPush(&es->program, token);
            exprTokenRetain(token);
            stack_items++;
            continue;
        }

        /* Handle operators. */
        if (token->token_type == EXPR_TOKEN_OP) {
            if (exprProcessOperator(es, token, &stack_items, errpos)) {
                exprFree(es);
                return NULL;
            }
            continue;
        }
    }

    /* Process remaining operators on the stack. */
    while (es->ops_stack.numitems > 0) {
        exprtoken *op = exprStackPop(&es->ops_stack);
        if (op->opcode == EXPR_OP_OPAREN) {
            if (errpos) *errpos = op->offset;
            exprTokenRelease(op);
            exprFree(es);
            return NULL;
        }

        int arity = exprGetOpArity(op->opcode);
        if (stack_items < arity) {
            if (errpos) *errpos = op->offset;
            exprTokenRelease(op);
            exprFree(es);
            return NULL;
        }

        exprStackPush(&es->program, op);
        stack_items = stack_items - arity + 1;
    }

    /* Verify that exactly one value would remain on the stack after
     * execution. We could also check that such value is a number, but this
     * would make the code more complex without much gains. */
    if (stack_items != 1) {
        if (errpos) {
            /* Point to the last token's offset for error reporting. */
            exprtoken *last = es->tokens.items[es->tokens.numitems - 1];
            *errpos = last->offset;
        }
        exprFree(es);
        return NULL;
    }
    return es;
}

/* ============================ Expression execution ======================== */

/* Convert a token to its numeric value. For strings we attempt to parse them
 * as numbers, returning 0 if conversion fails. */
double exprTokenToNum(exprtoken *t) {
    char buf[256];
    if (t->token_type == EXPR_TOKEN_NUM) {
        return t->num;
    } else if (t->token_type == EXPR_TOKEN_STR && t->str.len < sizeof(buf)) {
        memcpy(buf, t->str.start, t->str.len);
        buf[t->str.len] = '\0';
        char *endptr;
        double val = strtod(buf, &endptr);
        return *endptr == '\0' ? val : 0;
    } else {
        return 0;
    }
}

/* Convert object to true/false (0 or 1) */
double exprTokenToBool(exprtoken *t) {
    if (t->token_type == EXPR_TOKEN_NUM) {
        return t->num != 0;
    } else if (t->token_type == EXPR_TOKEN_STR && t->str.len == 0) {
        return 0; // Empty string are false, like in Javascript.
    } else if (t->token_type == EXPR_TOKEN_NULL) {
        return 0; // Null is surely more false than true...
    } else {
        return 1; // Every non numerical type is true.
    }
}

/* Compare two tokens. Returns true if they are equal. */
int exprTokensEqual(exprtoken *a, exprtoken *b) {
    // If both are strings, do string comparison.
    if (a->token_type == EXPR_TOKEN_STR && b->token_type == EXPR_TOKEN_STR) {
        return a->str.len == b->str.len &&
               memcmp(a->str.start, b->str.start, a->str.len) == 0;
    }

    // If both are numbers, do numeric comparison.
    if (a->token_type == EXPR_TOKEN_NUM && b->token_type == EXPR_TOKEN_NUM) {
        return a->num == b->num;
    }

    /* If one of the two is null, the expression is true only if
     * both are null. */
    if (a->token_type == EXPR_TOKEN_NULL || b->token_type == EXPR_TOKEN_NULL) {
        return a->token_type == b->token_type;
    }

    // Mixed types - convert to numbers and compare.
    return exprTokenToNum(a) == exprTokenToNum(b);
}

/* Return true if the string a is a substring of b. */
int exprTokensStringIn(exprtoken *a, exprtoken *b) {
    RedisModule_Assert(a->token_type == EXPR_TOKEN_STR &&
                       b->token_type == EXPR_TOKEN_STR);
    if (a->str.len > b->str.len) return 0; // A is bigger, can't be a substring.
    for (size_t i = 0; i <= b->str.len - a->str.len; i++) {
        if (memcmp(b->str.start+i,a->str.start,a->str.len) == 0) return 1;
    }
    return 0;
}

#include "fastjson.c" // JSON parser implementation used by exprRun().

/* Execute the compiled expression program. Returns 1 if the final stack value
 * evaluates to true, 0 otherwise. Also returns 0 if any selector callback
 * fails. */
int exprRun(exprstate *es, char *json, size_t json_len) {
    exprStackReset(&es->values_stack);

    // Execute each instruction in the program.
    for (int i = 0; i < es->program.numitems; i++) {
        exprtoken *t = es->program.items[i];

        // Handle selectors by calling the callback.
        if (t->token_type == EXPR_TOKEN_SELECTOR) {
            exprtoken *obj = NULL;
            if (t->str.len > 0)
                obj = jsonExtractField(json,json_len,t->str.start,t->str.len);

            // Selector not found or JSON object not convertible to
            // expression tokens. Evaluate the expression to false.
            if (obj == NULL) return 0;
            exprStackPush(&es->values_stack, obj);
            continue;
        }

        // Push non-operator values directly onto the stack.
        if (t->token_type != EXPR_TOKEN_OP) {
            exprStackPush(&es->values_stack, t);
            exprTokenRetain(t);
            continue;
        }

        // Handle operators.
        exprtoken *result = exprNewToken(EXPR_TOKEN_NUM);

        // Pop operands - we know we have enough from compile-time checks.
        exprtoken *b = exprStackPop(&es->values_stack);
        exprtoken *a = NULL;
        if (exprGetOpArity(t->opcode) == 2) {
            a = exprStackPop(&es->values_stack);
        }

        switch(t->opcode) {
        case EXPR_OP_NOT:
            result->num = exprTokenToBool(b) == 0 ? 1 : 0;
            break;
        case EXPR_OP_POW: {
            double base = exprTokenToNum(a);
            double exp = exprTokenToNum(b);
            result->num = pow(base, exp);
            break;
        }
        case EXPR_OP_MULT:
            result->num = exprTokenToNum(a) * exprTokenToNum(b);
            break;
        case EXPR_OP_DIV:
            result->num = exprTokenToNum(a) / exprTokenToNum(b);
            break;
        case EXPR_OP_MOD: {
            double va = exprTokenToNum(a);
            double vb = exprTokenToNum(b);
            result->num = fmod(va, vb);
            break;
        }
        case EXPR_OP_SUM:
            result->num = exprTokenToNum(a) + exprTokenToNum(b);
            break;
        case EXPR_OP_DIFF:
            result->num = exprTokenToNum(a) - exprTokenToNum(b);
            break;
        case EXPR_OP_GT:
            result->num = exprTokenToNum(a) > exprTokenToNum(b) ? 1 : 0;
            break;
        case EXPR_OP_GTE:
            result->num = exprTokenToNum(a) >= exprTokenToNum(b) ? 1 : 0;
            break;
        case EXPR_OP_LT:
            result->num = exprTokenToNum(a) < exprTokenToNum(b) ? 1 : 0;
            break;
        case EXPR_OP_LTE:
            result->num = exprTokenToNum(a) <= exprTokenToNum(b) ? 1 : 0;
            break;
        case EXPR_OP_EQ:
            result->num = exprTokensEqual(a, b) ? 1 : 0;
            break;
        case EXPR_OP_NEQ:
            result->num = !exprTokensEqual(a, b) ? 1 : 0;
            break;
        case EXPR_OP_IN: {
            /* For 'in' operator, b must be a tuple, and we check for
             * membership. Otherwise both a and b must be strings, and
             * in this case we check if a is a substring of b. */
            result->num = 0;  // Default to false.
            if (b->token_type == EXPR_TOKEN_TUPLE) {
                for (size_t j = 0; j < b->tuple.len; j++) {
                    if (exprTokensEqual(a, b->tuple.ele[j])) {
                        result->num = 1;  // Found a match.
                        break;
                    }
                }
            } else if (a->token_type == EXPR_TOKEN_STR &&
                       b->token_type == EXPR_TOKEN_STR)
            {
                result->num = exprTokensStringIn(a,b);
            }
            break;
        }
        case EXPR_OP_AND:
            result->num =
                exprTokenToBool(a) != 0 && exprTokenToBool(b) != 0 ? 1 : 0;
            break;
        case EXPR_OP_OR:
            result->num =
                exprTokenToBool(a) != 0 || exprTokenToBool(b) != 0 ? 1 : 0;
            break;
        default:
            // Do nothing: we don't want runtime errors.
            break;
        }

        // Free operands and push result.
        if (a) exprTokenRelease(a);
        exprTokenRelease(b);
        exprStackPush(&es->values_stack, result);
    }

    // Get final result from stack.
    exprtoken *final = exprStackPop(&es->values_stack);
    if (final == NULL) return 0;

    // Convert result to boolean.
    int retval = exprTokenToBool(final);
    exprTokenRelease(final);
    return retval;
}

/* ============================ Simple test main ============================ */

#ifdef TEST_MAIN
#include "fastjson_test.c"

void exprPrintToken(exprtoken *t) {
    switch(t->token_type) {
        case EXPR_TOKEN_EOF:
            printf("EOF");
            break;
        case EXPR_TOKEN_NUM:
            printf("NUM:%g", t->num);
            break;
        case EXPR_TOKEN_STR:
            printf("STR:\"%.*s\"", (int)t->str.len, t->str.start);
            break;
        case EXPR_TOKEN_SELECTOR:
            printf("SEL:%.*s", (int)t->str.len, t->str.start);
            break;
        case EXPR_TOKEN_OP:
            printf("OP:");
            for (int i = 0; ExprOptable[i].opname != NULL; i++) {
                if (ExprOptable[i].opcode == t->opcode) {
                    printf("%s", ExprOptable[i].opname);
                    break;
                }
            }
            break;
        default:
            printf("UNKNOWN");
            break;
    }
}

void exprPrintStack(exprstack *stack, const char *name) {
    printf("%s (%d items):", name, stack->numitems);
    for (int j = 0; j < stack->numitems; j++) {
        printf(" ");
        exprPrintToken(stack->items[j]);
    }
    printf("\n");
}

int main(int argc, char **argv) {
    /* Check for JSON parser test mode. */
    if (argc >= 2 && strcmp(argv[1], "--test-json-parser") == 0) {
        run_fastjson_test();
        return 0;
    }

    char *testexpr = "(5+2)*3 and .year > 1980 and 'foo' == 'foo'";
    char *testjson = "{\"year\": 1984, \"name\": \"The Matrix\"}";
    if (argc >= 2) testexpr = argv[1];
    if (argc >= 3) testjson = argv[2];

    printf("Compiling expression: %s\n", testexpr);

    int errpos = 0;
    exprstate *es = exprCompile(testexpr,&errpos);
    if (es == NULL) {
        printf("Compilation failed near \"...%s\"\n", testexpr+errpos);
        return 1;
    }

    exprPrintStack(&es->tokens, "Tokens");
    exprPrintStack(&es->program, "Program");
    printf("Running against object: %s\n", testjson);
    int result = exprRun(es,testjson,strlen(testjson));
    printf("Result1: %s\n", result ? "True" : "False");
    result = exprRun(es,testjson,strlen(testjson));
    printf("Result2: %s\n", result ? "True" : "False");

    exprFree(es);
    return 0;
}
#endif
