#include "CLV_Version.h"

namespace clv
{

	// 单一来源：三段数字只在 CLV_Version.h 定义，这里给 C++ 侧一份带类型的读法
	constexpr int Major = CLV_VERSION_MAJOR;
	constexpr int Minor = CLV_VERSION_MINOR;
	constexpr int Patch = CLV_VERSION_PATCH;

	/* bit1 读的是构建选项那一格（与 src/core/logger.cpp 选后端用的是同一个定义），
	 * 不在这里另判一次「后端到底是谁」：两处判据必须同真同假，所以只能同源读。 */
#if defined(CLV_LOGGER_USE_SPDLOG)
	constexpr uint32_t kLogStorageBit = CLV_CAP_LOG_STORAGE;
#else
	constexpr uint32_t kLogStorageBit = 0u;
#endif

	/* bit0 恒 1 = 内存缓冲句柄是实现态；bit2 恒 0 = 宿主注入分配器不做，且那位永不赋予新义。
	 * 两格都写成常量而不是探测结果：这一问的答案不该依赖任何运行时状态。 */
	constexpr uint32_t kCapabilities = CLV_CAP_MEMORY_FILE | kLogStorageBit;

}	 // namespace clv

extern "C" const char* CLV_VersionString(void) { return CLV_VERSION_STRING; }

extern "C" int CLV_VersionMajor(void) { return clv::Major; }

extern "C" int CLV_VersionMinor(void) { return clv::Minor; }

extern "C" int CLV_VersionPatch(void) { return clv::Patch; }

extern "C" uint32_t CLV_Capabilities(void) { return clv::kCapabilities; }
