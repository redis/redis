# Makefile template for shared library

CC = gcc # C compiler
CFLAGS = -fPIC -Wall -Wextra -O0 -g # C flags
LDFLAGS = -shared  # linking flags
RM = rm -f  # rm command
TARGET_LIB = libcompression_dummy.so # target lib
ODIR = ./

SRCS = dummy_plugin.c # source files
OBJS = $(SRCS:.c=.o)

.PHONY: all
all: ${TARGET_LIB}

$(TARGET_LIB): $(OBJS)
	$(CC) ${LDFLAGS} -o $@ $^

.PHONY: clean
clean:
	-${RM} ${TARGET_LIB} ${OBJS} $(SRCS:.c=.a)
