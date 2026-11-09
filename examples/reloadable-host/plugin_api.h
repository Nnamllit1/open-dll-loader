#pragma once
#include <stdint.h>

#ifdef _WIN32
#define ODL_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#define ODL_PLUGIN_CALL __cdecl
#else
#define ODL_PLUGIN_EXPORT extern "C" __attribute__((visibility("default")))
#define ODL_PLUGIN_CALL
#endif

// A cooperative ABI for hosts that you control. Nothing is injected.
// All callbacks run serially on the host's main loop, outside DllMain.
enum { ODL_PLUGIN_ABI = 1 };
struct OdlHostV1 {
    uint32_t size;
    uint32_t abi;
    void (ODL_PLUGIN_CALL *log)(const char* utf8);
};
struct OdlStateV1 { uint64_t ticks; };
struct OdlPluginV1 {
    uint32_t size;
    uint32_t abi;
    const char* name;
    int32_t (ODL_PLUGIN_CALL *start)(const OdlHostV1*, OdlStateV1*);
    // Return 1 only after joining workers and removing every callback/hook.
    // Return 0 to refuse unloading. The host then keeps this module mapped.
    int32_t (ODL_PLUGIN_CALL *stop)();
    void (ODL_PLUGIN_CALL *tick)();
};
using OdlGetPluginV1 = const OdlPluginV1* (ODL_PLUGIN_CALL *)();
