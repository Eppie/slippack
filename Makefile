CXX      ?= clang++
CXXFLAGS ?= -std=c++20 -O3 -Wall -Wextra
UNAME_S  := $(shell uname -s)

ifeq ($(UNAME_S),Darwin)
ZSTD_INC ?= /opt/homebrew/include
ZSTD_LIB ?= /opt/homebrew/lib
PLATFORM_LIBS :=
else
ZSTD_INC ?= /usr/include
ZSTD_LIB ?= /usr/lib/x86_64-linux-gnu
CXXFLAGS += -march=native
PLATFORM_LIBS := -lcrypto
endif

slpz: slpz.cpp
	$(CXX) $(CXXFLAGS) -I$(ZSTD_INC) -o $@ $< -L$(ZSTD_LIB) -lzstd -llzma -lbrotlienc -lbrotlidec $(PLATFORM_LIBS)

clean:
	rm -f slpz

.PHONY: clean
