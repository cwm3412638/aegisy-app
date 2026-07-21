// Keep the internal contract independently testable until it is connected to
// the Runtime/AAP compaction path. This test target intentionally adds no
// public module or execution authority.
#[path = "../src/context_threshold.rs"]
mod context_threshold;
