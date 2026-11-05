# ffmpeg-nvvfx

A Windows-focused FFmpeg fork adding the `nvvfx` filter for **NVIDIA Video
Effects** on D3D11 hardware frames.

Based on FFmpeg master (`2546b6f061`). Upstream project: <https://ffmpeg.org>.

## What it does

`nvvfx` runs NVIDIA's VideoSuperRes and ArtifactReduction models on decoded
D3D11 frames:

| `mode` | Effect | Output size |
|---|---|---|
| `superres` | VideoSuperRes upscale | input × `scale` |
| `denoise` | VideoSuperRes denoise | same as input |
| `deblur` | VideoSuperRes deblur | same as input |
| `artifact` | ArtifactReduction | same as input, max 1920x1080 |

Quality: `low`, `medium`, `high`, `ultra` for superres/denoise/deblur;
`conservative`, `aggressive` for artifact.

Input formats: NV12, P010, BGRA, RGBA. NV12/P010 is converted to BGRA
internally with a D3D11 video processor. For zero-copy encoding, follow the
filter with `scale_d3d11=format=nv12` (8-bit) or `format=p010` (10-bit).

## Changes from upstream

- New `libavfilter/vf_nvvfx.c`, registered as `nvvfx`.
- `libavfilter/vf_scale_d3d11.c`: fixed NV12/P010 output texture allocation and
  added explicit video-processor color handling (full-range RGB input,
  limited BT.709/BT.601 output) so colors match the source.
- Documentation and Changelog entries.

No NVIDIA SDK is linked. The filter dynamically loads the user's installed
NVIDIA Video Effects runtime; no NVIDIA DLLs or models are bundled.

## Requirements

- Windows 10/11 with an NVIDIA RTX GPU.
- NVIDIA Video Effects runtime installed by the user:
  - split runtime for `superres`, `denoise`, and `deblur`;
  - legacy `C:\Program Files\NVIDIA Corporation\NVIDIA Video Effects` runtime
    for `artifact` (its `models` directory is used).
- An FFmpeg build with `d3d11va`. For NVENC/NVDEC, install `nv-codec-headers`
  and `pkg-config` before configuring.

## Build (MinGW-w64)

```bash
./configure --arch=x86_64 --target-os=mingw32 --disable-x86asm \
    --enable-d3d11va --enable-ffnvcodec --enable-nvenc --enable-nvdec
make -j
```

## Runtime path

Point the filter at the split runtime directory:

```bash
export NVVFX_LIB_DIR='C:\path\to\nvvfx\libs'
```

Alternatively pass `lib_dir=` in the filter. For `artifact`, use `dep_dir=` if
the legacy runtime is not in the default Program Files location.

## Examples

8-bit, zero-copy to AV1 NVENC:

```bash
ffmpeg -hwaccel d3d11va -hwaccel_output_format d3d11 -i in.mkv \
  -vf "nvvfx=mode=superres:quality=ultra:scale=2,scale_d3d11=format=nv12" \
  -c:v av1_nvenc -preset p5 -tune hq -rc vbr -b:v 6000k -c:a copy out.mkv
```

10-bit AV1:

```bash
-vf "nvvfx=mode=superres:quality=ultra:scale=2,scale_d3d11=format=p010"
```

Denoise, deblur, artifact:

```bash
-vf "nvvfx=mode=denoise:quality=high,scale_d3d11=format=nv12"
-vf "nvvfx=mode=deblur:quality=high,scale_d3d11=format=nv12"
-vf "nvvfx=mode=artifact:quality=conservative,scale_d3d11=format=nv12"
```

Chain artifact removal and superres (artifact first):

```bash
-vf "nvvfx=mode=artifact:quality=conservative,nvvfx=mode=superres:quality=ultra:scale=2,scale_d3d11=format=nv12"
```

NVDEC input (not zero-copy; requires upload to D3D11):

```bash
ffmpeg -init_hw_device d3d11va=dev -filter_hw_device dev -hwaccel cuda -i in.mkv \
  -vf "format=nv12,hwupload,nvvfx=mode=denoise:quality=high,hwdownload,format=bgra,format=yuv420p" \
  -c:v av1_nvenc -preset p5 out.mkv
```

## Notes

- `nvvfx` processes 8-bit BGRA. A P010 output is a real 10-bit encode, but it
  is upconverted from 8-bit and cannot restore 10-bit detail.
- `artifact` supports at most 1920x1080.
- Do not run `artifact` and `denoise` in the same process concurrently.
- D3D11VA (`-hwaccel d3d11va -hwaccel_output_format d3d11`) is the zero-copy
  decode path. `-hwaccel cuda` (NVDEC) requires a download/upload round trip.

## License

FFmpeg is licensed under the LGPL/GPL; see `LICENSE`. The new filter is
LGPL-2.1-or-later.
