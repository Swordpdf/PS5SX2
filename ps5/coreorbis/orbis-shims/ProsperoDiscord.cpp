#include <discord_rpc.h>

extern "C" void Discord_Initialize(const char*, DiscordEventHandlers*, int, const char*) {}
extern "C" void Discord_Shutdown() {}
extern "C" void Discord_RunCallbacks() {}
extern "C" void Discord_UpdatePresence(const DiscordRichPresence*) {}
extern "C" void Discord_ClearPresence() {}
