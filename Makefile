CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -O2 -g -Iinclude -Isrc

SRCS = src/value.c src/chunk.c src/vm.c src/lexer.c src/compiler.c src/api.c src/main.c
OBJS = $(SRCS:.c=.o)
TARGET = chibidart.exe

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $(TARGET)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)
