## ADDED Requirements

### Requirement: Modern application shell
The application SHALL present global navigation, page actions, account status,
and operational status with clear hierarchy in one responsive shell.

#### Scenario: Main window opens at minimum supported size
- **WHEN** the main window is shown at its minimum size
- **THEN** every primary navigation command SHALL remain reachable without content overlap or clipping

### Requirement: Platform-aware appearance
The interface SHALL use system fonts, logical pixels, semantic design tokens,
and consistent icons on Windows and macOS.

#### Scenario: Display scale changes
- **WHEN** the application runs on Windows at 125% or 150% scaling or on a Retina macOS display
- **THEN** controls and labels SHALL preserve readable text, stable alignment, and usable hit targets

#### Scenario: Platform appearance differs
- **WHEN** the operating system uses a supported light or dark appearance
- **THEN** application surfaces and window chrome SHALL avoid incoherent light/dark combinations

### Requirement: Primary and secondary actions
Profile surfaces SHALL emphasize activation and launch while grouping destructive
or infrequent actions separately.

#### Scenario: Profile row is displayed
- **WHEN** a connection profile is rendered
- **THEN** activate or launch SHALL be visually primary and edit, test, and delete SHALL remain accessible without five equal-weight buttons
