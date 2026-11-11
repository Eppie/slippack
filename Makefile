CXX      ?= clang++
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

slpz: build/slpz.o build/file_io.o build/libslpz.a
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) $(PLATFORM_LIBS) -pthread

build/%.o: %.cpp
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -pthread -c $< -o $@

build/libslpz.a: $(OBJECTS)
	$(AR) rcs $@ $^

build/custom_codec_test: tests/custom_codec_test.cpp $(HEADERS)
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ $<

build/%_test: tests/%_test.cpp build/libslpz.a $(HEADERS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ $< build/libslpz.a $(LDFLAGS) $(PLATFORM_LIBS) -pthread

check: slpz build/custom_codec_test build/record_codec_test build/predictor_test build/archive_api_test
	./slpz selftest
	./build/custom_codec_test
	./build/record_codec_test
	./build/predictor_test
	./build/archive_api_test
	python3 tests/test_archive.py
	python3 tests/test_extraction.py

clean:
	rm -f slpz build/*.o build/*.d build/libslpz.a build/*_test

-include $(wildcard build/*.d)
.PHONY: clean check
