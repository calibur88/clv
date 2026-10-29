/* Bidi 后端：SheenBidi 的 SBText 一族。
 * run 数组是视觉序，run 是 level 段不是 script 段；off / len 按 UTF-8 字节计。 */
#include "subtitle/cal/cal.h"

#include <SheenBidi.h>
#include <new>

namespace clv
{
	namespace subtitle
	{
		namespace cal
		{
			namespace
			{
				struct TextBundle
				{
					SBTextConfigRef cfg = nullptr;
					SBTextRef text = nullptr;

					TextBundle(const uint8_t* utf8, size_t len)
					{
						if (utf8 == nullptr || len == 0u) return;
						cfg = SBTextConfigCreate();
						if (cfg == nullptr) return;
						text = SBTextCreate(utf8, static_cast<SBUInteger>(len), SBStringEncodingUTF8, cfg);
						if (text == nullptr)
						{
							SBTextConfigRelease(cfg);
							cfg = nullptr;
						}
					}

					~TextBundle()
					{
						if (text != nullptr) SBTextRelease(text);
						if (cfg != nullptr) SBTextConfigRelease(cfg);
					}

					TextBundle(const TextBundle&) = delete;
					TextBundle& operator=(const TextBundle&) = delete;

					bool Ok() const { return text != nullptr; }

					SBParagraphInfo Paragraph() const
					{
						SBParagraphInfo pi;
						pi.index = 0;
						pi.length = 0;
						pi.baseLevel = 0;
						SBTextGetCodeUnitParagraphInfo(text, 0, &pi);
						return pi;
					}
				};

				class SheenBidiEngine: public IBidiEngine
				{
					public:

					Err Paragraph(const uint8_t* utf8, size_t len, int32_t* base_level, int32_t* run_count) override
					{
						if (base_level == nullptr || run_count == nullptr || (utf8 == nullptr && len != 0u))
							return kInvalidArgument;
						if (len == 0u)
						{
							*base_level = 0;
							*run_count = 0;
							return kOk;
						}
						const TextBundle bundle(utf8, len);
						if (! bundle.Ok()) return kBackendFailure;
						const SBParagraphInfo pi = bundle.Paragraph();
						*base_level = static_cast<int32_t>(pi.baseLevel);

						SBVisualRunIteratorRef it = SBTextCreateVisualRunIterator(bundle.text, pi.index, pi.length);
						if (it == nullptr) return kBackendFailure;
						int32_t n = 0;
						while (SBVisualRunIteratorMoveNext(it) == SBTrue) ++n;
						SBVisualRunIteratorRelease(it);
						*run_count = n;
						return kOk;
					}

					Err Runs(const uint8_t* utf8, size_t len, BidiRun* runs, size_t cap, size_t* run_count) override
					{
						if (run_count == nullptr || (utf8 == nullptr && len != 0u) || (runs == nullptr && cap != 0u))
							return kInvalidArgument;
						*run_count = 0u;
						if (len == 0u) return kOk;
						const TextBundle bundle(utf8, len);
						if (! bundle.Ok()) return kBackendFailure;
						const SBParagraphInfo pi = bundle.Paragraph();
						SBVisualRunIteratorRef it = SBTextCreateVisualRunIterator(bundle.text, pi.index, pi.length);
						if (it == nullptr) return kBackendFailure;
						size_t n = 0;
						while (SBVisualRunIteratorMoveNext(it) == SBTrue)
						{
							const SBVisualRun* r = SBVisualRunIteratorGetCurrent(it);
							if (n < cap && r != nullptr)
							{
								runs[n].off = static_cast<uint32_t>(r->index);
								runs[n].len = static_cast<uint32_t>(r->length);
								runs[n].level = static_cast<int32_t>(r->level);
							}
							++n;
						}
						SBVisualRunIteratorRelease(it);
						*run_count = n;
						return n > cap ? kOutputTooSmall : kOk;
					}
				};
			}	 // namespace

			IBidiEngine* CreateSheenBidiEngine() { return new (std::nothrow) SheenBidiEngine(); }
		}	 // namespace cal
	}	 // namespace subtitle
}	 // namespace clv
