#include "CLV_Version.h"

namespace clv
{

	// 单一来源：三段数字只在 CLV_Version.h 定义，这里给 C++ 侧一份带类型的读法
	constexpr int Major = CLV_VERSION_MAJOR;
	constexpr int Minor = CLV_VERSION_MINOR;
	constexpr int Patch = CLV_VERSION_PATCH;

}	 // namespace clv

extern "C" const char* CLV_VersionString(void) { return CLV_VERSION_STRING; }

extern "C" int CLV_VersionMajor(void) { return clv::Major; }

extern "C" int CLV_VersionMinor(void) { return clv::Minor; }

extern "C" int CLV_VersionPatch(void) { return clv::Patch; }
