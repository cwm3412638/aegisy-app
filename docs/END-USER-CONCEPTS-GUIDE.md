# Aegisy End-User Concepts Guide

This guide explains the core concepts of Aegisy Agent Workbench for end users.

## Chat vs Work Modes

### Chat Mode

**Purpose**: Conversational assistance without modifying your files

**Characteristics**:
- Read-only by default
- Can view your code and answer questions
- Cannot create, modify, or delete files
- Cannot run commands or access terminals
- Safe for exploring and learning

**Best For**:
- Understanding code
- Getting explanations
- Asking questions
- Planning changes before making them
- Learning new concepts

**Example**: "Explain how this authentication system works"

### Work Mode

**Purpose**: Active development with file modifications and command execution

**Characteristics**:
- Can read and write files (with approval)
- Can run commands in terminals
- Can make Git commits
- Requires explicit permission for each action
- All changes are reviewable before applying

**Best For**:
- Implementing features
- Refactoring code
- Running tests
- Making Git commits
- Active development tasks

**Example**: "Implement user authentication with JWT tokens"

### Switching Between Modes

- Start in Chat mode to explore and plan
- Switch to Work mode when ready to make changes
- Chat history is preserved when converting to Work
- Work mode requires selecting a project

## Projects

### What is a Project?

A **project** is a directory on your computer that Aegisy can access. It typically contains:
- Source code files
- Configuration files
- Documentation
- Git repository (optional)

### Project Roots

- Each project has one or more **roots** (directories)
- Roots can be read-only or read-write
- Aegisy can only access files within project roots
- Files outside roots are never visible to the agent

### Trust and Security

When you first open a project, Aegisy shows a **trust review**:
- Lists all project roots
- Shows detected Git repositories
- Identifies instruction files (`.claude/` directory)
- Displays executable hooks
- Explains permission implications

**Important**: Trusting a project does NOT automatically grant write access. You still approve each individual change.

### Project States

- **Available**: Project directory exists and is accessible
- **Unavailable**: Directory was moved or deleted
- **Relink Required**: Directory moved; you can update the path

## Sessions

### What is a Session?

A **session** is a conversation thread with the agent. Think of it like a chat conversation that:
- Remembers previous messages
- Maintains context about your task
- Can be paused and resumed later
- Is saved automatically

### Session Types

**Chat Sessions**:
- Read-only conversations
- Not bound to a specific project
- Can discuss any topic
- Cannot make file changes

**Work Sessions**:
- Bound to a specific project
- Can make file changes (with approval)
- Tracks Git branch and workspace state
- Requires project selection

### Session Lifecycle

1. **Create**: Start a new conversation
2. **Active**: Ongoing conversation with the agent
3. **Archive**: Pause a session for later
4. **Resume**: Continue an archived session
5. **Fork**: Create a new session from a specific point
6. **Delete**: Permanently remove a session

### Session Management

- **Title**: Rename sessions for easy identification
- **Search**: Find sessions by content or metadata
- **Pin**: Keep important sessions at the top
- **Archive**: Hide completed sessions from main list

## Model Profiles

### What are Model Profiles?

**Model profiles** define which AI models are used for different tasks. Aegisy can use different models for:

- **Agent**: Main conversational model
- **Plan**: Task planning and decomposition
- **Apply**: Precise code modifications
- **Review**: Code review and quality checks
- **Utility**: Quick, simple tasks
- **Embedding**: Semantic search
- **Rerank**: Search result ranking

### Why Multiple Models?

Different models have different strengths:
- Some are better at conversation
- Some are faster for simple tasks
- Some are more cost-effective
- Some have larger context windows

### Switching Models

- **Compatible Switch**: Continue current session with a new model
  - Preserves full conversation history
  - Works when models support same features
  - Seamless transition

- **Portable Fork**: Start new session with different model
  - Creates a copy of conversation history
  - Required when switching to incompatible model
  - May lose some context (encrypted reasoning, cache)

## Permissions

### Permission Profiles

Aegisy uses **permission profiles** to control what the agent can do:

#### Read Only (Chat Mode Default)
- View files and directories
- Read Git status
- No modifications allowed
- Safest option

#### Workspace Write (Work Mode Default)
- Read files
- Create, modify, delete files (with approval)
- No command execution
- No Git commits
- Good for file-only changes

#### Developer
- All Workspace Write permissions
- Run commands in terminals (with approval)
- Make Git commits (with approval)
- Access language servers
- Most common for active development

#### Full Access
- All Developer permissions
- Run background processes
- Access network (with approval)
- Execute hooks and extensions
- Requires careful review

### Approval Workflow

Even with permissions granted, Aegisy asks for approval before:
- Creating or modifying files
- Running commands
- Making Git commits
- Accessing network
- Executing extensions

**You always have final control.**

## Context

### What is Context?

**Context** is the information the agent uses to understand your request:
- Your current message
- Previous conversation
- Attached files
- Project instructions
- Pinned content
- Search results
- Repository structure

### Context Budget

Models have a **context window** limit (e.g., 200K tokens). Aegisy manages this by:
- Prioritizing recent and relevant information
- Automatically including project instructions
- Letting you pin important content
- Excluding less relevant history when needed

### Pinned Context

You can **pin** content to ensure it's always included:
- Files or file selections
- Terminal output
- Git commits or diffs
- Diagnostic messages
- Images
- Previous agent responses

Pinned content has higher priority than automatic context.

### Context Inspector

Use the **context inspector** to see:
- What will be sent to the model
- Token counts for each item
- Why items were included or excluded
- Freshness of content
- Trust level of sources

## Checkpoints

### What are Checkpoints?

**Checkpoints** are snapshots of your conversation that help manage long sessions:
- Summarize decisions made
- List unresolved tasks
- Track changed files
- Record important commands
- Note test results and failures
- Outline next steps

### When to Create Checkpoints

- After completing a major task
- Before switching focus
- When context window is getting full
- To preserve important decisions

### Checkpoint Benefits

- Reduces context size
- Preserves key information
- Makes sessions resumable
- Helps agent stay focused
- Enables session forking

### Manual vs Automatic

- **Manual**: You create when needed
- **Automatic**: Aegisy suggests when context is full
- **Review**: Always review before activating

## Git Integration

### Git Features

Aegisy integrates with Git to:
- Show repository status
- Display commit history
- Preview diffs
- Create commits (with approval)
- Switch branches (with approval)
- Manage worktrees

### Git Safety

All Git operations:
- Require approval
- Show full preview of changes
- Check for conflicts
- Validate branch protection
- Never force-push without explicit confirmation

### Git Context

You can include Git information in context:
- Commit messages and diffs
- Current branch status
- Staged vs unstaged changes
- Conflict markers

### Worktrees

**Worktrees** let you work on multiple branches simultaneously:
- Each worktree is an independent working directory
- Changes in one don't affect others
- Useful for parallel tasks
- Automatically cleaned up when done

## Extensions

### What are Extensions?

**Extensions** add capabilities to Aegisy:

- **Skills**: Reusable workflows and commands
- **Hooks**: Automated actions on events
- **MCP (Model Context Protocol)**: External tool integrations
- **Plugins**: Custom functionality

### Extension Sources

- **Built-in**: Provided by Aegisy
- **User**: Your personal extensions
- **Project**: Project-specific extensions
- **Community**: Shared by others

### Extension Security

Extensions can be powerful but require trust:
- Review what extensions do before enabling
- Check extension permissions
- Understand what data they access
- Disable unused extensions

### Managing Extensions

- **Enable/Disable**: Control which extensions are active
- **Configure**: Set extension-specific options
- **Update**: Keep extensions current
- **Remove**: Uninstall unused extensions

## Best Practices

### Starting a New Task

1. Start in Chat mode to explore and plan
2. Ask questions to understand the codebase
3. Discuss approach before implementing
4. Switch to Work mode when ready
5. Review all changes before applying

### Managing Long Sessions

1. Create checkpoints at logical milestones
2. Pin important context
3. Archive completed sessions
4. Fork sessions when changing direction
5. Use context inspector to verify what's included

### Security and Privacy

1. Start with Read Only permissions
2. Grant higher permissions only when needed
3. Review all approvals carefully
4. Don't share sessions containing secrets
5. Use project-specific instructions for sensitive projects

### Performance Tips

1. Keep context focused on current task
2. Unpin content when no longer needed
3. Archive old sessions
4. Use appropriate model for each task
5. Close unused terminals

## Getting Help

### In-App Help

- Hover over UI elements for tooltips
- Check context menus for available actions
- Use command palette (Cmd/Ctrl+K) for quick access

### Documentation

- User Guide: Comprehensive feature documentation
- Troubleshooting: Common issues and solutions
- Security Guide: Understanding trust and permissions
- Extension Guide: Creating and using extensions

### Support

- GitHub Issues: Report bugs and request features
- Community Forum: Ask questions and share tips
- Documentation: Search for specific topics

## Glossary

- **Agent**: The AI assistant you interact with
- **Context**: Information provided to the agent
- **Session**: A conversation thread
- **Project**: A directory Aegisy can access
- **Root**: A directory within a project
- **Profile**: Configuration for models or permissions
- **Checkpoint**: A conversation snapshot
- **Fork**: Create a new session from an existing one
- **Pin**: Mark content as always-included
- **Approval**: Your confirmation before agent actions
- **Extension**: Add-on functionality
- **Worktree**: Independent Git working directory
