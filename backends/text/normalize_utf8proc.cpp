/* NFC 规范化后端：utf8proc。 */
#include "subtitle/cal/cal.h"

#include <cstring>
#include <new>
#include <utf8proc.h>
#include <vector>

namespace clv
{
	namespace subtitle
	{
		namespace cal
		{
			namespace
			{
				class Utf8procNormalizer: public INormalizer
				{
					public:

					Err Nfc(const uint8_t* in, size_t in_len, uint8_t* out, size_t cap, size_t* out_len) override
					{
						if (out_len == nullptr || (in == nullptr && in_len != 0u) || (out == nullptr && cap != 0u))
							return kInvalidArgument;
						if (in_len == 0u)
						{
							*out_len = 0u;
							return kOk;
						}
						std::vector<utf8proc_uint8_t> tmp(in, in + in_len);
						tmp.push_back(0);
						utf8proc_uint8_t* nfc = utf8proc_NFC(tmp.data());
						if (nfc == nullptr) return kBackendFailure;
						const size_t n = std::strlen(reinterpret_cast<const char*>(nfc));
						*out_len = n;
						Err rc = kOk;
						if (n > cap) rc = kOutputTooSmall;
						else if (n != 0u) std::memcpy(out, nfc, n);
						utf8proc_free(nfc);
						return rc;
					}
				};
			}	 // namespace

			INormalizer* CreateUtf8procNormalizer() { return new (std::nothrow) Utf8procNormalizer(); }
		}	 // namespace cal
	}	 // namespace subtitle
}	 // namespace clv
