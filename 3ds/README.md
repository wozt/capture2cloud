# Capture2Cloud Old 3DS client

This native client targets the original Nintendo 3DS, original 3DS XL,
and original 2DS. It does not require New 3DS CPU features.

The server exposes a dedicated 400x240@30 stream on TCP port `5085`.
The client can switch at runtime between independent baseline JPEG frames
and low-delay MPEG-4 Part 2. This path is separate from every VP8, H.264,
Wii U console, and Wii U GamePad encoder. MPEG-4 frames are consumed in
prediction order; after a loss, both ends wait for a fresh keyframe instead
of displaying corrupted P-frames.

Audio uses 48 kHz stereo low-delay Opus over UDP port `5084`. Four 5 ms
network packets are coalesced into each NDSP wave buffer to avoid underruns
at packet boundaries without increasing network packet latency. Network and
audio workers use CPU1 while video decoding stays on CPU0; MPEG-4 colour
conversion uses the 3DS Y2R hardware.

## Build

Install devkitARM, libctru, 3ds-tools, 3ds-libjpeg-turbo, and 3ds-libopus,
then run:

```sh
export DEVKITPRO=/opt/devkitpro
cd 3ds
make
```

The outputs are `capture2cloud-old3ds.3dsx` and
`capture2cloud-old3ds.cia`.
`make cia` remains available as an explicit packaging alias.

The CIA rule uses the open-source `makerom` binary in `build-tools/`.
It automatically runs `scripts/build-3ds-tools.sh` if the tool is absent.
The script builds a pinned Project_CTR revision from source; no proprietary
SDK or packaging tool is used.

## Runtime configuration

Configuration is stored at
`sdmc:/3ds/capture2cloud-old3ds/config.ini`. The stream port defaults to
5085 and the web login port defaults to 5080. The password is exchanged
for the same session token used by existing Capture2Cloud clients.

The lower screen switches between CONFIG and CONTROLLER modes. The
controller screen provides ZL, ZR, L3, R3, HOME, capture, minus, plus,
and a touch right stick. The STAT button shows measured receive, decode,
display, bitrate, queue, drop, upload, audio, and local latency values.
