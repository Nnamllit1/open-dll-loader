#include "plugin_api.h"
#include <string>

#ifndef ODL_SAMPLE_REVISION
#define ODL_SAMPLE_REVISION 1
#endif
namespace {
const OdlHostV1* host{};
OdlStateV1* state{};
std::string label = "Example plugin revision " + std::to_string(ODL_SAMPLE_REVISION);
int32_t ODL_PLUGIN_CALL start(const OdlHostV1* api, OdlStateV1* saved) {
    if (!api || api->abi != ODL_PLUGIN_ABI || api->size < sizeof(OdlHostV1) || !saved) return 0;
    host = api; state = saved; host->log((label + " started; preserved ticks=" + std::to_string(state->ticks)).c_str()); return 1;
}
int32_t ODL_PLUGIN_CALL stop() {
#ifdef ODL_SAMPLE_REFUSE_STOP
    if (host) host->log("Stop refused by test plugin; keep the module loaded.");
    return 0;
#else
    if (host) host->log((label + " stopped cleanly.").c_str());
    host = nullptr; state = nullptr; return 1;
#endif
}
void ODL_PLUGIN_CALL tick() { if (state) ++state->ticks; }
const OdlPluginV1 plugin{sizeof(OdlPluginV1), ODL_PLUGIN_ABI, label.c_str(), start, stop, tick};
}
ODL_PLUGIN_EXPORT const OdlPluginV1* ODL_PLUGIN_CALL odl_plugin_v1() { return &plugin; }
