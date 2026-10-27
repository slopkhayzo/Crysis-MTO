#pragma once
// Settings come from optiperf.ini next to the plugin; an environment variable (used by test scripts) overrides a key.

#include <string>

namespace optiperf {

void ConfigInit(const std::wstring& pluginDir);
const std::wstring& PluginDir();

// [section] key from the ini, unless the environment variable `env` is set.
std::wstring ConfigString(const wchar_t* section, const wchar_t* key, const wchar_t* env, const wchar_t* defaultValue);

}  // namespace optiperf
