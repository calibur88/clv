#include "CLV_Memory.h"

#include "gtest/gtest.h"
#include <cstring>

TEST(ClvMemory, AllocThenWriteThenFree)
{
	auto* p = static_cast<unsigned char*>(CLV_Alloc(64));
	ASSERT_NE(p, nullptr);
	for (size_t i = 0; i < 64; ++i) p[i] = static_cast<unsigned char>(i);
	for (size_t i = 0; i < 64; ++i) ASSERT_EQ(p[i], static_cast<unsigned char>(i));
	CLV_Free(p);
}

TEST(ClvMemory, ReallocPreservesThePrefix)
{
	auto* p = static_cast<unsigned char*>(CLV_Alloc(8));
	ASSERT_NE(p, nullptr);
	std::memcpy(p, "01234567", 8);

	auto* q = static_cast<unsigned char*>(CLV_Realloc(p, 4096));
	ASSERT_NE(q, nullptr);
	EXPECT_EQ(std::memcmp(q, "01234567", 8), 0) << "realloc 必须保住前 8 字节";
	q[4095] = 1;	// 新容量真的可用
	CLV_Free(q);
}

TEST(ClvMemory, ReallocFromNullBehavesLikeAlloc)
{
	auto* p = static_cast<unsigned char*>(CLV_Realloc(nullptr, 16));
	ASSERT_NE(p, nullptr);
	p[15] = 7;
	CLV_Free(p);
}

TEST(ClvMemory, FreeAcceptsNull)
{
	CLV_Free(nullptr);
	SUCCEED();
}
