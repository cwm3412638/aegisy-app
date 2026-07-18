# Aegisy Workbench Web Surface

This package builds the trusted local Monaco editor and xterm.js terminal loaded
by the Qt desktop host. It has no network runtime dependencies. The host blocks external
navigation and remote URL access, and communicates through Qt WebChannel only.

The bundled Monaco editor, xterm.js, and FitAddon are MIT licensed. Their full
licenses are copied into the generated application resources during the build.
