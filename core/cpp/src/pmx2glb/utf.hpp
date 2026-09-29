#pragma once

// go::pmx2glb —— UTF-16 ↔ UTF-8 转换小工具（内部实现，不进公开头）。
//
// 为什么不用 std::codecvt：C++17 起已弃用，各实现行为不一致；Windows 下
// 直接走 Win32 WideCharToMultiByte(CP_UTF8)，语义官方且有保证。
// 本仓库约束允许 Win32 API（core 禁的是 Qt，不禁 Win32）。

#ifndef _WIN32
#error "go_pmx2glb 目前仅支持 Windows（MMD/PMX 生态与部署目标均为 Windows）。"
#endif

#ifndef NOMINMAX
#define NOMINMAX  // windows.h 的 min/max 宏会打坏 std::min/max 调用
#endif
#include <windows.h>

#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

namespace go::pmx2glb {

// UTF-16LE 字节序列 → UTF-8。非法序列按 Win32 默认替换为 U+FFFD 而不是报错：
// PMX 文本字段偶见损坏，转换器要尽量保住其余内容而非整体失败。
// 为什么先 memcpy 到 wchar_t 缓冲：PMX 字符串数据不保证 2 字节对齐，
// 把未对齐字节直接 reinterpret 成 wchar_t* 是未定义行为。
inline std::string utf16le_to_utf8(const char* data, size_t byte_len)
{
    if (data == nullptr || byte_len < 2) {
        return {};
    }
    const int wchars = static_cast<int>(byte_len / 2);
    std::vector<wchar_t> buf(static_cast<size_t>(wchars));
    // byte_len < 2 已挡掉非偶数长度的一半风险；剩下的一半字符被丢弃（len 截断）。
    for (int i = 0; i < wchars; ++i) {
        std::memcpy(&buf[static_cast<size_t>(i)], data + static_cast<size_t>(i) * 2, 2);
    }
    int n = WideCharToMultiByte(CP_UTF8, 0, buf.data(), wchars, nullptr, 0, nullptr, nullptr);
    if (n <= 0) {
        return {};
    }
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, buf.data(), wchars, &out[0], n, nullptr, nullptr);
    return out;
}

// UTF-8 → wchar_t（Windows UTF-16），用于所有宽字符文件 API。
inline std::wstring utf8_to_wide(const std::string& text)
{
    if (text.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                      static_cast<int>(text.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                            static_cast<int>(text.size()), &out[0], n) != n) {
        return {};
    }
    return out;
}

// wchar_t（Windows 上即 UTF-16）→ UTF-8，用于 wmain 的 argv。
inline std::string wide_to_utf8(const wchar_t* w, int wchars)
{
    if (w == nullptr || wchars <= 0) {
        return {};
    }
    int n = WideCharToMultiByte(CP_UTF8, 0, w, wchars, nullptr, 0, nullptr, nullptr);
    if (n <= 0) {
        return {};
    }
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, wchars, &out[0], n, nullptr, nullptr);
    return out;
}

}  // namespace go::pmx2glb
