/* UTF-8 校验后端：simdutf。 */
#include "subtitle/cal/cal.h"

#include <new>
#include <simdutf.h>

namespace clv
{
	namespace subtitle
	{
		namespace cal
		{
			namespace
			{
				class SimdutfValidator: public IUtf8Validator
				{
					public:

					bool Valid(const uint8_t* data, size_t size) override
					{
						if (data == nullptr && size != 0u) return false;
						if (size == 0u) return true;
						return simdutf::validate_utf8(reinterpret_cast<const char*>(data), size);
					}
				};
			}	 // namespace

			IUtf8Validator* CreateSimdutfValidator() { return new (std::nothrow) SimdutfValidator(); }
		}	 // namespace cal
	}	 // namespace subtitle
}	 // namespace clv
