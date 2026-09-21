# docs/plans

Forward-looking architecture and implementation plans for subsystems that do
not yet exist (or are being substantially reworked) in the tree. Unlike
`docs/changelog/` (records of what *was* done) these describe what *will* be
built and why, deep enough to implement from.

Each plan is code-grounded: it cites the real vaios/NavHAL symbols it builds on
by file and symbol, and states its challenges together with the chosen
solution rather than only the happy path.

| Plan | Status | Summary |
|---|---|---|
| [`bus-subsystem.md`](bus-subsystem.md) | In progress (B0–B3 done) | Kernel IPC "Bus": fixed-pool, index-linked pub/sub with guaranteed/best-effort QoS, elastic borrowing, ref-counted delivery, and a multi-producer publish pipeline. |
