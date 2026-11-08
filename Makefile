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

slpz: slpz.cpp custom_codec.h static_context.h mask_codec.h
	$(CXX) $(CXXFLAGS) -I$(ZSTD_INC) -o $@ $< -L$(ZSTD_LIB) -lzstd -llzma -lbrotlienc -lbrotlidec $(PLATFORM_LIBS)

clean:
	rm -f slpz

build/custom_codec_test: tests/custom_codec_test.cpp custom_codec.h static_context.h mask_codec.h
	mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ $<

check: slpz build/custom_codec_test
	./slpz selftest
	./build/custom_codec_test
	python3 tests/test_archive.py

.PHONY: clean check
