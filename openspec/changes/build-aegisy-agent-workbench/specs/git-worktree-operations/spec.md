## ADDED Requirements

### Requirement: Git state is derived from structured commands
The runtime SHALL use Git porcelain/plumbing interfaces and stable identifiers
for repository status, branches, commits, remotes, worktrees, and diffs.

#### Scenario: Repository status is requested
- **WHEN** the project is inside a Git worktree
- **THEN** the system SHALL report repository root, HEAD, branch or detached state, upstream, ahead/behind counts, staged paths, unstaged paths, untracked paths, conflicts, and operation-in-progress state

#### Scenario: Git output is localized
- **WHEN** the user's Git locale differs from the application language
- **THEN** status and safety classification SHALL remain correct without parsing localized human output

#### Scenario: Project is not a repository
- **WHEN** Git inspection finds no worktree
- **THEN** Git actions SHALL be disabled except explicit initialization and the UI SHALL not fabricate a branch state

### Requirement: Branch changes protect user work
Creating, switching, renaming, and deleting branches SHALL account for dirty
files, active worktrees, upstream state, and protected branch policy.

#### Scenario: User creates a task branch
- **WHEN** the base revision is valid and no policy blocks creation
- **THEN** the new branch SHALL record its base and become associated with the Work session

#### Scenario: Branch switch would overwrite files
- **WHEN** Git reports that local changes prevent a safe switch
- **THEN** the operation SHALL stop and offer explicit stash, checkpoint, worktree, commit, or cancel choices

#### Scenario: Agent proposes deleting a branch
- **WHEN** the branch is unmerged, protected, checked out, or referenced by an active session
- **THEN** deletion SHALL be classified high risk and SHALL require explicit user confirmation with those conditions shown

### Requirement: Concurrent coding tasks use isolated worktrees
Every concurrent write-capable child or background task SHALL execute in a
dedicated Git worktree and branch unless a future spec defines an equivalent
isolated workspace.

#### Scenario: Child task receives write access
- **WHEN** the parent launches the child
- **THEN** the runtime SHALL create or assign a unique worktree, record base revision, and constrain file tools and terminals to that worktree

#### Scenario: Worktree creation fails
- **WHEN** Git, disk, path, or policy prevents isolation
- **THEN** the child SHALL remain read-only or fail before any write-capable tool is exposed

#### Scenario: Child finishes
- **WHEN** the child reaches a terminal state
- **THEN** its branch, changed paths, commits, tests, unresolved conflicts, and cleanup eligibility SHALL remain visible until integration or discard is confirmed

### Requirement: Commits are deliberate and attributable
The system SHALL separate user changes from Agent changes and SHALL preview the
exact staged diff and commit metadata before commit creation.

#### Scenario: Agent prepares a commit
- **WHEN** policy permits commit assistance
- **THEN** the UI SHALL show selected paths, excluded user changes, generated message, author/committer behavior, and hook policy before confirmation

#### Scenario: Commit hooks fail
- **WHEN** enabled hooks reject the commit
- **THEN** no successful commit state SHALL be shown and the hook output SHALL be linked to the attempted staged snapshot

#### Scenario: Automatic checkpoint commit is enabled
- **WHEN** project policy permits hidden or temporary checkpoint commits
- **THEN** they SHALL use an isolated namespace or branch and SHALL not alter shared history or push automatically

### Requirement: Destructive and remote Git operations are risk-gated
Git mutations SHALL be assigned risk classes enforced outside model prompts.

#### Scenario: Agent requests push
- **WHEN** a push would update a remote ref
- **THEN** the system SHALL display remote, refspec, commits, force status, authentication target, and protection state before approval

#### Scenario: Agent requests force push or history rewrite
- **WHEN** force, reset, rebase, clean, or another configured high-risk operation is requested
- **THEN** blanket auto-approval SHALL not apply and explicit confirmation SHALL identify data that may become unreachable

#### Scenario: Command bypasses the Git tool
- **WHEN** a shell command contains a recognized protected Git mutation
- **THEN** command policy SHALL apply equivalent or stricter risk classification instead of treating it as an ordinary shell command

### Requirement: Integration and conflict resolution are reviewable
Merging, rebasing, cherry-picking, or applying a child result SHALL expose the
before state, intended commits, conflicts, and resulting diff.

#### Scenario: Child branch applies cleanly
- **WHEN** the user approves integration into the target branch
- **THEN** the runtime SHALL verify target HEAD, apply the selected strategy, and report the resulting commits and worktree state

#### Scenario: Target changed after preview
- **WHEN** target HEAD differs from the reviewed integration base
- **THEN** integration SHALL pause for refreshed preview or explicit rebase and SHALL NOT apply against stale assumptions

#### Scenario: Conflicts occur
- **WHEN** Git enters a conflicted operation
- **THEN** the session SHALL expose conflicted paths and abort/continue controls, and SHALL preserve the operation state across restart
