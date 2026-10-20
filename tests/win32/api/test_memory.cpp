#include "CLV_Memory.h"

#include "gtest/gtest.h"
#include <cstring>

TEST(ClvMemory, AllocThenWriteThenFree)
{
	/* 依据注：单块「写进去再读回来」只证这块内存可用，证不了两次分配互不侵犯——
	 * 分配口若把同一区间回给两个句柄，单块写法照样全绿。所以这里两块同时在用：
	 * 填满 a 之后 b 的图样必须一个字节都没动过。 */
	auto* a = static_cast<unsigned char*>(CLV_Alloc(64));
	auto* b = static_cast<unsigned char*>(CLV_Alloc(64));
	ASSERT_NE(a, nullptr);
	ASSERT_NE(b, nullptr);
	EXPECT_NE(a, b) << "两次分配拿到了同一个地址";
	for (size_t i = 0; i < 64; ++i) b[i] = static_cast<unsigned char>(0xA5u);
	for (size_t i = 0; i < 64; ++i) a[i] = static_cast<unsigned char>(i);
	for (size_t i = 0; i < 64; ++i)
	{
		ASSERT_EQ(a[i], static_cast<unsigned char>(i)) << "a 的第 " << i << " 字节不是写进去的那个值";
		ASSERT_EQ(b[i], static_cast<unsigned char>(0xA5u)) << "写 a 越界踩到了 b";
	}
	CLV_Free(a);
	CLV_Free(b);
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
	/* 契约：空指针释放是 no-op。可断言的不是「它没崩」这一句，而是它没把分配口本身弄坏——
	 * 连着两次空释放之后，分配 / 扩容 / 逐字节可用照常成立。 */
	CLV_Free(nullptr);
	CLV_Free(nullptr);
	auto* p = static_cast<unsigned char*>(CLV_Alloc(16));
	ASSERT_NE(p, nullptr);
	for (size_t i = 0; i < 16; ++i) p[i] = static_cast<unsigned char>(0x10u + i);
	p = static_cast<unsigned char*>(CLV_Realloc(p, 64));
	ASSERT_NE(p, nullptr);
	for (size_t i = 0; i < 16; ++i) EXPECT_EQ(p[i], static_cast<unsigned char>(0x10u + i)) << "扩容丢了前缀";
	p[63] = 1u;	   // 新容量那一端真能写
	EXPECT_EQ(p[63], 1u);
	CLV_Free(p);
	CLV_Free(nullptr);	  // 收尾再来一次，路径两头都不许留状态
}
