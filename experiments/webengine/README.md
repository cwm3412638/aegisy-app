# Qt WebEngine Experiment

Isolated build experiments for OpenSpec tasks 2.1 and 2.2.

## Task 2.1: Basic WebEngine Linking

```bash
cmake -B build -DAEGISY_BUILD_WEBENGINE_EXPERIMENT=ON
cmake --build build --target aegisy_webengine_experiment
./build/aegisy_webengine_experiment
```

## Task 2.2: Secure Local Bundle with Network Blocking

```bash
cmake -B build -DAEGISY_BUILD_WEBENGINE_EXPERIMENT=ON
cmake --build build --target aegisy_webengine_secure_bundle
./build/aegisy_webengine_secure_bundle
```

Features:
- Local HTML bundle loaded via setHtml()
- Network navigation blocked via acceptNavigationRequest()
- Isolated QWebEngineProfile
- No cache, no persistent cookies
- LocalContentCanAccessRemoteUrls disabled
- Security test with blocked external link

This experiment verifies Qt WebEngine can render local workbench content with all network access disabled.

