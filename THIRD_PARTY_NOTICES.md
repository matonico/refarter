# Third-party notices

Refarter includes the following third-party software. Ship this file with any binary distribution.

The Windows installer (`Refarter-<version>-setup.exe`) is built with Inno Setup (Copyright (C) 1997-2026 Jordan Russell, Portions Copyright (C) 2000-2026 Martijn Laan; https://jrsoftware.org/isinfo.php), whose license permits distributing installers made with it. It is not part of Refarter itself.

## RNNoise 0.2

Noise suppression for the microphone track. Source: https://github.com/xiph/rnnoise (release v0.2, downloaded and verified by SHA-256 at build time; see `CMakeLists.txt`). License: BSD 3-Clause.

```
Copyright (c) 2007-2017, 2024 Jean-Marc Valin
Copyright (c) 2023 Amazon
Copyright (c) 2017, Mozilla
Copyright (c) 2005-2017, Xiph.Org Foundation
Copyright (c) 2003-2004, Mark Borgerding

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

- Redistributions of source code must retain the above copyright
notice, this list of conditions and the following disclaimer.

- Redistributions in binary form must reproduce the above copyright
notice, this list of conditions and the following disclaimer in the
documentation and/or other materials provided with the distribution.

- Neither the name of the Xiph.Org Foundation nor the names of its
contributors may be used to endorse or promote products derived from
this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION
OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

## FFmpeg 8.1.3

Reading, decoding and trimming clips in the built-in player, and encoding a new AAC mix when a trim leaves audio sources out plus hardware H.264 size-limited copies with video resizing (libavformat, libavcodec, libavutil, libswresample, libswscale). Source: https://ffmpeg.org/releases/ffmpeg-8.1.3.tar.xz (downloaded and verified by SHA-256 at build time; see `scripts/build-ffmpeg.ps1`, which lists the exact configuration). Refarter modifies it with `scripts/ffmpeg-mf-encoder-index.patch`, which lets the hardware H.264 encoder start from a later Media Foundation encoder. Built without `--enable-gpl` or `--enable-nonfree`, so these libraries are licensed under the GNU Lesser General Public License, version 2.1 or later; the full text ships as `FFMPEG-LICENSE.txt`. Each GitHub Release also attaches the unmodified FFmpeg and dav1d source archives used for it, plus that patch.

```
FFmpeg is free software; you can redistribute it and/or
modify it under the terms of the GNU Lesser General Public
License as published by the Free Software Foundation; either
version 2.1 of the License, or (at your option) any later version.

FFmpeg is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Lesser General Public License for more details.
```

## dav1d 1.5.4

AV1 decoding in software when the graphics card cannot decode AV1. Source: https://downloads.videolan.org/pub/videolan/dav1d/1.5.4/dav1d-1.5.4.tar.xz (verified by SHA-256 at build time). License: BSD 2-Clause.

```
Copyright © 2018-2025, VideoLAN and dav1d authors
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```
