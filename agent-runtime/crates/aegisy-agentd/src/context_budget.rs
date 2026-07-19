use serde::Serialize;

pub const SCHEMA_VERSION: &str = "context-budget/0.1";

#[derive(Debug, Clone, Copy)]
pub struct BudgetInput<'a> {
    pub id: &'a str,
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
            class: context_class(input.priority),
            priority_score: priority_score(input.priority),
            requested_bytes: input.requested_bytes,
            allocated_bytes: 0,
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
    let truncated = entries
        .iter()
        .any(|entry| entry.requested_bytes > entry.allocated_bytes);
    BudgetPlan {
        schema_version: SCHEMA_VERSION,
        max_total_bytes,
        max_item_bytes,
        requested_bytes,
        allocated_bytes,
        truncated,
        entries,
    }
}

fn context_class(priority: Option<&str>) -> String {
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
                priority: Some("pinned"),
                requested_bytes: 8,
                excluded: false,
            },
            BudgetInput {
                id: "managed",
                priority: Some("instruction-rank-1000"),
                requested_bytes: 8,
                excluded: false,
            },
            BudgetInput {
                id: "nested",
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
    }

    #[test]
    fn selected_pin_priority_remains_a_pinned_budget_class() {
        let plan = allocate(
            &[
                BudgetInput {
                    id: "ordinary",
                    priority: None,
                    requested_bytes: 8,
                    excluded: false,
                },
                BudgetInput {
                    id: "selected-pin",
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
    }

    #[test]
    fn per_item_and_total_limits_are_hard_bounds() {
        let plan = allocate(
            &[
                BudgetInput {
                    id: "one",
                    priority: None,
                    requested_bytes: 100,
                    excluded: false,
                },
                BudgetInput {
                    id: "two",
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
}
