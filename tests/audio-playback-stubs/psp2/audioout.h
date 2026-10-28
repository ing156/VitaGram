#pragma once
enum SceAudioOutPortType { SCE_AUDIO_OUT_PORT_TYPE_MAIN };
enum SceAudioOutMode { SCE_AUDIO_OUT_MODE_MONO, SCE_AUDIO_OUT_MODE_STEREO };
enum SceAudioOutChannelFlag { SCE_AUDIO_VOLUME_FLAG_L_CH=1, SCE_AUDIO_VOLUME_FLAG_R_CH=2 };
constexpr int SCE_AUDIO_VOLUME_0DB = 32768;
int sceAudioOutOpenPort(SceAudioOutPortType,int,int,SceAudioOutMode);
int sceAudioOutReleasePort(int);
int sceAudioOutOutput(int,const void*);
int sceAudioOutSetVolume(int,SceAudioOutChannelFlag,int*);
int sceAudioOutGetRestSample(int);
