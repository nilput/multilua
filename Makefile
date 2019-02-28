LDLIBS := $(shell pkg-config --libs lua5.1) 
CFLAGS := -Wall $(shell pkg-config --cflags lua5.1) -pthread

all: CFLAGS += -O2
all: main
debug: CFLAGS += -g3 -O0
debug: main

main : main.c
