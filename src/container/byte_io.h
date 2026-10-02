/* byte_io.h
 *
 * 容器结构的小端读写：线上整数一律小端。
 * 读一律有界：任一次取数超出剩余长度即置失败，此后每次读都失败，游标不越界。
 */
#ifndef CLV_CONTAINER_BYTE_IO_H
#define CLV_CONTAINER_BYTE_IO_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace clv
{
	namespace container
	{

		inline void WriteLE16(uint8_t* p, uint16_t v) noexcept
		{
			p[0] = static_cast<uint8_t>(v);
			p[1] = static_cast<uint8_t>(v >> 8);
		}

		inline void WriteLE32(uint8_t* p, uint32_t v) noexcept
		{
			for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
		}

		inline void WriteLE64(uint8_t* p, uint64_t v) noexcept
		{
			for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
		}

		inline uint16_t ReadLE16(const uint8_t* p) noexcept
		{
			return static_cast<uint16_t>(p[0]) | static_cast<uint16_t>(p[1] << 8);
		}

		inline uint32_t ReadLE32(const uint8_t* p) noexcept
		{
			uint32_t v = 0;
			for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(p[i]) << (8 * i);
			return v;
		}

		inline uint64_t ReadLE64(const uint8_t* p) noexcept
		{
			uint64_t v = 0;
			for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
			return v;
		}

		// 尾部追加式写字节缓冲
		class Sink
		{
			public:

			explicit Sink(std::vector<uint8_t>& buf) noexcept: buf_(buf) {}

			void U8(uint8_t v) { buf_.push_back(v); }

			void LE16(uint16_t v)
			{
				uint8_t b[2];
				WriteLE16(b, v);
				Bytes(b, 2);
			}

			void LE32(uint32_t v)
			{
				uint8_t b[4];
				WriteLE32(b, v);
				Bytes(b, 4);
			}

			void LE64(uint64_t v)
			{
				uint8_t b[8];
				WriteLE64(b, v);
				Bytes(b, 8);
			}

			void Bytes(const void* data, size_t n)
			{
				if (data == nullptr || n == 0) return;
				const uint8_t* p = static_cast<const uint8_t*>(data);
				buf_.insert(buf_.end(), p, p + n);
			}

			size_t Size() const noexcept { return buf_.size(); }

			private:

			std::vector<uint8_t>& buf_;
		};

		// 有界读游标：先定位再前进，失败后一律拒绝
		class Cursor
		{
			public:

			Cursor(const uint8_t* data, size_t size) noexcept: p_(data), n_(size)
			{
				if (data == nullptr) p_ = reinterpret_cast<const uint8_t*>(""), n_ = 0;
			}

			bool U8(uint8_t* v) noexcept
			{
				const uint8_t* at = nullptr;
				if (! Take(1, &at)) return false;
				*v = at[0];
				return true;
			}

			bool LE16(uint16_t* v) noexcept
			{
				const uint8_t* at = nullptr;
				if (! Take(2, &at)) return false;
				*v = ReadLE16(at);
				return true;
			}

			bool LE32(uint32_t* v) noexcept
			{
				const uint8_t* at = nullptr;
				if (! Take(4, &at)) return false;
				*v = ReadLE32(at);
				return true;
			}

			bool LE64(uint64_t* v) noexcept
			{
				const uint8_t* at = nullptr;
				if (! Take(8, &at)) return false;
				*v = ReadLE64(at);
				return true;
			}

			// 取 n 字节，*at 指向本段起点；越界则整次失败
			bool Take(size_t n, const uint8_t** at) noexcept
			{
				if (failed_ || at == nullptr || n > n_ - i_)
				{
					failed_ = true;
					return false;
				}
				*at = p_ + i_;
				i_ += n;
				return true;
			}

			bool CopyTo(std::vector<uint8_t>& dst, size_t n)
			{
				const uint8_t* at = nullptr;
				if (! Take(n, &at)) return false;
				dst.assign(at, at + n);
				return true;
			}

			size_t Used() const noexcept { return i_; }

			size_t Size() const noexcept { return n_; }

			size_t Remain() const noexcept { return failed_ ? 0 : n_ - i_; }

			const uint8_t* Base() const noexcept { return p_; }

			bool Failed() const noexcept { return failed_; }

			private:

			const uint8_t* p_ = nullptr;
			size_t n_ = 0;
			size_t i_ = 0;
			bool failed_ = false;
		};

	}	 // namespace container
}	 // namespace clv

#endif	  // CLV_CONTAINER_BYTE_IO_H
