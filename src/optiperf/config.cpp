#include "config.h"

#include <windows.h>

#include <iterator>

namespace optiperf {
namespace {

std::wstring g_dir, g_ini;

}  // namespace

void ConfigInit(const std::wstring& pluginDir) {
    g_dir = pluginDir;
    g_ini = pluginDir + L"\\optiperf.ini";
}

const std::wstring& PluginDir() { return g_dir; }

std::wstring ConfigString(const wchar_t* section, const wchar_t* key, const wchar_t* env, const wchar_t* defaultValue) {
    wchar_t buf[1024];
    if (env) {
        const DWORD n = GetEnvironmentVariableW(env, buf, DWORD(std::size(buf)));
        if (n > 0 && n < std::size(buf)) return buf;
    }
    if (!g_ini.empty()) {
        GetPrivateProfileStringW(section, key, defaultValue, buf, DWORD(std::size(buf)), g_ini.c_str());
        // strip trailing comments/whitespace ("fast ; comment")
        std::wstring v = buf;
        if (const size_t c = v.find(L';'); c != std::wstring::npos) v.erase(c);
        while (!v.empty() && (v.back() == L' ' || v.back() == L'\t')) v.pop_back();
        return v;
    }
    return defaultValue;
}

}  // namespace optiperf
