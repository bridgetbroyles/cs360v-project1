# 13. Mock Interview

## 1. “Give me an overview.”

**Testing:** architecture. **Weak:** “It logs from a VM.” **3/3 contains:** three
parts, two guest environments, translation, completion. **Ideal:** “Part I uses
Unicorn to provide an x86-64 CPU, host-backed RAM, and MMIO. Part II attaches a
custom register logger whose commands safely translate guest buffers into a
shared log store. Part III exposes the same service through vhost-user virtio so
Linux's stock console driver publishes descriptor chains; we consume them and
return them on the used ring.”

## 2. “Walk me through `vmm_create()`.”

**Testing:** setup order/state. **Weak:** “It initializes Unicorn.” **3/3:** store,
CPU, RAM allocation/mapping, MMIO, device, registers, hooks, cleanup. **Ideal:**
use the ten-step flow in Section 4 and explain why RIP is set during loading.

## 3. “What happens on invalid guest memory?”

**Testing:** containment. **Weak:** “It returns an error.” **3/3:** Unicorn hook,
state, stderr, stop, false, exit 77. **Ideal:** recite trace D and explain why
false means no retry.

## 4. “Why GPA-to-host translation?”

**Testing:** address spaces/security. **Weak:** “Addresses differ.” **3/3:** GPA
is guest-controlled number, HVA is dereferenceable, whole-range/overflow check.
**Ideal:** give the `end - gpa` example and contrast one-region versus region table.

## 5. “How do LOG and STAT differ?”

**Testing:** direction and validation. **Weak:** “One logs; one gives stats.”
**3/3:** LOG reads, max 4096, zero special case, counters; STAT writes eight bytes,
checks offered LEN first, no counters. **Ideal:** trace both commit paths and state
that failures have no side effects.

## 6. “Explain a virtqueue.”

**Testing:** protocol. **Weak:** “A circular queue.” **3/3:** descriptor table,
avail heads/index, chains, used heads/index, ownership direction. **Ideal:** draw
the diagram and explain free-running indices modulo `num`.

## 7. “Walk through `vlog_virtq_handle()`.”

**Testing:** full implementation. **Weak:** “It loops over descriptors.” **3/3:**
snapshot/acquire, bounds, direct/indirect, translation/cap, sink, completion/release,
cursors/return count. **Ideal:** give the 30-second answer, then discuss malformed
behavior and why used length is zero.

## 8. “Why indirect descriptors?”

**Testing:** scatter/gather scalability. **Weak:** “Another pointer.” **3/3:** one
main-table entry names a guest-resident descriptor table starting at zero; validate
shape/map, prevent nesting/cycles. **Ideal:** note it conserves scarce main-ring
entries and adds a second untrusted traversal surface.

## 9. “What if a descriptor hits a gap?”

**Testing:** translator correctness. **Weak:** “It crashes or errors.” **3/3:**
no region fully contains it, translator returns NULL, helper skips bytes, chain
still emits accumulated data and completes. **Ideal:** distinguish start-in-gap
and region-straddling cases.

## 10. “Why this safety check? Could you choose another design?”

**Testing:** tradeoff reasoning. **Weak:** “For security.” **3/3:** name threat,
invariant, consequence, alternative. **Ideal example:** “The hop cap prevents a
guest cycle from monopolizing the backend. A visited bitmap detects cycles sooner
but allocates or consumes stack proportional to queue size. The table-size cap is
allocation-free and every valid chain fits within it.”

## 11. “Why the used ring?”

**Testing:** lifecycle. **Weak:** “It says done.” **3/3:** ownership/resource
return, head ID, zero written length, ordering, driver hang if absent. **Ideal:**
explain entry-before-index publication and backend interrupt on positive count.

## 12. “Critique your solution.”

**Testing:** maturity. **Weak:** “Tests pass.” **3/3:** strengths and limitations.
**Ideal:** “It meets the fixed spec and survives tested malformed input through
full-range translation, direction checks, caps, and completion. It copies into a
4 KiB stack buffer, silently skips bad pieces, and assumes the provided backend's
single-handler context. Production code would formalize reset/errors, negotiated
features, concurrency, and notification suppression.”

