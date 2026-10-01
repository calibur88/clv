#include "CLV_Memory.h"

#include <cstdlib>

extern "C" void* CLV_Alloc(size_t size) { return std::malloc(size); }

extern "C" void* CLV_Realloc(void* p, size_t size) { return std::realloc(p, size); }

extern "C" void CLV_Free(void* p) { std::free(p); }
