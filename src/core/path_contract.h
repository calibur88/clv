/* path_contract.h
 *
 * 公开面路径判定的唯一定义处：所有吃路径的入口都判这一条，两处判定不许漂。
 *
 * - UTF-8 字节串（不含 BOM）。
 * - 只接受 '/' 作分隔符；NULL、空串、含 '\\' 的路径都在契约层拒收。
 * - 判定不碰任何平台代码，所以后端拿到的路径已经不含反斜杠。
 *
 * 字节序不是合法 UTF-8 时本件判不出来 —— 那是各后端自己的事：文件面走宽字符转换，
 * 转不了就在打开处失败；落盘口不转宽字符，由窄字符运行时决定。
 */
#ifndef CLV_CORE_PATH_CONTRACT_H
#define CLV_CORE_PATH_CONTRACT_H

namespace clv::core
{

	inline bool IsPathValid(const char* path) noexcept
	{
		if (path == nullptr || path[0] == '\0') return false;
		for (const char* p = path; *p != '\0'; ++p)
			if (*p == '\\') return false;
		return true;
	}

}	 // namespace clv::core

#endif	  // CLV_CORE_PATH_CONTRACT_H
