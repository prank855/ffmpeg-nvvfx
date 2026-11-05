# ffmpeg-nvvfx

**Command builder:** <https://prank855.github.io/ffmpeg-nvvfx/>

Windows FFmpeg with NVIDIA AI video effects: **upscale, denoise, deblur,
remove compression artifacts, true 10-bit, SDR→HDR10, and frame generation**
on D3D11 frames.

`nvvfx` modes:

| `mode` | What it does | Output |
|---|---|---|
| `superres` | AI upscale 2x / 3x / 4x | larger |
| `denoise` | Remove noise | same size |
| `deblur` | Remove blur | same size |
| `artifact` | Remove compression artifacts | same size (max 1080p) |
| `hdr` | SDR → HDR10 | same size, RGB10A2 |

Plus the two-input `nvvfx_framegen` filter for frame interpolation.

The release binaries are `ffmpeg-nvvfx.exe` and `ffprobe-nvvfx.exe`. A source
build still produces `ffmpeg.exe`/`ffprobe.exe`; copy or rename them if you
want the fork name.

## Setup (once)

**1. Install the NVIDIA models.** Use the NVIDIA Broadcast Video Effects
redistributable 0.7.6 ([download](https://www.nvidia.com/en-us/geforce/broadcasting/broadcast-sdk/resources/)). It installs:

```text
C:\Program Files\NVIDIA Corporation\NVIDIA Video Effects\models
```

**2. Install NVIDIA's runtime wheel** (needed for superres / denoise / deblur / hdr):

```bash
python -m pip install nvidia-vfx==0.2.0.0
```

`pip` fetches the real wheel from `pypi.nvidia.com`.

**3. Point the filter at the wheel's DLLs:**

```bash
export NVVFX_LIB_DIR='C:\path\to\venv\Lib\site-packages\nvvfx\libs'
```

Or make it automatic with a junction beside `ffmpeg-nvvfx.exe`:

```powershell
New-Item -ItemType Junction -Path 'C:\path\to\ffmpeg-nvvfx-win64\nvvfx\libs' -Target 'C:\path\to\venv\Lib\site-packages\nvvfx\libs'
```

Done. No NVIDIA DLLs or models are bundled with this fork.

## Use it

**Most common — 8-bit, zero-copy to AV1 NVENC:**

```bash
ffmpeg-nvvfx -hwaccel d3d11va -hwaccel_output_format d3d11 -i in.mkv \
  -vf "nvvfx=mode=superres:quality=ultra:scale=2,scale_d3d11=format=nv12" \
  -c:v av1_nvenc -preset p5 -tune hq -rc vbr -b:v 6000k -c:a copy out.mkv
```

**True 10-bit (P010/X2BGR10 input → P010 output, no extra conversion):**

```bash
ffmpeg-nvvfx -hwaccel d3d11va -hwaccel_output_format d3d11 -i in.mkv \
  -vf "nvvfx=mode=superres:quality=ultra:scale=2:bit_depth=10" \
  -c:v av1_nvenc -preset p5 -tune hq -rc vbr -b:v 6000k -c:a copy out.mkv
```

**8-bit filter, 10-bit encode:** append `scale_d3d11=format=p010` instead.
This upconverts; use `bit_depth=10` above for a true 10-bit pipeline.

**SDR → HDR10:**

```bash
ffmpeg-nvvfx -hwaccel d3d11va -hwaccel_output_format d3d11 -i in.mkv \
  -vf "nvvfx=mode=hdr,hwdownload,format=x2bgr10le,scale=in_range=full:out_color_matrix=bt2020nc:out_range=tv,format=p010le" \
  -c:v hevc_nvenc -profile:v main10 -pix_fmt p010le \
  -color_range tv -colorspace bt2020nc -color_trc smpte2084 -color_primaries bt2020 out.mkv
```

**Frame generation** (`nvvfx_framegen`, two independent inputs):

```bash
ffmpeg-nvvfx -init_hw_device d3d11va=dev -filter_hw_device dev \
  -f lavfi -i testsrc2=size=1280x720:rate=30:duration=5 \
  -f lavfi -i testsrc2=size=1280x720:rate=30:duration=5 \
  -filter_complex "[0:v]setpts=PTS+1,format=nv12,hwupload[a];[1:v]format=nv12,hwupload[b];[a][b]nvvfx_framegen=method=time:timestep=0.5" \
  -f null -
```

Use `method=index:frame_multiplier=4:frame_index=2` for index mode.
`nvvfx_framegen` outputs BGRA; append `,scale_d3d11=format=nv12` for NVENC.

**Other effects:**

```text
nvvfx=mode=denoise:quality=high
nvvfx=mode=deblur:quality=high
nvvfx=mode=artifact:quality=conservative
```

**Artifact removal + upscale (artifact first):**

```text
nvvfx=mode=artifact:quality=conservative,nvvfx=mode=superres:quality=ultra:scale=2
```

**Quality:** `low`, `medium`, `high`, `ultra`.
For `artifact`: `conservative` or `aggressive`.
**Strength:** `strength=0.0..1.0` for VideoSuperRes (default 1.0).

## Requirements

- Windows 10/11 + NVIDIA RTX GPU
- The Program Files model/dependency set (step 1)
- The `nvidia-vfx` wheel for superres / denoise / deblur / hdr (step 2)
- `artifact` does **not** need the wheel

## Notes

- Default VideoSuperRes is 8-bit. `bit_depth=10` runs true RGB10A2 and outputs
  P010; it requires P010 or X2BGR10 input. Use it for SDR; use `mode=hdr` for
  HDR sources.
- `mode=hdr` outputs RGB10A2 PQ/BT.2020 (`x2bgr10le`). Always use the `scale`
  chain above to make P010 HDR10; the D3D11 video processor does not do the
  PQ/BT.2020 conversion correctly on current drivers.
- `nvvfx_framegen` is RGB8-only for now.
- `artifact` is limited to 1920x1080.
- Don't run `artifact` and `denoise` in the same process.
- `-hwaccel d3d11va -hwaccel_output_format d3d11` is the zero-copy decode path.
  `-hwaccel cuda` (NVDEC) needs a download/upload round trip.
- Tested with `nvidia-vfx==0.2.0.0` and the Program Files models on an RTX 5080.

<details>
<summary><b>Build from source</b></summary>

```bash
./configure --arch=x86_64 --target-os=mingw32 --disable-x86asm \
    --enable-d3d11va --enable-ffnvcodec --enable-nvenc --enable-nvdec
make -j
```

</details>

<details>
<summary><b>What changed from upstream</b></summary>

- New `libavfilter/vf_nvvfx.c` (`nvvfx` filter) and
  `libavfilter/vf_nvvfx_framegen.c` (`nvvfx_framegen`).
- True 10-bit VideoSuperRes, TrueHDR, and frame generation.
- `vf_scale_d3d11.c`: fixed NV12/P010 texture allocation and color-space
  handling so GPU conversions match the source colors.
- Docs and Changelog.

</details>

## License

FFmpeg is LGPL/GPL; see `LICENSE`. The new filter is LGPL-2.1-or-later.
NVIDIA runtime DLLs and models are proprietary and are not included.
