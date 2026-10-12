#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

/**
 * Reference implementation of Medut's app-owned audio device on iOS.
 *
 * The SDK stops opening any capture or playback device
 * (`medut_audio_use_external(true)`); this class becomes the device, driving
 * `medut_audio_external_push()` / `_pull()` from a VoiceProcessingIO render
 * callback. SIP, ICE, SRTP, codecs and the jitter buffer stay in the SDK.
 *
 * VoiceProcessingIO is not an implementation detail here — it is where iOS's
 * hardware echo canceller lives, and the SDK's mobile AEC *is* the AudioUnit
 * this displaces. Losing it would mean losing echo cancellation entirely.
 *
 * Written in Objective-C rather than Swift for two reasons: the render callback
 * has to be a C function on a realtime thread regardless, and the shipped
 * xcframework carries no headers or modulemap, so the SDK's C symbols are
 * declared locally and resolved at link time.
 *
 * ## Session ownership
 *
 * The unit is only started once the audio session is known to be active, which
 * is reported from exactly one of two places:
 *  - a CallKit host: `provider(_:didActivate:)` -> [sessionActivated],
 *  - everyone else: the plugin's own `configureAudioSession(active:)`.
 *
 * That is the whole reason this is better than the SDK owning the device on a
 * CallKit app: the core would otherwise open its AudioUnit whenever media
 * started, which can be before CXProvider has activated the session.
 */
@interface MedutAudioEngine : NSObject

/// Begin watching for call media. The AudioUnit is not created until the
/// session is also active — see [sessionActivated].
- (void)arm;

/// Stop watching, stop and release the AudioUnit.
- (void)disarm;

/// The audio session is now active (CallKit `didActivate`, or our own
/// activation). Starts the unit if armed and a call has media.
- (void)sessionActivated;

/// The audio session was deactivated. Stops the unit but keeps watching.
- (void)sessionDeactivated;

/// Diagnostics: armed, sessionActive, running, sampleRate, channels, ptimeMs,
/// lastError.
- (NSDictionary<NSString *, id> *)status;

/// Reports failures that have no other path — the SDK is not holding the
/// device, so it cannot see them. Called on the main queue.
@property(nonatomic, copy, nullable) void (^onError)(NSString *code,
                                                     NSString *message);

@end

NS_ASSUME_NONNULL_END
