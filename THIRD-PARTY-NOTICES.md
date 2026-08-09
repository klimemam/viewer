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
| LibRaw | 0.22.2 | LGPL-2.1 **or** CDDL-1.0 - **this project uses it under the CDDL-1.0 option** | yes | no | no |
| OpenEXR (incl. its vendored libdeflate and OpenJPH) | v3.4.13 | BSD-3-Clause | yes | no | no |
| Imath | v3.2.2 | BSD-3-Clause | yes | no | no |

`viewer-serve` is the remote peer. It reads `.npy` only, so it links neither a
window toolkit nor an image decoder - which is also why the Ubuntu 20.04
compatibility rebuild in `build.yml` can still compile it with one `g++` line.
Formats added to `viewer` are deliberately not added to it.

The bundled plugins (`plugins/*.c`), the TIFF reader (`core/tiffread.cpp`), the
OpenEXR *wrapper* (`core/exrread.cpp` - the library itself is the row above), the
LibRaw *wrapper* (`core/rawread.cpp` - likewise) and the reader harness shipped
beside the binaries (`tools/import/*.py`) are this project's own code, under the
repository's own Apache-2.0 licence.

**LibRaw is dual-licensed and this project makes the choice explicitly: CDDL-1.0.**
LibRaw's own `COPYRIGHT` offers "one of two licenses as you choose" - LGPL-2.1 or
CDDL-1.0 - so a distributor must pick one, and the two are not interchangeable
for what this project ships:

- **LGPL-2.1 would not be met.** Section 6 requires that a work which statically
  links the library be distributed in a form that lets the recipient RELINK it
  against a modified LibRaw - object files, or an equivalent mechanism. What CI
  publishes on every push to `main` is one self-contained `viewer.exe` with
  `-static` (`CMakeLists.txt`, the `if(MINGW)` block) and nothing else. Offering
  a relink mechanism is not a line of YAML; it is a different distribution.
- **CDDL-1.0 is met.** Its obligation for an Executable distribution (§3.1, §3.5)
  is that the Source Code of the Covered Software stay available under the CDDL
  and that the licence travel with the binary. LibRaw is used **unmodified at a
  pinned tag**, so the first is satisfied by the URL in `CMakeLists.txt` -
  <https://github.com/LibRaw/LibRaw/archive/refs/tags/0.22.2.zip> - and the
  second by this file, which `build.yml` copies into every published archive.
  Nothing in the build declares or implies the LGPL option.

**This project's own code is not Covered Software.** CDDL §1.6 keeps a "Larger
Work" under whatever terms its own author chooses as long as the Covered
Software's own files stay under the CDDL; LibRaw's files are unmodified, so
`viewer` stays Apache-2.0 and no source of ours is relicensed by the link.

**Only part of LibRaw is compiled, and it matters for this list.** The build takes
upstream's own `Makefile.devel.noppr2i` source set: `src/demosaic`,
`src/postprocessing`, `src/preprocessing` and `src/write` are replaced by the
`*_ph.cpp` placeholders LibRaw ships for exactly this purpose. So the **DCB
demosaic and FBDD denoise code** that LibRaw's `COPYRIGHT` attributes to Jacek
Gozdz (a BSD-like 3-clause licence of its own) is **not compiled and not
distributed** by this project at all. The X3F unpacker (Roland Karlsson,
BSD-style) and the pieces taken from the Adobe DNG SDK 1.4 (MIT) ARE compiled;
both are attributed by the `COPYRIGHT` reproduced below, which is LibRaw's own
notice and is quoted in full for that reason.

**OpenEXR and Imath are in every build.** There used to be a
`-DVIEWER_WITH_EXR=OFF` that linked neither, so the `yes` column above had to be
qualified; that option was removed by #53's ruling (2026-08-09) and the two rows
are now as unconditional as the five above them. Nobody has to check how a
binary was configured before deciding whether these notices apply to it - they
always do. OpenEXR 3.4 vendors libdeflate and OpenJPH inside its own tree and
both are covered by the notice OpenEXR ships, quoted below with it.

The other side of that is a **build** property rather than a licensing one, and
it is recorded in `CMakeLists.txt` beside the fetch: with no OFF path, a clean
clone with no network cannot be built unless the machine already provides
OpenEXR and Imath (`find_package` is tried first, and
`FETCHCONTENT_SOURCE_DIR_IMATH` / `FETCHCONTENT_SOURCE_DIR_OPENEXR` point the
build at local source trees).

## Not bundled, and why it is worth saying

- **Operating-system libraries** - `opengl32`, `user32`, `gdi32`, `comdlg32`,
  `ole32`, `shell32`, `dwmapi`, `imm32` on Windows; X11 and `libdl`/`pthread` on
  Linux; the system frameworks on macOS. Dynamically linked, part of the
  platform, no redistribution.
- **The UI font.** The viewer picks a CJK face off the machine at runtime
  (`meiryo.ttc`, `NotoSansCJK`, Hiragino, ...) and falls back to Dear ImGui's
  embedded ProggyClean when it finds none. **No font file is redistributed**;
  the fallback is part of Dear ImGui and is covered by the MIT text below.
- **libtiff.** TIFF is read, and libtiff is **not** linked: `core/tiffread.cpp`
  is this project's own reader, and the only outside code behind it is miniz,
  which is listed above and was already linked for `.npz`. So there is nothing
  to attribute for TIFF - not because the obligation was overlooked, but
  because there is no third party in that path at all. The reasoning for
  writing rather than fetching one is in `core/tiffread.h`.
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


## OpenEXR - BSD-3-Clause

https://github.com/AcademySoftwareFoundation/openexr - `openexr-src/LICENSE.md`.
The same text covers the copies of libdeflate and OpenJPH that OpenEXR 3.4
vendors under `external/` and that this build compiles (OPENEXR_FORCE_INTERNAL_DEFLATE).

```
Copyright (c) Contributors to the OpenEXR Project. All rights reserved.

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote products derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```


## LibRaw - CDDL-1.0 (chosen from LGPL-2.1 or CDDL-1.0)

https://www.libraw.org - `LibRaw-0.22.2/COPYRIGHT` and `LibRaw-0.22.2/LICENSE.CDDL`.
The author offers a choice of two; **this project uses it under the CDDL-1.0
option**, for the reason given above. LibRaw's own copyright notice, which also
attributes the third-party code inside it, comes first:

```
 ** LibRaw: Raw images processing library **

Copyright (C) 2008-2025 LibRaw LLC (http://www.libraw.org, info@libraw.org)
 
LibRaw is free software; you can redistribute it and/or modify
it under the terms of the one of two licenses as you choose:

1. GNU LESSER GENERAL PUBLIC LICENSE version 2.1
   (See file LICENSE.LGPL provided in LibRaw distribution archive for details).

2. COMMON DEVELOPMENT AND DISTRIBUTION LICENSE (CDDL) Version 1.0
   (See file LICENSE.CDDL provided in LibRaw distribution archive for details).

LibRaw uses code from dcraw.c -- Dave Coffin's raw photo decoder,
dcraw.c is copyright 1997-2018 by Dave Coffin, dcoffin a cybercom o net.
LibRaw do not use RESTRICTED code from dcraw.c

LibRaw uses DCB demosaic and FBDD denoise licensed under BSD-like 3-clause license
DCB and FBDD are Copyright (C) 2010,  Jacek Gozdz (cuniek@kft.umcs.lublin.pl)

LibRaw uses X3F library to unpack Foveon Files, licensed BSD-style license
Copyright (c) 2010, Roland Karlsson (roland@proxel.se)
All rights reserved.

LibRaw uses pieces of code from Adobe DNG SDK 1.4,
Copyright (c) 2005 Adobe Systems Incorporated, licensed under MIT license
```

```
COMMON DEVELOPMENT AND DISTRIBUTION LICENSE (CDDL) Version 1.0

1. Definitions.

1.1. Contributor means each individual or entity that creates or
contributes to the creation of Modifications.

1.2. Contributor Version means the combination of the Original
Software, prior Modifications used by a Contributor (if any),
and the Modifications made by that particular Contributor.

1.3. Covered Software means (a) the Original Software, or (b)
Modifications, or (c) the combination of files containing
Original Software with files containing Modifications, in each
case including portions thereof.

1.4. Executable means the Covered Software in any form other
than Source Code.

1.5. Initial Developer means the individual or entity that first
makes Original Software available under this License.

1.6. Larger Workmeans a work which combines Covered Software or
portions thereof with code not governed by the terms of this
License.

1.7. License means this document.

1.8. Licensable means having the right to grant, to the maximum
extent possible, whether at the time of the initial grant or 
subsequently acquired, any and all of the rights conveyed herein.

1.9. Modifications means the Source Code and Executable form of
any of the following: A. Any file that results from an addition
to, deletion from or modification of the contents of a file
containing Original Software or previous Modifications; B. Any
new file that contains any part of the Original Software or
previous Modification; or C. Any new file that is contributed or
otherwise made available under the terms of this License.

1.10. Original Software means the Source Code and Executable
form of computer software code that is originally released under
this License.

1.11. Patent Claims means any patent claim(s), now owned or
hereafter acquired, including without limitation, method,
process, and apparatus claims, in any patent Licensable by
grantor.

1.12. Source Code means (a) the common form of computer software
code in which modifications are made and (b) associated
documentation included in or with such code.

1.13. You (or Your) means an individual or a legal entity
exercising rights under, and complying with all of the terms of,
this License. For legal entities, You includes any entity which
controls, is controlled by, or is under common control with You.
For purposes of this definition, control means (a) the power,
direct or indirect, to cause the direction or management of such
entity, whether by contract or otherwise, or (b) ownership of
more than fifty percent (50%) of the outstanding shares or
beneficial ownership of such entity.

2. License Grants.

2.1. The Initial Developer Grant. Conditioned upon Your
compliance with Section 3.1 below and subject to third party
intellectual property claims, the Initial Developer hereby
grants You a world-wide, royalty-free, non-exclusive license:

(a) under intellectual property rights (other than patent or
trademark) Licensable by Initial Developer, to use, reproduce,
modify, display, perform, sublicense and distribute the Original
Software (or portions thereof), with or without Modifications,
and/or as part of a Larger Work; and

(b) under Patent Claims infringed by the making, using or
selling of Original Software, to make, have made, use, practice,
sell, and offer for sale, and/or otherwise dispose of the
Original Software (or portions thereof);

(c) The licenses granted in Sections 2.1(a) and (b) are
effective on the date Initial Developer first distributes or
otherwise makes the Original Software available to a third party
under the terms of this License;

(d) Notwithstanding Section 2.1(b) above, no patent license is
granted: (1) for code that You delete from the Original
Software, or (2) for infringements caused by: (i) the
modification of the Original Software, or (ii) the combination
of the Original Software with other software or devices.

2.2. Contributor Grant. Conditioned upon Your compliance with
Section 3.1 below and subject to third party intellectual
property claims, each Contributor hereby grants You a
world-wide, royalty-free, non-exclusive license:

(a) under intellectual property rights (other than patent or
trademark) Licensable by Contributor to use, reproduce, modify,
display, perform, sublicense and distribute the Modifications
created by such Contributor (or portions thereof), either on an
unmodified basis, with other Modifications, as Covered Software
and/or as part of a Larger Work; and

(b) under Patent Claims infringed by the making, using, or
selling of Modifications made by that Contributor either alone
and/or in combination with its Contributor Version (or portions
of such combination), to make, use, sell, offer for sale, have
made, and/or otherwise dispose of: (1) Modifications made by
that Contributor (or portions thereof); and (2) the combination
of Modifications made by that Contributor with its Contributor
Version (or portions of such combination).

(c) The licenses granted in Sections 2.2(a) and 2.2(b)
areeffective on the date Contributor first distributes or
otherwise makes the Modifications available to a third party.

(d) Notwithstanding Section 2.2(b) above, no patent license is
granted: (1) for any code that Contributor has deleted from the
Contributor Version; (2) for infringements caused by: (i) third
party modifications of Contributor Version, or (ii) the
combination of Modifications made by that Contributor with other
software (except as part of the Contributor Version) or other
devices; or (3) under Patent Claims infringed by Covered
Software in the absence of Modifications made by that
Contributor.

3. Distribution Obligations.

3.1. Availability of Source Code. Any Covered Software that You
distribute or otherwise make available in Executable form must
also be made available in Source Code form and that Source Code
form must be distributed only under the terms of this License.
You must include a copy of this License with every copy of the
Source Code form of the Covered Software You distribute or
otherwise make available. You must inform recipients of any such
Covered Software in Executable form as to how they can obtain
such Covered Software in Source Code form in a reasonable manner
on or through a medium customarily used for software exchange.

3.2. Modifications. The Modifications that You create or to
which You contribute are governed by the terms of this License.
You represent that You believe Your Modifications are Your
original creation(s) and/or You have sufficient rights to grant
the rights conveyed by this License.

3.3. Required Notices. You must include a notice in each of Your
Modifications that identifies You as the Contributor of the
Modification. You may not remove or alter any copyright, patent
or trademark notices contained within the Covered Software, or
any notices of licensing or any descriptive text giving
attribution to any Contributor or the Initial Developer.

3.4. Application of Additional Terms. You may not offer or
impose any terms on any Covered Software in Source Code form
that alters or restricts the applicable version of this License
or the recipients rights hereunder. You may choose to offer, and
to charge a fee for, warranty, support, indemnity or liability
obligations to one or more recipients of Covered
Software. However, you may do so only on Your own behalf, and
not on behalf of the Initial Developer or any Contributor. You
must make it absolutely clear that any such warranty, support,
indemnity or liability obligation is offered by You alone, and
You hereby agree to indemnify the Initial Developer and every
Contributor for any liability incurred by the Initial Developer
or such Contributor as a result of warranty, support, indemnity
or liability terms You offer.

3.5. Distribution of Executable Versions. You may distribute the
Executable form of the Covered Software under the terms of this
License or under the terms of a license of Your choice, which
may contain terms different from this License, provided that You
are in compliance with the terms of this License and that the
license for the Executable form does not attempt to limit or
alter the recipients rights in the Source Code form from the
rights set forth in this License. If You distribute the Covered
Software in Executable form under a different license, You must
make it absolutely clear that any terms which differ from this
License are offered by You alone, not by the Initial Developer
or Contributor. You hereby agree to indemnify the Initial
Developer and every Contributor for any liability incurred by
the Initial Developer or such Contributor as a result of any
such terms You offer.

3.6. Larger Works. You may create a Larger Work by combining
Covered Software with other code not governed by the terms of
this License and distribute the Larger Work as a single product.
In such a case, You must make sure the requirements of this
License are fulfilled for the Covered Software.

4. Versions of the License.

4.1. New Versions. Sun Microsystems, Inc. is the initial license
steward and may publish revised and/or new versions of this
License from time to time. Each version will be given a
distinguishing version number. Except as provided in Section
4.3, no one other than the license steward has the right to
modify this License.

4.2. Effect of New Versions. You may always continue to use,
distribute or otherwise make the Covered Software available
under the terms of the version of the License under which You
originally received the Covered Software. If the Initial
Developer includes a notice in the Original Software prohibiting
it from being distributed or otherwise made available under any
subsequent version of the License, You must distribute and make
the Covered Software available under the terms of the version of
the License under which You originally received the Covered
Software.  Otherwise, You may also choose to use, distribute or
otherwise make the Covered Software available under the terms of
any subsequent version of the License published by the license
steward.

4.3. Modified Versions. When You are an Initial Developer and
You want to create a new license for Your Original Software, You
may create and use a modified version of this License if You:
(a) rename the license and remove any references to the name of
the license steward (except to note that the license differs
from this License); and (b) otherwise make it clear that the
license contains terms which differ from this License.

5. DISCLAIMER OF WARRANTY. COVERED SOFTWARE IS PROVIDED UNDER
THIS LICENSE ON AN AS IS BASIS, WITHOUT WARRANTY OF ANY KIND,
EITHER EXPRESSED OR IMPLIED, INCLUDING, WITHOUT LIMITATION,
WARRANTIES THAT THE COVERED SOFTWARE IS FREE OF DEFECTS,
MERCHANTABLE, FIT FOR A PARTICULAR PURPOSE OR NON-INFRINGING.
THE ENTIRE RISK AS TO THE QUALITY AND PERFORMANCE OF THE COVERED
SOFTWARE IS WITH YOU. SHOULD ANY COVERED SOFTWARE PROVE
DEFECTIVE IN ANY RESPECT, YOU (NOT THE INITIAL DEVELOPER OR ANY
OTHER CONTRIBUTOR) ASSUME THE COST OF ANY NECESSARY SERVICING,
REPAIR OR CORRECTION. THIS DISCLAIMER OF WARRANTY CONSTITUTES AN
ESSENTIAL PART OF THIS LICENSE. NO USE OF ANY COVERED SOFTWARE
IS AUTHORIZED HEREUNDER EXCEPT UNDER THIS DISCLAIMER.

6. TERMINATION.

6.1. This License and the rights granted hereunder will
terminate automatically if You fail to comply with terms herein
and fail to cure such breach within 30 days of becoming aware of
the breach. Provisions which, by their nature, must remain in
effect beyond the termination of this License shall survive.

6.2. If You assert a patent infringement claim (excluding
declaratory judgment actions) against Initial Developer or a
Contributor (the Initial Developer or Contributor against whom
You assert such claim is referred to as Participant) alleging
that the Participant Software (meaning the Contributor Version
where the Participant is a Contributor or the Original Software
where the Participant is the Initial Developer) directly or
indirectly infringes any patent, then any and all rights granted
directly or indirectly to You by such Participant, the Initial
Developer (if the Initial Developer is not the Participant) and
all Contributors under Sections 2.1 and/or 2.2 of this License
shall, upon 60 days notice from Participant terminate
prospectively and automatically at the expiration of such 60 day
notice period, unless if within such 60 day period You withdraw
Your claim with respect to the Participant Software against such
Participant either unilaterally or pursuant to a written
agreement with Participant.

6.3. In the event of termination under Sections 6.1 or 6.2
above, all end user licenses that have been validly granted by
You or any distributor hereunder prior to termination (excluding
licenses granted to You by any distributor) shall survive
termination.

7. LIMITATION OF LIABILITY. UNDER NO CIRCUMSTANCES AND UNDER NO
LEGAL THEORY, WHETHER TORT (INCLUDING NEGLIGENCE), CONTRACT, OR
OTHERWISE, SHALL YOU, THE INITIAL DEVELOPER, ANY OTHER
CONTRIBUTOR, OR ANY DISTRIBUTOR OF COVERED SOFTWARE, OR ANY
SUPPLIER OF ANY OF SUCH PARTIES, BE LIABLE TO ANY PERSON FOR ANY
INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES OF ANY
CHARACTER INCLUDING, WITHOUT LIMITATION, DAMAGES FOR LOST
PROFITS, LOSS OF GOODWILL, WORK STOPPAGE, COMPUTER FAILURE OR
MALFUNCTION, OR ANY AND ALL OTHER COMMERCIAL DAMAGES OR LOSSES,
EVEN IF SUCH PARTY SHALL HAVE BEEN INFORMED OF THE POSSIBILITY
OF SUCH DAMAGES. THIS LIMITATION OF LIABILITY SHALL NOT APPLY TO
LIABILITY FOR DEATH OR PERSONAL INJURY RESULTING FROM SUCH
PARTYS NEGLIGENCE TO THE EXTENT APPLICABLE LAW PROHIBITS SUCH
LIMITATION. SOME JURISDICTIONS DO NOT ALLOW THE EXCLUSION OR
LIMITATION OF INCIDENTAL OR CONSEQUENTIAL DAMAGES, SO THIS
EXCLUSION AND LIMITATION MAY NOT APPLY TO YOU.

8. U.S. GOVERNMENT END USERS. The Covered Software is a
commercial item, as that term is defined in 48 C.F.R. 2.101
(Oct. 1995), consisting of commercial computer software (as that
term is defined at 48 C.F.R. 252.227-7014(a)(1)) and commercial
computer software documentation as such terms are used in 48
C.F.R. 12.212 (Sept. 1995). Consistent with 48 C.F.R. 12.212 and
48 C.F.R. 227.7202-1 through 227.7202-4 (June 1995), all
U.S. Government End Users acquire Covered Software with only
those rights set forth herein. This U.S. Government Rights
clause is in lieu of, and supersedes, any other FAR, DFAR, or
other clause or provision that addresses Government rights in
computer software under this License.

9. MISCELLANEOUS. This License represents the complete agreement
concerning subject matter hereof. If any provision of this
License is held to be unenforceable, such provision shall be
reformed only to the extent necessary to make it enforceable.
This License shall be governed by the law of the jurisdiction
specified in a notice contained within the Original Software
(except to the extent applicable law, if any, provides
otherwise), excluding such jurisdictions conflict-of-law
provisions. Any litigation relating to this License shall be
subject to the jurisdiction of the courts located in the
jurisdiction and venue specified in a notice contained within
the Original Software, with the losing party responsible for
costs, including, without limitation, court costs and reasonable
attorneys fees and expenses. The application of the United
Nations Convention on Contracts for the International Sale of
Goods is expressly excluded. Any law or regulation which
provides that the language of a contract shall be construed
against the drafter shall not apply to this License. You agree
that You alone are responsible for compliance with the United
States export administration regulations (and the export control
laws and regulation of any other countries) when You use,
distribute or otherwise make available any Covered Software.

10. RESPONSIBILITY FOR CLAIMS. As between Initial Developer and
the Contributors, each party is responsible for claims and
damages arising, directly or indirectly, out of its utilization
of rights under this License and You agree to work with Initial
Developer and Contributors to distribute such responsibility on
an equitable basis. Nothing herein is intended or shall be
deemed to constitute any admission of liability.

----------------------------------------------------------------

NOTICE PURSUANT TO SECTION 9 OF THE COMMON DEVELOPMENT AND
DISTRIBUTION LICENSE (CDDL): This code is released under the
CDDL and shall be governed by the laws of the State of
California (excluding conflict-of-law provisions). Any
litigation relating to this License shall be subject to the
jurisdiction of the Federal Courts of the Northern District of
California and the state courts of the State of California, with
venue lying in Santa Clara County, California.

----------------------------------------------------------------
```


## Imath - BSD-3-Clause

https://github.com/AcademySoftwareFoundation/Imath - `imath-src/LICENSE.md`.
Declared by this project's own CMakeLists.txt rather than fetched by OpenEXR,
so that the pin is a URL like every other dependency here and no git is needed.

```
Copyright Contributors to the OpenEXR Project. All rights reserved.

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote products derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```
