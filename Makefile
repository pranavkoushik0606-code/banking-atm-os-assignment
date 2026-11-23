CC ?= cc
CFLAGS ?= -std=c11 -O0 -g -Wall -Wextra -Wpedantic
LDLIBS = -pthread -lm
.PHONY: all run test clean
all: build/banking_server build/banking_sim
build:
	mkdir -p build
build/banking_server: src/server.c src/banking_sim.c src/banking.h | build
	$(CC) $(CFLAGS) src/server.c src/banking_sim.c -o $@ $(LDLIBS)
build/banking_sim: src/banking_sim.c src/banking.h | build
	$(CC) $(CFLAGS) -DBANKING_CLI src/banking_sim.c -o $@ $(LDLIBS)
run: build/banking_server
	./build/banking_server public
test: all
	python3 tests/test_integration.py
clean:
	rm -rf build
