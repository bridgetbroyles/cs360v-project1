# 10. Design Decisions & Tradeoffs

| Decision | Why ours fits | Alternative and tradeoff | Strong interview point |
|---|---|---|---|
| Host-backed contiguous Part I RAM | zero-copy and simple offset translation | Unicorn-owned RAM plus `uc_mem_read`: more copies, stronger encapsulation | mapping and translator must agree on the same buffer |
| Separate MMIO mappings/callbacks | clear device ownership and relative offsets | one global dispatcher: centralized but more coupling | callback context is `v` for serial, `v->dev` for logger |
| Fault hook stops and returns false | fixed map, deterministic exit 77 | demand-map and retry for paging/lazy allocation | untrusted guest fault cannot crash host |
| Subtraction-based bounds checks | avoids integer overflow | checked-add helper is equally valid but more machinery | validate whole half-open range |
| Stateful operand registers | mirrors hardware and spec; values persist | pass a command struct in RAM: fewer MMIO writes but a different ABI | CMD is the synchronous commit point |
| Clear error before every CMD | STATUS reflects most recent command | overwrite status from scratch; simpler if READY were the only persistent bit | `set_error` ORs, so stale codes must be removed |
| Validate before side effect | prevents partial records/writes | streaming validation/copy: lower buffer use but partial failure risk | especially critical for STAT host-to-guest write |
| Snapshot `avail->idx` | bounded batch and clear acquire point | re-read continuously: lower latency, possible starvation | later publications wait for next invocation |
| Local 4096-byte record | simple concatenation with hard cap | dynamic allocation/scatter-gather sink: less stack/fewer copies, more failure paths | guest cannot control allocation or overflow buffer |
| Skip writable descriptors | correct direction and prevents leaking bytes | reject whole chain: stricter but loses valid readable segments | WRITE means permission for device output, not input data |
| Bound walks by table size | cycles terminate without allocation | visited bitmap/Floyd detection: detects sooner but costs memory/complexity | safety needs finite work, not perfect diagnosis |
| Complete malformed chains | avoids guest/driver stall | device reset/error status: more explicit, not exposed by this API | used ring is resource return, not merely success log |
| Full descriptor translation before truncation | never partly trusts invalid range | validate only copied prefix: faster but accepts malformed declaration | descriptor metadata remains untrusted even when record is full |
| Memory barriers around publication | shared-memory ordering | locks: heavier and not shared across VM boundary | ring entry must become visible before index |

Likely interviewer follow-up: “Would this be production-grade?” Strong answer:
“It has the assignment's key safety properties, but production virtio would add
feature negotiation constraints, event-index notification logic, device reset and
error reporting, stricter malformed-chain policy, concurrency synchronization,
and likely scatter/gather processing to avoid copies.”

