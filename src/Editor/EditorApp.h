#pragma once

namespace se::editor {

/// Creates the Qt application, the Vulkan instance, the application controller
/// and the main window, then runs the event loop. Returns the process exit code.
///
/// Command line:
///   --subdivisions <n>   icosphere level, 20 * 4^n triangles (default 6)
///   --vertex-page <n>    vertices per geometry page (power of two, default 1024)
///   --chunk <n>          triangles per spatial chunk (power of two, default 4096)
///   --threads <n>        helper threads for sculpting (0 = none, default: automatic)
///   --validation         enable the Vulkan validation layer when installed
///   --selftest <dir>     run the scripted smoke test, write screenshots and a
///                        report into <dir>, then exit (0 = all checks passed)
int runEditor(int argc, char** argv);

} // namespace se::editor
