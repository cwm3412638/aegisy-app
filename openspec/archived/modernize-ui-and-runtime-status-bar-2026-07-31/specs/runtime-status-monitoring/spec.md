## ADDED Requirements

### Requirement: Runtime status uses observable data
The system SHALL aggregate model, reasoning, context usage, context limit,
activity state, monitoring provenance, and balance without fabricating missing
telemetry.

#### Scenario: Gateway request reports usage
- **WHEN** a configured CLI request passes through the local gateway and its response contains provider usage metadata
- **THEN** the runtime status SHALL display the observed model and usage against a known context limit

#### Scenario: Direct mode cannot be observed
- **WHEN** a CLI is configured for direct mode and no in-app request is active
- **THEN** the runtime status SHALL label the model as configured and the live usage as unmonitored or unknown

### Requirement: Provider streams remain private
The gateway SHALL extract runtime metadata without persisting prompt,
completion, tool argument, or file content.

#### Scenario: Streaming completion is monitored
- **WHEN** an SSE or JSON response passes through the gateway
- **THEN** the gateway SHALL forward the response, emit only allowed metadata, and discard parsed content

### Requirement: Background status bar
The application SHALL show a compact draggable status bar when the main window
is minimized or hidden while the application remains running.

#### Scenario: User minimizes the application
- **WHEN** the main window becomes minimized
- **THEN** the runtime status bar SHALL appear with the latest snapshot and a click action that restores the main window

#### Scenario: Context approaches its limit
- **WHEN** observed input usage reaches warning or critical thresholds of a known limit
- **THEN** the status bar SHALL communicate the state through text and semantic color without relying on color alone

### Requirement: Cross-platform background behavior
The status bar SHALL remain usable on supported Windows and macOS displays.

#### Scenario: Saved display is disconnected
- **WHEN** the saved status-bar position is outside all current screens
- **THEN** the application SHALL place it inside the primary screen's available geometry
