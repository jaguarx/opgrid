CXX=g++
CC=gcc
SRC=src

all: test_cli test_node

test_cli: test-src/test_cli.cc
	${CXX} -g -I ${SRC} -pthread -std=c++11 $< -o test_cli

test_node: test-src/test_node.cc
	${CXX} -g -I ${SRC} -pthread -std=c++11 $< -o test_node 

cotest: test-src/cotest.c src/coroutine.c src/coroutine.h
	${CC} -g -I ${SRC} -o cotest test-src/cotest.c src/coroutine.c
clean:
	rm test_cli test_node
