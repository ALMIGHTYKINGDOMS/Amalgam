# Managed Java Runtimes

Amalgam manages portable Eclipse Temurin runtimes on demand. Runtime archives
are never bundled in the installer. The default managed root is:

```text
Amalgam/runtimes/java/<major>/
```

Supported managed majors are Java 8, 11, 17, 21, and 25. Existing compatible
Java installations are detected from `JAVA_HOME`, `PATH`, and common Windows
installation directories. Resolution prefers a validated Amalgam-managed
runtime, then a validated system runtime, then an on-demand Temurin download.

Each managed runtime contains `runtime.json` with its major version, Temurin
vendor, architecture, exact version, installation path, and managed-runtime
flag. Downloads use the Adoptium assets API, verify the advertised SHA-256 and
size before extraction, reject unsafe/oversized archives, validate `java.exe
-version`, and install atomically into the version-specific directory.

The Java Manager page can install, rescan, open, select, set defaults for, and
remove managed runtimes. The CLI equivalent is:

```text
amalgam_launcher.exe --java-install 21
```

The runtime download and extraction run on a worker thread when started from
the UI. Server startup resolves the required Java major from its Minecraft
version and uses the exact managed executable when available.
