#include <stdint.h>
typedef int OSStatus; typedef uint32_t UInt32; typedef double Float64; enum { noErr = 0 };
typedef void *AudioUnit; typedef void *Component; typedef void *ComponentInstance;
typedef struct { uint32_t componentType, componentSubType, componentManufacturer, componentFlags, componentFlagsMask; } ComponentDescription;
enum { kAudioUnitType_Output = 1, kAudioUnitSubType_DefaultOutput = 2, kAudioUnitManufacturer_Apple = 3,
       kAudioUnitProperty_StreamFormat = 8, kAudioUnitProperty_SetRenderCallback = 23, kAudioUnitScope_Input = 1,
       kAudioFormatLinearPCM = 0x6c70636d, kLinearPCMFormatFlagIsSignedInteger = 4, kLinearPCMFormatFlagIsPacked = 8,
       kLinearPCMFormatFlagIsBigEndian = 2 };
typedef uint32_t AudioUnitRenderActionFlags; typedef struct { double t; } AudioTimeStamp;
typedef struct { UInt32 mNumberChannels, mDataByteSize; void *mData; } AudioBuffer;
typedef struct { UInt32 mNumberBuffers; AudioBuffer mBuffers[1]; } AudioBufferList;
typedef struct { Float64 mSampleRate; UInt32 mFormatID, mFormatFlags, mBytesPerPacket, mFramesPerPacket, mBytesPerFrame, mChannelsPerFrame, mBitsPerChannel, mReserved; } AudioStreamBasicDescription;
typedef OSStatus (*AURenderCallback)(void *, AudioUnitRenderActionFlags *, const AudioTimeStamp *, UInt32, UInt32, AudioBufferList *);
typedef struct { AURenderCallback inputProc; void *inputProcRefCon; } AURenderCallbackStruct;
Component FindNextComponent(Component, ComponentDescription *); OSStatus OpenAComponent(Component, ComponentInstance *); OSStatus CloseComponent(ComponentInstance);
OSStatus AudioUnitSetProperty(AudioUnit, UInt32, UInt32, UInt32, const void *, UInt32); OSStatus AudioUnitInitialize(AudioUnit); OSStatus AudioUnitUninitialize(AudioUnit);
OSStatus AudioOutputUnitStart(AudioUnit); OSStatus AudioOutputUnitStop(AudioUnit);
