# Code Review Checklist

Use this checklist when reviewing pull requests or conducting code reviews.

## General

- [ ] Code compiles without warnings
- [ ] All tests pass
- [ ] No merge conflicts
- [ ] Commit messages are clear and follow conventions
- [ ] Changes are focused and atomic
- [ ] No commented-out code (unless with explanation)
- [ ] No debug print statements left in code

## Code Quality

- [ ] Code follows existing style and patterns
- [ ] Variable and function names are descriptive
- [ ] Functions are reasonably sized and focused
- [ ] Complex logic has explanatory comments
- [ ] No code duplication (DRY principle)
- [ ] Error handling is appropriate
- [ ] Edge cases are handled

## C++ Specific

- [ ] Memory management is correct (no leaks)
- [ ] Smart pointers used appropriately
- [ ] RAII principles followed
- [ ] No raw `new`/`delete` without good reason
- [ ] Qt signals/slots used correctly
- [ ] Thread safety considered for concurrent code
- [ ] No undefined behavior

## Rust Specific

- [ ] `cargo fmt` has been run
- [ ] `cargo clippy` passes with no warnings
- [ ] Ownership and borrowing are correct
- [ ] Error handling uses `Result` appropriately
- [ ] No `unwrap()` in production code without justification
- [ ] Unsafe code is minimal and justified
- [ ] Documentation comments for public APIs

## Testing

- [ ] New functionality has tests
- [ ] Tests cover success and failure cases
- [ ] Tests cover edge cases and boundaries
- [ ] Test names are descriptive
- [ ] Tests are independent and repeatable
- [ ] No flaky tests
- [ ] Test coverage is adequate

## Security

- [ ] Input validation is present
- [ ] No SQL injection vulnerabilities
- [ ] No command injection vulnerabilities
- [ ] Secrets are not hardcoded
- [ ] Credentials use secure storage
- [ ] File paths are validated (no traversal)
- [ ] Network requests use TLS
- [ ] User data is sanitized before display

## Performance

- [ ] No obvious performance issues
- [ ] Database queries are efficient
- [ ] Large operations are async/background
- [ ] Memory usage is reasonable
- [ ] No unnecessary allocations in hot paths
- [ ] Caching is used appropriately

## AAP Protocol (if applicable)

- [ ] Protocol version compatibility maintained
- [ ] Request/response schemas are valid
- [ ] Error codes are appropriate
- [ ] Capability requirements are correct
- [ ] Idempotency is handled correctly
- [ ] Frame size limits are respected

## UI/UX (if applicable)

- [ ] UI is responsive and doesn't freeze
- [ ] Error messages are user-friendly
- [ ] Loading states are shown
- [ ] Keyboard shortcuts work
- [ ] Accessibility is considered
- [ ] Works on both macOS and Windows
- [ ] Works at different display scales

## Documentation

- [ ] README updated if needed
- [ ] CHANGELOG.md updated for user-facing changes
- [ ] API documentation updated
- [ ] OpenSpec tasks.md updated if completing tasks
- [ ] ADR created for architectural decisions
- [ ] Comments explain "why" not "what"

## OpenSpec (if applicable)

- [ ] Changes align with OpenSpec design
- [ ] Tasks are marked complete when done
- [ ] Progress notes added to tasks.md
- [ ] Verification criteria met
- [ ] No deviation from approved design without discussion

## Git

- [ ] Branch name is descriptive
- [ ] Commits are logical and atomic
- [ ] Commit messages follow format: `type(scope): subject`
- [ ] Co-Authored-By attribution included
- [ ] No sensitive data in commits
- [ ] No large binary files added

## Before Merge

- [ ] All review comments addressed
- [ ] CI/CD checks pass
- [ ] At least one approval from maintainer
- [ ] No outstanding questions or concerns
- [ ] Squash commits if needed
- [ ] Rebase on latest main if needed

## Post-Merge

- [ ] Monitor for issues after merge
- [ ] Update related documentation
- [ ] Close related issues
- [ ] Notify stakeholders if needed

---

## Review Tips

### For Reviewers

- Be constructive and respectful
- Explain the "why" behind suggestions
- Distinguish between blocking issues and suggestions
- Approve when ready, don't nitpick
- Test the changes locally if possible

### For Authors

- Respond to all comments
- Don't take feedback personally
- Ask for clarification if needed
- Update the PR based on feedback
- Thank reviewers for their time

## Common Issues to Watch For

### Memory Issues
- Memory leaks (use Valgrind/ASAN)
- Use-after-free
- Double-free
- Buffer overflows

### Concurrency Issues
- Race conditions
- Deadlocks
- Missing synchronization
- Shared mutable state

### Security Issues
- Injection vulnerabilities
- Path traversal
- Insecure deserialization
- Missing authentication/authorization

### Performance Issues
- N+1 queries
- Unnecessary copies
- Blocking operations on UI thread
- Memory allocations in loops

## Resources

- [C++ Core Guidelines](https://isocpp.github.io/CppCoreGuidelines/)
- [Rust API Guidelines](https://rust-lang.github.io/api-guidelines/)
- [Qt Best Practices](https://doc.qt.io/qt-6/best-practices.html)
- Project: `docs/CONTRIBUTING.md`
- Project: `docs/QUICK-REFERENCE.md`
