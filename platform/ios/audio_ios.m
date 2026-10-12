/**
 * @file audio_ios.m  iOS platform audio configuration
 *
 * iOS requires explicit AVAudioSession configuration before VoIP audio works.
 * This must be compiled as Objective-C (.m) and linked against AVFoundation.
 *
 * Category:  PlayAndRecord — simultaneous input + output.
 * Mode:      VoiceChat — activates hardware AEC and appropriate gain for
 *            held-to-ear calls. Also routes to earpiece by default; the app
 *            can override to speaker via AVAudioSession.overrideOutputAudioPort.
 * Options:   AllowBluetooth + AllowBluetoothA2DP — enable BT headset routing.
 *            MixWithOthers is intentionally omitted so our session takes
 *            exclusive control of the hardware AEC path.
 *
 * Activation:  owned by cfg.platform_audio_activate.  With CallKit, Apple
 *            requires the session be activated only from
 *            -provider:didActivateAudioSession:, so a CallKit app passes
 *            false and this function stops after configuring — otherwise
 *            medut_init() (at app launch, or on a PushKit wake while
 *            CallKit is still reporting the call) would seize the exclusive
 *            PlayAndRecord route out from under CXProvider.
 *
 * Called from medut_init() (core.c) after modules_init.  Compiled as OBJC
 * (enabled in the SRC_MODE branch of CMakeLists for iOS/Darwin); AVFoundation
 * is linked by the shared-library step in scripts/build-ios.sh and declared
 * in the Flutter plugin podspec.
 */

#import <AVFoundation/AVFoundation.h>
#include <errno.h>
#include "../../src/medut_internal.h"

int bsdk_platform_audio_init(bool activate)
{
	AVAudioSession *session = [AVAudioSession sharedInstance];
	NSError *error = nil;

	AVAudioSessionCategoryOptions opts =
		AVAudioSessionCategoryOptionAllowBluetooth |
		AVAudioSessionCategoryOptionAllowBluetoothA2DP;

	[session setCategory:AVAudioSessionCategoryPlayAndRecord
	         withOptions:opts
	               error:&error];
	if (error) {
		warning("Medut/ios: AVAudioSession setCategory: %s\n",
		        [[error localizedDescription] UTF8String]);
		return EINVAL;
	}

	/* VoiceChat mode enables the hardware echo-cancellation path and sets
	 * mic gain appropriate for telephone-quality voice calls. */
	[session setMode:AVAudioSessionModeVoiceChat error:&error];
	if (error) {
		warning("Medut/ios: AVAudioSession setMode: %s\n",
		        [[error localizedDescription] UTF8String]);
		return EINVAL;
	}

	if (!activate) {
		/* Category and mode are set; CallKit will activate the session
		 * from -provider:didActivateAudioSession: when the call starts. */
		info("Medut/ios: AVAudioSession configured, activation left to "
		     "the app (platform_audio_activate = false)\n");
		return 0;
	}

	[session setActive:YES error:&error];
	if (error) {
		warning("Medut/ios: AVAudioSession setActive: %s\n",
		        [[error localizedDescription] UTF8String]);
		return EINVAL;
	}

	return 0;
}
