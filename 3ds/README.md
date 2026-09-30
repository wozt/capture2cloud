# Capture2Cloud Old 3DS client

This native client targets the original Nintendo 3DS, original 3DS XL,
and original 2DS. It does not require New 3DS CPU features.

The server exposes a dedicated stream on TCP port `5085`. It scales the
capture to 400x240 and encodes independent baseline JPEG frames at 24 Hz.
This path is separate from every VP8, H.264, Wii U console, and Wii U
GamePad encoder. Since each image is independent, a stale frame can be
dropped without corrupting the next one.

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
