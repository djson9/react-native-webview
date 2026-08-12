// gh337 — implementation of the pure pin-aware LRU decision core.
#include "RNCWebIslandLruCore.hpp"

#include <algorithm>

namespace rncwebisland {

Lease *LruModel::findSlot(const std::string &slotId) {
  for (auto &s : slots_) {
    if (s.slotId == slotId) return &s;
  }
  return nullptr;
}

const Lease *LruModel::findSlotConst(const std::string &slotId) const {
  for (const auto &s : slots_) {
    if (s.slotId == slotId) return &s;
  }
  return nullptr;
}

Lease *LruModel::findByActivation(const std::string &activationKey) {
  if (activationKey.empty()) return nullptr;
  for (auto &s : slots_) {
    if (s.occupied && s.activationKey == activationKey) return &s;
  }
  return nullptr;
}

Lease *LruModel::emptySlot() {
  for (auto &s : slots_) {
    if (!s.occupied) return &s;
  }
  return nullptr;
}

Lease *LruModel::victimBySmallestRecency() {
  Lease *victim = nullptr;
  for (auto &s : slots_) {
    if (!s.occupied) continue;
    if (isPinned(s)) continue;  // never a pinned owner.
    if (victim == nullptr || s.lastUsedSequence < victim->lastUsedSequence) {
      victim = &s;
    }
  }
  return victim;
}

bool LruModel::callbackMatches(const Lease &l, const CallbackIdentity &who) const {
  return l.occupied && l.slotId == who.slotId &&
         l.allocationGeneration == who.allocationGeneration &&
         l.leaseGeneration == who.leaseGeneration &&
         l.activationKey == who.activationKey;
}

void LruModel::beginEpochReset(Lease &l) {
  l.committed = false;
  l.painted = false;
  l.promotedThisEpoch = false;
}

void LruModel::tryPromote(Lease &l, DecisionResult &out) {
  // Used exactly once per presentation epoch: committed AND painted of the exact
  // current tuple, and not already promoted this epoch.
  if (l.state == PresentationState::CommittedVisible && l.committed && l.painted &&
      !l.promotedThisEpoch) {
    l.lastUsedSequence = ++sequenceCounter_;
    l.promotedThisEpoch = true;
    out.decision = Decision::Promote;
    out.changed = true;
  }
}

void LruModel::placeActivation(Lease &slot, const ActivationRequest &req, bool freshAllocation) {
  slot.occupied = true;
  slot.id = req.id;
  slot.activationKey = req.activationKey;
  slot.documentKey = req.documentKey;
  slot.propsRevision = req.propsRevision;
  slot.compatibilityKey = req.compatibilityKey;
  slot.retention = req.retention;
  slot.leaseGeneration = ++leaseCounter_;
  if (freshAllocation) {
    // Fresh allocation or WebContent replacement bumps the allocation generation
    // and invalidates any painted state.
    slot.allocationGeneration = ++allocationCounter_;
  }
  slot.state = PresentationState::Incoming;  // pinned incoming transition.
  slot.lastUsedSequence = 0;                 // not yet used; promoted on commit+paint.
  beginEpochReset(slot);
}

DecisionResult LruModel::requestActivation(const ActivationRequest &req) {
  DecisionResult out;
  if (req.activationKey.empty() || req.id.empty() || req.documentKey.empty() ||
      req.propsRevision.empty() || req.compatibilityKey.empty()) {
    out.decision = Decision::Reject;
    return out;
  }

  // single_pending_foreground: at most one incoming foreground transition may be
  // in flight. A request that would start a *second* distinct foreground
  // transition is queued/coalesced instead, and drained when the in-flight
  // transition commits or cancels. A re-request of the already-incoming
  // activation is idempotent and allowed to proceed.
  for (auto &s : slots_) {
    if (s.occupied && s.state == PresentationState::Incoming &&
        s.activationKey != req.activationKey) {
      out.decision = Decision::Queue;
      out.queue = pending_.has_value() ? QueueResult::Coalesced : QueueResult::Queued;
      pending_ = req;
      out.changed = true;
      return out;
    }
  }

  // Move the currently committed lease (if any, other than a same-activation
  // re-request) into an outgoing transition so it stays pinned until commit.
  auto beginOutgoing = [&](const std::string &keepSlotId) {
    for (auto &s : slots_) {
      if (s.occupied && s.slotId != keepSlotId &&
          s.state == PresentationState::CommittedVisible) {
        s.state = PresentationState::Outgoing;  // still pinned.
      }
    }
  };

  // (1) Re-request of a resident activation → reuse / retarget.
  if (Lease *existing = findByActivation(req.activationKey)) {
    bool familyChanged = existing->compatibilityKey != req.compatibilityKey;
    bool docChanged = existing->documentKey != req.documentKey;
    existing->id = req.id;
    existing->documentKey = req.documentKey;
    existing->propsRevision = req.propsRevision;
    existing->retention = req.retention;
    if (familyChanged) {
      // Cross-family reuse replaces the document once while retaining the slot's
      // allocation generation; a new lease identity gates stale paints.
      existing->compatibilityKey = req.compatibilityKey;
      existing->leaseGeneration = ++leaseCounter_;
      out.decision = Decision::Replace;
    } else if (docChanged) {
      existing->leaseGeneration = ++leaseCounter_;
      out.decision = Decision::Replace;
    } else {
      out.decision = Decision::Reuse;
    }
    existing->state = PresentationState::Incoming;  // re-present: pinned.
    beginEpochReset(*existing);
    beginOutgoing(existing->slotId);
    out.slotId = existing->slotId;
    out.tuple = existing->currentTuple();
    out.changed = true;
    return out;
  }

  // (2) A free slot → fresh allocate.
  if (Lease *empty = emptySlot()) {
    placeActivation(*empty, req, /*freshAllocation=*/true);
    beginOutgoing(empty->slotId);
    out.decision = Decision::Allocate;
    out.slotId = empty->slotId;
    out.tuple = empty->currentTuple();
    out.changed = true;
    return out;
  }

  // (3) Both slots occupied → evict the least-recently-used eligible candidate.
  if (Lease *victim = victimBySmallestRecency()) {
    out.evictedSlotId = victim->slotId;
    // Clear the victim, then fresh-allocate into that slot.
    victim->occupied = false;
    victim->state = PresentationState::Revoked;
    Lease *reused = victim;  // same slot object; fresh allocation resets identity.
    // Wipe residual identity before reuse.
    *reused = Lease{};
    reused->slotId = *out.evictedSlotId;
    placeActivation(*reused, req, /*freshAllocation=*/true);
    beginOutgoing(reused->slotId);
    out.decision = Decision::Evict;  // evict+allocate; victim was unpinned.
    out.slotId = reused->slotId;
    out.tuple = reused->currentTuple();
    out.changed = true;
    return out;
  }

  // (4) Every slot pinned → queue and coalesce the latest compatible activation.
  out.decision = Decision::Queue;
  out.slotId.clear();
  out.queue = pending_.has_value() ? QueueResult::Coalesced : QueueResult::Queued;
  pending_ = req;  // coalesce to the newest.
  out.changed = true;
  return out;
}

DecisionResult LruModel::commitPresentation(const CallbackIdentity &who) {
  DecisionResult out;
  Lease *l = findSlot(who.slotId);
  if (l == nullptr || !callbackMatches(*l, who)) {
    out.decision = Decision::NoOp;  // stale/mismatched callback: ignored.
    return out;
  }
  if (l->state == PresentationState::Incoming ||
      l->state == PresentationState::CommittedVisible) {
    if (l->state == PresentationState::Incoming) {
      l->state = PresentationState::CommittedVisible;
      out.changed = true;
    }
    l->committed = true;
    // Complete any outgoing transition on the other slot (and defensively any
    // stray committed-visible): only one foreground lease may exist. It settles
    // to parked, releasing its presentation pin.
    for (auto &s : slots_) {
      if (s.occupied && s.slotId != l->slotId &&
          (s.state == PresentationState::Outgoing ||
           s.state == PresentationState::CommittedVisible)) {
        s.state = PresentationState::Parked;
        beginEpochReset(s);
        out.changed = true;
      }
    }
    tryPromote(*l, out);
    out.slotId = l->slotId;
    out.tuple = l->currentTuple();
    if (out.decision == Decision::NoOp) out.decision = Decision::Reuse;
    // A freed (unpinned) candidate may now exist → drain a pending activation.
    DecisionResult drained = drainPending();
    if (drained.changed) {
      out.queue = QueueResult::Drained;
      out.changed = true;
    }
  }
  return out;
}

DecisionResult LruModel::paint(const CallbackIdentity &who, const IdentityTuple &painted) {
  DecisionResult out;
  Lease *l = findSlot(who.slotId);
  if (l == nullptr || !callbackMatches(*l, who)) {
    out.decision = Decision::NoOp;  // stale paint: ignored, cannot promote.
    return out;
  }
  if (l->currentTuple().matchesForPaint(painted)) {
    l->painted = true;
    out.changed = true;
    tryPromote(*l, out);
    out.slotId = l->slotId;
    out.tuple = l->currentTuple();
    if (out.decision == Decision::NoOp) out.decision = Decision::Reuse;
  } else {
    out.decision = Decision::NoOp;  // paint of a non-matching tuple: ignored.
  }
  return out;
}

DecisionResult LruModel::cancelTransition(const CallbackIdentity &who) {
  DecisionResult out;
  Lease *l = findSlot(who.slotId);
  if (l == nullptr || !callbackMatches(*l, who)) {
    out.decision = Decision::NoOp;
    return out;
  }
  if (l->state == PresentationState::Incoming) {
    // Unwind the incoming lease. Restore any outgoing lease to committed-visible
    // (no promotion — cancellation is not "used").
    l->occupied = false;
    std::string slotId = l->slotId;
    *l = Lease{};
    l->slotId = slotId;
    for (auto &s : slots_) {
      if (s.occupied && s.state == PresentationState::Outgoing) {
        s.state = PresentationState::CommittedVisible;
        // committed flag preserved; promotedThisEpoch preserved (same epoch).
      }
    }
    out.decision = Decision::NoOp;
    out.queue = QueueResult::Cancelled;
    out.slotId = slotId;
    out.changed = true;
    DecisionResult drained = drainPending();
    if (drained.changed) out.queue = QueueResult::Drained;
  }
  return out;
}

DecisionResult LruModel::park(const CallbackIdentity &who) {
  DecisionResult out;
  Lease *l = findSlot(who.slotId);
  if (l == nullptr || !callbackMatches(*l, who)) {
    out.decision = Decision::NoOp;
    return out;
  }
  if (l->state == PresentationState::CommittedVisible ||
      l->state == PresentationState::Outgoing) {
    l->state = PresentationState::Parked;  // releases the presentation pin.
    beginEpochReset(*l);                   // next presentation is a new epoch.
    out.decision = Decision::Reuse;
    out.slotId = l->slotId;
    out.changed = true;
    DecisionResult drained = drainPending();
    if (drained.changed) out.queue = QueueResult::Drained;
  }
  return out;
}

DecisionResult LruModel::recover(const CallbackIdentity &who) {
  DecisionResult out;
  Lease *l = findSlot(who.slotId);
  if (l == nullptr || !callbackMatches(*l, who)) {
    out.decision = Decision::NoOp;
    return out;
  }
  // WebContent replacement: fresh allocation generation, painted state cleared.
  l->allocationGeneration = ++allocationCounter_;
  l->painted = false;
  l->promotedThisEpoch = false;  // must re-earn recency for the new content.
  out.decision = Decision::Replace;
  out.slotId = l->slotId;
  out.tuple = l->currentTuple();
  out.changed = true;
  return out;
}

DecisionResult LruModel::setRetention(const CallbackIdentity &who, Retention retention) {
  DecisionResult out;
  Lease *l = findSlot(who.slotId);
  if (l == nullptr || !callbackMatches(*l, who)) {
    out.decision = Decision::NoOp;
    return out;
  }
  if (l->retention != retention) {
    l->retention = retention;
    out.changed = true;
    if (retention == Retention::None) {
      // Releasing a retention pin may free a candidate.
      DecisionResult drained = drainPending();
      if (drained.changed) out.queue = QueueResult::Drained;
    }
  }
  out.decision = Decision::Reuse;
  out.slotId = l->slotId;
  return out;
}

DecisionResult LruModel::drainPending() {
  DecisionResult out;
  if (!pending_.has_value()) return out;
  // Only drain onto a genuinely available (empty or unpinned) slot; never touch a
  // pinned owner.
  if (emptySlot() == nullptr && victimBySmallestRecency() == nullptr) {
    return out;  // still fully pinned; keep the coalesced pending request.
  }
  ActivationRequest req = *pending_;
  pending_.reset();
  DecisionResult placed = requestActivation(req);
  out.changed = placed.changed;
  out.decision = placed.decision;
  out.slotId = placed.slotId;
  out.tuple = placed.tuple;
  out.queue = QueueResult::Drained;
  return out;
}

bool LruModel::invariantsHold(std::string *why) const {
  auto fail = [&](const char *msg) {
    if (why) *why = msg;
    return false;
  };

  int occupied = 0;
  for (const auto &s : slots_) if (s.occupied) occupied++;
  if (occupied > kCapacity) return fail("capacity exceeded");

  // A pinned owner is never evicted/transferred: tracked as a hard-zero counter.
  if (pinnedEvictionAttempts_ != 0) return fail("pinned eviction attempted");

  for (const auto &s : slots_) {
    if (!s.occupied) continue;
    // committed-visible is always pinned.
    if (s.state == PresentationState::CommittedVisible && !isPinned(s))
      return fail("committed-visible lease not pinned");
    // in-transition leases are always pinned.
    if ((s.state == PresentationState::Incoming || s.state == PresentationState::Outgoing) &&
        !isPinned(s))
      return fail("in-transition lease not pinned");
    // recency only advances via promotion (bounded by the global counter).
    if (s.lastUsedSequence > sequenceCounter_) return fail("recency ahead of counter");
    // a promoted lease must have a positive sequence.
    if (s.promotedThisEpoch && s.lastUsedSequence == 0)
      return fail("promoted lease with zero recency");
    // promotion requires committed + painted.
    if (s.promotedThisEpoch && (!s.committed || !s.painted))
      return fail("promotion without commit+paint");
  }

  // At most one committed-visible foreground lease at a time.
  int committedVisible = 0;
  for (const auto &s : slots_)
    if (s.occupied && s.state == PresentationState::CommittedVisible) committedVisible++;
  if (committedVisible > 1) return fail("more than one committed-visible lease");

  return true;
}

}  // namespace rncwebisland
