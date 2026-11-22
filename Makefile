CC ?= gcc
CFLAGS ?= -Wall -Wextra -std=c11 -O2 -Iinclude -Isrc
LDFLAGS ?= -lm

SRCS = src/value.c src/chunk.c src/vm.c src/lexer.c src/compiler.c src/api.c src/main.c
OBJS = $(SRCS:.c=.o)

ifeq ($(OS),Windows_NT)
    TARGET = chibidart.exe
    ifeq ($(findstring sh,$(SHELL)),sh)
        RM = rm -f
        CLEAN_OBJS = $(OBJS) $(TARGET)
    else ifeq ($(findstring bash,$(SHELL)),bash)
        RM = rm -f
        CLEAN_OBJS = $(OBJS) $(TARGET)
    else
        RM = del /Q /F
        CLEAN_OBJS = $(subst /,\,$(OBJS)) $(TARGET)
    endif
else
    TARGET = chibidart
    RM = rm -f
    CLEAN_OBJS = $(OBJS) $(TARGET)
endif

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $(TARGET) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	-$(RM) $(CLEAN_OBJS)
