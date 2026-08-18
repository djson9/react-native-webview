// gh337 — C boundary between the owned WebView pool state (defined in
// RNCWebViewImpl.m) and the runtime module that advertises it to JavaScript.
// These functions are the single fork-owned source of the native tuple binding
// (slot id + allocation/lease generations + presentation/pin/recency) and the
// immutable capability block.

#ifndef RNCWebIslandRuntimeSupport_h
#define RNCWebIslandRuntimeSupport_h

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

/// The generation-rich native binding for a slot, shaped exactly as
/// native/src/web-islands/runtimeProtocol.ts:normalizeNativeBinding expects, or
/// nil for an unallocated/invalid slot (JS then fails closed to a null binding).
FOUNDATION_EXPORT NSDictionary *_Nullable RNCWebIslandSlotBindingDictionary(NSString *slotId);

/// The immutable capability block the owned runtime advertises in Debug and
/// Release (pool protocol major 1, capacity 2, and app-configurable document
/// families supplied through each WebView's pool props).
FOUNDATION_EXPORT NSDictionary *RNCWebIslandRuntimeCapabilitiesDictionary(void);

#if DEBUG
/// Invokes the WebKit reset-state selector against the exact retained view for
/// one fixed slot. Successful results contain the captured slot generations
/// and presentation state. Failures contain one bounded `errorCode` string.
FOUNDATION_EXPORT NSDictionary *RNCWebIslandTerminateWebContentForTest(NSString *slotId);
#endif

NS_ASSUME_NONNULL_END

#endif /* RNCWebIslandRuntimeSupport_h */
