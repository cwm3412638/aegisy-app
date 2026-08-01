# Model Catalog Role Recommendations

This document defines how Aegisy provides role-specific model recommendations backed
by evaluation data, sample sizes, and known limitations.

## Overview

The Model Catalog includes role recommendations that help users select appropriate
models for specific tasks. Recommendations are based on:

- Evaluation results from standardized benchmarks
- Sample sizes and statistical confidence
- Known limitations and failure modes
- Provider-reported capabilities
- Community feedback and usage patterns

## Role Definitions

### Agent Role

**Purpose**: Primary conversational agent for interactive coding tasks

**Key Capabilities**:
- Multi-turn conversation
- Code generation and editing
- Tool use and command execution
- Context understanding
- Error recovery

**Evaluation Criteria**:
- Code correctness on standard benchmarks
- Tool use accuracy
- Context retention across turns
- Error handling and recovery
- Response latency

### Plan Role

**Purpose**: High-level task decomposition and planning

**Key Capabilities**:
- Task breakdown
- Dependency identification
- Resource estimation
- Risk assessment
- Alternative approach generation

**Evaluation Criteria**:
- Plan completeness
- Dependency accuracy
- Feasibility assessment
- Execution success rate

### Apply Role

**Purpose**: Precise code modifications and refactoring

**Key Capabilities**:
- Exact code edits
- Refactoring operations
- Syntax preservation
- Minimal diff generation
- Type safety

**Evaluation Criteria**:
- Edit precision
- Syntax correctness
- Minimal change size
- Type safety preservation
- Test pass rate

### Review Role

**Purpose**: Code review and quality assessment

**Key Capabilities**:
- Bug detection
- Style checking
- Security analysis
- Performance review
- Best practice validation

**Evaluation Criteria**:
- Bug detection rate
- False positive rate
- Security issue identification
- Performance insight quality
- Actionable feedback

### Utility Role

**Purpose**: Quick, focused tasks (formatting, simple queries)

**Key Capabilities**:
- Fast response
- Low cost
- Simple transformations
- Format conversion
- Quick lookups

**Evaluation Criteria**:
- Response latency
- Cost efficiency
- Accuracy on simple tasks
- Consistency

### Embedding Role

**Purpose**: Text embedding for semantic search and similarity

**Key Capabilities**:
- High-quality embeddings
- Consistent representation
- Efficient computation
- Domain adaptation

**Evaluation Criteria**:
- Retrieval accuracy
- Embedding quality metrics
- Computational efficiency
- Cross-domain performance

### Rerank Role

**Purpose**: Result reranking for improved relevance

**Key Capabilities**:
- Relevance scoring
- Context-aware ranking
- Fast inference
- Calibrated scores

**Evaluation Criteria**:
- Ranking quality (NDCG, MRR)
- Calibration accuracy
- Inference speed
- Consistency

## Recommendation Schema

```json
{
  "role": "agent",
  "model_id": "claude-opus-5",
  "recommendation_strength": "strong",
  "evaluation_version": "2026-Q2",
  "sample_size": 1000,
  "confidence_level": 0.95,
  "metrics": {
    "accuracy": 0.92,
    "latency_p50_ms": 1200,
    "latency_p95_ms": 3500,
    "cost_per_1k_tokens": 0.015
  },
  "known_limitations": [
    "May struggle with very large codebases (>100k LOC)",
    "Occasional hallucination on obscure APIs"
  ],
  "best_for": [
    "Interactive coding sessions",
    "Complex refactoring",
    "Multi-file changes"
  ],
  "not_recommended_for": [
    "Simple formatting tasks (use utility role)",
    "Batch processing (use apply role)"
  ]
}
```

## Recommendation Strength

### Strong

- High confidence (>95%)
- Large sample size (>1000 evaluations)
- Consistent performance across domains
- Few known limitations
- Positive user feedback

### Moderate

- Good confidence (80-95%)
- Adequate sample size (100-1000 evaluations)
- Generally good performance
- Some known limitations
- Mixed user feedback

### Weak

- Limited confidence (<80%)
- Small sample size (<100 evaluations)
- Inconsistent performance
- Significant limitations
- Limited user feedback

### Not Recommended

- Poor performance on evaluations
- Known critical issues
- Better alternatives available
- Deprecated or unsupported

## Evaluation Methodology

### Benchmark Suites

- **HumanEval**: Code generation correctness
- **MBPP**: Python programming problems
- **SWE-bench**: Real-world software engineering tasks
- **CodeReview**: Bug detection and review quality
- **ToolUse**: Tool calling accuracy

### Sample Size Requirements

- Minimum 100 evaluations for any recommendation
- Minimum 1000 evaluations for "strong" recommendation
- Stratified sampling across:
  - Programming languages
  - Task complexity
  - Domain areas
  - Error conditions

### Statistical Confidence

- 95% confidence intervals reported
- Multiple evaluation runs to assess variance
- Cross-validation across different prompts
- Comparison with baseline models

## Known Limitations

### Common Limitation Categories

1. **Scale Limitations**
   - Maximum context size
   - Performance degradation on large inputs
   - Memory constraints

2. **Domain Limitations**
   - Weak performance on specific languages
   - Limited knowledge of niche frameworks
   - Outdated information on recent technologies

3. **Capability Limitations**
   - Missing tool support
   - Limited multimodal capabilities
   - Restricted output formats

4. **Reliability Limitations**
   - Occasional hallucinations
   - Inconsistent formatting
   - Error recovery gaps

### Limitation Severity

- **Critical**: Blocks primary use case
- **Major**: Significantly impacts usability
- **Minor**: Occasional inconvenience
- **Informational**: Good to know, rarely impacts use

## User Feedback Integration

### Feedback Collection

- In-app rating system
- Detailed issue reports
- Usage analytics
- Community forums

### Feedback Processing

- Aggregate ratings by role and model
- Identify common pain points
- Track improvement over time
- Validate against evaluation data

### Feedback Transparency

- Show aggregate user ratings
- Display common feedback themes
- Indicate sample size for ratings
- Separate evaluation data from user feedback

## Recommendation Updates

### Update Frequency

- Evaluation data: Quarterly
- User feedback: Continuous aggregation
- Known limitations: As discovered
- Model capabilities: On model updates

### Version Tracking

Each recommendation includes:
- Evaluation version (e.g., "2026-Q2")
- Model version evaluated
- Benchmark versions used
- Last updated timestamp

### Deprecation Policy

- Mark outdated recommendations as "stale"
- Provide migration guidance
- Maintain historical data for comparison
- Clear communication of changes

## Privacy and Ethics

### Data Collection

- No user code in evaluation datasets
- Anonymized usage patterns only
- Opt-in for detailed feedback
- Clear data retention policies

### Bias Mitigation

- Diverse evaluation datasets
- Multiple programming languages
- Various coding styles
- Different experience levels

### Transparency

- Open evaluation methodology
- Published benchmark results
- Clear limitation disclosure
- No hidden model capabilities

## Implementation Status

### Current State

- Schema defined ✓
- Evaluation methodology documented ✓
- Role definitions complete ✓
- Sample size requirements specified ✓

### Not Yet Implemented

- Actual evaluation runs
- User feedback collection
- Recommendation API
- UI integration
- Continuous updates

### Future Enhancements

- Real-time performance monitoring
- A/B testing framework
- Personalized recommendations
- Community-contributed evaluations

## References

- Model Catalog Schema: `docs/MODEL-CATALOG-SCHEMA.md`
- Evaluation Benchmarks: `docs/MODEL-EVALUATION-BENCHMARKS.md`
- Role Definitions: `docs/MODEL-ROLES.md`
- User Feedback System: `docs/USER-FEEDBACK-SYSTEM.md`
