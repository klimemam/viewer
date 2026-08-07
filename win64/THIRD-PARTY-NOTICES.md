# THIRD-PARTY NOTICES

`viewer` is **distributed as binaries**: every push to `main` force-pushes
`viewer.exe` / `viewer` / `viewer-serve` and the bundled plugins onto the
`binaries` branch (`.github/workflows/build.yml`, job `publish-binaries`), and
that branch is what a machine with no build tools clones. Distributing a binary
that has other people's code inside it carries attribution obligations, and this
file is where they are met.

**This list is what is actually linked, not what one might expect to be.** It was
produced by reading the link lines out of the generated build (`build.ninja`:
the object list and `LINK_LIBRARIES` for both executables) and the licence text
out of each dependency's own source tree as FetchContent downloaded it - not
from memory and not from the README. To reproduce it:

```
cmake -S . -B build && cmake --build build
grep -A3 '^build viewer' build/build.ninja        # or the .vcxproj link step on MSVC
ls build/_deps/*/LICENSE*                          # the texts quoted below
```

## What ends up in which binary

| Component | Version pinned in CMakeLists.txt | Licence | `viewer` | `viewer-serve` | plugins |
|---|---|---|---|---|---|
| Dear ImGui (incl. its embedded `imstb_*` and the ProggyClean font) | v1.91.8-docking | MIT | yes | no | no |
| GLFW | 3.4 | zlib/libpng | yes | no | no |
| miniz | 3.0.2 | MIT | yes | yes | no |
| portable-file-dialogs | commit `c12ea8c` | WTFPL-2.0 | yes (header-only) | no | no |
| stb_image | commit `013ac3b` (v2.30) | MIT **or** public domain (Unlicense) | yes | no | no |

`viewer-serve` is the remote peer. It reads `.npy` only, so it links neither a
window toolkit nor an image decoder - which is also why the Ubuntu 20.04
compatibility rebuild in `build.yml` can still compile it with one `g++` line.

The bundled plugins (`plugins/*.c`) and the reader harness shipped beside the
binaries (`tools/import/*.py`) are this project's own code.

## Not bundled, and why it is worth saying

- **Operating-system libraries** - `opengl32`, `user32`, `gdi32`, `comdlg32`,
  `ole32`, `shell32`, `dwmapi`, `imm32` on Windows; X11 and `libdl`/`pthread` on
  Linux; the system frameworks on macOS. Dynamically linked, part of the
  platform, no redistribution.
- **The UI font.** The viewer picks a CJK face off the machine at runtime
  (`meiryo.ttc`, `NotoSansCJK`, Hiragino, ...) and falls back to Dear ImGui's
  embedded ProggyClean when it finds none. **No font file is redistributed**;
  the fallback is part of Dear ImGui and is covered by the MIT text below.
- **Python, numpy** - the reader harness runs in the user's own interpreter.
  Nothing is shipped.
- **zenity / kdialog** - portable-file-dialogs shells out to whatever the desktop
  has. Not linked, not shipped.

## Toolchain runtimes

These are not dependencies of the source, but they are inside the published
binaries, so they are named here:

- **Windows** (CI builds with MSVC): the Microsoft C/C++ runtime, linked the
  way the Visual Studio generator defaults - Microsoft's own redistribution
  terms apply. A local MinGW build instead links libstdc++ / libgcc /
  libwinpthread **statically** (`-static`, `CMakeLists.txt`, the `if(MINGW)`
  block); those are GPL-3.0 code carried under the **GCC Runtime Library
  Exception**, which exists to permit exactly this.
- **Linux**: `viewer` links libstdc++ dynamically; `viewer-serve` is rebuilt in
  an Ubuntu 20.04 container with `-static-libgcc -static-libstdc++`, so it
  carries the same GCC runtime under the same exception.
- **macOS**: the system libc++.

---

The full licence texts follow, quoted from the source trees the build
downloaded. They are reproduced in full because the MIT and zlib texts both
require it.


## Dear ImGui - MIT

https://github.com/ocornut/imgui - `imgui-src/LICENSE.txt`

```
The MIT License (MIT)

Copyright (c) 2014-2025 Omar Cornut

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## GLFW - zlib/libpng

https://www.glfw.org - `glfw-src/LICENSE.md`

```
Copyright (c) 2002-2006 Marcus Geelnard

Copyright (c) 2006-2019 Camilla Löwy

This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would
   be appreciated but is not required.

2. Altered source versions must be plainly marked as such, and must not
   be misrepresented as being the original software.

3. This notice may not be removed or altered from any source
   distribution.
```

## miniz - MIT

https://github.com/richgel999/miniz - `miniz-src/LICENSE`

```
Copyright 2013-2014 RAD Game Tools and Valve Software
Copyright 2010-2014 Rich Geldreich and Tenacious Software LLC

All Rights Reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

## portable-file-dialogs - WTFPL-2.0

https://github.com/samhocevar/portable-file-dialogs - `pfd-src/COPYING`

```
            DO WHAT THE FUCK YOU WANT TO PUBLIC LICENSE
                    Version 2, December 2004

 Copyright (C) 2004 Sam Hocevar

 Everyone is permitted to copy and distribute verbatim or modified
 copies of this license document, and changing it is allowed as long
 as the name is changed.

            DO WHAT THE FUCK YOU WANT TO PUBLIC LICENSE
   TERMS AND CONDITIONS FOR COPYING, DISTRIBUTION AND MODIFICATION

  0. You just DO WHAT THE FUCK YOU WANT TO.
```

## stb_image - MIT or public domain

https://github.com/nothings/stb - the licence block at the end of `stb_image.h`.
The author offers a choice of two; **this project uses it under the MIT option**,
because a public-domain dedication is not effective in every jurisdiction.

```
------------------------------------------------------------------------------
ALTERNATIVE A - MIT License
Copyright (c) 2017 Sean Barrett
Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:
The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
------------------------------------------------------------------------------
ALTERNATIVE B - Public Domain (www.unlicense.org)
This is free and unencumbered software released into the public domain.
Anyone is free to copy, modify, publish, use, compile, sell, or distribute this
software, either in source code form or as a compiled binary, for any purpose,
commercial or non-commercial, and by any means.
In jurisdictions that recognize copyright laws, the author or authors of this
software dedicate any and all copyright interest in the software to the public
domain. We make this dedication for the benefit of the public at large and to
the detriment of our heirs and successors. We intend this dedication to be an
overt act of relinquishment in perpetuity of all present and future rights to
this software under copyright law.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
------------------------------------------------------------------------------
```
