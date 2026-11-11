CXX      ?= clang++
CLANG_FORMAT ?= clang-format
CXXFLAGS ?= -std=c++20 -O3 -Wall -Wextra
UNAME_S  := $(shell uname -s)

ifeq ($(UNAME_S),Darwin)
PLATFORM_LIBS :=
else
CXXFLAGS += -march=native
PLATFORM_LIBS := -lcrypto
endif

CORE := archive frames record_codec slp platform
OBJECTS := $(addprefix build/,$(addsuffix .o,$(CORE)))
HEADERS := $(wildcard *.h)

slippack: build/slippack.o build/file_io.o build/libslippack.a
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) $(PLATFORM_LIBS) -pthread

build/%.o: %.cpp
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -pthread -c $< -o $@

build/libslippack.a: $(OBJECTS)
	$(AR) rcs $@ $^

build/custom_codec_test: tests/custom_codec_test.cpp $(HEADERS)
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ $<

build/%_test: tests/%_test.cpp build/libslippack.a $(HEADERS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ $< build/libslippack.a $(LDFLAGS) $(PLATFORM_LIBS) -pthread

check: slippack build/custom_codec_test build/record_codec_test build/predictor_test build/archive_api_test
	./slippack selftest
	./build/custom_codec_test
	./build/record_codec_test
	./build/predictor_test
	./build/archive_api_test
	python3 tests/test_archive.py
	python3 tests/test_extraction.py

clean:
	rm -f slippack build/*.o build/*.d build/libslippack.a build/*_test

-include $(wildcard build/*.d)
FORMAT_SOURCES := $(wildcard *.cpp *.h tests/*.cpp fuzz/*.cpp benchmarks/*/*.cpp benchmarks/*/*.h)

format:
	$(CLANG_FORMAT) -i $(FORMAT_SOURCES)

check-format:
	$(CLANG_FORMAT) --dry-run --Werror $(FORMAT_SOURCES)

.PHONY: clean check format check-format
