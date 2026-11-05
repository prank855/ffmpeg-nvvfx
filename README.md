# ffmpeg-nvvfx

**Command builder:** <https://prank855.github.io/ffmpeg-nvvfx/>

Windows x86_64 FFmpeg with NVIDIA AI video effects on D3D11 frames:
**upscale, denoise, deblur, compression-artifact removal, true 10-bit,
SDR→HDR10, frame-rate conversion, and frame generation.**

The release binaries are `ffmpeg-nvvfx.exe` and `ffprobe-nvvfx.exe`. A source
build still produces `ffmpeg.exe`/`ffprobe.exe`; copy or rename them if you
want the fork name. NVIDIA runtime DLLs and models are **not** bundled.

## Filter overview

| Filter | What it does | Input | Output |
|---|---|---|---|
| `nvvfx` | VideoSuperRes, VSR denoise, deblur, artifact reduction, or TrueHDR | D3D11 NV12/P010/BGRA/RGBA | BGRA/RGBA, P010, or X2BGR10 |
| `nvvfx_upscale` | Fast 8-bit upscale by 4/3, 3/2, 2x, 3x, or 4x | D3D11 NV12/P010/BGRA/RGBA | BGRA, same aspect |
| `nvvfx_denoise` | Dedicated TensorRT temporal video denoiser | D3D11 NV12/P010/BGRA/RGBA | BGRA, same size |
| `nvvfx_framerate` | Automatic 2x–8x frame-rate conversion | D3D11 NV12/P010/BGRA/RGBA | BGRA or P010 |
| `nvvfx_framegen` | One interpolated frame from a previous/current pair | Two D3D11 streams | BGRA or P010 |

`nvvfx` is the general VideoSuperRes filter. The other filters expose
separate SDK 1.3 effects that have their own formats, limits, and options.

## Setup (once)

### 1. Install an NVIDIA Video Effects runtime

For all filters, install the NVIDIA Video Effects **SDK 1.3** runtime and its
models. This is the setup used for `nvvfx_upscale`, `nvvfx_denoise`,
`nvvfx_framerate`, and `nvvfx_framegen`.

The runtime contains the DLLs this fork loads, including:

```text
NVVideoEffects.dll
nvngxruntime.dll
nvVFXVideoSuperRes.dll
nvVFXTrueHDR.dll
nvVFXUpscale.dll
nvVFXDenoising.dll
nvVFXVideoFrameGeneration.dll
```

Keep the Denoise models in a `models` directory next to the runtime libs:

```text
<libs>\models\Denoise-agg-100.engine.trtpkg
<libs>\models\Denoise-con-100.engine.trtpkg
```

`mode=artifact` is the exception. It uses the older
`ArtifactReduction`/Program Files runtime, normally:

```text
C:\Program Files\NVIDIA Corporation\NVIDIA Video Effects
```

The older `nvidia-vfx==0.2.0.0` wheel plus the 0.7.6 Program Files models
still covers the original VSR effects, but it does not provide the SDK 1.3
Denoise models needed by `nvvfx_denoise`.

### 2. Point the filters at the runtime

Use `NVVFX_LIB_DIR`:

```bash
export NVVFX_LIB_DIR='C:\path\to\libs'
```

Or create a junction beside `ffmpeg-nvvfx.exe` so it is found automatically:

```powershell
New-Item -ItemType Junction -Path 'C:\path\to\ffmpeg-nvvfx-win64\nvvfx\libs' -Target 'C:\path\to\libs'
```

The filter also accepts `lib_dir=` and `dep_dir=`. On a command line, escape
the colon in a Windows drive path (`lib_dir=C\:/path/to/libs`) or use the
environment variable instead.

### 3. Verify

```bash
ffmpeg-nvvfx -filters | findstr nvvfx
```

## `nvvfx`

```text
nvvfx=mode=superres:quality=ultra:scale=2:bit_depth=10
```

### Modes

| `mode` | Behavior | Output size | Formats and limits |
|---|---|---|---|
| `superres` | AI super resolution | `scale`× or explicit `out_w`/`out_h` | 8-bit BGRA/RGBA; `bit_depth=10` gives P010 |
| `denoise` | VideoSuperRes denoise path | Same as input | 8-bit BGRA/RGBA or 10-bit RGB10A2/P010 |
| `deblur` | VideoSuperRes deblur | Same as input | 8-bit BGRA/RGBA or 10-bit RGB10A2/P010 |
| `artifact` | Compression-artifact reduction | Same as input | 8-bit BGRA only; maximum 1920x1080; legacy runtime |
| `hdr` | TrueHDR SDR→HDR10 | Same as input | X2BGR10, BT.2020/PQ, tagged HDR10 |

`mode=denoise` is the VideoSuperRes denoise path (quality levels 8–11), **not**
the dedicated TensorRT filter. Use `nvvfx_denoise` for the separate
`Denoising` effect.

`mode=hdr` is TrueHDR. It is different from `hdr=on`, which enables the
HDR/PQ path for 10-bit `mode=superres`.

### Options

| Option | Accepted values | Default | Applies to |
|---|---|---|---|
| `mode` | `superres`, `denoise`, `deblur`, `artifact`, `hdr` | `superres` | All |
| `quality` | `low`, `medium`, `high`, `ultra` | `high` | VideoSuperRes modes; ignored by `hdr` |
| `quality` for artifact | `conservative` (alias `low`), `aggressive` (alias `medium`) | `high` → aggressive | `artifact` |
| `scale` | `2`, `3`, `4` | `2` | `superres` only |
| `out_w` | `0` to `16384`; `0` = input width × `scale` | `0` | `superres` only |
| `out_h` | `0` to `16384`; `0` = input height × `scale` | `0` | `superres` only |
| `strength` | `0.0`–`1.0` | `1.0` | `superres`, `denoise`, `deblur` |
| `bit_depth` | `8`, `10` | `8` | VideoSuperRes modes; ignored by `artifact` and `hdr` |
| `hdr` | `auto`, `on`, `off` | `auto` | 10-bit `superres` only |
| `contrast` | `0`–`200` | `100` | `mode=hdr` |
| `saturation` | `0`–`200` | `100` | `mode=hdr` |
| `middle_gray` | `10`–`100` | `50` | `mode=hdr` |
| `luminance` | `400`–`2000` nits | `650` | `mode=hdr` |
| `debanding_off` | `0`, `1` | `0` | `mode=hdr` |
| `lib_dir` | directory string | auto | All |
| `dep_dir` | directory string | auto | All |

`out_w` and `out_h` are independent. If only one is set, the other uses the
input dimension multiplied by `scale`. VideoSuperRes only upscales; either
dimension smaller than the input is rejected. Values above 16384 or beyond
the D3D11 limit are rejected.

`quality=low` is fastest, `high` is the default balance, and `ultra` is the
best VSR quality. For `artifact`, prefer `conservative` when you want to
preserve detail and `aggressive` for heavy blocking.

`strength` is continuous for VideoSuperRes. Lower values preserve more
detail and apply less correction; `1.0` applies the model's full strength.
The dedicated `nvvfx_denoise` filter uses a different, quantized strength
model.

### Format rules

- 8-bit VSR output is BGRA/RGBA. Append `scale_d3d11=format=nv12` for NVENC.
- `bit_depth=10` requires P010 or X2BGR10 input and outputs P010.
- To process an 8-bit source in a 10-bit VSR pipeline, add
  `scale_d3d11=format=p010` before `nvvfx`.
- `mode=artifact` and `mode=hdr` ignore `bit_depth` and `scale`.
- Odd-dimension 10-bit output uses X2BGR10 instead of P010. Convert it with
  `hwdownload,format=x2bgr10le` and an explicit `scale`/`format=p010le` step
  before encoding.

### Examples

**8-bit VSR to NVENC:**

```bash
ffmpeg-nvvfx -hwaccel d3d11va -hwaccel_output_format d3d11 -i in.mkv \
  -vf "nvvfx=mode=superres:quality=ultra:scale=2,scale_d3d11=format=nv12" \
  -c:v av1_nvenc -preset p5 -tune hq -rc vbr -b:v 6000k -c:a copy out.mkv
```

**True 10-bit VSR:**

```bash
ffmpeg-nvvfx -hwaccel d3d11va -hwaccel_output_format d3d11 -i in.mkv \
  -vf "nvvfx=mode=superres:quality=ultra:scale=2:bit_depth=10" \
  -c:v av1_nvenc -preset p5 -tune hq -rc vbr -b:v 6000k -c:a copy out.mkv
```

**8-bit source, 10-bit pipeline:**

```text
scale_d3d11=format=p010,nvvfx=mode=superres:quality=ultra:scale=2:bit_depth=10
```

**Arbitrary VSR output:**

```text
nvvfx=mode=superres:quality=ultra:scale=2:out_w=800:out_h=450
```

**HDR/PQ VSR:**

```text
nvvfx=mode=superres:quality=ultra:scale=2:bit_depth=10:hdr=on,hwdownload,format=x2bgr10le,scale=in_range=full:out_color_matrix=bt2020nc:out_range=tv,format=p010le
```

**SDR → HDR10 with TrueHDR:**

```bash
ffmpeg-nvvfx -hwaccel d3d11va -hwaccel_output_format d3d11 -i in.mkv \
  -vf "nvvfx=mode=hdr:contrast=110:saturation=105:luminance=800,hwdownload,format=x2bgr10le,scale=in_range=full:out_color_matrix=bt2020nc:out_range=tv,format=p010le" \
  -c:v hevc_nvenc -profile:v main10 -pix_fmt p010le \
  -color_range tv -colorspace bt2020nc -color_trc smpte2084 -color_primaries bt2020 out.mkv
```

**Artifact removal then upscale:**

```text
nvvfx=mode=artifact:quality=conservative,nvvfx=mode=superres:quality=ultra:scale=2
```

## `nvvfx_upscale`

Fast 8-bit upscaling. Output is BGRA at the exact scaled size. P010 input is
down-converted to 8-bit.

### Options

| Option | Accepted values | Default |
|---|---|---|
| `ratio` | `4/3`, `3/2`, `2`, `3`, `4` | `2` |
| `strength` | `0.0`–`1.0` | `0.4` |
| `lib_dir` | directory string | auto |
| `dep_dir` | directory string | auto |

The output must be an exact integer size in both dimensions. For example,
`4/3` requires dimensions divisible by 3; `2.5` and `1` are rejected.

### Example

```text
nvvfx_upscale=ratio=3/2:strength=0.5,scale_d3d11=format=nv12
```

## `nvvfx_denoise`

Dedicated TensorRT webcam/video denoiser. It keeps temporal state, outputs
BGRA at the same size, and is limited to 8-bit input and output. P010 is
down-converted. Supported size: 80p through 1080p, width ≤ 1920.

This is not the VideoSuperRes denoise path. Use `nvvfx=mode=denoise` when you
need 10-bit or want to chain denoise with superres/deblur/artifact.

### Options

| Option | Accepted values | Default |
|---|---|---|
| `strength` | `0.0`–`1.0`; below `0.5` is conservative, `0.5` and above aggressive | `0.5` |
| `model_dir` | directory containing the Denoising models | `<lib_dir>\models`, then `<dep_dir>\models` |
| `lib_dir` | directory string | auto |
| `dep_dir` | directory string | auto |

The Denoise models are mandatory:

```text
Denoise-agg-100.engine.trtpkg
Denoise-con-100.engine.trtpkg
```

### Example

```text
nvvfx_denoise=strength=0.5,scale_d3d11=format=nv12
```

## `nvvfx_framerate`

Automatic frame-rate conversion using NVIDIA VideoFrameGeneration. One input;
the filter emits `multiplier - 1` interpolated frames plus the current frame
for each input pair. The first input frame is passed through unchanged. The
output frame rate is `multiplier` times the input rate, and the output time
base is divided by `multiplier`.

Use this for simple 2x–8x conversion. Use `nvvfx_framegen` when you already
have two aligned inputs and want to generate a specific intermediate frame.

### Options

| Option | Accepted values | Default |
|---|---|---|
| `multiplier` | `2`–`8` | `2` |
| `mode` | `low`, `medium`, `high` | `medium` |
| `shot_change` | `0`, `1` | `0` |
| `auto_shot_change` | `0`, `1` | `1` |
| `encoding` | `rgb8`, `rgb10a2` | `rgb8` |
| `lib_dir` | directory string | auto |
| `dep_dir` | directory string | auto |

`rgb8` output is BGRA; append `scale_d3d11=format=nv12` for NVENC.
`rgb10a2` requires P010 or X2BGR10 input and outputs P010.

### Example

```text
nvvfx_framerate=multiplier=2:mode=medium:encoding=rgb10a2
```

For 10-bit from an 8-bit source, promote first:

```text
scale_d3d11=format=p010,nvvfx_framerate=multiplier=2:mode=medium:encoding=rgb10a2
```

## `nvvfx_framegen`

Manual interpolation from two independent inputs: `previous` and `current`.
The filter generates one intermediate frame per input pair. It is not an
automatic rate converter; use `nvvfx_framerate` for 2x–8x output.

Both inputs must be D3D11 frames with the same dimensions and format, and
must be independent frames. A pair that references the same decoded frame is
rejected. Align the input timestamps or shift one input with `setpts`.

### Options

| Option | Accepted values | Default |
|---|---|---|
| `mode` | `low`, `medium`, `high` | `medium` |
| `method` | `time`, `index` | `time` |
| `timestep` | strictly between `0.0` and `1.0` | `0.5` |
| `frame_multiplier` | `2`–`8` | `2` |
| `frame_index` | `1` to `frame_multiplier - 1` | `1` |
| `shot_change` | `0`, `1` | `0` |
| `auto_shot_change` | `0`, `1` | `1` |
| `encoding` | `rgb8`, `rgb10a2` | `rgb8` |
| `lib_dir` | directory string | auto |
| `dep_dir` | directory string | auto |

`method=time` uses `timestep`. `method=index` uses `frame_multiplier` and
`frame_index`; for example, multiplier 4 with index 2 generates the second
of three intermediate frames.

`rgb8` output is BGRA; append `scale_d3d11=format=nv12` for NVENC.
`rgb10a2` requires P010 or X2BGR10 input and outputs P010. For 8-bit sources
in a 10-bit pipeline, promote both inputs with
`scale_d3d11=format=p010` first.

### Example

```bash
ffmpeg-nvvfx -init_hw_device d3d11va=dev -filter_hw_device dev \
  -f lavfi -i testsrc2=size=1280x720:rate=30:duration=5 \
  -f lavfi -i testsrc2=size=1280x720:rate=30:duration=5 \
  -filter_complex "[0:v]setpts=PTS+1,format=nv12,hwupload[a];[1:v]format=nv12,hwupload[b];[a][b]nvvfx_framegen=method=time:timestep=0.5,scale_d3d11=format=nv12" \
  -f null -
```

Index mode:

```text
nvvfx_framegen=method=index:frame_multiplier=4:frame_index=2,scale_d3d11=format=nv12
```

## Common options

`lib_dir` and `dep_dir` are available on every filter:

| Option | Meaning | Default |
|---|---|---|
| `lib_dir` | Directory with `NVVideoEffects.dll` and the feature DLLs | search `nvvfx\libs`/`libs` beside ffmpeg, then `NVVFX_LIB_DIR`, then the standard install |
| `dep_dir` | Legacy dependency/model directory, including NPP/TensorRT/CUDA and `models` | standard NVIDIA Video Effects install |

Prefer `NVVFX_LIB_DIR` over `lib_dir=` in scripts to avoid escaping Windows
drive colons.

## Requirements

- Windows 10/11 and an NVIDIA RTX GPU
- D3D11VA hardware frames
- NVIDIA Video Effects runtime and models; no NVIDIA binaries are bundled

| Filter | Runtime requirement |
|---|---|
| `nvvfx` (`superres`, `denoise`, `deblur`, `hdr`) | VideoSuperRes/TrueHDR runtime |
| `nvvfx` (`artifact`) | Legacy Program Files 0.7.6 runtime |
| `nvvfx_upscale` | `nvVFXUpscale.dll` |
| `nvvfx_denoise` | `nvVFXDenoising.dll` + Denoise models |
| `nvvfx_framerate` | `nvVFXVideoFrameGeneration.dll` |
| `nvvfx_framegen` | `nvVFXVideoFrameGeneration.dll` |

## Notes

- `-hwaccel d3d11va -hwaccel_output_format d3d11` is the zero-copy decode
  path. `-hwaccel cuda` (NVDEC) needs a download/upload round trip.
- `mode=hdr` outputs RGB10A2 PQ/BT.2020 (`x2bgr10le`). Use the explicit
  `hwdownload,format=x2bgr10le` and `scale` chain above to make P010 HDR10;
  the D3D11 video processor does not do that conversion correctly on current
  drivers.
- `nvvfx_upscale` and `nvvfx_denoise` are 8-bit only. P010 is down-converted.
- `artifact` is limited to 1920x1080. Don't run `artifact` and `denoise` in
  the same process.
- `nvvfx_framerate` and `nvvfx_framegen` support RGB8 and RGB10A2. 10-bit
  requires P010 or X2BGR10 input.
- Tested with the NVIDIA VFX SDK 1.3 runtime on an RTX 5080.

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
- True 10-bit VideoSuperRes, TrueHDR, frame generation, fast upscale, TRT
  denoise, and automatic frame-rate conversion.
- `vf_scale_d3d11.c`: fixed NV12/P010 texture allocation and color-space
  handling so GPU conversions match the source colors.
- Docs and Changelog.

</details>

## License

FFmpeg is LGPL/GPL; see `LICENSE`. The new filter is LGPL-2.1-or-later.
NVIDIA runtime DLLs and models are proprietary and are not included.
