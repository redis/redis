# Makefile template for shared library

CC = gcc # C compiler
CFLAGS = -fPIC -Wall -Wextra -O0 -g -I../../ # C flags
LDFLAGS = -fPIC -shared # linking flags
RM = rm -f  # rm command
TARGET_LIB = libcompression_lzf.so # target lib
ODIR = ./

SRCS = lzf_plugin.c ../../lzf_c.c ../../lzf_d.c # source files
OBJS = $(SRCS:.c=.o)

.PHONY: all
all: ${TARGET_LIB}

$(TARGET_LIB): $(OBJS)
	$(CC) ${LDFLAGS} -o $@ $^

.PHONY: clean
clean:
	-${RM} ${TARGET_LIB} ${OBJS} $(SRCS:.c=.a)
