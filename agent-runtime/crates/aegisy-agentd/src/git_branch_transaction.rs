use crate::git_query::{overview, GitOverview};
use crate::git_status::{status, GitRunner, GitStatusError};
use serde::Serialize;
use std::path::Path;

const PLAN_SCHEMA_VERSION: &str = "git-branch-plan/0.1";
const DEFAULT_PROTECTED: &[&str] = &["main", "master", "develop", "production", "release/*"];

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
#[serde(tag = "kind", rename_all = "kebab-case")]
pub enum GitBranchRequest {
    Create { name: String },
    Switch { name: String },
    Rename { from: String, to: String },
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct GitBranchPlan {
    pub schema_version: String,
    pub project_root: String,
    pub request: GitBranchRequest,
    pub expected_head: String,
    pub expected_branch: Option<String>,
    pub target_oid: Option<String>,
    pub protected_patterns: Vec<String>,
    pub blocking_reasons: Vec<String>,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct GitBranchResult {
    pub operation: String,
    pub previous_head: String,
    pub previous_branch: Option<String>,
    pub head: String,
    pub branch: Option<String>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct GitBranchTransactionError {
    pub message: String,
    pub rollback_complete: bool,
}

pub fn default_protected_patterns() -> Vec<String> {
    DEFAULT_PROTECTED
        .iter()
        .map(|pattern| (*pattern).into())
        .collect()
}

pub fn plan_branch_operation(
    root: &Path,
    request: GitBranchRequest,
    protected_patterns: &[String],
) -> Result<GitBranchPlan, GitBranchTransactionError> {
    validate_policy(protected_patterns)?;
    let snapshot = status(root).map_err(from_git)?;
    if !snapshot.repository || !snapshot.worktree {
        return Err(error("project is not inside a Git worktree"));
    }
    let expected_head = snapshot
        .head_oid
        .ok_or_else(|| error("branch operations require an existing HEAD commit"))?;
    let project_root = crate::plain_path(
        &root
            .canonicalize()
            .map_err(|_| error("Git project root is unavailable"))?,
    )
    .to_str()
    .ok_or_else(|| error("Git project root is not UTF-8"))?
    .to_owned();
    let repository = overview(root).map_err(from_git)?;
    let mut blocking_reasons = Vec::new();
    if snapshot.repository_root.as_deref() != Some(project_root.as_str()) {
        blocking_reasons.push("project-is-not-worktree-root".into());
    }
    if !snapshot.entries.is_empty() {
        blocking_reasons.push("dirty-worktree".into());
    }
    let target_oid = evaluate_request(
        root,
        &request,
        snapshot.branch.as_deref(),
        &repository,
        protected_patterns,
        &mut blocking_reasons,
    )?;
    blocking_reasons.sort();
    blocking_reasons.dedup();
    Ok(GitBranchPlan {
        schema_version: PLAN_SCHEMA_VERSION.into(),
        project_root,
        request,
        expected_head,
        expected_branch: snapshot.branch,
        target_oid,
        protected_patterns: protected_patterns.to_vec(),
        blocking_reasons,
    })
}

pub fn execute_branch_operation(
    root: &Path,
    plan: &GitBranchPlan,
    protected_patterns: &[String],
) -> Result<GitBranchResult, GitBranchTransactionError> {
    execute_with_fault(root, plan, protected_patterns, &NoFault)
}

trait FaultInjector {
    fn after_command(&self) -> Result<(), GitBranchTransactionError> {
        Ok(())
    }
}

struct NoFault;

impl FaultInjector for NoFault {}

fn execute_with_fault<F: FaultInjector>(
    root: &Path,
    plan: &GitBranchPlan,
    protected_patterns: &[String],
    fault: &F,
) -> Result<GitBranchResult, GitBranchTransactionError> {
    let current = plan_branch_operation(root, plan.request.clone(), protected_patterns)?;
    if current != *plan {
        return Err(error("Git branch plan is stale or was modified"));
    }
    if !plan.blocking_reasons.is_empty() {
        return Err(error(format!(
            "Git branch operation is blocked: {}",
            plan.blocking_reasons.join(",")
        )));
    }
    let runner = GitRunner::new(root).map_err(from_git)?;
    let operation = match &plan.request {
        GitBranchRequest::Create { name } => {
            run(&runner, &["switch", "-c", name, &plan.expected_head])?;
            "create"
        }
        GitBranchRequest::Switch { name } => {
            run(&runner, &["switch", name])?;
            "switch"
        }
        GitBranchRequest::Rename { from, to } => {
            run(&runner, &["branch", "-m", from, to])?;
            "rename"
        }
    };
    if let Err(cause) = fault.after_command() {
        let rollback_complete = rollback(&runner, plan).is_ok();
        return Err(GitBranchTransactionError {
            message: cause.message,
            rollback_complete,
        });
    }
    let result = verify_result(root, plan, operation);
    if let Err(cause) = result {
        let rollback_complete = rollback(&runner, plan).is_ok();
        return Err(GitBranchTransactionError {
            message: cause.message,
            rollback_complete,
        });
    }
    result
}

fn evaluate_request(
    root: &Path,
    request: &GitBranchRequest,
    current_branch: Option<&str>,
    overview: &GitOverview,
    protected_patterns: &[String],
    blocking: &mut Vec<String>,
) -> Result<Option<String>, GitBranchTransactionError> {
    let runner = GitRunner::new(root).map_err(from_git)?;
    match request {
        GitBranchRequest::Create { name } => {
            validate_branch_name(&runner, name)?;
            if branch_oid(overview, name).is_some() {
                blocking.push("target-branch-exists".into());
            }
            if is_protected(name, protected_patterns) {
                blocking.push("target-branch-protected".into());
            }
            Ok(None)
        }
        GitBranchRequest::Switch { name } => {
            validate_branch_name(&runner, name)?;
            let oid = branch_oid(overview, name);
            if oid.is_none() {
                blocking.push("target-branch-missing".into());
            }
            if current_branch == Some(name.as_str()) {
                blocking.push("target-branch-already-current".into());
            }
            if occupied_elsewhere(overview, name, root) {
                blocking.push("target-branch-occupied-by-worktree".into());
            }
            Ok(oid)
        }
        GitBranchRequest::Rename { from, to } => {
            validate_branch_name(&runner, from)?;
            validate_branch_name(&runner, to)?;
            let oid = branch_oid(overview, from);
            if oid.is_none() {
                blocking.push("source-branch-missing".into());
            }
            if branch_oid(overview, to).is_some() {
                blocking.push("target-branch-exists".into());
            }
            if is_protected(from, protected_patterns) {
                blocking.push("source-branch-protected".into());
            }
            if is_protected(to, protected_patterns) {
                blocking.push("target-branch-protected".into());
            }
            if occupied_elsewhere(overview, from, root) {
                blocking.push("source-branch-occupied-by-worktree".into());
            }
            Ok(oid)
        }
    }
}

fn verify_result(
    root: &Path,
    plan: &GitBranchPlan,
    operation: &str,
) -> Result<GitBranchResult, GitBranchTransactionError> {
    let snapshot = status(root).map_err(from_git)?;
    let head = snapshot
        .head_oid
        .ok_or_else(|| error("Git branch result has no HEAD"))?;
    let expected_branch = match &plan.request {
        GitBranchRequest::Create { name } | GitBranchRequest::Switch { name } => {
            Some(name.as_str())
        }
        GitBranchRequest::Rename { from, to } => {
            if plan.expected_branch.as_deref() == Some(from.as_str()) {
                Some(to.as_str())
            } else {
                plan.expected_branch.as_deref()
            }
        }
    };
    if snapshot.branch.as_deref() != expected_branch {
        return Err(error(
            "Git branch result did not select the expected branch",
        ));
    }
    let expected_head = match plan.request {
        GitBranchRequest::Switch { .. } => {
            plan.target_oid.as_deref().unwrap_or(&plan.expected_head)
        }
        GitBranchRequest::Create { .. } | GitBranchRequest::Rename { .. } => &plan.expected_head,
    };
    if head != expected_head {
        return Err(error(
            "Git branch result HEAD does not match the reviewed plan",
        ));
    }
    Ok(GitBranchResult {
        operation: operation.into(),
        previous_head: plan.expected_head.clone(),
        previous_branch: plan.expected_branch.clone(),
        head,
        branch: snapshot.branch,
    })
}

fn rollback(runner: &GitRunner, plan: &GitBranchPlan) -> Result<(), GitBranchTransactionError> {
    match &plan.request {
        GitBranchRequest::Create { name } => {
            restore_previous(runner, plan)?;
            run(runner, &["branch", "-D", name])
        }
        GitBranchRequest::Switch { .. } => restore_previous(runner, plan),
        GitBranchRequest::Rename { from, to } => run(runner, &["branch", "-m", to, from]),
    }
}

fn restore_previous(
    runner: &GitRunner,
    plan: &GitBranchPlan,
) -> Result<(), GitBranchTransactionError> {
    if let Some(branch) = &plan.expected_branch {
        run(runner, &["switch", branch])
    } else {
        run(runner, &["switch", "--detach", &plan.expected_head])
    }
}

fn run(runner: &GitRunner, args: &[&str]) -> Result<(), GitBranchTransactionError> {
    let output = runner.run(args, None, 256 * 1024).map_err(from_git)?;
    if output.success {
        Ok(())
    } else {
        Err(error("Git branch command failed"))
    }
}

fn validate_branch_name(runner: &GitRunner, name: &str) -> Result<(), GitBranchTransactionError> {
    if name.is_empty() || name.len() > 255 || name.starts_with('-') {
        return Err(error("Git branch name is invalid"));
    }
    let output = runner
        .run(&["check-ref-format", "--branch", name], None, 1024)
        .map_err(from_git)?;
    if output.success {
        Ok(())
    } else {
        Err(error("Git branch name is invalid"))
    }
}

fn branch_oid(overview: &GitOverview, name: &str) -> Option<String> {
    overview
        .branches
        .iter()
        .find(|branch| branch.name == name)
        .map(|branch| branch.oid.clone())
}

fn occupied_elsewhere(overview: &GitOverview, branch: &str, root: &Path) -> bool {
    let canonical_root = root.canonicalize().ok();
    overview.worktrees.iter().any(|worktree| {
        worktree.branch.as_deref() == Some(branch)
            && canonical_root.as_ref().is_none_or(|root| {
                Path::new(&worktree.path).canonicalize().ok().as_ref() != Some(root)
            })
    })
}

fn validate_policy(patterns: &[String]) -> Result<(), GitBranchTransactionError> {
    if patterns.len() > 128
        || patterns.iter().any(|pattern| {
            pattern.is_empty()
                || pattern.len() > 255
                || pattern.matches('*').count() > 1
                || (pattern.contains('*') && !pattern.ends_with("/*"))
        })
    {
        return Err(error("protected branch policy is invalid"));
    }
    Ok(())
}

fn is_protected(name: &str, patterns: &[String]) -> bool {
    patterns.iter().any(|pattern| {
        pattern
            .strip_suffix("/*")
            .map_or(name == pattern, |prefix| {
                name.starts_with(&format!("{prefix}/"))
            })
    })
}

fn from_git(error: GitStatusError) -> GitBranchTransactionError {
    GitBranchTransactionError {
        message: error.message,
        rollback_complete: true,
    }
}

fn error(message: impl Into<String>) -> GitBranchTransactionError {
    GitBranchTransactionError {
        message: message.into(),
        rollback_complete: true,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;
    use std::path::PathBuf;
    use std::process::Command;
    use std::sync::atomic::{AtomicU64, Ordering};

    static SEQUENCE: AtomicU64 = AtomicU64::new(0);

    struct FailAfterCommand;

    impl FaultInjector for FailAfterCommand {
        fn after_command(&self) -> Result<(), GitBranchTransactionError> {
            Err(error("injected post-command failure"))
        }
    }

    fn root() -> PathBuf {
        let root = std::env::temp_dir().join(format!(
            "aegisy-git-branch-{}-{}",
            std::process::id(),
            SEQUENCE.fetch_add(1, Ordering::Relaxed)
        ));
        fs::create_dir_all(&root).unwrap();
        let root = root.canonicalize().unwrap();
        git(&root, &["init", "-q"], true);
        git(&root, &["config", "user.name", "Aegisy Test"], true);
        git(&root, &["config", "user.email", "test@aegisy.local"], true);
        fs::write(root.join("tracked.txt"), "initial\n").unwrap();
        git(&root, &["add", "."], true);
        git(&root, &["commit", "-q", "-m", "initial"], true);
        root
    }

    fn git(root: &Path, args: &[&str], success: bool) -> Vec<u8> {
        let output = Command::new("git")
            .arg("-C")
            .arg(root)
            .args(args)
            .output()
            .unwrap();
        assert_eq!(output.status.success(), success, "git command: {args:?}");
        output.stdout
    }

    fn branch(root: &Path) -> String {
        String::from_utf8(git(root, &["branch", "--show-current"], true))
            .unwrap()
            .trim()
            .into()
    }

    #[test]
    fn creates_switches_and_renames_only_from_fresh_clean_reviewed_state() {
        let root = root();
        let policy = default_protected_patterns();
        let original = branch(&root);
        let create = plan_branch_operation(
            &root,
            GitBranchRequest::Create {
                name: "aegisy/task-one".into(),
            },
            &policy,
        )
        .unwrap();
        assert!(create.blocking_reasons.is_empty());
        let created = execute_branch_operation(&root, &create, &policy).unwrap();
        assert_eq!(created.branch.as_deref(), Some("aegisy/task-one"));

        let rename = plan_branch_operation(
            &root,
            GitBranchRequest::Rename {
                from: "aegisy/task-one".into(),
                to: "aegisy/task-renamed".into(),
            },
            &policy,
        )
        .unwrap();
        assert!(rename.blocking_reasons.is_empty());
        execute_branch_operation(&root, &rename, &policy).unwrap();
        assert_eq!(branch(&root), "aegisy/task-renamed");

        let switch = plan_branch_operation(
            &root,
            GitBranchRequest::Switch {
                name: original.clone(),
            },
            &policy,
        )
        .unwrap();
        assert!(switch.blocking_reasons.is_empty());
        execute_branch_operation(&root, &switch, &policy).unwrap();
        assert_eq!(branch(&root), original);

        fs::write(root.join("tracked.txt"), "dirty\n").unwrap();
        let dirty = plan_branch_operation(
            &root,
            GitBranchRequest::Switch {
                name: "aegisy/task-renamed".into(),
            },
            &policy,
        )
        .unwrap();
        assert!(dirty.blocking_reasons.contains(&"dirty-worktree".into()));
        assert!(execute_branch_operation(&root, &dirty, &policy).is_err());
        git(&root, &["checkout", "--", "tracked.txt"], true);

        let protected = plan_branch_operation(
            &root,
            GitBranchRequest::Rename {
                from: branch(&root),
                to: "renamed-protected".into(),
            },
            &policy,
        )
        .unwrap();
        assert!(protected
            .blocking_reasons
            .contains(&"source-branch-protected".into()));
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn rejects_stale_and_other_worktree_plans_and_rolls_back_each_operation() {
        let root = root();
        let policy = default_protected_patterns();
        let original = branch(&root);
        git(&root, &["branch", "target"], true);
        let stale = plan_branch_operation(
            &root,
            GitBranchRequest::Switch {
                name: "target".into(),
            },
            &policy,
        )
        .unwrap();
        git(&root, &["branch", "-f", "target", "HEAD"], true);
        fs::write(root.join("new.txt"), "new\n").unwrap();
        git(&root, &["add", "."], true);
        git(&root, &["commit", "-q", "-m", "advance"], true);
        git(&root, &["branch", "-f", "target", "HEAD"], true);
        assert!(execute_branch_operation(&root, &stale, &policy).is_err());

        let worktree = root.parent().unwrap().join(format!(
            "aegisy-git-branch-worktree-{}-{}",
            std::process::id(),
            SEQUENCE.fetch_add(1, Ordering::Relaxed)
        ));
        git(
            &root,
            &[
                "worktree",
                "add",
                "-q",
                worktree.to_str().unwrap(),
                "target",
            ],
            true,
        );
        let occupied = plan_branch_operation(
            &root,
            GitBranchRequest::Switch {
                name: "target".into(),
            },
            &policy,
        )
        .unwrap();
        assert!(occupied
            .blocking_reasons
            .contains(&"target-branch-occupied-by-worktree".into()));
        git(
            &root,
            &["worktree", "remove", "--force", worktree.to_str().unwrap()],
            true,
        );

        let create = plan_branch_operation(
            &root,
            GitBranchRequest::Create {
                name: "rollback-create".into(),
            },
            &policy,
        )
        .unwrap();
        let failure = execute_with_fault(&root, &create, &policy, &FailAfterCommand).unwrap_err();
        assert!(failure.rollback_complete);
        assert_eq!(branch(&root), original);
        assert!(
            !String::from_utf8(git(&root, &["branch", "--list", "rollback-create"], true))
                .unwrap()
                .contains("rollback-create")
        );

        let switch = plan_branch_operation(
            &root,
            GitBranchRequest::Switch {
                name: "target".into(),
            },
            &policy,
        )
        .unwrap();
        let failure = execute_with_fault(&root, &switch, &policy, &FailAfterCommand).unwrap_err();
        assert!(failure.rollback_complete);
        assert_eq!(branch(&root), original);

        git(&root, &["branch", "rename-source"], true);
        let rename = plan_branch_operation(
            &root,
            GitBranchRequest::Rename {
                from: "rename-source".into(),
                to: "rename-target".into(),
            },
            &policy,
        )
        .unwrap();
        let failure = execute_with_fault(&root, &rename, &policy, &FailAfterCommand).unwrap_err();
        assert!(failure.rollback_complete);
        let branches = String::from_utf8(git(&root, &["branch", "--list"], true)).unwrap();
        assert!(branches.contains("rename-source"));
        assert!(!branches.contains("rename-target"));
        fs::remove_dir_all(root).unwrap();
    }
}
