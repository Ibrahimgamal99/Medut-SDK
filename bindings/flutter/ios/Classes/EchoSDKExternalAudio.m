#import "EchoSDKExternalAudio.h"

#import <AVFoundation/AVFoundation.h>
#import <AudioToolbox/AudioToolbox.h>
#include <dlfcn.h>

/* The shipped xcframework carries no headers or modulemap (build-ios.sh's
 * make_framework copies only the dylib), so declare what we call. The symbols
 * are exported by the dylib and resolved through -framework EchoSDK. */
extern int  echosdk_audio_external_push(const int16_t *pcm, size_t nsamp);
extern int  echosdk_audio_external_pull(int16_t *pcm, size_t nsamp);
extern int  echosdk_audio_external_format(uint32_t *srate, uint8_t *ch,
                                          uint32_t *ptime);
extern bool echosdk_audio_external_is_active(void);

/* Tell the SDK VPIO is under the microphone, so its own NS and AGC stay off
 * (stacked on Apple's they only cost speech). Looked up at run time rather
 * than linked: echosdk_audio_external_set_voice_path() is newer than some
 * vendored frameworks, and against one of those the plugin must still link —
 * the SDK then keeps running its NS/AGC on top, as it always did. Called from
 * unit start and teardown on self.queue, never from RenderCB. */
static void EchoSDKReportVoicePath(bool active)
{
	static void (*fn)(bool);
	static dispatch_once_t once;

	dispatch_once(&once, ^{
		fn = (void (*)(bool))dlsym(RTLD_DEFAULT,
		                           "echosdk_audio_external_set_voice_path");
		if (!fn)
			NSLog(@"[EchoSDK] SDK predates set_voice_path; its NS/AGC "
			      @"will run on top of VPIO");
	});
	if (fn)
		fn(active);
}

/* Poll interval for the format watcher. There is no "media is up" event in the
 * SDK: call state "established" is a SIP state and races the device, and a
 * mid-call re-INVITE renegotiates the codec with no state change at all. */
static const NSTimeInterval kWatchInterval = 0.02;

/* One frame per render callback at ~20 ms; VPIO converts to the hardware rate
 * internally, so unlike Android there is no resampler to write even at 8 kHz. */
static const NSTimeInterval kPreferredIOBuffer = 0.02;

@interface EchoSDKAudioEngine () {
	AudioUnit         _unit;
	AudioBufferList  *_micList;
	void             *_micData;
	UInt32            _micCapacity;   /* bytes */

	uint32_t          _srate;
	uint8_t           _ch;
	uint32_t          _ptime;
}

@property(nonatomic) BOOL armed;
@property(nonatomic) BOOL sessionActive;
@property(nonatomic) BOOL running;
@property(nonatomic, strong, nullable) dispatch_source_t watchTimer;
@property(nonatomic, strong) dispatch_queue_t queue;
@property(nonatomic, copy, nullable) NSString *lastError;

@end

#pragma mark -

@implementation EchoSDKAudioEngine

#pragma mark - Render callback

/* Realtime thread. No locks of ours, no allocation, no Objective-C messaging
 * beyond the ivar reads below — everything it touches is set up before the
 * unit starts and torn down after it stops.
 *
 * Defined inside @implementation on purpose: `self->_unit` and friends are
 * @protected, so a C function above the @implementation cannot reach them
 * ("instance variable '_unit' is private"). Moving it out breaks the build. */
static OSStatus RenderCB(void *inRefCon,
                         AudioUnitRenderActionFlags *ioActionFlags,
                         const AudioTimeStamp *inTimeStamp,
                         UInt32 inBusNumber,
                         UInt32 inNumberFrames,
                         AudioBufferList *ioData)
{
	EchoSDKAudioEngine *self = (__bridge EchoSDKAudioEngine *)inRefCon;
	OSStatus err;

	/* Objective-C ivars are reachable from C in the same @implementation. */
	AudioUnit unit = self->_unit;
	AudioBufferList *mic = self->_micList;
	uint8_t ch = self->_ch;

	if (!unit || !mic || !ioData || ioData->mNumberBuffers == 0)
		return noErr;

	/* Capture: bus 1 is the microphone side of the VPIO unit. */
	mic->mBuffers[0].mDataByteSize = inNumberFrames * ch * sizeof(int16_t);
	err = AudioUnitRender(unit, ioActionFlags, inTimeStamp, 1,
	                      inNumberFrames, mic);
	if (err == noErr) {
		echosdk_audio_external_push((const int16_t *)mic->mBuffers[0].mData,
		                            inNumberFrames * ch);
	}

	/* Playback: always fills, silence when no call is up, so there is nothing
	 * to branch on and no need to zero ioData ourselves. */
	echosdk_audio_external_pull((int16_t *)ioData->mBuffers[0].mData,
	                            inNumberFrames * ch);

	return noErr;
}

- (instancetype)init
{
	self = [super init];
	if (self) {
		_queue = dispatch_queue_create("dev.echosdk.appaudio",
		                               DISPATCH_QUEUE_SERIAL);
		/* Media services can be torn down under us; ignoring this is a hard
		 * silent-audio bug, because the unit is dead but still "running". */
		[[NSNotificationCenter defaultCenter]
		    addObserver:self
		       selector:@selector(mediaServicesReset:)
		           name:AVAudioSessionMediaServicesWereResetNotification
		         object:nil];
	}
	return self;
}

- (void)dealloc
{
	[[NSNotificationCenter defaultCenter] removeObserver:self];
	[self teardownUnit];
}

#pragma mark - Public

- (void)arm
{
	dispatch_async(self.queue, ^{
		if (self.armed)
			return;
		self.armed = YES;
		self.lastError = nil;
		[self startWatcher];
	});
}

- (void)disarm
{
	dispatch_async(self.queue, ^{
		if (!self.armed)
			return;
		self.armed = NO;
		[self stopWatcher];
		[self teardownUnit];
	});
}

- (void)sessionActivated
{
	dispatch_async(self.queue, ^{
		self.sessionActive = YES;
		/* The watcher opens the unit on its next tick if a call has media. */
	});
}

- (void)sessionDeactivated
{
	dispatch_async(self.queue, ^{
		self.sessionActive = NO;
		[self teardownUnit];
	});
}

- (NSDictionary<NSString *, id> *)status
{
	__block NSDictionary *out;
	dispatch_sync(self.queue, ^{
		out = @{
			@"armed"         : @(self.armed),
			@"running"       : @(self.running),
			/* `paused` and `available` are part of the documented status
			 * contract (platform_channel.dart) and are supplied by Android;
			 * iOS omitted both, so a diagnostics view read null for two
			 * fields on one platform only.
			 *
			 * paused: Android pauses on audio-focus loss. The iOS analogue is
			 * armed-but-the-session-is-not-ours, which is what an
			 * interruption or a CallKit deactivation leaves behind.
			 *
			 * available: on Android this is whether libechosdk.so loaded. iOS
			 * resolves the same symbols at link time through
			 * -framework EchoSDK, so reaching this line at all means they are
			 * present — it is constant YES rather than a runtime probe. */
			@"paused"        : @(self.armed && !self.sessionActive),
			@"available"     : @YES,
			/* iOS-only extra: whether the AVAudioSession is currently active.
			 * Android has no equivalent — it has no session to own. */
			@"sessionActive" : @(self.sessionActive),
			@"sampleRate"    : self.running ? @(self->_srate) : NSNull.null,
			@"channels"      : self.running ? @(self->_ch)    : NSNull.null,
			@"ptimeMs"       : self.running ? @(self->_ptime) : NSNull.null,
			@"lastError"     : self.lastError ?: NSNull.null,
		};
	});
	return out;
}

#pragma mark - Watcher

- (void)startWatcher
{
	if (self.watchTimer)
		return;

	dispatch_source_t t = dispatch_source_create(
	    DISPATCH_SOURCE_TYPE_TIMER, 0, 0, self.queue);
	dispatch_source_set_timer(t, dispatch_time(DISPATCH_TIME_NOW, 0),
	                          (uint64_t)(kWatchInterval * NSEC_PER_SEC),
	                          (uint64_t)(0.005 * NSEC_PER_SEC));
	__weak typeof(self) weakSelf = self;
	dispatch_source_set_event_handler(t, ^{ [weakSelf tick]; });
	dispatch_resume(t);
	self.watchTimer = t;
}

- (void)stopWatcher
{
	if (self.watchTimer) {
		dispatch_source_cancel(self.watchTimer);
		self.watchTimer = nil;
	}
}

- (void)tick
{
	if (!self.armed)
		return;

	if (!self.sessionActive) {
		/* CallKit has not activated the session (or has deactivated it).
		 * Starting the unit here would be the exact ordering bug that owning
		 * the device is meant to eliminate. */
		if (self.running)
			[self teardownUnit];
		return;
	}

	uint32_t srate = 0, ptime = 0;
	uint8_t ch = 0;
	int err = echosdk_audio_external_format(&srate, &ch, &ptime);

	if (err != 0) {
		/* ENODEV: no call has media — between calls, or the call ended. */
		if (self.running)
			[self teardownUnit];
		return;
	}

	if (!self.running) {
		[self setupUnitWithRate:srate channels:ch ptime:ptime];
	} else if (srate != _srate || ch != _ch || ptime != _ptime) {
		/* Mid-call codec renegotiation. */
		NSLog(@"[EchoSDK] audio format changed %u/%u/%u -> %u/%u/%u",
		      _srate, _ch, _ptime, srate, ch, ptime);
		[self teardownUnit];
		[self setupUnitWithRate:srate channels:ch ptime:ptime];
	}
}

#pragma mark - AudioUnit

- (void)setupUnitWithRate:(uint32_t)srate
                 channels:(uint8_t)ch
                    ptime:(uint32_t)ptime
{
	OSStatus err;

	/* Category and mode are already set by the core at init
	 * (platform/ios/audio_ios.m: PlayAndRecord + VoiceChat). Re-setting them
	 * here would churn the route for no reason. Only the buffer hint is ours. */
	NSError *sessionErr = nil;
	[[AVAudioSession sharedInstance]
	    setPreferredIOBufferDuration:kPreferredIOBuffer error:&sessionErr];

	AudioComponentDescription desc = {
		.componentType         = kAudioUnitType_Output,
		.componentSubType      = kAudioUnitSubType_VoiceProcessingIO,
		.componentManufacturer = kAudioUnitManufacturer_Apple,
	};
	AudioComponent comp = AudioComponentFindNext(NULL, &desc);
	if (!comp) {
		/* No VPIO component on this device: the feature cannot run at
		 * all, which is what the documented `unavailable` code means
		 * (Android reports it when libechosdk.so did not load). */
		[self reportCode:@"unavailable"
		         message:@"VoiceProcessingIO unit not available"];
		return;
	}

	err = AudioComponentInstanceNew(comp, &_unit);
	if (err != noErr) {
		[self reportCode:@"device-open"
		         message:[NSString stringWithFormat:
		                  @"vpio-alloc failed: AudioComponentInstanceNew: %d",
		                  (int)err]];
		return;
	}

	UInt32 one = 1;
	/* Bus 1 = input (mic), bus 0 = output (speaker). Both must be enabled. */
	err = AudioUnitSetProperty(_unit, kAudioOutputUnitProperty_EnableIO,
	                           kAudioUnitScope_Input, 1, &one, sizeof(one));
	if (err == noErr)
		err = AudioUnitSetProperty(_unit, kAudioOutputUnitProperty_EnableIO,
		                           kAudioUnitScope_Output, 0, &one, sizeof(one));
	if (err != noErr) {
		[self failSetup:@"enable-io" status:err];
		return;
	}

	/* S16 interleaved at the *negotiated* rate; VPIO resamples to the hardware
	 * rate internally, which is why no SRC is needed here. */
	AudioStreamBasicDescription fmt = {0};
	fmt.mSampleRate       = srate;
	fmt.mFormatID         = kAudioFormatLinearPCM;
	fmt.mFormatFlags      = kAudioFormatFlagIsSignedInteger |
	                        kAudioFormatFlagIsPacked;
	fmt.mFramesPerPacket  = 1;
	fmt.mChannelsPerFrame = ch;
	fmt.mBitsPerChannel   = 16;
	fmt.mBytesPerFrame    = ch * sizeof(int16_t);
	fmt.mBytesPerPacket   = fmt.mBytesPerFrame;

	err = AudioUnitSetProperty(_unit, kAudioUnitProperty_StreamFormat,
	                           kAudioUnitScope_Output, 1, &fmt, sizeof(fmt));
	if (err == noErr)
		err = AudioUnitSetProperty(_unit, kAudioUnitProperty_StreamFormat,
		                           kAudioUnitScope_Input, 0, &fmt, sizeof(fmt));
	if (err != noErr) {
		[self failSetup:@"stream-format" status:err];
		return;
	}

	AURenderCallbackStruct cb = {
		.inputProc       = RenderCB,
		.inputProcRefCon = (__bridge void *)self,
	};
	err = AudioUnitSetProperty(_unit, kAudioUnitProperty_SetRenderCallback,
	                           kAudioUnitScope_Input, 0, &cb, sizeof(cb));
	if (err != noErr) {
		[self failSetup:@"render-callback" status:err];
		return;
	}

	/* Capture scratch, sized generously: the callback is told how many frames
	 * it got, and asking for 20 ms while the device grants 5 ms is fine, but
	 * the reverse would overflow. */
	_micCapacity = (UInt32)(srate * ch * sizeof(int16_t) / 10);  /* 100 ms */
	_micData = calloc(1, _micCapacity);
	_micList = calloc(1, sizeof(AudioBufferList));
	if (!_micData || !_micList) {
		[self failSetup:@"alloc" status:noErr];
		return;
	}
	_micList->mNumberBuffers = 1;
	_micList->mBuffers[0].mNumberChannels = ch;
	_micList->mBuffers[0].mDataByteSize = _micCapacity;
	_micList->mBuffers[0].mData = _micData;

	_srate = srate;
	_ch    = ch;
	_ptime = ptime;

	err = AudioUnitInitialize(_unit);
	if (err == noErr)
		err = AudioOutputUnitStart(_unit);
	if (err != noErr) {
		[self failSetup:@"start" status:err];
		return;
	}

	self.running = YES;
	/* Capture now goes through VPIO's AEC, NS and AGC. */
	EchoSDKReportVoicePath(true);
	NSLog(@"[EchoSDK] app-owned audio open: %u Hz %u ch ptime=%u ms (VPIO)",
	      srate, ch, ptime);
}

/* Map an internal setup stage onto the documented onAppOwnedAudioError code
 * vocabulary (platform_channel.dart: mic-permission, unsupported-rate,
 * device-open, capture-dead, playback-dead, unavailable).
 *
 * The stage names below — enable-io, stream-format, render-callback, alloc,
 * start — are precise and worth keeping, but they were being emitted AS the
 * code, and they overlap the documented set nowhere. Android emits the
 * documented six; iOS emitted five names that appear in no contract, so a host
 * switching on the documented codes handled every Android failure and no iOS
 * one. Translate the code and keep the stage in the message, so nothing is
 * lost from the logs. */
static NSString *EchoSDKPublicErrorCode(NSString *stage)
{
	/* A format the VoiceProcessingIO unit refused is the same class of
	 * failure Android reports when it cannot open a stream at the rate the
	 * core asked for. */
	if ([stage isEqualToString:@"stream-format"])
		return @"unsupported-rate";

	/* Everything else here is "the device would not open": enabling I/O,
	 * installing the render callback, AudioUnitInitialize, AudioOutputUnitStart. */
	return @"device-open";
}

- (void)failSetup:(NSString *)code status:(OSStatus)err
{
	[self teardownUnit];
	[self reportCode:EchoSDKPublicErrorCode(code)
	         message:[NSString stringWithFormat:@"%@ failed: OSStatus %d",
	                                            code, (int)err]];
}

- (void)teardownUnit
{
	BOOL wasRunning = self.running;

	self.running = NO;

	if (_unit) {
		AudioOutputUnitStop(_unit);
		AudioUnitUninitialize(_unit);
		AudioComponentInstanceDispose(_unit);
		_unit = NULL;
	}
	if (wasRunning)
		EchoSDKReportVoicePath(false);
	/* Only after the unit is disposed: the render callback must not be able to
	 * run against freed buffers. */
	free(_micList);
	free(_micData);
	_micList = NULL;
	_micData = NULL;
	_micCapacity = 0;
	_srate = 0;
	_ch = 0;
	_ptime = 0;
}

- (void)mediaServicesReset:(NSNotification *)note
{
	/* Everything audio is gone; the unit handle is dead even though we still
	 * hold it. Drop it and let the watcher rebuild from scratch. */
	dispatch_async(self.queue, ^{
		NSLog(@"[EchoSDK] media services reset — rebuilding audio unit");
		[self teardownUnit];
	});
}

- (void)reportCode:(NSString *)code message:(NSString *)message
{
	NSLog(@"[EchoSDK] app-owned audio %@: %@", code, message);
	self.lastError = [NSString stringWithFormat:@"%@: %@", code, message];
	void (^handler)(NSString *, NSString *) = self.onError;
	if (handler) {
		dispatch_async(dispatch_get_main_queue(), ^{
			handler(code, message);
		});
	}
}

@end
