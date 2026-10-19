#pragma once
constexpr int SCE_NETCTL_STATE_DISCONNECTED = 0;
constexpr int SCE_NETCTL_STATE_CONNECTING = 1;
constexpr int SCE_NETCTL_STATE_FINALIZING = 2;
constexpr int SCE_NETCTL_STATE_CONNECTED = 3;
int sceNetCtlInit();
int sceNetCtlTerm();
int sceNetCtlInetGetState(int*);
