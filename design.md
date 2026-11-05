# XBlox package follow-ups

The host still compiles the engine into `pm-media`. These translation units include host headers (`cli/`, `core/command_variables.hpp`, `pm_branding_config.hpp`, `constants.hpp`). `pm-xblox-lib` is an INTERFACE include root in that build so the objects are not compiled twice.

1. Move host calls behind one hook header. The host installer TU is the only file that includes the settings store and the branding header.
2. Keep Win32 block bodies (`fs_blocks_win32.cpp`, overlay, transcription HUD) on the host image.
3. Link `pm-xblox-lib` as a static library from Tanit once those includes are gone. Until then, do not compile the same `.cpp` in both the lib and `pm-media`.
4. `pm-xblox` now runs the same command surface as `tanit-cli xblox`. Host hooks still live in `src/cli/pm_image_cmd_xblox.cpp`. Rebuild `tanit-cli` before relying on that adapter.
5. Android consumes the same source list after the desktop host configure is green.
