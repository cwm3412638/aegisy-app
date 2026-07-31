# Contributing to Aegisy Coding Workbench

Thank you for your interest in contributing to Aegisy Coding Workbench!

## Getting Started

### Prerequisites

- CMake 3.16+
- C++17 compiler (GCC, Clang, or MSVC)
- Qt 5.15+ or Qt 6.x
- Rust toolchain (for agent-runtime)
- Node.js (for local gateway and CLI management)
- Git

See [README.md](../README.md) for detailed system requirements.

### Development Setup

```bash
# Clone the repository
git clone https://github.com/cwm3412638/aegisy-app.git
cd aegisy-app

# Build the project
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel

# Run tests
cd build && ctest --output-on-failure

# Or use the helper script
./scripts/dev-helper.sh build
./scripts/dev-helper.sh test
```

## Development Workflow

### 1. Check OpenSpec Tasks

Before starting work, check the current OpenSpec status:

```bash
./scripts/openspec-status.sh
```

Review `openspec/changes/build-aegisy-agent-workbench/tasks.md` to find tasks
that need work.

### 2. Create a Branch

```bash
git checkout -b feature/your-feature-name
# or
git checkout -b fix/your-bug-fix
```

### 3. Make Changes

- Follow existing code style and patterns
- Add tests for new functionality
- Update documentation as needed
- Keep commits focused and atomic

### 4. Test Your Changes

```bash
# Run all tests
./scripts/run-tests.sh all

# Run specific test categories
./scripts/run-tests.sh update
./scripts/run-tests.sh aap
./scripts/run-tests.sh runtime
```

### 5. Commit Your Changes

Use descriptive commit messages following this format:

```
<type>(<scope>): <subject>

<body>

Co-Authored-By: Your Name <your.email@example.com>
```

Types:
- `feat`: New feature
- `fix`: Bug fix
- `docs`: Documentation changes
- `test`: Test additions or changes
- `refactor`: Code refactoring
- `chore`: Build process or auxiliary tool changes
- `style`: Code style changes (formatting, etc.)

Examples:
```
feat(aap): add session compaction support

Implement session compaction with checkpoint creation and review.
Includes SQLite schema updates and Qt UI integration.

Co-Authored-By: Your Name <your.email@example.com>
```

```
fix(runtime): handle reconnection after heartbeat timeout

Fix issue where heartbeat timeout didn't properly trigger reconnection.
Add test coverage for timeout scenarios.

Co-Authored-By: Your Name <your.email@example.com>
```

### 6. Push and Create Pull Request

```bash
git push origin feature/your-feature-name
```

Then create a pull request on GitHub with:
- Clear description of changes
- Reference to related OpenSpec tasks
- Test results
- Screenshots (for UI changes)

## Code Style

### C++

- Follow existing code style in the project
- Use meaningful variable and function names
- Add comments for complex logic
- Keep functions focused and reasonably sized

### Rust

- Run `cargo fmt` before committing
- Run `cargo clippy` and fix all warnings
- Follow Rust naming conventions
- Add documentation comments for public APIs

### Documentation

- Use Markdown for all documentation
- Keep line length reasonable (80-100 characters)
- Use clear, concise language
- Include code examples where helpful

## Testing Guidelines

### Writing Tests

- Test files go in `tests/` directory
- Name test files `*_test.cpp` or `*_test.cmake`
- Use descriptive test names
- Test both success and failure cases
- Include edge cases and boundary conditions

### Test Categories

- **Unit tests**: Test individual functions/classes
- **Integration tests**: Test component interactions
- **Protocol tests**: Test AAP protocol compliance
- **Regression tests**: Prevent known bugs from returning

See [docs/TEST-COVERAGE-ANALYSIS.md](TEST-COVERAGE-ANALYSIS.md) for coverage
analysis and recommendations.

## Documentation

### When to Update Documentation

- Adding new features → Update relevant design docs
- Changing APIs → Update API documentation
- Fixing bugs → Update CHANGELOG.md
- Completing OpenSpec tasks → Update tasks.md

### Documentation Files

- `README.md`: Project overview and setup
- `docs/WORKBENCH-STATUS.md`: Development status
- `docs/adr/*.md`: Architecture decisions
- `openspec/changes/*/tasks.md`: Task tracking
- `CHANGELOG.md`: Version history

## OpenSpec Process

### Understanding OpenSpec

OpenSpec is our specification-driven development process. Each major change has:

- `proposal.md`: Initial proposal
- `design.md`: Detailed design
- `tasks.md`: Tracked tasks
- `verification.md`: Verification criteria
- `specs/*/spec.md`: Component specifications

### Working on OpenSpec Tasks

1. Find an unchecked task in `tasks.md`
2. Read related design and spec documents
3. Implement the task
4. Update `tasks.md` with progress notes
5. Mark task as complete when done

### Creating New OpenSpec Changes

For major new features:

1. Create proposal in `openspec/changes/your-change/`
2. Get feedback from maintainers
3. Write detailed design document
4. Break down into tasks
5. Implement incrementally

## Security

### Reporting Security Issues

**Do not** open public issues for security vulnerabilities.

Instead, email security concerns to: [security contact to be added]

### Security Guidelines

- Never commit secrets or credentials
- Use platform secure storage for sensitive data
- Validate all user input
- Follow principle of least privilege
- Review security boundaries in design docs

## Getting Help

### Resources

- OpenSpec documentation: `openspec/changes/build-aegisy-agent-workbench/`
- Design documents: `docs/*.md`
- ADRs: `docs/adr/*.md`
- Project memory: `PROJECT-MEMORY.md`

### Communication

- GitHub Issues: Bug reports and feature requests
- GitHub Discussions: Questions and general discussion
- Pull Requests: Code review and feedback

## Review Process

### What Reviewers Look For

- Code quality and style
- Test coverage
- Documentation updates
- OpenSpec task completion
- Security considerations
- Performance implications

### Review Timeline

- Initial review: Within 1-2 business days
- Follow-up reviews: Within 1 business day
- Merge: After approval and CI passes

## License

By contributing, you agree that your contributions will be licensed under the
same license as the project. See [docs/THIRD-PARTY-COMPONENT-INVENTORY.md](THIRD-PARTY-COMPONENT-INVENTORY.md)
for component licenses.

## Recognition

Contributors are recognized in:
- Git commit history (Co-Authored-By)
- Release notes
- Project documentation

Thank you for contributing to Aegisy Coding Workbench!
