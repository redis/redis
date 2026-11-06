#include "test/jemalloc_test.h"

#include "jemalloc/internal/util.h"

typedef enum {
	TOKEN_TYPE_NONE,
	TOKEN_TYPE_ERROR,
	TOKEN_TYPE_EOI,
	TOKEN_TYPE_NULL,
	TOKEN_TYPE_FALSE,
	TOKEN_TYPE_TRUE,
	TOKEN_TYPE_LBRACKET,
	TOKEN_TYPE_RBRACKET,
	TOKEN_TYPE_LBRACE,
	TOKEN_TYPE_RBRACE,
	TOKEN_TYPE_COLON,
	TOKEN_TYPE_COMMA,
	TOKEN_TYPE_STRING,
	TOKEN_TYPE_NUMBER
} token_type_t;

typedef struct parser_s parser_t;
typedef struct {
	parser_t	*parser;
	token_type_t	token_type;
	size_t		pos;
	size_t		len;
	size_t		line;
	size_t		col;
} token_t;

struct parser_s {
	bool verbose;
	char	*buf; /* '\0'-terminated. */
	size_t	len; /* Number of characters preceding '\0' in buf. */
	size_t	pos;
	size_t	line;
	size_t	col;
	token_t	token;
};

static void
token_init(token_t *token, parser_t *parser, token_type_t token_type,
    size_t pos, size_t len, size_t line, size_t col) {
	token->parser = parser;
	token->token_type = token_type;
	token->pos = pos;
	token->len = len;
	token->line = line;
	token->col = col;
}

static void
token_error(token_t *token) {
	if (!token->parser->verbose) {
		return;
	}
	switch (token->token_type) {
	case TOKEN_TYPE_NONE:
		not_reached();
	case TOKEN_TYPE_ERROR:
		malloc_printf("%zu:%zu: Unexpected character in token: ",
		    token->line, token->col);
		break;
	default:
		malloc_printf("%zu:%zu: Unexpected token: ", token->line,
		    token->col);
		break;
	}
	UNUSED ssize_t err = malloc_write_fd(STDERR_FILENO,
	    &token->parser->buf[token->pos], token->len);
	malloc_printf("\n");
}

static void
parser_init(parser_t *parser, bool verbose) {
	parser->verbose = verbose;
	parser->buf = NULL;
	parser->len = 0;
	parser->pos = 0;
	parser->line = 1;
	parser->col = 0;
}

static void
parser_fini(parser_t *parser) {
	if (parser->buf != NULL) {
		dallocx(parser->buf, MALLOCX_TCACHE_NONE);
	}
}

static bool
parser_append(parser_t *parser, const char *str) {
	size_t len = strlen(str);
	char *buf = (parser->buf == NULL) ? mallocx(len + 1,
	    MALLOCX_TCACHE_NONE) : rallocx(parser->buf, parser->len + len + 1,
	    MALLOCX_TCACHE_NONE);
	if (buf == NULL) {
		return true;
	}
	memcpy(&buf[parser->len], str, len + 1);
	parser->buf = buf;
	parser->len += len;
	return false;
}

static bool
parser_tokenize(parser_t *parser) {
	enum {
		STATE_START,
		STATE_EOI,
		STATE_N, STATE_NU, STATE_NUL, STATE_NULL,
		STATE_F, STATE_FA, STATE_FAL, STATE_FALS, STATE_FALSE,
		STATE_T, STATE_TR, STATE_TRU, STATE_TRUE,
		STATE_LBRACKET,
		STATE_RBRACKET,
		STATE_LBRACE,
		STATE_RBRACE,
		STATE_COLON,
		STATE_COMMA,
		STATE_CHARS,
		STATE_CHAR_ESCAPE,
		STATE_CHAR_U, STATE_CHAR_UD, STATE_CHAR_UDD, STATE_CHAR_UDDD,
		STATE_STRING,
		STATE_MINUS,
		STATE_LEADING_ZERO,
		STATE_DIGITS,
		STATE_DECIMAL,
		STATE_FRAC_DIGITS,
		STATE_EXP,
		STATE_EXP_SIGN,
		STATE_EXP_DIGITS,
		STATE_ACCEPT
	} state = STATE_START;
	size_t token_pos JEMALLOC_CC_SILENCE_INIT(0);
	size_t token_line JEMALLOC_CC_SILENCE_INIT(1);
	size_t token_col JEMALLOC_CC_SILENCE_INIT(0);

	expect_zu_le(parser->pos, parser->len,
	    "Position is past end of buffer");

	while (state != STATE_ACCEPT) {
		char c = parser->buf[parser->pos];

		switch (state) {
		case STATE_START:
			token_pos = parser->pos;
			token_line = parser->line;
			token_col = parser->col;
			switch (c) {
			case ' ': case '\b': case '\n': case '\r': case '\t':
				break;
			case '\0':
				state = STATE_EOI;
				break;
			case 'n':
				state = STATE_N;
				break;
			case 'f':
				state = STATE_F;
				break;
			case 't':
				state = STATE_T;
				break;
			case '[':
				state = STATE_LBRACKET;
				break;
			case ']':
				state = STATE_RBRACKET;
				break;
			case '{':
				state = STATE_LBRACE;
				break;
			case '}':
				state = STATE_RBRACE;
				break;
			case ':':
				state = STATE_COLON;
				break;
			case ',':
				state = STATE_COMMA;
				break;
			case '"':
				state = STATE_CHARS;
				break;
			case '-':
				state = STATE_MINUS;
				break;
			case '0':
				state = STATE_LEADING_ZERO;
				break;
			case '1': case '2': case '3': case '4':
			case '5': case '6': case '7': case '8': case '9':
				state = STATE_DIGITS;
				break;
			default:
				token_init(&parser->token, parser,
				    TOKEN_TYPE_ERROR, token_pos, parser->pos + 1
				    - token_pos, token_line, token_col);
				return true;
			}
			break;
		case STATE_EOI:
			token_init(&parser->token, parser,
			    TOKEN_TYPE_EOI, token_pos, parser->pos -
			    token_pos, token_line, token_col);
			state = STATE_ACCEPT;
			break;
		case STATE_N:
			switch (c) {
			case 'u':
				state = STATE_NU;
				break;
			default:
				token_init(&parser->token, parser,
				    TOKEN_TYPE_ERROR, token_pos, parser->pos + 1
				    - token_pos, token_line, token_col);
				return true;
			}
			break;
		case STATE_NU:
			switch (c) {
			case 'l':
				state = STATE_NUL;
				break;
			default:
				token_init(&parser->token, parser,
				    TOKEN_TYPE_ERROR, token_pos, parser->pos + 1
				    - token_pos, token_line, token_col);
				return true;
			}
			break;
		case STATE_NUL:
			switch (c) {
			case 'l':
				state = STATE_NULL;
				break;
			default:
				token_init(&parser->token, parser,
				    TOKEN_TYPE_ERROR, token_pos, parser->pos + 1
				    - token_pos, token_line, token_col);
				return true;
			}
			break;
		case STATE_NULL:
			switch (c) {
			case ' ': case '\b': case '\n': case '\r': case '\t':
			case '\0':
			case '[': case ']': case '{': case '}': case ':':
			case ',':
				break;
			default:
				token_init(&parser->token, parser,
				    TOKEN_TYPE_ERROR, token_pos, parser->pos + 1
				    - token_pos, token_line, token_col);
				return true;
			}
			token_init(&parser->token, parser, TOKEN_TYPE_NULL,
			    token_pos, parser->pos - token_pos, token_line,
			    token_col);
			state = STATE_ACCEPT;
			break;
		case STATE_F:
			switch (c) {
			case 'a':
				state = STATE_FA;
				break;
			default:
				token_init(&parser->token, parser,
				    TOKEN_TYPE_ERROR, token_pos, parser->pos + 1
				    - token_pos, token_line, token_col);
				return true;
			}
			break;
		case STATE_FA:
			switch (c) {
			case 'l':
				state = STATE_FAL;
				break;
			default:
				token_init(&parser->token, parser,
				    TOKEN_TYPE_ERROR, token_pos, parser->pos + 1
				    - token_pos, token_line, token_col);
				return true;
			}
			break;
		case STATE_FAL:
			switch (c) {
			case 's':
				state = STATE_FALS;
				break;
			default:
				token_init(&parser->token, parser,
				    TOKEN_TYPE_ERROR, token_pos, parser->pos + 1
				    - token_pos, token_line, token_col);
				return true;
			}
			break;
		case STATE_FALS:
			switch (c) {
			case 'e':
				state = STATE_FALSE;
				break;
			default:
				token_init(&parser->token, parser,
				    TOKEN_TYPE_ERROR, token_pos, parser->pos + 1
				    - token_pos, token_line, token_col);
				return true;
			}
			break;
		case STATE_FALSE:
			switch (c) {
			case ' ': case '\b': case '\n': case '\r': case '\t':
			case '\0':
			case '[': case ']': case '{': case '}': case ':':
			case ',':
				break;
			default:
				token_init(&parser->token, parser,
				    TOKEN_TYPE_ERROR, token_pos, parser->pos + 1
				    - token_pos, token_line, token_col);
				return true;
			}
			token_init(&parser->token, parser,
			    TOKEN_TYPE_FALSE, token_pos, parser->pos -
			    token_pos, token_line, token_col);
			state = STATE_ACCEPT;
			break;
		case STATE_T:
			switch (c) {
			case 'r':
				state = STATE_TR;
				break;
			default:
				token_init(&parser->token, parser,
				    TOKEN_TYPE_ERROR, token_pos, parser->pos + 1
				    - token_pos, token_line, token_col);
				return true;
			}
			break;
		case STATE_TR:
			switch (c) {
			case 'u':
				state = STATE_TRU;
				break;
			default:
				token_init(&parser->token, parser,
				    TOKEN_TYPE_ERROR, token_pos, parser->pos + 1
				    - token_pos, token_line, token_col);
				return true;
			}
			break;
		case STATE_TRU:
			switch (c) {
			case 'e':
				state = STATE_TRUE;
				break;
			default:
				token_init(&parser->token, parser,
				    TOKEN_TYPE_ERROR, token_pos, parser->pos + 1
				    - token_pos, token_line, token_col);
				return true;
			}
			break;
		case STATE_TRUE:
			switch (c) {
			case ' ': case '\b': case '\n': case '\r': case '\t':
			case '\0':
			case '[': case ']': case '{': case '}': case ':':
			case ',':
				break;
			default:
				token_init(&parser->token, parser,
				    TOKEN_TYPE_ERROR, token_pos, parser->pos + 1
				    - token_pos, token_line, token_col);
				return true;
			}
			token_init(&parser->token, parser, TOKEN_TYPE_TRUE,
			    token_pos, parser->pos - token_pos, token_line,
			    token_col);
			state = STATE_ACCEPT;
			break;
		case STATE_LBRACKET:
			token_init(&parser->token, parser, TOKEN_TYPE_LBRACKET,
			    token_pos, parser->pos - token_pos, token_line,
			    token_col);
			state = STATE_ACCEPT;
			break;
		case STATE_RBRACKET:
			token_init(&parser->token, parser, TOKEN_TYPE_RBRACKET,
			    token_pos, parser->pos - token_pos, token_line,
			    token_col);
			state = STATE_ACCEPT;
			break;
		case STATE_LBRACE:
			token_init(&parser->token, parser, TOKEN_TYPE_LBRACE,
			    token_pos, parser->pos - token_pos, token_line,
			    token_col);
			state = STATE_ACCEPT;
			break;
		case STATE_RBRACE:
			token_init(&parser->token, parser, TOKEN_TYPE_RBRACE,
			    token_pos, parser->pos - token_pos, token_line,
			    token_col);
			state = STATE_ACCEPT;
			break;
		case STATE_COLON:
			token_init(&parser->token, parser, TOKEN_TYPE_COLON,
			    token_pos, parser->pos - token_pos, token_line,
			    token_col);
			state = STATE_ACCEPT;
			break;
		case STATE_COMMA:
			token_init(&parser->token, parser, TOKEN_TYPE_COMMA,
			    token_pos, parser->pos - token_pos, token_line,
			    token_col);
			state = STATE_ACCEPT;
			break;
		case STATE_CHARS:
			switch (c) {
			case '\\':
				state = STATE_CHAR_ESCAPE;
				break;
			case '"':
				state = STATE_STRING;
				break;
			case 0x00: case 0x01: case 0x02: case 0x03: case 0x04:
			case 0x05: case 0x06: case 0x07: case 0x08: case 0x09:
			case 0x0a: case 0x0b: case 0x0c: case 0x0d: case 0x0e:
			case 0x0f: case 0x10: case 0x11: case 0x12: case 0x13:
			case 0x14: case 0x15: case 0x16: case 0x17: case 0x18:
			case 0x19: case 0x1a: case 0x1b: case 0x1c: case 0x1d:
			case 0x1e: case 0x1f:
				token_init(&parser->token, parser,
				    TOKEN_TYPE_ERROR, token_pos, parser->pos + 1
				    - token_pos, token_line, token_col);
				return true;
			default:
				break;
			}
			break;
		case STATE_CHAR_ESCAPE:
			switch (c) {
			case '"': case '\\': case '/': case 'b': case 'n':
			case 'r': case 't':
				state = STATE_CHARS;
				break;
			case 'u':
				state = STATE_CHAR_U;
				break;
			default:
				token_init(&parser->token, parser,
				    TOKEN_TYPE_ERROR, token_pos, parser->pos + 1
				    - token_pos, token_line, token_col);
				return true;
			}
			break;
		case STATE_CHAR_U:
			switch (c) {
			case '0': case '1': case '2': case '3': case '4':
			case '5': case '6': case '7': case '8': case '9':
			case 'a': case 'b': case 'c': case 'd': case 'e':
			case 'f':
			case 'A': case 'B': case 'C': case 'D': case 'E':
			case 'F':
				state = STATE_CHAR_UD;
				break;
			default:
				token_init(&parser->token, parser,
				    TOKEN_TYPE_ERROR, token_pos, parser->pos + 1
				    - token_pos, token_line, token_col);
				return true;
			}
			break;
		case STATE_CHAR_UD:
			switch (c) {
			case '0': case '1': case '2': case '3': case '4':
			case '5': case '6': case '7': case '8': case '9':
			case 'a': case 'b': case 'c': case 'd': case 'e':
			case 'f':
			case 'A': case 'B': case 'C': case 'D': case 'E':
			case 'F':
				state = STATE_CHAR_UDD;
				break;
			default:
				token_init(&parser->token, parser,
				    TOKEN_TYPE_ERROR, token_pos, parser->pos + 1
				    - token_pos, token_line, token_col);
				return true;
			}
			break;
		case STATE_CHAR_UDD:
			switch (c) {
			case '0': case '1': case '2': case '3': case '4':
			case '5': case '6': case '7': case '8': case '9':
			case 'a': case 'b': case 'c': case 'd': case 'e':
			case 'f':
			case 'A': case 'B': case 'C': case 'D': case 'E':
			case 'F':
				state = STATE_CHAR_UDDD;
				break;
			default:
				token_init(&parser->token, parser,
				    TOKEN_TYPE_ERROR, token_pos, parser->pos + 1
				    - token_pos, token_line, token_col);
				return true;
			}
			break;
		case STATE_CHAR_UDDD:
			switch (c) {
			case '0': case '1': case '2': case '3': case '4':
			case '5': case '6': case '7': case '8': case '9':
			case 'a': case 'b': case 'c': case 'd': case 'e':
			case 'f':
			case 'A': case 'B': case 'C': case 'D': case 'E':
			case 'F':
				state = STATE_CHARS;
				break;
			default:
				token_init(&parser->token, parser,
				    TOKEN_TYPE_ERROR, token_pos, parser->pos + 1
				    - token_pos, token_line, token_col);
				return true;
			}
			break;
		case STATE_STRING:
			token_init(&parser->token, parser, TOKEN_TYPE_STRING,
			    token_pos, parser->pos - token_pos, token_line,
			    token_col);
			state = STATE_ACCEPT;
			break;
		case STATE_MINUS:
			switch (c) {
			case '0':
				state = STATE_LEADING_ZERO;
				break;
			case '1': case '2': case '3': case '4':
			case '5': case '6': case '7': case '8': case '9':
				state = STATE_DIGITS;
				break;
			default:
				token_init(&parser->token, parser,
				    TOKEN_TYPE_ERROR, token_pos, parser->pos + 1
				    - token_pos, token_line, token_col);
				return true;
			}
			break;
		case STATE_LEADING_ZERO:
			switch (c) {
			case '.':
				state = STATE_DECIMAL;
				break;
			default:
				token_init(&parser->token, parser,
				    TOKEN_TYPE_NUMBER, token_pos, parser->pos -
				    token_pos, token_line, token_col);
				state = STATE_ACCEPT;
				break;
			}
			break;
		case STATE_DIGITS:
			switch (c) {
			case '0': case '1': case '2': case '3': case '4':
			case '5': case '6': case '7': case '8': case '9':
				break;
			case '.':
				state = STATE_DECIMAL;
				break;
			default:
				token_init(&parser->token, parser,
				    TOKEN_TYPE_NUMBER, token_pos, parser->pos -
				    token_pos, token_line, token_col);
				state = STATE_ACCEPT;
				break;
			}
			break;
		case STATE_DECIMAL:
			switch (c) {
			case '0': case '1': case '2': case '3': case '4':
			case '5': case '6': case '7': case '8': case '9':
				state = STATE_FRAC_DIGITS;
				break;
			default:
				token_init(&parser->token, parser,
				    TOKEN_TYPE_ERROR, token_pos, parser->pos + 1
				    - token_pos, token_line, token_col);
				return true;
			}
			break;
		case STATE_FRAC_DIGITS:
			switch (c) {
			case '0': case '1': case '2': case '3': case '4':
			case '5': case '6': case '7': case '8': case '9':
				break;
			case 'e': case 'E':
				state = STATE_EXP;
				break;
			default:
				token_init(&parser->token, parser,
				    TOKEN_TYPE_NUMBER, token_pos, parser->pos -
				    token_pos, token_line, token_col);
				state = STATE_ACCEPT;
				break;
			}
			break;
		case STATE_EXP:
			switch (c) {
			case '-': case '+':
				state = STATE_EXP_SIGN;
				break;
			case '0': case '1': case '2': case '3': case '4':
			case '5': case '6': case '7': case '8': case '9':
				state = STATE_EXP_DIGITS;
				break;
			default:
				token_init(&parser->token, parser,
				    TOKEN_TYPE_ERROR, token_pos, parser->pos + 1
				    - token_pos, token_line, token_col);
				return true;
			}
			break;
		case STATE_EXP_SIGN:
			switch (c) {
			case '0': case '1': case '2': case '3': case '4':
			case '5': case '6': case '7': case '8': case '9':
				state = STATE_EXP_DIGITS;
				break;
			default:
				token_init(&parser->token, parser,
				    TOKEN_TYPE_ERROR, token_pos, parser->pos + 1
				    - token_pos, token_line, token_col);
				return true;
			}
			break;
		case STATE_EXP_DIGITS:
			switch (c) {
			case '0': case '1': case '2': case '3': case '4':
			case '5': case '6': case '7': case '8': case '9':
				break;
			default:
				token_init(&parser->token, parser,
				    TOKEN_TYPE_NUMBER, token_pos, parser->pos -
				    token_pos, token_line, token_col);
				state = STATE_ACCEPT;
				break;
			}
			break;
		default:
			not_reached();
		}

		if (state != STATE_ACCEPT) {
			if (c == '\n') {
				parser->line++;
				parser->col = 0;
			} else {
				parser->col++;
			}
			parser->pos++;
		}
	}
	return false;
}

static bool	parser_parse_array(parser_t *parser);
static bool	parser_parse_object(parser_t *parser);

static bool
parser_parse_value(parser_t *parser) {
	switch (parser->token.token_type) {
	case TOKEN_TYPE_NULL:
	case TOKEN_TYPE_FALSE:
	case TOKEN_TYPE_TRUE:
	case TOKEN_TYPE_STRING:
	case TOKEN_TYPE_NUMBER:
		return false;
	case TOKEN_TYPE_LBRACE:
		return parser_parse_object(parser);
	case TOKEN_TYPE_LBRACKET:
		return parser_parse_array(parser);
	default:
		return true;
	}
	not_reached();
}

static bool
parser_parse_pair(parser_t *parser) {
	expect_d_eq(parser->token.token_type, TOKEN_TYPE_STRING,
	    "Pair should start with string");
	if (parser_tokenize(parser)) {
		return true;
	}
	switch (parser->token.token_type) {
	case TOKEN_TYPE_COLON:
		if (parser_tokenize(parser)) {
			return true;
		}
		return parser_parse_value(parser);
	default:
		return true;
	}
}

static bool
parser_parse_values(parser_t *parser) {
	if (parser_parse_value(parser)) {
		return true;
	}

	while (true) {
		if (parser_tokenize(parser)) {
			return true;
		}
		switch (parser->token.token_type) {
		case TOKEN_TYPE_COMMA:
			if (parser_tokenize(parser)) {
				return true;
			}
			if (parser_parse_value(parser)) {
				return true;
			}
			break;
		case TOKEN_TYPE_RBRACKET:
			return false;
		default:
			return true;
		}
	}
}

static bool
parser_parse_array(parser_t *parser) {
	expect_d_eq(parser->token.token_type, TOKEN_TYPE_LBRACKET,
	    "Array should start with [");
	if (parser_tokenize(parser)) {
		return true;
	}
	switch (parser->token.token_type) {
	case TOKEN_TYPE_RBRACKET:
		return false;
	default:
		return parser_parse_values(parser);
	}
	not_reached();
}

static bool
parser_parse_pairs(parser_t *parser) {
	expect_d_eq(parser->token.token_type, TOKEN_TYPE_STRING,
	    "Object should start with string");
	if (parser_parse_pair(parser)) {
		return true;
	}

	while (true) {
		if (parser_tokenize(parser)) {
			return true;
		}
		switch (parser->token.token_type) {
		case TOKEN_TYPE_COMMA:
			if (parser_tokenize(parser)) {
				return true;
			}
			switch (parser->token.token_type) {
			case TOKEN_TYPE_STRING:
				if (parser_parse_pair(parser)) {
					return true;
				}
				break;
			default:
				return true;
			}
			break;
		case TOKEN_TYPE_RBRACE:
			return false;
		default:
			return true;
		}
	}
}

static bool
parser_parse_object(parser_t *parser) {
	expect_d_eq(parser->token.token_type, TOKEN_TYPE_LBRACE,
	    "Object should start with {");
	if (parser_tokenize(parser)) {
		return true;
	}
	switch (parser->token.token_type) {
	case TOKEN_TYPE_STRING:
		return parser_parse_pairs(parser);
	case TOKEN_TYPE_RBRACE:
		return false;
	default:
		return true;
	}
	not_reached();
}

static bool
parser_parse(parser_t *parser) {
	if (parser_tokenize(parser)) {
		goto label_error;
	}
	if (parser_parse_value(parser)) {
		goto label_error;
	}

	if (parser_tokenize(parser)) {
		goto label_error;
	}
	switch (parser->token.token_type) {
	case TOKEN_TYPE_EOI:
		return false;
	default:
		goto label_error;
	}
	not_reached();

label_error:
	token_error(&parser->token);
	return true;
}

TEST_BEGIN(test_json_parser) {
	size_t i;
	const char *invalid_inputs[] = {
		/* Tokenizer error case tests. */
		"{ \"string\": X }",
		"{ \"string\": nXll }",
		"{ \"string\": nuXl }",
		"{ \"string\": nulX }",
		"{ \"string\": nullX }",
		"{ \"string\": fXlse }",
		"{ \"string\": faXse }",
		"{ \"string\": falXe }",
		"{ \"string\": falsX }",
		"{ \"string\": falseX }",
		"{ \"string\": tXue }",
		"{ \"string\": trXe }",
		"{ \"string\": truX }",
		"{ \"string\": trueX }",
		"{ \"string\": \"\n\" }",
		"{ \"string\": \"\\z\" }",
		"{ \"string\": \"\\uX000\" }",
		"{ \"string\": \"\\u0X00\" }",
		"{ \"string\": \"\\u00X0\" }",
		"{ \"string\": \"\\u000X\" }",
		"{ \"string\": -X }",
		"{ \"string\": 0.X }",
		"{ \"string\": 0.0eX }",
		"{ \"string\": 0.0e+X }",

		/* Parser error test cases. */
		"{\"string\": }",
		"{\"string\" }",
		"{\"string\": [ 0 }",
		"{\"string\": {\"a\":0, 1 } }",
		"{\"string\": {\"a\":0: } }",
		"{",
		"{}{",
	};
	const char *valid_inputs[] = {
		/* Token tests. */
		"null",
		"false",
		"true",
		"{}",
		"{\"a\": 0}",
		"[]",
		"[0, 1]",
		"0",
		"1",
		"10",
		"-10",
		"10.23",
		"10.23e4",
		"10.23e-4",
		"10.23e+4",
		"10.23E4",
		"10.23E-4",
		"10.23E+4",
		"-10.23",
		"-10.23e4",
		"-10.23e-4",
		"-10.23e+4",
		"-10.23E4",
		"-10.23E-4",
		"-10.23E+4",
		"\"value\"",
		"\" \\\" \\/ \\b \\n \\r \\t \\u0abc \\u1DEF \"",

		/* Parser test with various nesting. */
		"{\"a\":null, \"b\":[1,[{\"c\":2},3]], \"d\":{\"e\":true}}",
	};

	for (i = 0; i < sizeof(invalid_inputs)/sizeof(const char *); i++) {
		const char *input = invalid_inputs[i];
		parser_t parser;
		parser_init(&parser, false);
		expect_false(parser_append(&parser, input),
		    "Unexpected input appending failure");
		expect_true(parser_parse(&parser),
		    "Unexpected parse success for input: %s", input);
		parser_fini(&parser);
	}

	for (i = 0; i < sizeof(valid_inputs)/sizeof(const char *); i++) {
		const char *input = valid_inputs[i];
		parser_t parser;
		parser_init(&parser, true);
		expect_false(parser_append(&parser, input),
		    "Unexpected input appending failure");
		expect_false(parser_parse(&parser),
		    "Unexpected parse error for input: %s", input);
		parser_fini(&parser);
	}
}
TEST_END

void
write_cb(void *opaque, const char *str) {
	parser_t *parser = (parser_t *)opaque;
	if (parser_append(parser, str)) {
		test_fail("Unexpected input appending failure");
	}
}

TEST_BEGIN(test_stats_print_json) {
	const char *opts[] = {
		"J",
		"Jg",
		"Jm",
		"Jd",
		"Jmd",
		"Jgd",
		"Jgm",
		"Jgmd",
		"Ja",
		"Jb",
		"Jl",
		"Jx",
		"Jbl",
		"Jal",
		"Jab",
		"Jabl",
		"Jax",
		"Jbx",
		"Jlx",
		"Jablx",
		"Jgmdablx",
	};
	unsigned arena_ind, i;

	for (i = 0; i < 3; i++) {
		unsigned j;

		switch (i) {
		case 0:
			break;
		case 1: {
			size_t sz = sizeof(arena_ind);
			expect_d_eq(mallctl("arenas.create", (void *)&arena_ind,
			    &sz, NULL, 0), 0, "Unexpected mallctl failure");
			break;
		} case 2: {
			size_t mib[3];
			size_t miblen = sizeof(mib)/sizeof(size_t);
			expect_d_eq(mallctlnametomib("arena.0.destroy",
			    mib, &miblen), 0,
			    "Unexpected mallctlnametomib failure");
			mib[1] = arena_ind;
			expect_d_eq(mallctlbymib(mib, miblen, NULL, NULL, NULL,
			    0), 0, "Unexpected mallctlbymib failure");
			break;
		} default:
			not_reached();
		}

		for (j = 0; j < sizeof(opts)/sizeof(const char *); j++) {
			parser_t parser;

			parser_init(&parser, true);
			malloc_stats_print(write_cb, (void *)&parser, opts[j]);
			expect_false(parser_parse(&parser),
			    "Unexpected parse error, opts=\"%s\"", opts[j]);
			parser_fini(&parser);
		}
	}
}
TEST_END

int
main(void) {
	return test(
	    test_json_parser,
	    test_stats_print_json);
}
