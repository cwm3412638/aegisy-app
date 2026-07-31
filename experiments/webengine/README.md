# Qt WebEngine Experiment

Isolated build experiment for task 2.1 to verify Qt WebEngine linking without affecting production builds.

## Build

```bash
cmake -B build -DAEGISY_BUILD_WEBENGINE_EXPERIMENT=ON
cmake --build build --target aegisy_webengine_experiment
```

## Run

```bash
./build/aegisy_webengine_experiment
```

This experiment creates a minimal QWebEngineView that loads a simple HTML page to verify Qt WebEngine integration.
