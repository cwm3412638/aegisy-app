use serde::Serialize;
use serde_json::Value;

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct CommandRisk {
    pub level: String,
    pub confidence: String,
    pub categories: Vec<String>,
    pub reasons: Vec<String>,
}

pub fn classify(raw_command: &str, actions: &Value) -> CommandRisk {
    let command = raw_command.to_lowercase();
    let mut categories = Vec::new();
    let mut reasons = Vec::new();
    let mut level = 0_u8;

    let high_risk = [
        (
            "git reset",
            "git-history",
            "Git reset can discard repository state",
        ),
        (
            "git clean",
            "git-clean",
            "Git clean can delete untracked files",
        ),
        (
            "push --force",
            "git-remote",
            "force push can rewrite remote history",
        ),
        (
            "push -f",
            "git-remote",
            "force push can rewrite remote history",
        ),
        (
            "rm -rf",
            "filesystem-delete",
            "recursive forced deletion can destroy data",
        ),
        (
            "diskpart",
            "system-storage",
            "diskpart can modify system storage",
        ),
        (
            "format ",
            "system-storage",
            "format can destroy filesystem data",
        ),
        (
            "remove-item -recurse",
            "filesystem-delete",
            "recursive deletion can destroy data",
        ),
    ];
    for (needle, category, reason) in high_risk {
        if command.contains(needle) {
            level = level.max(3);
            push_unique(&mut categories, category);
            push_unique(&mut reasons, reason);
        }
    }

    let medium_risk = [
        ("curl ", "network", "command can access the network"),
        ("wget ", "network", "command can access the network"),
        (
            "invoke-webrequest",
            "network",
            "command can access the network",
        ),
        (
            "git push",
            "git-remote",
            "command can mutate a remote repository",
        ),
        (
            "git commit",
            "git-write",
            "command can create repository history",
        ),
        (
            "git rebase",
            "git-history",
            "command can rewrite local history",
        ),
        (
            "git merge",
            "git-write",
            "command can modify the working tree",
        ),
        ("sudo ", "privilege", "command requests elevated privileges"),
        (
            "chmod ",
            "filesystem-metadata",
            "command can change file permissions",
        ),
        (
            "chown ",
            "filesystem-metadata",
            "command can change file ownership",
        ),
        (
            "npm install",
            "dependency-execution",
            "package installation can run project scripts",
        ),
        (
            "cargo build",
            "project-execution",
            "builds can execute project-controlled code",
        ),
        (
            "cargo test",
            "project-execution",
            "tests can execute project-controlled code",
        ),
    ];
    for (needle, category, reason) in medium_risk {
        if command.contains(needle) {
            level = level.max(2);
            push_unique(&mut categories, category);
            push_unique(&mut reasons, reason);
        }
    }

    let action_types = actions
        .as_array()
        .into_iter()
        .flatten()
        .filter_map(|action| action.get("type").and_then(Value::as_str))
        .collect::<Vec<_>>();
    if action_types.is_empty() || action_types.contains(&"unknown") {
        level = level.max(2);
        push_unique(&mut categories, "unknown-action");
        push_unique(
            &mut reasons,
            "command contains an action that could not be classified safely",
        );
    }
    if level == 0 {
        level = 1;
        push_unique(&mut categories, "read-only-observed");
        push_unique(
            &mut reasons,
            "vendor parser reported only read, list, or search actions",
        );
    }
    CommandRisk {
        level: match level {
            1 => "low",
            2 => "medium",
            _ => "high",
        }
        .into(),
        confidence: if action_types.is_empty() || action_types.contains(&"unknown") {
            "low"
        } else {
            "conservative"
        }
        .into(),
        categories,
        reasons,
    }
}

fn push_unique(values: &mut Vec<String>, value: &str) {
    if !values.iter().any(|existing| existing == value) {
        values.push(value.into());
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn classifies_vendor_read_actions_as_low_risk() {
        let risk = classify(
            "git status --short",
            &json!([{"type":"listFiles","command":"git status --short","path":null}]),
        );
        assert_eq!(risk.level, "low");
        assert_eq!(risk.confidence, "conservative");
    }

    #[test]
    fn destructive_git_and_filesystem_commands_are_high_risk() {
        let git = classify("git reset --hard HEAD~1", &json!([{"type":"unknown"}]));
        assert_eq!(git.level, "high");
        assert!(git.categories.contains(&"git-history".into()));
        let files = classify("rm -rf build", &json!([{"type":"unknown"}]));
        assert_eq!(files.level, "high");
        assert!(files.categories.contains(&"filesystem-delete".into()));
    }

    #[test]
    fn network_project_execution_and_unknown_actions_are_not_low_risk() {
        assert_eq!(
            classify("curl https://example.com", &json!([{"type":"unknown"}])).level,
            "medium"
        );
        assert_eq!(
            classify("cargo test", &json!([{"type":"unknown"}])).level,
            "medium"
        );
        assert_eq!(classify("custom-tool", &json!([])).level, "medium");
    }
}
