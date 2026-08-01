# Model Capability and Switching Guide

This guide explains model capabilities, compatibility, and the difference between
compatible continuation and portable session fork when switching models.

## Understanding Model Capabilities

### What are Model Capabilities?

**Model capabilities** are the features and functions a model supports:

- **Conversation**: Multi-turn dialogue
- **Tools**: Ability to call functions and tools
- **Attachments**: Support for images, files, etc.
- **Reasoning**: Extended thinking and planning
- **Context Size**: Maximum input length
- **Streaming**: Real-time response generation
- **Structured Output**: JSON, code blocks, etc.

### Capability Differences

Different models have different capabilities:

| Capability | Claude Opus 5 | Claude Sonnet 5 | Claude Haiku 4.5 |
|------------|---------------|-----------------|------------------|
| Context | 200K tokens | 200K tokens | 200K tokens |
| Tools | ✓ | ✓ | ✓ |
| Reasoning | Extended | Standard | Fast |
| Images | ✓ | ✓ | ✓ |
| Cost | Higher | Medium | Lower |
| Speed | Slower | Medium | Faster |

### Checking Compatibility

Before switching models, Aegisy checks:
- Required capabilities for current session
- Model support for those capabilities
- Context size requirements
- Tool availability
- Attachment support

## Model Switching Options

### Compatible Continuation

**What it is**: Seamlessly continue your session with a new model

**When available**:
- New model supports all required capabilities
- Context fits within new model's limits
- No incompatible features in use
- Session history is compatible

**What's preserved**:
- Complete conversation history
- All context and attachments
- Tool call history
- Session state
- Pinned content
- Git bindings

**What happens**:
1. Aegisy validates compatibility
2. Context is transferred to new model
3. Next response uses new model
4. Session continues seamlessly
5. No data loss

**Example**:
```
You: [Using Claude Opus 5]
     "Explain this authentication system"

Agent: [Detailed explanation]

You: [Switch to Claude Sonnet 5 - Compatible]
     "Now implement JWT tokens"

Agent: [Using Claude Sonnet 5]
       [Continues with full context]
```

### Portable Session Fork

**What it is**: Create a new session with a different model

**When required**:
- New model lacks required capabilities
- Context exceeds new model's limits
- Incompatible features in use
- Encrypted reasoning present
- Cache handles not transferable

**What's preserved**:
- Conversation messages (redacted)
- File attachments (metadata only)
- Basic session metadata
- Project bindings

**What's NOT preserved**:
- Encrypted reasoning
- Cache handles
- Opaque response IDs
- Hidden thinking
- Provider-specific state
- Some tool call details

**What happens**:
1. Aegisy creates a copy of session
2. Removes incompatible content
3. Redacts sensitive data
4. Creates new session with new model
5. Original session remains unchanged

**Example**:
```
You: [Using Claude Opus 5 with extended reasoning]
     "Plan a complex refactoring"

Agent: [Detailed plan with extended reasoning]

You: [Switch to Claude Haiku 4.5 - Incompatible]
     "Continue with implementation"

Aegisy: "Portable fork required. Extended reasoning
         cannot be transferred. Create new session?"

You: "Yes"

Agent: [Using Claude Haiku 4.5]
       [New session with conversation history,
        but without extended reasoning details]
```

## Choosing Between Options

### Use Compatible Continuation When:

✓ Model supports all features you're using
✓ Context fits within new model's limits
✓ You want seamless transition
✓ You need complete history
✓ Session state is important

### Use Portable Fork When:

✓ New model lacks some capabilities
✓ Context is too large for new model
✓ You want a fresh start
✓ You're switching to very different model
✓ You want to try different approach

## Capability Indicators

### In Model Picker

Aegisy shows capability status for each model:

**Compatible** (Green ✓):
- All required capabilities supported
- Context fits within limits
- Seamless continuation available

**Incompatible** (Yellow ⚠):
- Some capabilities missing
- Portable fork required
- Some data will be lost

**Blocked** (Red ✗):
- Critical capabilities missing
- Cannot continue session
- Must start new session

### Capability Details

Click on a model to see:
- Supported capabilities
- Missing capabilities
- Context size comparison
- Expected cost difference
- Performance characteristics

## Context Size Considerations

### What is Context Size?

**Context size** is the maximum amount of text (measured in tokens) a model can process at once.

**Typical sizes**:
- Claude Opus 5: 200K tokens (~150K words)
- Claude Sonnet 5: 200K tokens (~150K words)
- Claude Haiku 4.5: 200K tokens (~150K words)

### When Context Matters

Context size affects:
- How much conversation history fits
- How many files can be included
- Repository map size
- Pinned content limits

### Context Warnings

Aegisy warns when:
- Context is approaching model limit
- Switching would exceed new model's limit
- Compaction is recommended
- Fork is required due to size

## Feature Compatibility

### Tools and Functions

**Compatible**: Both models support tools
**Incompatible**: New model doesn't support tools
**Impact**: Tool call history may be lost

### Attachments

**Compatible**: Both models support same attachment types
**Incompatible**: New model doesn't support some types
**Impact**: Unsupported attachments excluded

### Reasoning

**Compatible**: Both models support reasoning
**Incompatible**: New model doesn't support extended reasoning
**Impact**: Reasoning details lost in fork

### Structured Output

**Compatible**: Both models support structured output
**Incompatible**: New model has different output format
**Impact**: Output format may change

## Cost and Performance Trade-offs

### Cost Considerations

When switching models, consider:
- Input token cost (context)
- Output token cost (responses)
- Reasoning token cost (if applicable)
- Total session cost

**Example**:
- Opus 5: Higher cost, best quality
- Sonnet 5: Medium cost, good quality
- Haiku 4.5: Lower cost, fast responses

### Performance Considerations

Different models have different characteristics:
- **Speed**: Haiku fastest, Opus slowest
- **Quality**: Opus highest, Haiku good
- **Context**: All support 200K tokens
- **Reasoning**: Opus has extended reasoning

### Optimization Strategy

1. **Start with Opus** for complex planning
2. **Switch to Sonnet** for implementation
3. **Use Haiku** for simple tasks
4. **Return to Opus** for review

## Switching Process

### Compatible Continuation Steps

1. Click model selector
2. Choose new model
3. Review compatibility check
4. Confirm switch
5. Continue conversation

**No interruption to workflow**

### Portable Fork Steps

1. Click model selector
2. Choose new model
3. Review incompatibility warning
4. Review what will be preserved
5. Review what will be lost
6. Confirm fork creation
7. New session opens
8. Original session remains available

**Original session preserved**

## Best Practices

### When to Switch Models

**Good reasons**:
- Task complexity changed
- Cost optimization needed
- Speed requirements changed
- Different capabilities needed

**Poor reasons**:
- Dissatisfied with single response
- Hoping for different answer
- Avoiding proper context management

### Maintaining Context

Before switching:
1. Create checkpoint if needed
2. Pin important content
3. Review context size
4. Ensure critical info is preserved

### After Switching

After a switch:
1. Verify context is correct
2. Check pinned content
3. Confirm capabilities work
4. Test critical features

## Troubleshooting

### "Incompatible Model" Error

**Cause**: New model lacks required capabilities

**Solution**:
1. Review missing capabilities
2. Choose compatible model, or
3. Create portable fork

### "Context Too Large" Error

**Cause**: Context exceeds new model's limit

**Solution**:
1. Create checkpoint to reduce context
2. Unpin unnecessary content
3. Choose model with larger context, or
4. Create portable fork with reduced context

### "Fork Required" Warning

**Cause**: Incompatible features in use

**Solution**:
1. Review what will be lost
2. Decide if acceptable
3. Create fork if acceptable, or
4. Stay with current model

### Lost Context After Fork

**Cause**: Some content not transferable

**Expected**: Portable forks intentionally exclude:
- Encrypted reasoning
- Cache handles
- Provider-specific state

**Solution**:
- Use compatible continuation when possible
- Create checkpoint before forking
- Pin critical content

## Advanced Topics

### Multi-Model Workflows

Use different models for different stages:

**Planning** (Opus):
- Complex problem decomposition
- Architecture decisions
- Extended reasoning

**Implementation** (Sonnet):
- Code generation
- File modifications
- Tool usage

**Review** (Opus):
- Code review
- Quality checks
- Final validation

**Quick Tasks** (Haiku):
- Simple queries
- Fast iterations
- Cost-effective operations

### Model Profiles

Configure model profiles for different roles:
- **Agent**: Primary conversational model
- **Plan**: Task planning model
- **Apply**: Code modification model
- **Review**: Code review model

Each role can use a different model optimized for that task.

### Session Forking Strategy

**When to fork**:
- Trying different approaches
- Experimenting with solutions
- Preserving decision points
- Creating variants

**Fork management**:
- Name forks descriptively
- Archive unused forks
- Merge insights back to main session

## Summary

### Compatible Continuation

- ✓ Seamless transition
- ✓ Complete history preserved
- ✓ No data loss
- ✓ Requires compatible models

### Portable Fork

- ✓ Works with any model
- ✓ Original session preserved
- ⚠ Some data lost
- ⚠ New session created

### Choosing Wisely

1. Check compatibility first
2. Use continuation when possible
3. Fork when necessary
4. Understand trade-offs
5. Manage context proactively

## Additional Resources

- Model Catalog: View all available models
- Capability Matrix: Compare model features
- Cost Calculator: Estimate session costs
- Performance Benchmarks: Compare model speed
- User Guide: `docs/END-USER-CONCEPTS-GUIDE.md`
