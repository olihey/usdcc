// A DLL of the test plugin's own, found next to it (docs/PLUGIN_SDK.md).
#if defined(_WIN32)
extern "C" __declspec(dllexport) int usdcc_test_plugin_helper_value() { return 42; }
#else
extern "C" __attribute__((visibility("default"))) int usdcc_test_plugin_helper_value() { return 42; }
#endif
