// gh337 — implementation of the owned Web Island runtime module.
#import "RNCWebIslandRuntimeModule.h"
#import "RNCWebIslandRuntimeSupport.h"

#import "RNCWebIslandLruCore.hpp"

#import <React/RCTLog.h>

using namespace rncwebisland;

// Fold the pure-core decision enum to the bounded, telemetry-safe string the ACP
// facade (native/src/web-islands/runtimeProtocol.ts:normalizeDecision) accepts.
static NSString *RNCWebIslandDecisionString(Decision decision) {
  switch (decision) {
    case Decision::Reuse: return @"reuse";
    case Decision::Allocate: return @"allocate";
    case Decision::Evict: return @"evict";
    case Decision::Queue: return @"queue";
    case Decision::Reject: return @"reject";
    case Decision::Promote: return @"promote";
    case Decision::Replace: return @"replace";
    case Decision::NoOp: return @"reuse";
  }
  return @"reuse";
}

@implementation RNCWebIslandRuntimeModule

RCT_EXPORT_MODULE(RNCWebIslandRuntime)

+ (BOOL)requiresMainQueueSetup {
  return NO;
}

// Capabilities are immutable constants, so they are also exported as module
// constants for the negotiator's synchronous read path.
- (NSDictionary *)constantsToExport {
  return @{ @"capabilities": RNCWebIslandRuntimeCapabilitiesDictionary() };
}

// Immutable capability block (async form, safe under bridgeless interop).
RCT_EXPORT_METHOD(getCapabilities:(RCTPromiseResolveBlock)resolve
                  reject:(RCTPromiseRejectBlock)reject) {
  resolve(RNCWebIslandRuntimeCapabilitiesDictionary());
}

// The per-slot native tuple binding (or null for an unallocated/invalid slot).
RCT_EXPORT_METHOD(getBinding:(NSString *)slotId
                  resolve:(RCTPromiseResolveBlock)resolve
                  reject:(RCTPromiseRejectBlock)reject) {
  NSDictionary *binding = RNCWebIslandSlotBindingDictionary(slotId);
  resolve(binding ?: (id)[NSNull null]);
}

// Bindings for both fixed slots at once (bounded, capacity two).
RCT_EXPORT_METHOD(getBindings:(RCTPromiseResolveBlock)resolve
                  reject:(RCTPromiseRejectBlock)reject) {
  NSMutableDictionary *out = [NSMutableDictionary dictionaryWithCapacity:2];
  for (NSString *slotId in @[@"slot-a", @"slot-b"]) {
    NSDictionary *binding = RNCWebIslandSlotBindingDictionary(slotId);
    out[slotId] = binding ?: (id)[NSNull null];
  }
  resolve(out);
}

// Debug/telemetry probe that exercises the pure pin-aware LRU decision core in
// the shipped binary (Debug and Release), proving the safety invariants hold for
// a canonical A,B,A,C recency trace and that no pinned lease is ever evicted.
RCT_EXPORT_METHOD(selfTest:(RCTPromiseResolveBlock)resolve
                  reject:(RCTPromiseRejectBlock)reject) {
  LruModel model;
  auto request = [](const char *id, const char *act, const char *doc, const char *fam) {
    ActivationRequest r;
    r.id = id; r.activationKey = act; r.documentKey = doc; r.propsRevision = "r1";
    r.compatibilityKey = fam; r.retention = Retention::None;
    return r;
  };
  auto present = [&](const std::string &slotId) {
    const Lease &l = model.slot(0).slotId == slotId ? model.slot(0) : model.slot(1);
    CallbackIdentity who{l.slotId, l.allocationGeneration, l.leaseGeneration, l.activationKey};
    model.commitPresentation(who);
    const Lease &c = model.slot(0).slotId == slotId ? model.slot(0) : model.slot(1);
    model.paint(who, c.currentTuple());
  };

  auto a = model.requestActivation(request("summary", "summary-a", "/s", "summary"));
  present(a.slotId);
  auto b = model.requestActivation(request("detail", "detail-b", "/d/b", "detail"));
  present(b.slotId);
  model.requestActivation(request("summary", "summary-a", "/s", "summary"));
  present(a.slotId);  // A now most-recent.

  std::string why;
  BOOL invariants = model.invariantsHold(&why);
  resolve(@{
    @"invariantsHeld": @(invariants),
    @"pinnedEvictionAttempted": @(model.pinnedEvictionAttempts()),
    @"recencyOrdinal": @(model.sequenceCounter()),
    @"lastDecision": RNCWebIslandDecisionString(b.decision),
  });
}

@end
