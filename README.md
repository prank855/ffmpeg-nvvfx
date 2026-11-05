# ffmpeg-nvvfx

Windows FFmpeg with NVIDIA AI video effects: **upscale, denoise, deblur, and
remove compression artifacts** on D3D11 frames.

Adds the `nvvfx` filter:

| `mode` | What it does | Output |
|---|---|---|
| `superres` | AI upscale 2x / 3x / 4x | larger |
| `denoise` | Remove noise | same size |
| `deblur` | Remove blur | same size |
| `artifact` | Remove compression artifacts | same size (max 1080p) |

## Setup (once)

**1. Install the NVIDIA models.** Use the NVIDIA Broadcast Video Effects
redistributable 0.7.6 ([download](https://www.nvidia.com/en-us/geforce/broadcasting/broadcast-sdk/resources/)). It installs:

```text
C:\Program Files\NVIDIA Corporation\NVIDIA Video Effects\models
```

**2. Install NVIDIA's runtime wheel** (needed for superres / denoise / deblur):

```bash
python -m pip install nvidia-vfx==0.2.0.0
```

`pip` fetches the real wheel from `pypi.nvidia.com`.

**3. Point the filter at the wheel's DLLs:**

```bash
export NVVFX_LIB_DIR='C:\path\to\venv\Lib\site-packages\nvvfx\libs'
```

Or make it automatic with a junction beside `ffmpeg.exe`:

```powershell
New-Item -ItemType Junction -Path 'C:\path\to\ffmpeg-nvvfx-win64\nvvfx\libs' -Target 'C:\path\to\venv\Lib\site-packages\nvvfx\libs'
```

Done. No NVIDIA DLLs or models are bundled with this fork.

## Use it

**Most common — 8-bit, zero-copy to AV1 NVENC:**

```bash
ffmpeg -hwaccel d3d11va -hwaccel_output_format d3d11 -i in.mkv \
  -vf "nvvfx=mode=superres:quality=ultra:scale=2,scale_d3d11=format=nv12" \
  -c:v av1_nvenc -preset p5 -tune hq -rc vbr -b:v 6000k -c:a copy out.mkv
```

**10-bit:** change `format=nv12` to `format=p010`.

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

## Requirements

- Windows 10/11 + NVIDIA RTX GPU
- The Program Files model/dependency set (step 1)
- The `nvidia-vfx` wheel for superres / denoise / deblur (step 2)
- `artifact` does **not** need the wheel

## Notes

- `artifact` is limited to 1920x1080.
- Don't run `artifact` and `denoise` in the same process.
- The VFX pipeline is 8-bit. P010 output is a real 10-bit encode, but it is
  upconverted from 8-bit.
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

- New `libavfilter/vf_nvvfx.c` (`nvvfx` filter).
- `vf_scale_d3d11.c`: fixed NV12/P010 texture allocation and color-space
  handling so GPU conversions match the source colors.
- Docs and Changelog.

</details>

## License

FFmpeg is LGPL/GPL; see `LICENSE`. The new filter is LGPL-2.1-or-later.
NVIDIA runtime DLLs and models are proprietary and are not included.
