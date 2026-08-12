// gh337 — Pure pin-aware LRU decision core for the owned react-native-webview
// runtime. No React, UIKit, WebKit, or Objective-C dependency: this is the
// portable production decision core the design calls for, so it can be exercised
// headlessly (see RNCWebIslandLruCoreTests.cpp) and reused unchanged by the
// Objective-C++ coordinator (RNCWebIslandPoolCoordinator.mm).
//
// The contract it enforces (issue #337):
//   * Capacity is fixed at two slots.
//   * "Used" (recency promotion) happens exactly once per presentation epoch,
//     when the lease is BOTH committed as the foreground view AND has an
//     acknowledged paint of the exact current identity tuple. Requests, mounts,
//     transition starts, cancels, parks, and background traffic never promote.
//   * Recency is a monotonic sequence, never wall-clock time.
//   * Pins: the committed-visible lease, every lease in an unfinished native
//     transition (incoming/outgoing), and an explicit caller-retention lease.
//   * Candidates = resident leases minus pins. The victim is the eligible lease
//     with the smallest last-used sequence.
//   * When every slot is pinned, queue and coalesce the latest compatible
//     activation until a candidate frees; never transfer/retarget/detach/evict a
//     pinned owner.
//   * A stale callback may change ownership/pin/recency only when slot id,
//     allocation generation, lease generation, and activation key still match.
//   * Fresh allocation or WebContent replacement increments the allocation
//     generation and invalidates painted state.

#ifndef RNCWebIslandLruCore_hpp
#define RNCWebIslandLruCore_hpp

#include <cstdint>
#include <string>
#include <array>
#include <optional>
#include <vector>

namespace rncwebisland {

using Seq = std::uint64_t;
using Gen = std::uint64_t;

static constexpr int kCapacity = 2;

enum class PresentationState {
  Empty,
  Incoming,
  CommittedVisible,
  Outgoing,
  Parked,
  Revoked,
};

enum class PinReason {
  CommittedVisible,
  IncomingTransition,
  OutgoingTransition,
  CallerRetention,
};

enum class Decision {
  Reuse,
  Allocate,
  Evict,
  Queue,
  Reject,
  Promote,
  Replace,
  NoOp,
};

enum class QueueResult {
  None,
  Queued,
  Coalesced,
  Replaced,
  Cancelled,
  Drained,
};

enum class Retention { None, Caller };

// The exact identity tuple compared for paint/promotion. Compatibility family is
// deliberately NOT part of paint identity (it only governs retarget strategy),
// mirroring native/src/web-islands/runtimeProtocol.ts:tuplesMatchForPaint.
struct IdentityTuple {
  std::string id;
  std::string activationKey;
  std::string documentKey;
  std::string propsRevision;
  std::string slotId;
  Gen allocationGeneration = 0;
  Gen leaseGeneration = 0;

  bool matchesForPaint(const IdentityTuple &o) const {
    return id == o.id && activationKey == o.activationKey &&
           documentKey == o.documentKey && propsRevision == o.propsRevision &&
           slotId == o.slotId && allocationGeneration == o.allocationGeneration &&
           leaseGeneration == o.leaseGeneration;
  }
};

// A caller request for a foreground activation.
struct ActivationRequest {
  std::string id;
  std::string activationKey;
  std::string documentKey;
  std::string propsRevision;
  std::string compatibilityKey;  // document family, retarget-only
  Retention retention = Retention::None;
};

// Identity a delayed/stale callback claims to act on. A callback is honored only
// when slotId + allocationGeneration + leaseGeneration + activationKey all still
// match the live lease at that slot.
struct CallbackIdentity {
  std::string slotId;
  Gen allocationGeneration = 0;
  Gen leaseGeneration = 0;
  std::string activationKey;
};

struct Lease {
  bool occupied = false;
  std::string slotId;
  std::string id;
  std::string activationKey;
  std::string documentKey;
  std::string propsRevision;
  std::string compatibilityKey;
  Gen allocationGeneration = 0;
  Gen leaseGeneration = 0;
  PresentationState state = PresentationState::Empty;
  Retention retention = Retention::None;
  Seq lastUsedSequence = 0;  // 0 until first committed+painted promotion.

  // Presentation-epoch bookkeeping for "used exactly once per epoch".
  bool committed = false;      // native presentation committed as foreground.
  bool painted = false;        // acknowledged paint of the exact current tuple.
  bool promotedThisEpoch = false;

  IdentityTuple currentTuple() const {
    return IdentityTuple{id, activationKey, documentKey, propsRevision,
                         slotId, allocationGeneration, leaseGeneration};
  }
};

struct DecisionResult {
  Decision decision = Decision::NoOp;
  std::string slotId;          // affected slot, when applicable.
  QueueResult queue = QueueResult::None;
  std::optional<std::string> evictedSlotId;  // set when a victim was evicted.
  IdentityTuple tuple;         // the resulting live tuple for the slot.
  bool changed = false;        // whether the model mutated.
};

// The fixed two-slot pin-aware LRU model. Deterministic, single-threaded; the
// coordinator serializes calls on the main thread.
class LruModel {
 public:
  LruModel() {
    slots_[0].slotId = "slot-a";
    slots_[1].slotId = "slot-b";
  }

  // ---- pin / candidate helpers -------------------------------------------

  std::vector<PinReason> pinReasons(const Lease &l) const {
    std::vector<PinReason> reasons;
    if (!l.occupied) return reasons;
    if (l.state == PresentationState::CommittedVisible) reasons.push_back(PinReason::CommittedVisible);
    if (l.state == PresentationState::Incoming) reasons.push_back(PinReason::IncomingTransition);
    if (l.state == PresentationState::Outgoing) reasons.push_back(PinReason::OutgoingTransition);
    if (l.retention == Retention::Caller) reasons.push_back(PinReason::CallerRetention);
    return reasons;
  }

  bool isPinned(const Lease &l) const { return !pinReasons(l).empty(); }

  // ---- the alphabet of native events -------------------------------------

  // A foreground activation request. Returns the bounded decision. On success
  // the target slot's lease is moved into an Incoming (pinned) transition; the
  // previously committed lease, if any, is moved Outgoing (also pinned) so no
  // pinned owner is ever detached mid-transition.
  DecisionResult requestActivation(const ActivationRequest &req);

  // Native presentation committed the incoming lease as the foreground view.
  // Completes any outgoing transition and drains a pending activation.
  DecisionResult commitPresentation(const CallbackIdentity &who);

  // The web content acknowledged and painted the exact tuple.
  DecisionResult paint(const CallbackIdentity &who, const IdentityTuple &painted);

  // An in-flight transition was cancelled (interactive back-swipe release). The
  // incoming lease unwinds; the outgoing lease is restored to committed-visible.
  DecisionResult cancelTransition(const CallbackIdentity &who);

  // The lease was navigated away and settled: committed -> parked. New epoch.
  DecisionResult park(const CallbackIdentity &who);

  // WebContent process terminated / was replaced: bump allocation generation and
  // invalidate painted state.
  DecisionResult recover(const CallbackIdentity &who);

  // Explicit bounded caller-retention pin toggle.
  DecisionResult setRetention(const CallbackIdentity &who, Retention retention);

  // ---- introspection ------------------------------------------------------

  const Lease &slot(int i) const { return slots_[static_cast<std::size_t>(i)]; }
  int residentCount() const {
    int n = 0;
    for (const auto &s : slots_) if (s.occupied) n++;
    return n;
  }
  Seq sequenceCounter() const { return sequenceCounter_; }
  Gen allocationCounter() const { return allocationCounter_; }
  bool hasPending() const { return pending_.has_value(); }

  // Guard counter: the number of times an external caller asked to evict/transfer
  // a pinned owner. Correct operation keeps this at ZERO forever (issue #337
  // telemetry contract). Any deterministic/property trace that raises it is a bug.
  std::uint64_t pinnedEvictionAttempts() const { return pinnedEvictionAttempts_; }

  // Whole-model invariant check used by the test harness after every prefix.
  bool invariantsHold(std::string *why = nullptr) const;

 private:
  Lease *findSlot(const std::string &slotId);
  const Lease *findSlotConst(const std::string &slotId) const;
  Lease *findByActivation(const std::string &activationKey);
  Lease *emptySlot();
  Lease *victimBySmallestRecency();  // unpinned only; nullptr if all pinned.
  bool callbackMatches(const Lease &l, const CallbackIdentity &who) const;
  void tryPromote(Lease &l, DecisionResult &out);
  void beginEpochReset(Lease &l);
  DecisionResult drainPending();
  void placeActivation(Lease &slot, const ActivationRequest &req, bool freshAllocation);

  std::array<Lease, kCapacity> slots_{};
  Seq sequenceCounter_ = 0;
  Gen allocationCounter_ = 0;
  Gen leaseCounter_ = 0;
  std::optional<ActivationRequest> pending_;
  std::uint64_t pinnedEvictionAttempts_ = 0;
};

}  // namespace rncwebisland

#endif  // RNCWebIslandLruCore_hpp
