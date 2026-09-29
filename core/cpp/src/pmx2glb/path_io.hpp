#pragma once

// Windows UTF-8 路径与二进制文件 IO。避开 MinGW 8.1 无法编译的 <filesystem>。
#include "utf.hpp"

#include <cstdint>
#include <cstdio>
#include <cwctype>
#include <string>
#include <utility>
#include <vector>

namespace go::pmx2glb::path_io {

inline std::wstring normalize(std::wstring path) {
    for (wchar_t& ch : path) {
        if (ch == L'/') ch = L'\\';
    }
    return path;
}

inline bool is_absolute(const std::wstring& path) {
    return (!path.empty() && path[0] == L'\\') ||
           (path.size() >= 2 && path[1] == L':');
}

inline std::wstring join(const std::wstring& base, const std::wstring& child) {
    std::wstring normalized = normalize(child);
    if (base.empty() || is_absolute(normalized)) return normalized;
    std::wstring result = normalize(base);
    if (!result.empty() && result.back() != L'\\') result.push_back(L'\\');
    result += normalized;
    return result;
}

inline std::wstring parent(const std::wstring& path) {
    const std::wstring normalized = normalize(path);
    const size_t slash = normalized.find_last_of(L'\\');
    if (slash == std::wstring::npos) return L".";
    if (slash == 0 || (slash == 2 && normalized[1] == L':')) return normalized.substr(0, slash + 1);
    return normalized.substr(0, slash);
}

inline std::wstring extension(const std::wstring& path) {
    const std::wstring normalized = normalize(path);
    const size_t slash = normalized.find_last_of(L'\\');
    const size_t dot = normalized.find_last_of(L'.');
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) return {};
    return normalized.substr(dot);
}

inline std::wstring replace_extension(const std::wstring& path, const wchar_t* ext) {
    const std::wstring normalized = normalize(path);
    const size_t slash = normalized.find_last_of(L'\\');
    const size_t dot = normalized.find_last_of(L'.');
    const size_t end = dot != std::wstring::npos &&
                       (slash == std::wstring::npos || dot > slash) ? dot : normalized.size();
    return normalized.substr(0, end) + ext;
}

inline bool exists(const std::wstring& path, bool require_file = false) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES &&
           (!require_file || (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0);
}

inline std::wstring lower(std::wstring value) {
    for (wchar_t& ch : value) ch = static_cast<wchar_t>(std::towlower(ch));
    return value;
}

inline bool find_ci_child(const std::wstring& dir, const std::wstring& name,
                          std::wstring* out) {
    WIN32_FIND_DATAW data{};
    const std::wstring pattern = join(dir, L"*");
    HANDLE handle = FindFirstFileW(pattern.c_str(), &data);
    if (handle == INVALID_HANDLE_VALUE) return false;
    const std::wstring wanted = lower(name);
    bool found = false;
    do {
        if (lower(data.cFileName) == wanted) {
            *out = join(dir, data.cFileName);
            found = true;
            break;
        }
    } while (FindNextFileW(handle, &data));
    FindClose(handle);
    return found;
}

inline bool resolve_relative(const std::wstring& base, const std::wstring& relative,
                             std::wstring* out) {
    const std::wstring direct = join(base, relative);
    if (exists(direct, true)) {
        *out = direct;
        return true;
    }
    std::wstring current = normalize(base);
    std::wstring rel = normalize(relative);
    size_t offset = 0;
    while (offset < rel.size()) {
        const size_t slash = rel.find(L'\\', offset);
        const std::wstring part = rel.substr(offset, slash - offset);
        offset = slash == std::wstring::npos ? rel.size() : slash + 1;
        if (part.empty() || part == L".") continue;
        if (part == L"..") {
            current = parent(current);
            continue;
        }
        std::wstring next;
        if (!find_ci_child(current, part, &next)) return false;
        current = std::move(next);
    }
    if (!exists(current, true)) return false;
    *out = std::move(current);
    return true;
}

inline bool read_bytes(const std::wstring& path, std::vector<uint8_t>* out) {
    FILE* file = _wfopen(path.c_str(), L"rb");
    if (!file) return false;
    bool ok = _fseeki64(file, 0, SEEK_END) == 0;
    const __int64 size = ok ? _ftelli64(file) : -1;
    ok = size >= 0 && static_cast<unsigned long long>(size) <= SIZE_MAX &&
         _fseeki64(file, 0, SEEK_SET) == 0;
    if (ok) {
        out->resize(static_cast<size_t>(size));
        ok = out->empty() || std::fread(out->data(), 1, out->size(), file) == out->size();
    }
    ok = std::fclose(file) == 0 && ok;
    if (!ok) out->clear();
    return ok;
}

inline bool write_bytes(const std::wstring& path, const std::vector<uint8_t>& bytes) {
    FILE* file = _wfopen(path.c_str(), L"wb");
    if (!file) return false;
    const bool wrote = bytes.empty() ||
                       std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
    return std::fclose(file) == 0 && wrote;
}

}  // namespace go::pmx2glb::path_io
