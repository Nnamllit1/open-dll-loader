# Contributing

Keep the Windows loading path transparent and use documented APIs. Do not add concealment, security-setting changes, or forced unloading of arbitrary modules.

Run `build.cmd all` on Windows with MSVC, CMake, and Python installed. On Linux, configure and test the native host with the commands in the README. Changes to the plugin ABI or lifecycle must include a test using a newly created example host. Keep callbacks outside `DllMain`, and require successful cleanup before unloading a plugin image.

For UI changes, inspect the native test renders at regular and enlarged scaling. For Wine/Proton changes, report the runtime version and exact tests performed. A clean static scan, a successful build, or Wine compatibility does not establish behavior under every Proton version or target application.

Releases are driven by version tags. Update the project version, CLI/UI version strings, application manifest, and documentation together before tagging. Tagged releases are published only when Windows, Linux, and Wine checks pass.
