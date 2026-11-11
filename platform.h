#pragma once
#include "binary_io.h"
namespace slippack {
std::string sha1(View data);
void release_unused_memory();
} // namespace slippack
