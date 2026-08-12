// gh337 — owned Web Island runtime module.
//
// A legacy RCTBridgeModule (not a codegen TurboModule) so it compiles and runs
// under both the old and new (Fabric/bridgeless interop) architectures without
// adding a codegen spec surface. It advertises the immutable capability block
// and the per-slot native tuple binding that the ACP facade
// (native/src/web-islands/runtimeProtocol.ts) negotiates and consumes. It never
// echoes payload, route, or identity content — only bounded, telemetry-safe
// primitives.

#import <React/RCTBridgeModule.h>

@interface RNCWebIslandRuntimeModule : NSObject <RCTBridgeModule>
@end
