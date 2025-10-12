.PHONY: all clean compile compile-producer

all: compile compile-producer

compile: server.c
	mkdir -p build
	gcc -o build/server server.c -lrt -lpthread

compile-producer: producer.c
	mkdir -p build
	gcc -o build/producer producer.c -lrt -lpthread

clean:
	rm -rf build
