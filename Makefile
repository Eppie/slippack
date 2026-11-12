CXX      ?= clang++
CLANG_FORMAT ?= clang-format
CXXFLAGS ?= -std=c++20 -O3 -Wall -Wextra
BUILD_DIR ?= build
PROGRAM ?= slippack
PROFILE ?=
.DEFAULT_GOAL := all
UNAME_S  := $(shell uname -s)

ifeq ($(UNAME_S),Darwin)
PLATFORM_LIBS :=
LLVM_PROFDATA ?= xcrun llvm-profdata
else
CXXFLAGS += -march=native
PLATFORM_LIBS := -lcrypto
LLVM_PROFDATA ?= llvm-profdata
endif

CORE := archive frames record_codec slp platform
OBJECTS := $(addprefix $(BUILD_DIR)/,$(addsuffix .o,$(CORE)))
HEADERS := $(wildcard *.h)

ifneq ($(PROFILE),)
PROFILE_DATA := $(BUILD_DIR)/profile.profdata
PROFILE_FLAGS := -fprofile-use=$(PROFILE_DATA)
ifeq ($(UNAME_S),Darwin)
# Apple's linker deduplication regresses this profiled build's encoding speed.
PROFILE_LINK_FLAGS := -Wl,-no_deduplicate
endif
$(PROFILE_DATA): $(PROFILE)
	mkdir -p $(BUILD_DIR)
	$(LLVM_PROFDATA) merge -o $@ $<
endif

all: $(PROGRAM)

$(PROGRAM): $(BUILD_DIR)/slippack.o $(BUILD_DIR)/file_io.o $(BUILD_DIR)/libslippack.a
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) $(PROFILE_LINK_FLAGS) $(PLATFORM_LIBS) -pthread

$(BUILD_DIR)/%.o: %.cpp Makefile $(PROFILE_DATA)
	mkdir -p $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(PROFILE_FLAGS) -MMD -MP -pthread -c $< -o $@

$(BUILD_DIR)/libslippack.a: $(OBJECTS)
	$(AR) rcs $@ $^

$(BUILD_DIR)/custom_codec_test: tests/custom_codec_test.cpp $(HEADERS)
	mkdir -p $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ $<

$(BUILD_DIR)/%_test: tests/%_test.cpp $(BUILD_DIR)/libslippack.a $(HEADERS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ $< $(BUILD_DIR)/libslippack.a $(LDFLAGS) $(PLATFORM_LIBS) -pthread

check: $(PROGRAM) $(addprefix $(BUILD_DIR)/,custom_codec_test entropy_model_test record_codec_test predictor_test archive_api_test)
	$(abspath $(PROGRAM)) selftest
	$(abspath $(BUILD_DIR))/custom_codec_test
	$(abspath $(BUILD_DIR))/entropy_model_test
	$(abspath $(BUILD_DIR))/record_codec_test
	$(abspath $(BUILD_DIR))/predictor_test
	$(abspath $(BUILD_DIR))/archive_api_test
	SLIPPACK_BINARY="$(abspath $(PROGRAM))" python3 tests/test_archive.py
	SLIPPACK_BINARY="$(abspath $(PROGRAM))" python3 tests/test_extraction.py

release:
	python3 tools/profile_build.py check --cxx "$(CXX)"
	$(MAKE) BUILD_DIR=build/release PROGRAM=build/release/slippack PROFILE=profiles/level3.proftext all
	cp build/release/slippack build/release/slippack.install
	mv build/release/slippack.install slippack

release-check: release
	$(MAKE) BUILD_DIR=build/release PROGRAM=build/release/slippack PROFILE=profiles/level3.proftext check

clean:
	rm -f $(PROGRAM) $(BUILD_DIR)/*.o $(BUILD_DIR)/*.d $(BUILD_DIR)/libslippack.a $(BUILD_DIR)/*_test

-include $(wildcard $(BUILD_DIR)/*.d)
FORMAT_SOURCES := $(wildcard *.cpp *.h tests/*.cpp fuzz/*.cpp benchmarks/*/*.cpp benchmarks/*/*.h)

format:
	$(CLANG_FORMAT) -i $(FORMAT_SOURCES)

check-format:
	$(CLANG_FORMAT) --dry-run --Werror $(FORMAT_SOURCES)

.PHONY: all clean check release release-check format check-format
