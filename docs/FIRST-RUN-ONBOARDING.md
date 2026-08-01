# Aegisy Agent Workbench - First Run Onboarding

## Welcome to Aegisy Agent Workbench

Aegisy Agent Workbench is an AI-powered development environment that helps you build software with intelligent assistance. This guide will help you get started safely and effectively.

## Initial Setup

### 1. Choose Your First Project

When you first launch Aegisy, you'll be prompted to select a project:

**Option A: Open Sample Project (Recommended)**
- Click "Open Sample Project" to explore a pre-configured safe environment
- The sample project demonstrates key features without risking your code
- Perfect for learning how Aegisy works before using it on real projects

**Option B: Select Existing Project**
- Click "Select Project Folder" to choose an existing codebase
- Navigate to your project's root directory
- Aegisy will analyze the project structure and prepare the workspace

**Option C: Create New Project**
- Click "Create New Project" to start fresh
- Choose a location and project name
- Aegisy will initialize an empty project structure

### 2. Project Trust Review

Before opening any project, Aegisy shows a **Trust Review** dialog:

```
┌─────────────────────────────────────────────────┐
│ Project Trust Review                            │
├─────────────────────────────────────────────────┤
│ Project: /Users/you/my-project                  │
│                                                 │
│ This project contains:                          │
│ ✓ 1 filesystem root                            │
│ ✓ Git repository (.git)                        │
│ ⚠ Executable hooks (pre-commit, post-merge)    │
│ ⚠ Configuration files (.aegisy/config.yaml)    │
│                                                 │
│ Recommended Permission: Read Only               │
│                                                 │
│ [Trust with Read Only]  [Advanced Options]     │
└─────────────────────────────────────────────────┘
```

**What This Means:**
- Aegisy inspects the project for security-relevant content
- Executable hooks and configuration files are flagged for review
- You choose the permission level before proceeding

### 3. Select Permission Profile

Aegisy **never defaults to Full Access**. Choose the appropriate permission level:

#### Read Only (Recommended for First Run)
- ✓ Browse and search files
- ✓ Ask questions about code
- ✓ Get explanations and suggestions
- ✗ Cannot modify files
- ✗ Cannot run commands
- ✗ Cannot access network

**Best for:** Learning Aegisy, code review, understanding unfamiliar codebases

#### Workspace Write
- ✓ All Read Only permissions
- ✓ Create, edit, and delete files
- ✓ Propose changes with preview
- ✗ Cannot run commands
- ✗ Cannot access Git
- ✗ Cannot access network

**Best for:** Focused editing tasks, refactoring, documentation updates

#### Developer (Requires Explicit Approval)
- ✓ All Workspace Write permissions
- ✓ Run terminal commands (with approval)
- ✓ Git operations (with approval)
- ✓ Install dependencies (with approval)
- ✗ Network access restricted

**Best for:** Active development, testing, build tasks

#### Full Access (Not Recommended for First Run)
- ✓ All Developer permissions
- ✓ Network access
- ✓ System-level operations
- ⚠ Maximum risk - use only when necessary

**Best for:** Deployment, cloud operations, external integrations

### 4. First Session

After selecting permissions, Aegisy creates your first session:

1. **Chat Mode** opens by default (safe, non-mutating)
2. The **Timeline** shows your conversation history
3. The **Context Panel** displays files and information the AI can see
4. The **Composer** is where you type prompts

**Try These First Prompts:**

```
"Explain the structure of this project"
"What does the main.py file do?"
"Show me all the test files"
"Find functions that handle user authentication"
```

### 5. Understanding Chat vs Work Modes

**Chat Mode (Default)**
- Non-mutating by default
- Safe for exploration and questions
- Cannot modify files without explicit approval
- Ideal for learning and planning

**Work Mode**
- Enables bounded mutations based on permission profile
- Shows approval dialogs before making changes
- Tracks all modifications in Timeline
- Use when you're ready to make changes

**Switching Modes:**
- Click the mode selector in the product rail
- Aegisy will confirm the switch and explain implications
- Your session history is preserved across modes

## Safety Features

### Approval Workflow
When Aegisy proposes changes, you'll see:

```
┌─────────────────────────────────────────────────┐
│ Workspace Edit Proposal                         │
├─────────────────────────────────────────────────┤
│ Session: work-session-abc123                    │
│ Files affected: 3                               │
│                                                 │
│ Changes:                                        │
│ + src/auth.py         (+45 lines, -12 lines)   │
│ + tests/test_auth.py  (+23 lines, -0 lines)    │
│ ~ README.md           (+5 lines, -2 lines)     │
│                                                 │
│ [Review Changes]  [Approve]  [Decline]         │
└─────────────────────────────────────────────────┘
```

- **Review Changes**: See detailed diffs before approving
- **Approve**: Apply changes to your workspace
- **Decline**: Reject the proposal (no changes made)

### Checkpoints
- Aegisy creates Git-aware checkpoints before mutations
- You can restore previous states if needed
- Checkpoints preserve your work and Agent changes separately

### Secret Detection
- Aegisy automatically detects and masks secrets
- API keys, tokens, and passwords are never logged
- Redaction happens before any storage or transmission

## Next Steps

### Explore the Interface

**Product Rail (Left)**
- Chat: Conversation mode
- Work: Development mode
- Projects: Switch between projects
- Sessions: View session history
- Extensions: Manage plugins and skills
- Settings: Configure preferences

**Work Canvas (Center)**
- Timeline: Conversation and event history
- Composer: Input area for prompts
- Context: Files and information in scope

**Workspace (Right)**
- Files: Browse project structure
- Editor: View and edit code
- Terminal: Run commands (Developer+ permission)
- Git: Version control operations
- Structure: Code navigation and search

### Learn Key Concepts

1. **Projects**: Bounded workspaces with filesystem roots
2. **Sessions**: Durable conversation threads
3. **Context**: Information the AI can see and use
4. **Timeline**: Ordered sequence of events and changes
5. **Checkpoints**: Snapshots for safe experimentation

### Customize Your Experience

**Model Selection**
- Click the model picker to see available models
- Different models have different capabilities and costs
- Aegisy shows compatibility indicators

**Permission Adjustment**
- You can change permissions at any time
- Settings → Project → Permission Profile
- Changes take effect immediately

**Theme and Appearance**
- Settings → Appearance
- Choose light, dark, or system theme
- Adjust font size and editor preferences

## Common First-Run Questions

**Q: Can Aegisy access my files without permission?**
A: No. Aegisy only accesses files within the project root you selected, and only with the permission level you approved.

**Q: What happens if I decline a change proposal?**
A: Nothing. Your workspace remains unchanged. You can ask Aegisy to revise the proposal or try a different approach.

**Q: Can I undo changes Aegisy makes?**
A: Yes. Use checkpoints to restore previous states, or use Git to revert changes. Aegisy integrates with your existing version control.

**Q: Is my code sent to the cloud?**
A: Only prompts and explicitly included context are sent to the model provider. Your full codebase stays local. See Settings → Privacy for details.

**Q: How do I add more files to context?**
A: Use the Files panel to select files, then click "Add to Context" or "Pin to Context". Pinned context persists across turns.

**Q: What if Aegisy suggests something wrong?**
A: Always review proposals before approving. Aegisy is a tool to assist you, not replace your judgment. Decline incorrect suggestions and provide feedback.

## Getting Help

- **Documentation**: Help → Documentation
- **Troubleshooting**: Help → Troubleshooting Guide
- **Security**: Help → Security Documentation
- **Keyboard Shortcuts**: Help → Shortcuts (or press Cmd+K / Ctrl+K)

## Best Practices for First-Time Users

1. **Start with Read Only**: Get comfortable with the interface before enabling mutations
2. **Use Sample Projects**: Practice on safe, disposable code first
3. **Review Every Proposal**: Never approve changes you don't understand
4. **Keep Backups**: Commit your work to Git before major changes
5. **Ask Questions**: Use Chat mode to understand before using Work mode
6. **Check Context**: Verify what files Aegisy can see before asking questions
7. **Read Diffs**: Always review the detailed diff before approving edits
8. **Start Small**: Begin with simple tasks and build confidence gradually

## Ready to Begin

You're now ready to use Aegisy Agent Workbench safely and effectively. Remember:

- **Security first**: Choose appropriate permissions
- **Review everything**: Never approve changes blindly
- **Experiment safely**: Use checkpoints and version control
- **Learn gradually**: Start simple and build expertise

Click "Continue" to open your first project and start your first session.

---

**Need More Help?**
- Press `F1` for context-sensitive help
- Visit the documentation browser (Help → Documentation)
- Check the troubleshooting guide for common issues
