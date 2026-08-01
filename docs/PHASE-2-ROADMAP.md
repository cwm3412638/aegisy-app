# OpenSpec Phase 2 Roadmap

## Current Status
- **Progress**: 86/239 tasks (36%)
- **Phase 1**: Complete (Timeline UI)
- **Phase 2**: Backend Integration

## High-Priority Sections

### 1. Section 5: Event Store (80% → 100%)
**Remaining**: 2 tasks
- 5.1: Complete SQLite schema (partial done)
- 5.2: Event persistence (partial done)

**Effort**: 1-2 days
**Value**: Critical for data persistence

### 2. Section 12: Timeline (86% → 100%)
**Remaining**: 2 tasks
- 12.9: Chat-to-Work conversion
- 12.10: Timeline stress tests

**Effort**: 1-2 days
**Value**: Complete timeline system

### 3. Section 2: UI Spike (89% → 100%)
**Remaining**: 1 task
- 2.6: Windows testing

**Effort**: 1 day (requires Windows)
**Value**: Cross-platform validation

## Medium-Priority Sections

### 4. Section 3: AAP Foundation (58% → 80%)
**Remaining**: 5 tasks
**Effort**: 3-4 days
**Value**: Protocol foundation

### 5. Section 14: Terminal (56% → 80%)
**Remaining**: 4 tasks
**Effort**: 2-3 days
**Value**: Command execution

### 6. Section 4: Runtime Sidecar (20% → 50%)
**Remaining**: 8 tasks
**Effort**: 4-5 days
**Value**: Backend runtime

## Integration Work

### Timeline → AAP Integration
1. Connect TimelineAPI to real event store
2. Replace mock data with AAP events
3. Implement real command execution
4. Add file picker for attachments

**Effort**: 3-4 days
**Value**: Production deployment

### Runtime Integration
1. Connect to sidecar process
2. Implement IPC communication
3. Add authentication
4. Handle lifecycle

**Effort**: 4-5 days
**Value**: Full system integration

## Estimated Timeline

### Week 1: Complete High-Priority
- Day 1-2: Section 5 (Event Store)
- Day 3-4: Section 12 (Timeline)
- Day 5: Section 2 (Windows testing)

**Target**: 40% overall (95/239 tasks)

### Week 2: Medium-Priority
- Day 1-3: Section 3 (AAP Foundation)
- Day 4-5: Section 14 (Terminal)

**Target**: 45% overall (107/239 tasks)

### Week 3-4: Integration
- Week 3: Timeline → AAP
- Week 4: Runtime integration

**Target**: 50% overall (120/239 tasks)

## Success Criteria

### Phase 2 Complete When:
- ✅ Section 5: 100%
- ✅ Section 12: 100%
- ✅ Timeline connected to real data
- ✅ Command execution working
- ✅ Overall: 50%+

## Next Steps

1. **Immediate**: Complete Section 5 (2 tasks)
2. **Short-term**: Complete Section 12 (2 tasks)
3. **Medium-term**: AAP integration
4. **Long-term**: Runtime integration

---

**Phase 1**: Complete ✓
**Phase 2**: Planned
**Target**: 50% by end of Phase 2
