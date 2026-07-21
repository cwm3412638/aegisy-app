use serde::Serialize;

use crate::tokenizer::{estimate_bytes, summary as tokenizer_summary, TokenizerSummary};

pub const SCHEMA_VERSION: &str = "context-budget/0.1";

#[derive(Debug, Clone, Copy)]
pub struct BudgetInput<'a> {
    pub id: &'a str,
    pub kind: Option<&'a str>,
    pub priority: Option<&'a str>,
    pub requested_bytes: usize,
    pub excluded: bool,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct BudgetEntry {
    pub id: String,
    pub class: String,
    pub priority_score: u32,
    pub requested_bytes: usize,
    pub allocated_bytes: usize,
    pub estimated_tokens: u64,
    pub included: bool,
    pub reason: String,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct BudgetPlan {
    pub schema_version: &'static str,
    pub max_total_bytes: usize,
    pub max_item_bytes: usize,
    pub requested_bytes: usize,
    pub allocated_bytes: usize,
    pub truncated: bool,
    pub estimated_tokens: u64,
    pub tokenizer: TokenizerSummary,
    pub entries: Vec<BudgetEntry>,
}

pub fn allocate(
    inputs: &[BudgetInput<'_>],
    max_total_bytes: usize,
    max_item_bytes: usize,
) -> BudgetPlan {
    let mut entries = inputs
        .iter()
        .map(|input| BudgetEntry {
            id: input.id.to_owned(),
            class: context_class(input.kind, input.priority),
            priority_score: priority_score(input.priority),
            requested_bytes: input.requested_bytes,
            allocated_bytes: 0,
            estimated_tokens: 0,
            included: false,
            reason: if input.excluded {
                "excluded".into()
            } else {
                "pending".into()
            },
        })
        .collect::<Vec<_>>();
    let requested_bytes = inputs
        .iter()
        .map(|input| input.requested_bytes)
        .fold(0_usize, usize::saturating_add);
    let mut order = (0..inputs.len()).collect::<Vec<_>>();
    order.sort_by(|left, right| {
        entries[*right]
            .priority_score
            .cmp(&entries[*left].priority_score)
            .then_with(|| left.cmp(right))
    });
    let mut remaining = max_total_bytes;
    for index in order {
        if inputs[index].excluded {
            continue;
        }
        let allocation = inputs[index]
            .requested_bytes
            .min(max_item_bytes)
            .min(remaining);
        entries[index].allocated_bytes = allocation;
        entries[index].included = allocation > 0;
        entries[index].reason = if allocation < inputs[index].requested_bytes {
            "context-budget".into()
        } else {
            "allocated".into()
        };
        remaining = remaining.saturating_sub(allocation);
    }
    let allocated_bytes = entries
        .iter()
        .map(|entry| entry.allocated_bytes)
        .fold(0_usize, usize::saturating_add);
    for entry in &mut entries {
        entry.estimated_tokens = estimate_bytes(entry.allocated_bytes).tokens;
    }
    let estimated_tokens = entries
        .iter()
        .map(|entry| entry.estimated_tokens)
        .fold(0_u64, u64::saturating_add);
    // Intentional exclusions are policy decisions, not budget truncation. Keep
    // the signal reserved for eligible context that could not fit the limits.
    let truncated = entries
        .iter()
        .zip(inputs.iter())
        .any(|(entry, input)| !input.excluded && entry.requested_bytes > entry.allocated_bytes);
    BudgetPlan {
        schema_version: SCHEMA_VERSION,
        max_total_bytes,
        max_item_bytes,
        requested_bytes,
        allocated_bytes,
        truncated,
        estimated_tokens,
        tokenizer: tokenizer_summary(),
        entries,
    }
}

fn context_class(kind: Option<&str>, priority: Option<&str>) -> String {
    match kind {
        Some("task-state") => return "task-state".into(),
        Some("recent-turn") => return "recent-turn".into(),
        Some("tool-result") => return "tool-result".into(),
        Some("search") | Some("search-result") => return "search".into(),
        Some("repository-map") => return "repository-map".into(),
        _ => {}
    }
    if priority.is_some_and(|value| value.starts_with("instruction-rank-")) {
        "instruction".into()
    } else if priority == Some("pinned")
        || priority.is_some_and(|value| value.starts_with("pinned-priority-"))
    {
        "pinned".into()
    } else {
        "context".into()
    }
}

fn priority_score(priority: Option<&str>) -> u32 {
    let Some(priority) = priority else {
        return 500;
    };
    if let Some(rank) = priority.strip_prefix("instruction-rank-") {
        return rank.parse::<u32>().unwrap_or(400).min(1_000);
    }
    if priority == "pinned" {
        return 850;
    }
    if let Some(rank) = priority.strip_prefix("pinned-priority-") {
        return rank.parse::<u32>().unwrap_or(850).min(1_000);
    }
    500
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn allocates_deterministically_by_priority_without_reordering_entries() {
        let inputs = [
            BudgetInput {
                id: "user",
                kind: None,
                priority: Some("pinned"),
                requested_bytes: 8,
                excluded: false,
            },
            BudgetInput {
                id: "managed",
                kind: None,
                priority: Some("instruction-rank-1000"),
                requested_bytes: 8,
                excluded: false,
            },
            BudgetInput {
                id: "nested",
                kind: None,
                priority: Some("instruction-rank-401"),
                requested_bytes: 8,
                excluded: false,
            },
        ];
        let plan = allocate(&inputs, 16, 8);
        assert_eq!(
            plan.entries
                .iter()
                .map(|entry| entry.id.as_str())
                .collect::<Vec<_>>(),
            vec!["user", "managed", "nested"]
        );
        assert_eq!(plan.entries[1].allocated_bytes, 8);
        assert_eq!(plan.entries[0].allocated_bytes, 8);
        assert_eq!(plan.entries[2].allocated_bytes, 0);
        assert_eq!(plan.entries[2].reason, "context-budget");
        assert!(plan.truncated);
        assert_eq!(plan.estimated_tokens, 4);
        assert_eq!(plan.tokenizer.authority, "conservative-unknown");
    }

    #[test]
    fn selected_pin_priority_remains_a_pinned_budget_class() {
        let plan = allocate(
            &[
                BudgetInput {
                    id: "ordinary",
                    kind: None,
                    priority: None,
                    requested_bytes: 8,
                    excluded: false,
                },
                BudgetInput {
                    id: "selected-pin",
                    kind: None,
                    priority: Some("pinned-priority-900"),
                    requested_bytes: 8,
                    excluded: false,
                },
            ],
            8,
            8,
        );
        assert_eq!(plan.entries[0].allocated_bytes, 0);
        assert_eq!(plan.entries[1].class, "pinned");
        assert_eq!(plan.entries[1].priority_score, 900);
        assert_eq!(plan.entries[1].allocated_bytes, 8);
    }

    #[test]
    fn excluded_entries_never_receive_budget() {
        let plan = allocate(
            &[BudgetInput {
                id: "excluded",
                kind: None,
                priority: Some("instruction-rank-1000"),
                requested_bytes: 64,
                excluded: true,
            }],
            128,
            32,
        );
        assert_eq!(plan.allocated_bytes, 0);
        assert_eq!(plan.entries[0].reason, "excluded");
        assert!(!plan.entries[0].included);
        assert!(!plan.truncated);
    }

    #[test]
    fn large_monorepo_exclusions_do_not_look_like_budget_overflow() {
        let mut inputs = vec![BudgetInput {
            id: "selected-file",
            kind: Some("file"),
            priority: Some("pinned"),
            requested_bytes: 16 * 1024,
            excluded: false,
        }];
        let irrelevant_ids = (0..15)
            .map(|index| format!("irrelevant-{index}"))
            .collect::<Vec<_>>();
        inputs.extend(irrelevant_ids.iter().map(|id| BudgetInput {
            id: id.as_str(),
            kind: Some("repository-map"),
            priority: None,
            requested_bytes: 16 * 1024,
            excluded: true,
        }));
        let plan = allocate(&inputs, 64 * 1024, 16 * 1024);
        assert_eq!(plan.entries[0].allocated_bytes, 16 * 1024);
        assert!(plan.entries[1..].iter().all(|entry| {
            entry.allocated_bytes == 0 && !entry.included && entry.reason == "excluded"
        }));
        assert!(!plan.truncated);
        assert_eq!(plan.allocated_bytes, 16 * 1024);
    }

    #[test]
    fn relevant_pinned_context_resists_irrelevant_repository_map_under_tight_budget() {
        let mut inputs = vec![BudgetInput {
            id: "selected-file",
            kind: Some("file"),
            priority: Some("pinned"),
            requested_bytes: 16 * 1024,
            excluded: false,
        }];
        let repository_map_ids = (0..4)
            .map(|index| format!("repo-map-{index}"))
            .collect::<Vec<_>>();
        inputs.extend(repository_map_ids.iter().map(|id| BudgetInput {
            id: id.as_str(),
            kind: Some("repository-map"),
            priority: None,
            requested_bytes: 16 * 1024,
            excluded: false,
        }));
        let plan = allocate(&inputs, 16 * 1024, 16 * 1024);
        assert_eq!(plan.entries[0].allocated_bytes, 16 * 1024);
        assert!(plan.entries[1..].iter().all(|entry| {
            entry.allocated_bytes == 0 && !entry.included && entry.reason == "context-budget"
        }));
        assert!(plan.truncated);
    }

    #[test]
    fn per_item_and_total_limits_are_hard_bounds() {
        let plan = allocate(
            &[
                BudgetInput {
                    id: "one",
                    kind: None,
                    priority: None,
                    requested_bytes: 100,
                    excluded: false,
                },
                BudgetInput {
                    id: "two",
                    kind: None,
                    priority: None,
                    requested_bytes: 100,
                    excluded: false,
                },
            ],
            150,
            80,
        );
        assert_eq!(plan.allocated_bytes, 150);
        assert!(plan.entries.iter().all(|entry| entry.allocated_bytes <= 80));
        assert!(plan.truncated);
    }

    #[test]
    fn classifies_existing_context_consumers_without_changing_priority_order() {
        let plan = allocate(
            &[
                BudgetInput {
                    id: "search-item",
                    kind: Some("search-result"),
                    priority: None,
                    requested_bytes: 4,
                    excluded: false,
                },
                BudgetInput {
                    id: "tool-item",
                    kind: Some("tool-result"),
                    priority: Some("pinned-priority-900"),
                    requested_bytes: 4,
                    excluded: false,
                },
                BudgetInput {
                    id: "repo-map",
                    kind: Some("repository-map"),
                    priority: None,
                    requested_bytes: 4,
                    excluded: false,
                },
            ],
            8,
            8,
        );
        assert_eq!(plan.entries[0].class, "search");
        assert_eq!(plan.entries[1].class, "tool-result");
        assert_eq!(plan.entries[2].class, "repository-map");
        assert_eq!(plan.entries[1].allocated_bytes, 4);
        assert_eq!(plan.entries[0].allocated_bytes, 4);
        assert_eq!(plan.entries[2].allocated_bytes, 0);
        assert_eq!(plan.entries[0].estimated_tokens, 1);
        assert_eq!(plan.entries[1].estimated_tokens, 1);
        assert_eq!(plan.entries[2].estimated_tokens, 0);
    }
}
