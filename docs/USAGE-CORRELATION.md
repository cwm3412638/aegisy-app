# Usage Correlation and Attribution

This document defines how Aegisy tracks and attributes model usage across retries,
reroutes, cache hits, reasoning tokens, and child task consumption.

## Overview

Accurate usage tracking is essential for:
- Cost attribution and billing
- Performance optimization
- Debugging and troubleshooting
- Capacity planning
- User transparency

Aegisy separates usage into distinct categories to provide clear visibility into
where tokens and costs are consumed.

## Usage Categories

### Primary Request

**Definition**: The initial user-initiated request that produces a response

**Includes**:
- User prompt tokens
- System prompt tokens
- Context tokens (files, history, etc.)
- Response output tokens
- Tool call tokens

**Excludes**:
- Retry attempts
- Rerouted requests
- Cache hits (counted separately)
- Child task consumption

### Retry

**Definition**: Repeated attempts of the same request due to transient failures

**Triggers**:
- Network timeouts
- Rate limit errors (429)
- Server errors (5xx)
- Connection failures
- Transient provider errors

**Attribution**:
- Counted separately from primary request
- Includes full input/output tokens
- Marked with retry attempt number
- Associated with original request ID

**Not Retried**:
- Client errors (4xx except 429)
- Authentication failures
- Invalid requests
- User cancellations

### Reroute

**Definition**: Redirecting a request to a different model or provider

**Triggers**:
- Model unavailable
- Capacity constraints
- Fallback policy
- Cost optimization
- Performance requirements

**Attribution**:
- Counted separately from primary request
- Includes full input/output tokens
- Marked with reroute reason
- Associated with original request ID
- Original model and target model recorded

**Reroute Policies**:
- Explicit user selection
- Automatic fallback (with user consent)
- Cost-based routing
- Performance-based routing

### Cache Hit

**Definition**: Tokens served from cache without model inference

**Types**:
1. **Prompt Cache**: Reused system/context tokens
2. **Response Cache**: Identical request/response pairs
3. **Reasoning Cache**: Reused intermediate reasoning

**Attribution**:
- Counted separately with reduced cost
- Cache hit rate tracked
- Cache source identified
- Original tokens vs cached tokens

**Cache Policies**:
- TTL-based expiration
- LRU eviction
- Size limits
- Invalidation rules

### Reasoning Tokens

**Definition**: Internal reasoning tokens not shown to user

**Includes**:
- Extended thinking tokens
- Internal planning tokens
- Self-correction tokens
- Verification tokens

**Attribution**:
- Counted separately from output tokens
- Marked as reasoning category
- Associated with primary request
- Shown in detailed usage breakdown

**Visibility**:
- Summary count always shown
- Detailed reasoning optional (privacy setting)
- Encrypted reasoning excluded from export

### Child Task Consumption

**Definition**: Tokens consumed by spawned subtasks

**Examples**:
- Plan generation subtask
- Apply operation subtask
- Review subtask
- Parallel tool calls
- Background jobs

**Attribution**:
- Counted separately from parent task
- Hierarchical relationship tracked
- Aggregated to parent for total cost
- Individual subtask breakdown available

**Hierarchy**:
```
Parent Task (100k tokens)
├─ Plan Subtask (20k tokens)
├─ Apply Subtask 1 (30k tokens)
├─ Apply Subtask 2 (25k tokens)
└─ Review Subtask (25k tokens)
```

## Usage Schema

```json
{
  "request_id": "req_abc123",
  "session_id": "sess_xyz789",
  "turn_id": "turn_001",
  "timestamp": "2026-08-01T12:00:00Z",
  "model_id": "claude-opus-5",
  "provider": "anthropic",
  
  "primary": {
    "input_tokens": 5000,
    "output_tokens": 2000,
    "reasoning_tokens": 1000,
    "cached_input_tokens": 3000,
    "cost_usd": 0.15
  },
  
  "retries": [
    {
      "attempt": 1,
      "reason": "timeout",
      "input_tokens": 5000,
      "output_tokens": 0,
      "cost_usd": 0.075
    }
  ],
  
  "reroutes": [
    {
      "from_model": "claude-opus-5",
      "to_model": "claude-sonnet-5",
      "reason": "capacity",
      "input_tokens": 5000,
      "output_tokens": 2000,
      "cost_usd": 0.05
    }
  ],
  
  "child_tasks": [
    {
      "task_id": "task_plan_001",
      "role": "plan",
      "input_tokens": 2000,
      "output_tokens": 1000,
      "cost_usd": 0.045
    }
  ],
  
  "totals": {
    "total_input_tokens": 12000,
    "total_output_tokens": 5000,
    "total_reasoning_tokens": 1000,
    "total_cached_tokens": 3000,
    "total_cost_usd": 0.32,
    "effective_cost_usd": 0.15
  }
}
```

## Cost Attribution

### User-Facing Cost

**Includes**:
- Primary request tokens
- Reasoning tokens (if enabled)
- Child task tokens
- Cache hits (at reduced rate)

**Excludes**:
- Retry tokens (Aegisy absorbs)
- Reroute tokens (Aegisy absorbs)
- System errors (Aegisy absorbs)

### Internal Cost Tracking

**Includes**:
- All token consumption
- All retry attempts
- All reroutes
- Infrastructure overhead

**Purpose**:
- Capacity planning
- Cost optimization
- Provider negotiation
- Debugging

### Cost Transparency

Users see:
- Primary request cost
- Child task breakdown
- Cache savings
- Reasoning token cost (if enabled)

Users don't see:
- Retry costs
- Reroute costs
- Infrastructure overhead
- Provider-specific details

## Correlation Mechanisms

### Request ID

- Unique identifier for each user request
- Persists across retries and reroutes
- Links child tasks to parent
- Enables end-to-end tracing

### Session ID

- Groups requests within a session
- Enables session-level aggregation
- Tracks context reuse
- Supports session cost limits

### Turn ID

- Identifies conversation turn
- Links request/response pairs
- Enables turn-level analysis
- Supports turn replay

### Task Hierarchy

- Parent-child relationships
- Depth tracking
- Aggregation support
- Circular reference prevention

## Usage Reporting

### Real-Time Display

- Current request progress
- Token count updates
- Cost estimates
- Cache hit indicators

### Session Summary

- Total tokens consumed
- Total cost
- Cache hit rate
- Retry/reroute counts

### Historical Analysis

- Usage trends over time
- Cost breakdown by category
- Model comparison
- Optimization opportunities

## Privacy Considerations

### Token Content

- Token counts tracked
- Token content never logged
- Reasoning content optional
- User data never shared

### Aggregation

- Individual requests anonymized
- Aggregate statistics only
- No cross-user correlation
- Opt-in for detailed tracking

### Retention

- Usage data retained 90 days
- Aggregates retained longer
- User can request deletion
- Compliance with data regulations

## Implementation Status

### Current State

- Schema defined ✓
- Categories specified ✓
- Attribution rules documented ✓
- Privacy policy defined ✓

### Not Yet Implemented

- Actual usage tracking
- Cost calculation
- Retry/reroute correlation
- Child task attribution
- UI display
- Historical reporting

### Future Enhancements

- Real-time cost alerts
- Budget enforcement
- Usage optimization suggestions
- Anomaly detection
- Cost forecasting

## Error Handling

### Tracking Failures

- Graceful degradation
- Best-effort attribution
- Missing data indicators
- Audit trail

### Reconciliation

- Provider bill comparison
- Discrepancy resolution
- Adjustment mechanisms
- Transparency in corrections

## References

- Model Catalog: `docs/MODEL-CATALOG-SCHEMA.md`
- Cost Policies: `docs/COST-POLICIES.md`
- Privacy Policy: `docs/PRIVACY-POLICY.md`
- Provider Integration: `docs/PROVIDER-INTEGRATION.md`
