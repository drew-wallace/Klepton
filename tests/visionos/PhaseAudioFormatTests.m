// Exercise the actual runtime's device formats in Apple's offline graph. The
// AudioUnits in VoiceIOTests are fake, and AVAudioSourceNode provides synthetic
// PCM: this test starts neither microphone capture nor speaker hardware.
#import <AVFAudio/AVFAudio.h>
#define main voice_io_contracts
#include "VoiceIOTests.c"
#undef main

int main(void) { @autoreleasepool {
    assert(!voice_io_contracts());
    assert(!kl_audio_open(48000,2,16)&&!kl_audio_mic_open(48000,2));
    fake_unit *unit=(fake_unit*)g_cap_unit;
    AVAudioFormat *input=[[AVAudioFormat alloc] initWithStreamDescription:&unit->capture];
    AVAudioFormat *output=[[AVAudioFormat alloc] initWithStreamDescription:&unit->playback];
    assert(input&&output);
    AVAudioEngine *engine=[AVAudioEngine new];
    AVAudioSourceNode *source=[[AVAudioSourceNode alloc] initWithFormat:input renderBlock:
        ^OSStatus(BOOL *silent,const AudioTimeStamp *time,AVAudioFrameCount frames,AudioBufferList *data){
            *silent=NO;
            assert(data->mNumberBuffers==2);
            for(unsigned ch=0;ch<2;ch++){
                assert(data->mBuffers[ch].mDataByteSize>=frames*sizeof(float));
                float *samples=data->mBuffers[ch].mData;
                for(unsigned i=0;i<frames;i++)samples[i]=ch?-.25f:.25f;
            }
            return noErr;
        }];
    [engine attachNode:source];
    [engine connect:source to:engine.mainMixerNode format:input];
    NSError *error=nil;
    assert([engine enableManualRenderingMode:AVAudioEngineManualRenderingModeOffline
        format:output maximumFrameCount:128 error:&error]);
    assert([engine startAndReturnError:&error]);
    AVAudioPCMBuffer *buffer=[[AVAudioPCMBuffer alloc] initWithPCMFormat:output frameCapacity:128];
    assert([engine renderOffline:128 toBuffer:buffer error:&error]==AVAudioEngineManualRenderingStatusSuccess);
    assert(buffer.frameLength==128&&buffer.floatChannelData);
    for(unsigned i=0;i<128;i++){
        assert(fabsf(buffer.floatChannelData[0][i]-.25f)<.00001f);
        assert(fabsf(buffer.floatChannelData[1][i]+.25f)<.00001f);
    }
    [engine stop];
    kl_audio_mic_close();kl_audio_close();assert(!live_units);
    puts("Apple offline audio graph: runtime Float32 capture/playback formats connect and preserve distinct stereo samples");
} return 0; }
