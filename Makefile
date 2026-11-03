# Terminator — CS69201 Computing Lab term project
CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -g -std=c11 -Isrc
LDLIBS  = -lX11 -lpthread -lrt
TARGET  = terminator
SRC     = $(wildcard src/*.c)

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDLIBS)

clean:
	rm -f $(TARGET) src/*.o

test: $(TARGET)
	bash tests/run_tests.sh

.PHONY: all clean test
