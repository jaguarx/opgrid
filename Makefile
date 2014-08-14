CXX=g++
SRC=src
test_cli: test-src/test_cli.cc
	${CXX} -I ${SRC} -pthread -std=c++11 $< -o test_cli
 
