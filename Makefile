CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -pedantic -pthread

SERVER_OBJS = server.o database.o
CLIENT_OBJS = client.o flexql.o
BENCHMARK_OBJS = benchmark_flexql.o flexql.o

.PHONY: all clean

all: flexql-server flexql-client benchmark_flexql

flexql-server: $(SERVER_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $(SERVER_OBJS)

flexql-client: $(CLIENT_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $(CLIENT_OBJS)

benchmark_flexql: $(BENCHMARK_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $(BENCHMARK_OBJS)

server.o: server.cpp database.h
database.o: database.cpp database.h indexes.h
client.o: client.cpp flexql.h
benchmark_flexql.o: benchmark_flexql.cpp flexql.h
flexql.o: flexql.cpp flexql.h database.h

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f *.o flexql-server flexql-client benchmark_flexql
