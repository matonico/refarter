# Combining fixtures

Synthetic one-second clips generated locally with FFmpeg's `color` and `sine`
sources, not personal recordings. Red/blue are 16 × 16 H.264 Baseline (2 fps,
no B-frames, keyframe interval 2), stereo AAC 48 kHz / 64 kb/s, handler `mix`.
The wider red clip is 32 × 16. The colours and tones differ so the tests can
check packet order and unchanged payloads. The encoder was libx264 with
`-preset ultrafast -tune zerolatency -profile:v baseline -pix_fmt yuv420p -g 2 -bf 0`.
These files contain no third-party footage or music.

Derived fixtures exercise 44.1 kHz audio, no audio, two audio sources with/without
a named mix, display rotation, and AV1 (libaom-av1, CPU-used 8, one thread). These are file-only
checks: unit tests never activate a hardware encoder or capture device.
