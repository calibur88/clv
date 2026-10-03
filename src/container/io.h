/* io.h
 *
 * 容器层的字节源/汇抽象：容器核心不碰平台 IO，门面层把 CLV_File 适配进来，
 * 单测用内存实现（不落临时文件）。
 *
 * 写方需要 Seek + 回填（文件头的 total_packets / index_offset 在末尾才知道），
 * 读方需要顺序读 + 文件大小（重同步窗口按剩余量决定）。
 */
#ifndef CLV_CONTAINER_IO_H
#define CLV_CONTAINER_IO_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace clv
{
	namespace container
	{

		struct ByteSinkIf
		{
			virtual ~ByteSinkIf() = default;
			virtual bool Write(const uint8_t* data, size_t n) = 0;
			virtual bool Seek(uint64_t pos) = 0;
			virtual uint64_t Tell() = 0;
		};

		struct ByteSourceIf
		{
			virtual ~ByteSourceIf() = default;
			virtual bool Seek(uint64_t pos) = 0;
			// got 为实际读到字节数；到文件尾时返回 true 且 *got == 0；真失败返回 false
			virtual bool Read(uint8_t* data, size_t n, size_t* got) = 0;
			virtual uint64_t Size() = 0;
		};

		// 内存汇：写满可回跳改写，供单测与内存后端使用
		class MemorySink final: public ByteSinkIf
		{
			public:

			bool Write(const uint8_t* data, size_t n) override
			{
				if (data == nullptr && n != 0) return false;
				if (pos_ + n > buf_.size()) buf_.resize(pos_ + n, 0);
				if (n != 0) std::copy_n(data, n, buf_.begin() + static_cast<std::ptrdiff_t>(pos_));
				pos_ += n;
				return true;
			}

			// 只允许落在已有内容内或恰好末尾，不支持越尾预留空洞
			bool Seek(uint64_t pos) override
			{
				if (pos > buf_.size()) return false;
				pos_ = static_cast<size_t>(pos);
				return true;
			}

			uint64_t Tell() override { return pos_; }

			const std::vector<uint8_t>& Bytes() const noexcept { return buf_; }

			size_t Size() const noexcept { return buf_.size(); }

			private:

			std::vector<uint8_t> buf_;
			size_t pos_ = 0;
		};

		// 内存源：指向一段已有缓冲，不持有
		class MemorySource final: public ByteSourceIf
		{
			public:

			MemorySource(const uint8_t* data, size_t size) noexcept: data_(data), size_(size) {}

			explicit MemorySource(const std::vector<uint8_t>& v) noexcept: data_(v.data()), size_(v.size()) {}

			bool Seek(uint64_t pos) override
			{
				if (pos > size_) return false;
				pos_ = static_cast<size_t>(pos);
				return true;
			}

			bool Read(uint8_t* data, size_t n, size_t* got) override
			{
				if (got == nullptr) return false;
				if (data == nullptr && n != 0) return false;
				const size_t avail = size_ - pos_ < n ? size_ - pos_ : n;
				if (avail != 0) std::copy_n(data_ + pos_, avail, data);
				pos_ += avail;
				*got = avail;
				return true;
			}

			uint64_t Size() override { return size_; }

			private:

			const uint8_t* data_ = nullptr;
			size_t size_ = 0;
			size_t pos_ = 0;
		};

	}	 // namespace container
}	 // namespace clv

#endif	  // CLV_CONTAINER_IO_H
