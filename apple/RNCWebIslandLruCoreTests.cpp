// gh337 — headless test harness for the pure pin-aware LRU decision core.
//
// Build & run on any machine with a C++17 clang (no Xcode project required):
//   clang++ -std=c++17 -I apple apple/RNCWebIslandLruCore.cpp \
//     apple/RNCWebIslandLruCoreTests.cpp -o /tmp/lru_tests && /tmp/lru_tests
//
// Covers the issue's deterministic scenarios (Back-then-push, consecutive
// pushes, interactive cancel, paint before/after commit, delayed completion
// after a newer lease, duplicate stable-id claims, all-slots-pinned queueing,
// eviction/replacement, WebContent termination, and A,B,A,C recency) plus a
// randomized property test asserting the safety invariants on EVERY prefix.

#include "RNCWebIslandLruCore.hpp"

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <random>
#include <string>
#include <vector>

using namespace rncwebisland;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    g_checks++;                                                                \
    if (!(cond)) {                                                             \
      g_failures++;                                                            \
      std::printf("  FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__);          \
    }                                                                          \
  } while (0)

static CallbackIdentity idOf(const Lease &l) {
  return CallbackIdentity{l.slotId, l.allocationGeneration, l.leaseGeneration, l.activationKey};
}

static const Lease &slotById(const LruModel &m, const std::string &slotId) {
  return m.slot(0).slotId == slotId ? m.slot(0) : m.slot(1);
}

static ActivationRequest req(const std::string &id, const std::string &act,
                             const std::string &doc, const std::string &fam,
                             const std::string &rev = "r1") {
  return ActivationRequest{id, act, doc, rev, fam, Retention::None};
}

// Drive a lease to committed+painted (a full presentation epoch) so it earns a
// recency ordinal.
static void present(LruModel &m, const std::string &slotId) {
  const Lease &l = slotById(m, slotId);
  CallbackIdentity who = idOf(l);
  m.commitPresentation(who);
  IdentityTuple t = slotById(m, slotId).currentTuple();
  m.paint(who, t);
}

static void scenario_back_then_push() {
  std::printf("scenario: Back followed immediately by push\n");
  LruModel m;
  auto a = m.requestActivation(req("thread", "thread-a", "/t/a", "thread"));
  CHECK(a.decision == Decision::Allocate, "A allocates");
  present(m, a.slotId);
  // Back: park A.
  m.park(idOf(slotById(m, a.slotId)));
  // Immediate push B: reuses the freed slot (empty? no — parked still resident).
  auto b = m.requestActivation(req("thread", "thread-b", "/t/b", "thread"));
  CHECK(b.decision == Decision::Allocate || b.decision == Decision::Evict, "push B lands");
  std::string why;
  CHECK(m.invariantsHold(&why), why.c_str());
  CHECK(m.pinnedEvictionAttempts() == 0, "no pinned eviction");
}

static void scenario_consecutive_pushes() {
  std::printf("scenario: consecutive pushes fill both slots\n");
  LruModel m;
  auto a = m.requestActivation(req("list", "list-a", "/l", "list"));
  present(m, a.slotId);
  auto b = m.requestActivation(req("thread", "thread-b", "/t/b", "thread"));
  present(m, b.slotId);
  CHECK(m.residentCount() == 2, "both slots resident");
  std::string why;
  CHECK(m.invariantsHold(&why), why.c_str());
}

static void scenario_interactive_cancel() {
  std::printf("scenario: interactive cancellation restores outgoing\n");
  LruModel m;
  auto a = m.requestActivation(req("list", "list-a", "/l", "list"));
  present(m, a.slotId);
  // Start a transition to B (A becomes outgoing, pinned).
  auto b = m.requestActivation(req("thread", "thread-b", "/t/b", "thread"));
  CHECK(slotById(m, a.slotId).state == PresentationState::Outgoing, "A outgoing during transition");
  CHECK(m.isPinned(slotById(m, a.slotId)), "outgoing A pinned");
  // Cancel: B unwinds, A restored to committed-visible without re-promotion.
  Seq before = slotById(m, a.slotId).lastUsedSequence;
  m.cancelTransition(idOf(slotById(m, b.slotId)));
  CHECK(slotById(m, a.slotId).state == PresentationState::CommittedVisible, "A restored visible");
  CHECK(slotById(m, a.slotId).lastUsedSequence == before, "cancel does not promote");
  std::string why;
  CHECK(m.invariantsHold(&why), why.c_str());
}

static void scenario_paint_order() {
  std::printf("scenario: matching paint before and after commit both promote once\n");
  // paint after commit
  {
    LruModel m;
    auto a = m.requestActivation(req("list", "list-a", "/l", "list"));
    CallbackIdentity who = idOf(slotById(m, a.slotId));
    m.commitPresentation(who);
    CHECK(slotById(m, a.slotId).lastUsedSequence == 0, "commit alone does not promote");
    m.paint(who, slotById(m, a.slotId).currentTuple());
    CHECK(slotById(m, a.slotId).lastUsedSequence == 1, "commit then paint promotes once");
  }
  // paint before commit
  {
    LruModel m;
    auto a = m.requestActivation(req("list", "list-a", "/l", "list"));
    CallbackIdentity who = idOf(slotById(m, a.slotId));
    m.paint(who, slotById(m, a.slotId).currentTuple());
    CHECK(slotById(m, a.slotId).lastUsedSequence == 0, "paint alone does not promote");
    m.commitPresentation(who);
    CHECK(slotById(m, a.slotId).lastUsedSequence == 1, "paint then commit promotes once");
    // A second paint of the same tuple must not re-promote within the epoch.
    m.paint(who, slotById(m, a.slotId).currentTuple());
    CHECK(slotById(m, a.slotId).lastUsedSequence == 1, "no double promotion in one epoch");
  }
}

static void scenario_delayed_stale_callback() {
  std::printf("scenario: delayed completion after a newer lease is ignored\n");
  LruModel m;
  auto a = m.requestActivation(req("thread", "thread-a", "/t/a", "thread"));
  CallbackIdentity staleWho = idOf(slotById(m, a.slotId));  // capture old generation.
  present(m, a.slotId);
  m.park(idOf(slotById(m, a.slotId)));
  // A newer activation reuses the same slot object via eviction path: force it by
  // filling the other slot then re-requesting into the LRU victim.
  auto b = m.requestActivation(req("thread", "thread-b", "/t/b", "thread"));
  present(m, b.slotId);
  auto c = m.requestActivation(req("thread", "thread-c", "/t/c", "thread"));  // evicts LRU
  std::string sC = c.slotId;
  Seq beforeSeq = m.sequenceCounter();
  // The stale callback (old allocation/lease gen) targeting that slot must be a
  // no-op: no promotion, no ownership change.
  auto ignored = m.paint(staleWho, IdentityTuple{});
  CHECK(ignored.decision == Decision::NoOp, "stale paint ignored");
  CHECK(m.sequenceCounter() == beforeSeq, "stale callback did not promote");
  std::string why;
  CHECK(m.invariantsHold(&why), why.c_str());
}

static void scenario_duplicate_stable_id() {
  std::printf("scenario: duplicate stable-id claim reuses the same lease\n");
  LruModel m;
  auto a = m.requestActivation(req("thread", "thread-a", "/t/a", "thread"));
  present(m, a.slotId);
  Gen alloc = slotById(m, a.slotId).allocationGeneration;
  // Same activation re-request (duplicate claim) → reuse, no new slot, no fresh
  // allocation generation.
  auto again = m.requestActivation(req("thread", "thread-a", "/t/a", "thread"));
  CHECK(again.slotId == a.slotId, "duplicate claim keeps the same slot");
  CHECK(again.decision == Decision::Reuse, "duplicate claim reuses");
  CHECK(slotById(m, a.slotId).allocationGeneration == alloc, "no fresh allocation on reuse");
  CHECK(m.residentCount() == 1, "no third slot for duplicate id");
}

static void scenario_all_pinned_queue() {
  std::printf("scenario: all-slots-pinned queueing and coalescing\n");
  LruModel m;
  // Pin both slots: one committed-visible, one incoming (transition in flight).
  auto a = m.requestActivation(req("list", "list-a", "/l", "list"));
  present(m, a.slotId);  // committed-visible (pinned)
  auto b = m.requestActivation(req("thread", "thread-b", "/t/b", "thread"));  // incoming (pinned), A->outgoing
  CHECK(m.isPinned(m.slot(0)) && m.isPinned(m.slot(1)), "both slots pinned mid-transition");
  // A third activation while both pinned → queue.
  auto c = m.requestActivation(req("thread", "thread-c", "/t/c", "thread"));
  CHECK(c.decision == Decision::Queue, "third activation queued when all pinned");
  CHECK(c.queue == QueueResult::Queued, "queued result");
  CHECK(m.pinnedEvictionAttempts() == 0, "no pinned eviction while queued");
  // A fourth coalesces onto the latest.
  auto d = m.requestActivation(req("thread", "thread-d", "/t/d", "thread"));
  CHECK(d.queue == QueueResult::Coalesced, "coalesced to newest");
  // Commit B: A parks, a candidate frees, the pending (D) drains in.
  present(m, b.slotId);
  bool draining = m.hasPending() == false;
  CHECK(draining, "pending drained after a candidate freed");
  std::string why;
  CHECK(m.invariantsHold(&why), why.c_str());
}

static void scenario_eviction_replacement() {
  std::printf("scenario: eviction selects least-recently-used eligible lease\n");
  LruModel m;
  auto a = m.requestActivation(req("list", "list-a", "/l", "list"));
  present(m, a.slotId);           // A recency 1
  auto b = m.requestActivation(req("thread", "thread-b", "/t/b", "thread"));
  present(m, b.slotId);           // B recency 2, A parked
  // Both resident, A older. Park B so both are unpinned candidates.
  m.park(idOf(slotById(m, b.slotId)));
  auto c = m.requestActivation(req("automations", "auto-c", "/a", "automations"));
  CHECK(c.evictedSlotId.has_value(), "an eviction occurred");
  CHECK(*c.evictedSlotId == a.slotId, "evicted the least-recently-used (A)");
  std::string why;
  CHECK(m.invariantsHold(&why), why.c_str());
}

static void scenario_webcontent_termination() {
  std::printf("scenario: WebContent termination bumps allocation generation\n");
  LruModel m;
  auto a = m.requestActivation(req("thread", "thread-a", "/t/a", "thread"));
  present(m, a.slotId);
  Gen before = slotById(m, a.slotId).allocationGeneration;
  bool paintedBefore = slotById(m, a.slotId).painted;
  CHECK(paintedBefore, "painted before termination");
  auto r = m.recover(idOf(slotById(m, a.slotId)));
  CHECK(r.decision == Decision::Replace, "recover replaces content");
  CHECK(slotById(m, a.slotId).allocationGeneration == before + 1, "allocation generation bumped");
  CHECK(!slotById(m, a.slotId).painted, "painted state invalidated");
}

static void scenario_ABAC_recency() {
  std::printf("scenario: A,B,A,C — A bubbles to most-recent only after presentation\n");
  LruModel m;
  auto a = m.requestActivation(req("list", "list-a", "/l", "list"));
  present(m, a.slotId);   // seq 1
  auto b = m.requestActivation(req("thread", "thread-b", "/t/b", "thread"));
  present(m, b.slotId);   // seq 2 ; A parked (outgoing->parked)
  Seq aSeqBeforeRepresent = slotById(m, a.slotId).lastUsedSequence;
  CHECK(aSeqBeforeRepresent == 1, "A recency still 1 before re-presentation");
  // Re-activate A. Its recency must NOT advance until it is committed+painted.
  auto a2 = m.requestActivation(req("list", "list-a", "/l", "list"));
  CHECK(slotById(m, a.slotId).lastUsedSequence == 1, "reactivation request alone does not promote A");
  present(m, a.slotId);   // seq 3 — now A is most-recent
  CHECK(slotById(m, a.slotId).lastUsedSequence == 3, "A promoted only after committed presentation");
  // Now C evicts the least-recently-used eligible lease. Park A and B so both are
  // candidates; B (seq 2) is older than A (seq 3), so B is evicted.
  m.park(idOf(slotById(m, a.slotId)));
  auto c = m.requestActivation(req("automations", "auto-c", "/a", "automations"));
  CHECK(c.evictedSlotId.has_value() && *c.evictedSlotId == b.slotId,
        "least-recent eligible (B) evicted, not the freshly-presented A");
  std::string why;
  CHECK(m.invariantsHold(&why), why.c_str());
}

static void scenario_never_evict_pinned() {
  std::printf("scenario: a pinned lease is never evicted or transferred\n");
  LruModel m;
  auto a = m.requestActivation(req("list", "list-a", "/l", "list"));
  present(m, a.slotId);  // committed-visible pinned
  auto b = m.requestActivation(req("thread", "thread-b", "/t/b", "thread"));
  present(m, b.slotId);  // B committed, A parked
  // Re-present A so it's committed-visible again; keep B parked.
  m.requestActivation(req("list", "list-a", "/l", "list"));
  present(m, a.slotId);
  // Now A is pinned (committed-visible), B parked (candidate). A new activation
  // must evict B, never pinned A.
  auto c = m.requestActivation(req("automations", "auto-c", "/a", "automations"));
  CHECK(!c.evictedSlotId.has_value() || *c.evictedSlotId == b.slotId, "only unpinned B evictable");
  CHECK(m.pinnedEvictionAttempts() == 0, "pinned eviction counter stays zero");
}

// ---- randomized property test: invariants on every prefix ------------------

static void property_random_traces() {
  std::printf("property: safety invariants hold on every prefix of random traces\n");
  std::mt19937 rng(0xC0FFEE);
  const char *families[] = {"list", "thread", "automations"};
  int worstFailures = g_failures;

  for (int trial = 0; trial < 4000; ++trial) {
    LruModel m;
    int actCounter = 0;
    for (int step = 0; step < 40; ++step) {
      int op = rng() % 8;
      switch (op) {
        case 0:
        case 1:
        case 2: {  // request activation (weighted)
          int a = static_cast<int>(rng() % 5);
          std::string act = "act-" + std::to_string(a);
          std::string fam = families[rng() % 3];
          std::string rev = "r" + std::to_string(rng() % 3);
          m.requestActivation(req("id-" + std::to_string(a), act, "/d/" + std::to_string(a), fam, rev));
          actCounter++;
          break;
        }
        case 3: {  // commit a random slot
          const Lease &l = m.slot(rng() % 2);
          if (l.occupied) m.commitPresentation(idOf(l));
          break;
        }
        case 4: {  // paint a random slot (sometimes stale)
          const Lease &l = m.slot(rng() % 2);
          if (l.occupied) {
            CallbackIdentity who = idOf(l);
            if (rng() % 4 == 0) who.leaseGeneration += 99;  // inject staleness
            m.paint(who, l.currentTuple());
          }
          break;
        }
        case 5: {  // cancel a random slot
          const Lease &l = m.slot(rng() % 2);
          if (l.occupied) m.cancelTransition(idOf(l));
          break;
        }
        case 6: {  // park a random slot
          const Lease &l = m.slot(rng() % 2);
          if (l.occupied) m.park(idOf(l));
          break;
        }
        case 7: {  // recover or toggle retention
          const Lease &l = m.slot(rng() % 2);
          if (l.occupied) {
            if (rng() % 2)
              m.recover(idOf(l));
            else
              m.setRetention(idOf(l), (rng() % 2) ? Retention::Caller : Retention::None);
          }
          break;
        }
      }
      std::string why;
      if (!m.invariantsHold(&why)) {
        g_failures++;
        std::printf("  FAIL: invariant broken trial=%d step=%d: %s\n", trial, step, why.c_str());
        break;
      }
      if (m.pinnedEvictionAttempts() != 0) {
        g_failures++;
        std::printf("  FAIL: pinned eviction attempted trial=%d step=%d\n", trial, step);
        break;
      }
    }
  }
  g_checks++;
  if (g_failures == worstFailures) std::printf("  ok: 4000 random traces x 40 steps clean\n");
}

int main() {
  scenario_back_then_push();
  scenario_consecutive_pushes();
  scenario_interactive_cancel();
  scenario_paint_order();
  scenario_delayed_stale_callback();
  scenario_duplicate_stable_id();
  scenario_all_pinned_queue();
  scenario_eviction_replacement();
  scenario_webcontent_termination();
  scenario_ABAC_recency();
  scenario_never_evict_pinned();
  property_random_traces();

  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
