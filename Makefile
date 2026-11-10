CXX      ?= clang++
CXXFLAGS ?= -std=c++20 -O3 -Wall -Wextra
UNAME_S  := $(shell uname -s)

ifeq ($(UNAME_S),Darwin)
PLATFORM_LIBS :=
else
CXXFLAGS += -march=native
PLATFORM_LIBS := -lcrypto
endif

slpz: slpz.cpp custom_codec.h static_context.h mask_codec.h
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ $< $(LDFLAGS) $(PLATFORM_LIBS)

clean:
	rm -f slpz

build/custom_codec_test: tests/custom_codec_test.cpp custom_codec.h static_context.h mask_codec.h
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ $<

build/record_codec_test: tests/record_codec_test.cpp slpz.cpp custom_codec.h static_context.h mask_codec.h
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -Wno-unused-function -o $@ $< $(LDFLAGS) $(PLATFORM_LIBS)

build/predictor_test: tests/predictor_test.cpp slpz.cpp custom_codec.h static_context.h mask_codec.h
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -Wno-unused-function -o $@ $< $(LDFLAGS) $(PLATFORM_LIBS)

check: slpz build/custom_codec_test build/record_codec_test build/predictor_test
	./slpz selftest
	./build/custom_codec_test
	./build/record_codec_test
	./build/predictor_test
	python3 tests/test_archive.py

.PHONY: clean check
