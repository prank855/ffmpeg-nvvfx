/*
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "libavutil/opt.h"
#include "libavutil/avstring.h"
#include "libavutil/mem.h"
#include "libavutil/pixdesc.h"
#include "libavutil/mastering_display_metadata.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include <initguid.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <d3dcommon.h>

#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_d3d11va.h"

#include "filters.h"
#include "video.h"

/* Minimal NVVFX ABI, copied from the MIT-licensed nvCVImage.h and
 * nvVideoEffects.h. Keeping a local copy avoids an SDK build dependency. */

typedef int NvCV_Status;

struct CUstream_st;
typedef struct CUstream_st *CUstream;

typedef struct NvCVImage {
    unsigned int width;
    unsigned int height;
    int pitch;
    int pixelFormat;
    int componentType;
    unsigned char pixelBytes;
    unsigned char componentBytes;
    unsigned char numComponents;
    unsigned char planar;
    unsigned char gpuMem;
    unsigned char colorspace;
    unsigned char reserved[2];
    void *pixels;
    void *deletePtr;
    void (*deleteProc)(void *p);
    unsigned long long bufferBytes;
} NvCVImage;

struct NvVFX_Object;
typedef struct NvVFX_Object *NvVFX_Handle;

enum {
    NVCV_BGR     = 5,
    NVCV_RGBA    = 6,
    NVCV_BGRA    = 7,
    NVCV_RGB10A2 = 13,
    NVCV_U8      = 1,
    NVCV_U16     = 2,
    NVCV_F32     = 7,
    NVCV_P32     = 11,
    NVCV_CHUNKY  = 0,
    NVCV_PLANAR  = 1,
    NVCV_GPU     = 1,
};

typedef NvCV_Status (__cdecl *PFN_NvVFX_CreateEffect)(const char *, NvVFX_Handle *);
typedef void (__cdecl *PFN_NvVFX_DestroyEffect)(NvVFX_Handle);
typedef NvCV_Status (__cdecl *PFN_NvVFX_SetU32)(NvVFX_Handle, const char *, unsigned int);
typedef NvCV_Status (__cdecl *PFN_NvVFX_SetF32)(NvVFX_Handle, const char *, float);
typedef NvCV_Status (__cdecl *PFN_NvVFX_SetString)(NvVFX_Handle, const char *, const char *);
typedef NvCV_Status (__cdecl *PFN_NvVFX_SetImage)(NvVFX_Handle, const char *, NvCVImage *);
typedef NvCV_Status (__cdecl *PFN_NvVFX_SetCudaStream)(NvVFX_Handle, const char *, CUstream);
typedef NvCV_Status (__cdecl *PFN_NvVFX_Load)(NvVFX_Handle);
typedef NvCV_Status (__cdecl *PFN_NvVFX_Run)(NvVFX_Handle, int);
typedef NvCV_Status (__cdecl *PFN_NvVFX_CudaStreamCreate)(CUstream *);
typedef NvCV_Status (__cdecl *PFN_NvVFX_CudaStreamDestroy)(CUstream);
typedef NvCV_Status (__cdecl *PFN_NvVFX_CudaStreamSynchronize)(CUstream);
typedef NvCV_Status (__cdecl *PFN_NvCVImage_Alloc)(NvCVImage *, unsigned int,
                                                   unsigned int, int, int,
                                                   unsigned int, unsigned int,
                                                   unsigned int);
typedef void (__cdecl *PFN_NvCVImage_Dealloc)(NvCVImage *);
typedef NvCV_Status (__cdecl *PFN_NvCVImage_Transfer)(const NvCVImage *, NvCVImage *,
                                                      float, CUstream, NvCVImage *);
typedef NvCV_Status (__cdecl *PFN_NvCVImage_InitFromD3D11Texture)(NvCVImage *,
                                                                  ID3D11Texture2D *);
typedef NvCV_Status (__cdecl *PFN_NvCVImage_MapResource)(NvCVImage *, CUstream);
typedef NvCV_Status (__cdecl *PFN_NvCVImage_UnmapResource)(NvCVImage *, CUstream);
typedef const char *(__cdecl *PFN_NvCV_GetErrorStringFromCode)(NvCV_Status);

/* D3DCompile is resolved at runtime from d3dcompiler_47.dll; the filter does
 * not link against d3dcompiler. */
typedef HRESULT (WINAPI *PFN_D3DCompile)(const void *data, SIZE_T data_size,
                                         const char *filename,
                                         const D3D_SHADER_MACRO *defines,
                                         ID3DInclude *include,
                                         const char *entrypoint,
                                         const char *target, UINT sflags,
                                         UINT eflags, ID3DBlob **shader,
                                         ID3DBlob **error_messages);

#define NVVFX_INPUT_IMAGE  "SrcImage0"
#define NVVFX_OUTPUT_IMAGE "DstImage0"
#define NVVFX_CUDA_STREAM  "CudaStream"
#define NVVFX_QUALITY      "QualityLevel"
#define NVVFX_STRENGTH     "Strength"
#define NVVFX_ENCODING     "ImageEncodingMode"
#define NVVFX_MODE         "Mode"
#define NVVFX_MODEL_DIR    "ModelDir"

#define NVVFX_CONTRAST      "Contrast"
#define NVVFX_SATURATION    "Saturation"
#define NVVFX_MIDDLE_GRAY   "MiddleGray"
#define NVVFX_LUMINANCE     "Luminance"
#define NVVFX_DEBANDING_OFF "DebandingOff"

enum NVVFXMode {
    MODE_SUPERRES,
    MODE_DENOISE,
    MODE_DEBLUR,
    MODE_ARTIFACT,
    MODE_HDR,
    MODE_NB
};

enum NVVFXQuality {
    QUALITY_LOW,
    QUALITY_MEDIUM,
    QUALITY_HIGH,
    QUALITY_ULTRA,
    QUALITY_NB
};

enum NVVFXHdrMode {
    HDR_AUTO,
    HDR_ON,
    HDR_OFF,
    HDR_NB
};

/* Largest 2D texture size at D3D11 feature level 11_0. */
#define NVVFX_MAX_DIM 16384

typedef struct NVVFXContext {
    const AVClass *class;

    int mode;
    int quality;
    int scale;
    int out_w;
    int out_h;
    int bit_depth;
    int hdr;
    int hdr_out;
    int packed_out;
    float strength;
    int contrast;
    int saturation;
    int middle_gray;
    int luminance;
    int debanding_off;
    char *lib_dir;
    char *dep_dir;

    ID3D11Device *device;
    ID3D11DeviceContext *device_ctx;
    ID3D11VideoDevice *video_device;
    ID3D11VideoContext *video_context;
    ID3D11VideoContext1 *video_context1;
    ID3D11VideoProcessorEnumerator *vp_enum;
    ID3D11VideoProcessor *vp;
    ID3D11VideoProcessorOutputView *vp_out_view;
    ID3D11VideoProcessorEnumerator *vp_out_enum;
    ID3D11VideoProcessor *vp_out;
    ID3D11VideoProcessorOutputView *vp_out_p010_view;

    AVBufferRef *hw_device_ctx;
    AVBufferRef *hw_frames_ctx_out;

    enum AVPixelFormat in_sw_format;
    enum AVPixelFormat out_sw_format;

    HMODULE h_nvcv;
    HMODULE h_vfx;
    HMODULE h_ngxruntime;
    HMODULE h_feature;
    HMODULE h_vsr;
    HMODULE h_d3dcompiler;

    PFN_NvVFX_CreateEffect CreateEffect;
    PFN_NvVFX_DestroyEffect DestroyEffect;
    PFN_NvVFX_SetU32 SetU32;
    PFN_NvVFX_SetF32 SetF32;
    PFN_NvVFX_SetString SetString;
    PFN_NvVFX_SetImage SetImage;
    PFN_NvVFX_SetCudaStream SetCudaStream;
    PFN_NvVFX_Load Load;
    PFN_NvVFX_Run Run;
    PFN_NvVFX_CudaStreamCreate CudaStreamCreate;
    PFN_NvVFX_CudaStreamDestroy CudaStreamDestroy;
    PFN_NvVFX_CudaStreamSynchronize CudaStreamSynchronize;
    PFN_NvCVImage_Alloc Alloc;
    PFN_NvCVImage_Dealloc Dealloc;
    PFN_NvCVImage_Transfer Transfer;
    PFN_NvCVImage_InitFromD3D11Texture InitFromD3D11Texture;
    PFN_NvCVImage_MapResource MapResource;
    PFN_NvCVImage_UnmapResource UnmapResource;
    PFN_NvCV_GetErrorStringFromCode ErrorString;

    int libs_loaded;

    NvVFX_Handle effect;
    CUstream stream;

    ID3D11Texture2D *in_tex;
    ID3D11Texture2D *in_yuv_tex;
    ID3D11Texture2D *in_copy_tex;
    ID3D11Texture2D *out_tex;
    ID3D11Texture2D *in_r10_tex;
    ID3D11Texture2D *in_u16_tex;
    ID3D11Texture2D *out_u16_tex;
    ID3D11Texture2D *out_r10_tex;
    ID3D11Texture2D *out_p010_tex;
    ID3D11Query *sync_query;

    ID3D11ShaderResourceView *in_r10_srv;
    ID3D11ShaderResourceView *out_u16_srv;
    ID3D11RenderTargetView *in_u16_rtv;
    ID3D11RenderTargetView *out_r10_rtv;
    ID3D11VertexShader *bridge_vs;
    ID3D11PixelShader *bridge_ps;

    NvCVImage in_d3d;
    NvCVImage in_copy_d3d;
    NvCVImage out_d3d;
    NvCVImage in_u16_d3d;
    NvCVImage out_u16_d3d;
    NvCVImage in_gpu;
    NvCVImage out_gpu;
    NvCVImage in_u16_gpu;
    NvCVImage out_u16_gpu;
    NvCVImage hdr_in_gpu;
    NvCVImage in_f32;
    NvCVImage out_f32;

    int width;
    int height;
    int out_width;
    int out_height;
    int pixfmt;
    int quality_level;
    int vp_needed;
    DXGI_FORMAT in_format;
    int ready;
} NVVFXContext;

static av_cold int nvvfx_init(AVFilterContext *ctx)
{
    NVVFXContext *s = ctx->priv;

    if (s->mode == MODE_ARTIFACT && s->bit_depth != 8) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx: bit_depth is only supported for "
               "superres/denoise/deblur and hdr.\n");
        return AVERROR(EINVAL);
    }

    if (s->bit_depth != 8 && s->bit_depth != 10) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx: bit_depth must be 8 or 10.\n");
        return AVERROR(EINVAL);
    }

    if (s->mode != MODE_SUPERRES && (s->out_w || s->out_h)) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx: out_w/out_h are only supported with "
               "mode=superres; denoise, deblur and artifact keep the input "
               "dimensions.\n");
        return AVERROR(EINVAL);
    }

    if (s->hdr == HDR_ON &&
        (s->mode != MODE_SUPERRES || s->bit_depth != 10)) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx: hdr=on requires mode=superres and "
               "bit_depth=10.\n");
        return AVERROR(EINVAL);
    }

    if ((s->out_w > NVVFX_MAX_DIM) || (s->out_h > NVVFX_MAX_DIM)) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx: out_w/out_h must not exceed %d.\n",
               NVVFX_MAX_DIM);
        return AVERROR(EINVAL);
    }

    return 0;
}

static int nvvfx_hdr_mode(const NVVFXContext *s)
{
    return s->mode == MODE_HDR;
}

/* True RGB10A2 VSR: RGB10A2 inputs/outputs and the D3D11 shader bridge. */
static int nvvfx_10bit_mode(const NVVFXContext *s)
{
    return s->mode != MODE_ARTIFACT && s->mode != MODE_HDR &&
           s->bit_depth == 10;
}

static int nvvfx_input_needs_vp(const NVVFXContext *s)
{
    if (s->mode == MODE_HDR)
        return s->in_sw_format == AV_PIX_FMT_NV12 ||
               s->in_sw_format == AV_PIX_FMT_P010 ||
               s->in_sw_format == AV_PIX_FMT_X2BGR10;

    if (nvvfx_10bit_mode(s))
        return s->in_sw_format == AV_PIX_FMT_P010;

    return s->in_sw_format == AV_PIX_FMT_NV12 ||
           s->in_sw_format == AV_PIX_FMT_P010;
}

/* SuperRes accepts any output size at least as large as the input; the
 * denoise/deblur/artifact effects keep the input dimensions. */
static void nvvfx_output_dims(const NVVFXContext *s, int width, int height,
                              int *out_width, int *out_height)
{
    if (s->mode == MODE_SUPERRES) {
        *out_width  = s->out_w > 0 ? s->out_w : width * s->scale;
        *out_height = s->out_h > 0 ? s->out_h : height * s->scale;
    } else {
        *out_width  = width;
        *out_height = height;
    }
}

static int nvvfx_validate_output_dims(AVFilterContext *ctx, const NVVFXContext *s,
                                      int width, int height,
                                      int out_width, int out_height)
{
    if (width <= 0 || height <= 0 || out_width <= 0 || out_height <= 0) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx: invalid output size %dx%d for a "
               "%dx%d input.\n", out_width, out_height, width, height);
        return AVERROR(EINVAL);
    }

    if (s->mode == MODE_SUPERRES) {
        if (out_width < width || out_height < height) {
            av_log(ctx, AV_LOG_ERROR, "nvvfx: VideoSuperRes only upscales, but "
                   "%dx%d was requested for a %dx%d input.\n",
                   out_width, out_height, width, height);
            return AVERROR(EINVAL);
        }
        if (out_width > NVVFX_MAX_DIM || out_height > NVVFX_MAX_DIM) {
            av_log(ctx, AV_LOG_ERROR, "nvvfx: output size %dx%d exceeds the "
                   "D3D11 limit of %d.\n", out_width, out_height, NVVFX_MAX_DIM);
            return AVERROR(EINVAL);
        }
    } else if (out_width != width || out_height != height) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx: mode %d keeps the input dimensions, "
               "but %dx%d was requested for a %dx%d input.\n", s->mode,
               out_width, out_height, width, height);
        return AVERROR(EINVAL);
    }

    return 0;
}

/* config_props runs before the first frame, so a frame's color_trc is not
 * available yet. Use the link-level HDR hints the buffer source negotiated:
 * BT.2020 (HDR10's matrix) or static HDR metadata cloned from the stream. */
static int nvvfx_link_is_hdr(const AVFilterLink *link)
{
    int i;

    if (link->colorspace == AVCOL_SPC_BT2020_NCL ||
        link->colorspace == AVCOL_SPC_BT2020_CL)
        return 1;

    for (i = 0; i < link->nb_side_data; i++) {
        if (link->side_data[i]->type == AV_FRAME_DATA_MASTERING_DISPLAY_METADATA ||
            link->side_data[i]->type == AV_FRAME_DATA_CONTENT_LIGHT_LEVEL)
            return 1;
    }

    return 0;
}

/* One fullscreen triangle vertex shader with SV_VertexID and one pixel shader
 * that just Load()s texel (x, y). The same shader converts both directions:
 * R10G10B10A2_UNORM -> R16G16B16A16_UNORM and back (10 <-> 16 bits is exact
 * for the scale factor the NvCVImage transfer uses). Alpha is forced opaque
 * (1.0 -> the 2-bit RGB10A2 alpha becomes 3). */
static const char nvvfx_bridge_hlsl[] =
    "struct VSOut { float4 pos : SV_Position; };\n"
    "VSOut VS(uint id : SV_VertexID)\n"
    "{\n"
    "    VSOut o;\n"
    "    float2 uv = float2((id << 1) & 2, id & 2);\n"
    "    o.pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);\n"
    "    return o;\n"
    "}\n"
    "Texture2D<float4> Src : register(t0);\n"
    "float4 PS(VSOut i) : SV_Target\n"
    "{\n"
    "    float4 c = Src.Load(int3((int2)i.pos.xy, 0));\n"
    "    return float4(c.rgb, 1.0);\n"
    "}\n";

static int bridge_ensure(AVFilterContext *ctx, NVVFXContext *s)
{
    PFN_D3DCompile compile;
    ID3DBlob *vs_blob = NULL;
    ID3DBlob *ps_blob = NULL;
    ID3DBlob *err = NULL;
    HRESULT hr;

    if (s->bridge_vs && s->bridge_ps)
        return 0;

    if (!s->h_d3dcompiler) {
        s->h_d3dcompiler = LoadLibraryA("d3dcompiler_47.dll");
        if (!s->h_d3dcompiler) {
            av_log(ctx, AV_LOG_ERROR, "nvvfx: failed to load d3dcompiler_47.dll "
                   "(needed for the RGB10A2 bridge).\n");
            return AVERROR_EXTERNAL;
        }
    }

    compile = (PFN_D3DCompile)GetProcAddress(s->h_d3dcompiler, "D3DCompile");
    if (!compile) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx: d3dcompiler_47.dll has no D3DCompile.\n");
        return AVERROR_EXTERNAL;
    }

    hr = compile(nvvfx_bridge_hlsl, sizeof(nvvfx_bridge_hlsl) - 1,
                 "nvvfx_bridge", NULL, NULL, "VS", "vs_4_0", 0, 0,
                 &vs_blob, &err);
    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx: failed to compile the bridge vertex "
               "shader: %s\n", err ? (const char *)err->lpVtbl->GetBufferPointer(err) :
               "unknown error");
        if (err)
            err->lpVtbl->Release(err);
        return AVERROR_EXTERNAL;
    }
    if (err) {
        err->lpVtbl->Release(err);
        err = NULL;
    }

    hr = compile(nvvfx_bridge_hlsl, sizeof(nvvfx_bridge_hlsl) - 1,
                 "nvvfx_bridge", NULL, NULL, "PS", "ps_4_0", 0, 0,
                 &ps_blob, &err);
    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx: failed to compile the bridge pixel "
               "shader: %s\n", err ? (const char *)err->lpVtbl->GetBufferPointer(err) :
               "unknown error");
        if (err)
            err->lpVtbl->Release(err);
        vs_blob->lpVtbl->Release(vs_blob);
        return AVERROR_EXTERNAL;
    }
    if (err) {
        err->lpVtbl->Release(err);
        err = NULL;
    }

    hr = s->device->lpVtbl->CreateVertexShader(s->device,
                                               vs_blob->lpVtbl->GetBufferPointer(vs_blob),
                                               vs_blob->lpVtbl->GetBufferSize(vs_blob),
                                               NULL, &s->bridge_vs);
    if (SUCCEEDED(hr))
        hr = s->device->lpVtbl->CreatePixelShader(s->device,
                                                  ps_blob->lpVtbl->GetBufferPointer(ps_blob),
                                                  ps_blob->lpVtbl->GetBufferSize(ps_blob),
                                                  NULL, &s->bridge_ps);
    vs_blob->lpVtbl->Release(vs_blob);
    ps_blob->lpVtbl->Release(ps_blob);

    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx: failed to create the bridge shaders "
               "(HRESULT 0x%lX).\n", hr);
        return AVERROR_EXTERNAL;
    }
    return 0;
}

static int bridge_views(NVVFXContext *s)
{
    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {
        .ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
        .Texture2D = { .MipLevels = 1 },
    };
    D3D11_RENDER_TARGET_VIEW_DESC rtv_desc = {
        .ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D,
        .Texture2D = { .MipSlice = 0 },
    };
    HRESULT hr;

    if (s->in_r10_tex) {
        srv_desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
        hr = s->device->lpVtbl->CreateShaderResourceView(s->device,
                                                         (ID3D11Resource *)s->in_r10_tex,
                                                         &srv_desc, &s->in_r10_srv);
        if (FAILED(hr))
            return AVERROR_EXTERNAL;

        rtv_desc.Format = DXGI_FORMAT_R16G16B16A16_UNORM;
        hr = s->device->lpVtbl->CreateRenderTargetView(s->device,
                                                       (ID3D11Resource *)s->in_u16_tex,
                                                       &rtv_desc, &s->in_u16_rtv);
        if (FAILED(hr))
            return AVERROR_EXTERNAL;
    }

    if (s->out_u16_tex) {
        srv_desc.Format = DXGI_FORMAT_R16G16B16A16_UNORM;
        hr = s->device->lpVtbl->CreateShaderResourceView(s->device,
                                                         (ID3D11Resource *)s->out_u16_tex,
                                                         &srv_desc, &s->out_u16_srv);
        if (FAILED(hr))
            return AVERROR_EXTERNAL;

        rtv_desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
        hr = s->device->lpVtbl->CreateRenderTargetView(s->device,
                                                       (ID3D11Resource *)s->out_r10_tex,
                                                       &rtv_desc, &s->out_r10_rtv);
        if (FAILED(hr))
            return AVERROR_EXTERNAL;
    }

    return 0;
}

static void bridge_draw(NVVFXContext *s, ID3D11ShaderResourceView *srv,
                        ID3D11RenderTargetView *rtv, int width, int height)
{
    D3D11_VIEWPORT viewport = {
        .TopLeftX = 0.0f,
        .TopLeftY = 0.0f,
        .Width = (FLOAT)width,
        .Height = (FLOAT)height,
        .MinDepth = 0.0f,
        .MaxDepth = 1.0f,
    };
    ID3D11ShaderResourceView *null_srv = NULL;
    ID3D11RenderTargetView *null_rtv = NULL;
    ID3D11DeviceContext *dc = s->device_ctx;

    dc->lpVtbl->IASetInputLayout(dc, NULL);
    dc->lpVtbl->IASetPrimitiveTopology(dc, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    dc->lpVtbl->RSSetViewports(dc, 1, &viewport);
    dc->lpVtbl->OMSetRenderTargets(dc, 1, &rtv, NULL);
    dc->lpVtbl->VSSetShader(dc, s->bridge_vs, NULL, 0);
    dc->lpVtbl->PSSetShader(dc, s->bridge_ps, NULL, 0);
    dc->lpVtbl->PSSetShaderResources(dc, 0, 1, &srv);
    dc->lpVtbl->Draw(dc, 3, 0);
    dc->lpVtbl->PSSetShaderResources(dc, 0, 1, &null_srv);
    dc->lpVtbl->OMSetRenderTargets(dc, 1, &null_rtv, NULL);
}

static const char *vfx_err(NVVFXContext *s, NvCV_Status status)
{
    return s->ErrorString ? s->ErrorString(status) : "unknown error";
}

#define NVVFX_MAX_PATH 4096

static int dir_has_file(const char *dir, const char *name)
{
    char *path = av_asprintf("%s\\%s", dir, name);
    int found;

    if (!path)
        return 0;
    found = GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
    av_free(path);
    return found;
}

static HMODULE load_dll(const char *dir, const char *name)
{
    char *path = av_asprintf("%s\\%s", dir, name);
    HMODULE mod;

    if (!path)
        return NULL;
    mod = LoadLibraryExA(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    av_free(path);
    return mod;
}

static int vfx_load_libs(AVFilterContext *ctx, NVVFXContext *s)
{
    static const char *const deps[] = {
        "nppc64_12.dll", "nppial64_12.dll", "nppicc64_12.dll",
        "nppidei64_12.dll", "nppif64_12.dll", "nppig64_12.dll",
        "nppim64_12.dll", "nppist64_12.dll", "nppitc64_12.dll",
        "cudart64_12.dll", "nvinfer_10.dll", "nvinfer_plugin_10.dll",
        "nvonnxparser_10.dll",
    };
    char old_dir[NVVFX_MAX_PATH];
    int i;

    if (s->libs_loaded)
        return 0;

    snprintf(old_dir, sizeof(old_dir), "%s",
             s->dep_dir && s->dep_dir[0] ? s->dep_dir :
             "C:\\Program Files\\NVIDIA Corporation\\NVIDIA Video Effects");

    if (s->mode == MODE_ARTIFACT) {
        if (!dir_has_file(old_dir, "NVVideoEffects.dll") ||
            !dir_has_file(old_dir, "NVCVImage.dll")) {
            av_log(ctx, AV_LOG_ERROR, "nvvfx: the ArtifactReduction runtime was "
                   "not found in %s. Install the NVIDIA Video Effects SDK or set "
                   "the dep_dir option.\n", old_dir);
            return AVERROR_EXTERNAL;
        }

        av_log(ctx, AV_LOG_VERBOSE, "nvvfx: artifact runtime %s\n", old_dir);

        for (i = 0; i < FF_ARRAY_ELEMS(deps); i++) {
            if (!load_dll(old_dir, deps[i]))
                av_log(ctx, AV_LOG_VERBOSE, "nvvfx: could not load %s\n", deps[i]);
        }

        s->h_nvcv = load_dll(old_dir, "NVCVImage.dll");
        s->h_vfx = load_dll(old_dir, "NVVideoEffects.dll");
        if (!s->h_nvcv || !s->h_vfx) {
            av_log(ctx, AV_LOG_ERROR, "nvvfx: failed to load the ArtifactReduction "
                   "runtime from %s (error %lu).\n", old_dir, GetLastError());
            return AVERROR_EXTERNAL;
        }
    } else {
        /* 10-bit VSR and TrueHDR need the NvCVImage.dll shipped with the
         * NVIDIA VFX 0.2.0.0 wheel: only that build accommodates the packed
         * NVCV_RGB10A2 (pixelFormat 13) / NVCV_P32 (componentType 11) images.
         * Keep the legacy deps/models from dep_dir loaded first; they are
         * compatible. A process can only have one NVCVImage.dll (Windows
         * loads modules by base name), so a process that already loaded the
         * old 8-bit NVCVImage cannot switch to the 0.2.0.0 one (or vice
         * versa). Do not mix old and new NVCVImage in one process. */
        char dirs[5][NVVFX_MAX_PATH] = { 0 };
        char exe_dir[MAX_PATH];
        const char *feature_dll = s->mode == MODE_HDR ?
                                  "nvVFXTrueHDR.dll" : "nvVFXVideoSuperRes.dll";
        const char *ngx_dll = s->mode == MODE_HDR ?
                              "nvngx_truehdr.dll" : "nvngx_vsr.dll";
        const char *lib_dir = NULL;
        const char *dep_source;
        const char *env_dir;
        int nb_dirs = 0;
        int have_old;
        int want_new_nvcv = s->mode == MODE_HDR || nvvfx_10bit_mode(s);

        if (s->lib_dir && s->lib_dir[0]) {
            av_strlcpy(dirs[nb_dirs], s->lib_dir, NVVFX_MAX_PATH);
            nb_dirs++;
        }
        if (GetModuleFileNameA(NULL, exe_dir, MAX_PATH)) {
            char *slash = strrchr(exe_dir, '\\');
            if (slash) {
                *slash = 0;
                snprintf(dirs[nb_dirs++], NVVFX_MAX_PATH, "%s\\nvvfx\\libs", exe_dir);
                snprintf(dirs[nb_dirs++], NVVFX_MAX_PATH, "%s\\libs", exe_dir);
            }
        }
        env_dir = getenv("NVVFX_LIB_DIR");
        if (env_dir && env_dir[0]) {
            av_strlcpy(dirs[nb_dirs], env_dir, NVVFX_MAX_PATH);
            nb_dirs++;
        }
        snprintf(dirs[nb_dirs++], NVVFX_MAX_PATH,
                 "C:\\Program Files\\NVIDIA Corporation\\NVIDIA Video Effects");

        for (i = 0; i < nb_dirs; i++) {
            if (dir_has_file(dirs[i], feature_dll)) {
                lib_dir = dirs[i];
                break;
            }
        }
        if (!lib_dir) {
            av_log(ctx, AV_LOG_ERROR, "nvvfx: could not find %s. Install the "
                   "NVIDIA Video Effects SDK or set the lib_dir option.\n",
                   feature_dll);
            return AVERROR_EXTERNAL;
        }

        have_old = dir_has_file(old_dir, "NVVideoEffects.dll");
        dep_source = have_old ? old_dir : lib_dir;

        av_log(ctx, AV_LOG_VERBOSE, "nvvfx: runtime %s, legacy runtime %s\n",
               lib_dir, have_old ? old_dir : "not found");

        for (i = 0; i < FF_ARRAY_ELEMS(deps); i++) {
            if (!load_dll(dep_source, deps[i]))
                av_log(ctx, AV_LOG_VERBOSE, "nvvfx: could not load %s\n", deps[i]);
        }

        if (want_new_nvcv) {
            if (!dir_has_file(lib_dir, "NVCVImage.dll")) {
                av_log(ctx, AV_LOG_WARNING, "nvvfx: %s requires the NVCVImage.dll "
                       "from the NVIDIA VFX 0.2.0.0 wheel; none was found in %s.\n",
                       s->mode == MODE_HDR ? "TrueHDR" : "10-bit VideoSuperRes",
                       lib_dir);
            }
            s->h_nvcv = load_dll(dir_has_file(lib_dir, "NVCVImage.dll") ?
                                 lib_dir : old_dir, "NVCVImage.dll");
        } else {
            s->h_nvcv = load_dll(dir_has_file(old_dir, "NVCVImage.dll") ?
                                 old_dir : lib_dir, "NVCVImage.dll");
        }
        s->h_ngxruntime = load_dll(lib_dir, "nvngxruntime.dll");
        s->h_vfx = load_dll(lib_dir, "NVVideoEffects.dll");
        s->h_feature = load_dll(lib_dir, feature_dll);
        s->h_vsr = load_dll(lib_dir, ngx_dll);

        if (!s->h_nvcv || !s->h_vfx || !s->h_ngxruntime || !s->h_feature || !s->h_vsr) {
            av_log(ctx, AV_LOG_ERROR, "nvvfx: failed to load the NVIDIA Video Effects "
                   "runtime from %s (error %lu).\n", lib_dir, GetLastError());
            return AVERROR_EXTERNAL;
        }
    }

    s->CreateEffect = (PFN_NvVFX_CreateEffect)GetProcAddress(s->h_vfx, "NvVFX_CreateEffect");
    s->DestroyEffect = (PFN_NvVFX_DestroyEffect)GetProcAddress(s->h_vfx, "NvVFX_DestroyEffect");
    s->SetU32 = (PFN_NvVFX_SetU32)GetProcAddress(s->h_vfx, "NvVFX_SetU32");
    s->SetF32 = (PFN_NvVFX_SetF32)GetProcAddress(s->h_vfx, "NvVFX_SetF32");
    s->SetString = (PFN_NvVFX_SetString)GetProcAddress(s->h_vfx, "NvVFX_SetString");
    s->SetImage = (PFN_NvVFX_SetImage)GetProcAddress(s->h_vfx, "NvVFX_SetImage");
    s->SetCudaStream = (PFN_NvVFX_SetCudaStream)GetProcAddress(s->h_vfx, "NvVFX_SetCudaStream");
    s->Load = (PFN_NvVFX_Load)GetProcAddress(s->h_vfx, "NvVFX_Load");
    s->Run = (PFN_NvVFX_Run)GetProcAddress(s->h_vfx, "NvVFX_Run");
    s->CudaStreamCreate = (PFN_NvVFX_CudaStreamCreate)GetProcAddress(s->h_vfx, "NvVFX_CudaStreamCreate");
    s->CudaStreamDestroy = (PFN_NvVFX_CudaStreamDestroy)GetProcAddress(s->h_vfx, "NvVFX_CudaStreamDestroy");
    s->CudaStreamSynchronize = (PFN_NvVFX_CudaStreamSynchronize)GetProcAddress(s->h_vfx, "NvVFX_CudaStreamSynchronize");
    s->Alloc = (PFN_NvCVImage_Alloc)GetProcAddress(s->h_nvcv, "NvCVImage_Alloc");
    s->Dealloc = (PFN_NvCVImage_Dealloc)GetProcAddress(s->h_nvcv, "NvCVImage_Dealloc");
    s->Transfer = (PFN_NvCVImage_Transfer)GetProcAddress(s->h_nvcv, "NvCVImage_Transfer");
    s->InitFromD3D11Texture = (PFN_NvCVImage_InitFromD3D11Texture)GetProcAddress(s->h_nvcv, "NvCVImage_InitFromD3D11Texture");
    s->MapResource = (PFN_NvCVImage_MapResource)GetProcAddress(s->h_nvcv, "NvCVImage_MapResource");
    s->UnmapResource = (PFN_NvCVImage_UnmapResource)GetProcAddress(s->h_nvcv, "NvCVImage_UnmapResource");
    s->ErrorString = (PFN_NvCV_GetErrorStringFromCode)GetProcAddress(s->h_nvcv, "NvCV_GetErrorStringFromCode");

    if (!s->CreateEffect || !s->DestroyEffect || !s->SetU32 || !s->SetImage ||
        !s->SetCudaStream || !s->Load || !s->Run || !s->CudaStreamCreate ||
        !s->CudaStreamDestroy || !s->Alloc || !s->Dealloc || !s->Transfer ||
        !s->InitFromD3D11Texture || !s->MapResource || !s->UnmapResource ||
        (s->mode == MODE_ARTIFACT && !s->SetString) ||
        (s->mode != MODE_ARTIFACT && !s->SetF32)) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx: runtime is missing required symbols.\n");
        return AVERROR_EXTERNAL;
    }

    s->libs_loaded = 1;
    return 0;
}

static void release_video_processor(NVVFXContext *s)
{
    if (s->vp_out_view) {
        s->vp_out_view->lpVtbl->Release(s->vp_out_view);
        s->vp_out_view = NULL;
    }
    if (s->vp) {
        s->vp->lpVtbl->Release(s->vp);
        s->vp = NULL;
    }
    if (s->vp_enum) {
        s->vp_enum->lpVtbl->Release(s->vp_enum);
        s->vp_enum = NULL;
    }
    if (s->vp_out_p010_view) {
        s->vp_out_p010_view->lpVtbl->Release(s->vp_out_p010_view);
        s->vp_out_p010_view = NULL;
    }
    if (s->vp_out) {
        s->vp_out->lpVtbl->Release(s->vp_out);
        s->vp_out = NULL;
    }
    if (s->vp_out_enum) {
        s->vp_out_enum->lpVtbl->Release(s->vp_out_enum);
        s->vp_out_enum = NULL;
    }
}

static void vfx_destroy(NVVFXContext *s)
{
    if (s->effect && s->DestroyEffect)
        s->DestroyEffect(s->effect);
    s->effect = NULL;

    release_video_processor(s);

    if (s->Dealloc) {
        if (s->in_d3d.pixels || s->in_d3d.deletePtr)
            s->Dealloc(&s->in_d3d);
        if (s->in_copy_d3d.pixels || s->in_copy_d3d.deletePtr)
            s->Dealloc(&s->in_copy_d3d);
        if (s->out_d3d.pixels || s->out_d3d.deletePtr)
            s->Dealloc(&s->out_d3d);
        if (s->in_u16_d3d.pixels || s->in_u16_d3d.deletePtr)
            s->Dealloc(&s->in_u16_d3d);
        if (s->out_u16_d3d.pixels || s->out_u16_d3d.deletePtr)
            s->Dealloc(&s->out_u16_d3d);
        if (s->in_gpu.pixels || s->in_gpu.deletePtr)
            s->Dealloc(&s->in_gpu);
        if (s->out_gpu.pixels || s->out_gpu.deletePtr)
            s->Dealloc(&s->out_gpu);
        if (s->in_u16_gpu.pixels || s->in_u16_gpu.deletePtr)
            s->Dealloc(&s->in_u16_gpu);
        if (s->out_u16_gpu.pixels || s->out_u16_gpu.deletePtr)
            s->Dealloc(&s->out_u16_gpu);
        if (s->hdr_in_gpu.pixels || s->hdr_in_gpu.deletePtr)
            s->Dealloc(&s->hdr_in_gpu);
        if (s->in_f32.pixels || s->in_f32.deletePtr)
            s->Dealloc(&s->in_f32);
        if (s->out_f32.pixels || s->out_f32.deletePtr)
            s->Dealloc(&s->out_f32);
    }
    memset(&s->in_d3d, 0, sizeof(s->in_d3d));
    memset(&s->in_copy_d3d, 0, sizeof(s->in_copy_d3d));
    memset(&s->out_d3d, 0, sizeof(s->out_d3d));
    memset(&s->in_u16_d3d, 0, sizeof(s->in_u16_d3d));
    memset(&s->out_u16_d3d, 0, sizeof(s->out_u16_d3d));
    memset(&s->in_gpu, 0, sizeof(s->in_gpu));
    memset(&s->out_gpu, 0, sizeof(s->out_gpu));
    memset(&s->in_u16_gpu, 0, sizeof(s->in_u16_gpu));
    memset(&s->out_u16_gpu, 0, sizeof(s->out_u16_gpu));
    memset(&s->hdr_in_gpu, 0, sizeof(s->hdr_in_gpu));
    memset(&s->in_f32, 0, sizeof(s->in_f32));
    memset(&s->out_f32, 0, sizeof(s->out_f32));

    if (s->in_r10_srv) {
        s->in_r10_srv->lpVtbl->Release(s->in_r10_srv);
        s->in_r10_srv = NULL;
    }
    if (s->out_u16_srv) {
        s->out_u16_srv->lpVtbl->Release(s->out_u16_srv);
        s->out_u16_srv = NULL;
    }
    if (s->in_u16_rtv) {
        s->in_u16_rtv->lpVtbl->Release(s->in_u16_rtv);
        s->in_u16_rtv = NULL;
    }
    if (s->out_r10_rtv) {
        s->out_r10_rtv->lpVtbl->Release(s->out_r10_rtv);
        s->out_r10_rtv = NULL;
    }
    if (s->bridge_vs) {
        s->bridge_vs->lpVtbl->Release(s->bridge_vs);
        s->bridge_vs = NULL;
    }
    if (s->bridge_ps) {
        s->bridge_ps->lpVtbl->Release(s->bridge_ps);
        s->bridge_ps = NULL;
    }

    if (s->in_tex) {
        s->in_tex->lpVtbl->Release(s->in_tex);
        s->in_tex = NULL;
    }
    if (s->in_yuv_tex) {
        s->in_yuv_tex->lpVtbl->Release(s->in_yuv_tex);
        s->in_yuv_tex = NULL;
    }
    if (s->in_copy_tex) {
        s->in_copy_tex->lpVtbl->Release(s->in_copy_tex);
        s->in_copy_tex = NULL;
    }
    if (s->out_tex) {
        s->out_tex->lpVtbl->Release(s->out_tex);
        s->out_tex = NULL;
    }
    if (s->in_r10_tex) {
        s->in_r10_tex->lpVtbl->Release(s->in_r10_tex);
        s->in_r10_tex = NULL;
    }
    if (s->in_u16_tex) {
        s->in_u16_tex->lpVtbl->Release(s->in_u16_tex);
        s->in_u16_tex = NULL;
    }
    if (s->out_u16_tex) {
        s->out_u16_tex->lpVtbl->Release(s->out_u16_tex);
        s->out_u16_tex = NULL;
    }
    if (s->out_r10_tex) {
        s->out_r10_tex->lpVtbl->Release(s->out_r10_tex);
        s->out_r10_tex = NULL;
    }
    if (s->out_p010_tex) {
        s->out_p010_tex->lpVtbl->Release(s->out_p010_tex);
        s->out_p010_tex = NULL;
    }
    if (s->sync_query) {
        s->sync_query->lpVtbl->Release(s->sync_query);
        s->sync_query = NULL;
    }

    if (s->stream && s->CudaStreamDestroy)
        s->CudaStreamDestroy(s->stream);
    s->stream = NULL;

    s->ready = 0;
    s->width = s->height = 0;
    s->out_width = s->out_height = 0;
    s->pixfmt = 0;
    s->quality_level = 0;
    s->vp_needed = 0;
    s->in_format = DXGI_FORMAT_UNKNOWN;
}

static int create_texture(NVVFXContext *s, int width, int height,
                          DXGI_FORMAT format, UINT bind_flags,
                          ID3D11Texture2D **tex)
{
    D3D11_TEXTURE2D_DESC desc = {
        .Width = width,
        .Height = height,
        .MipLevels = 1,
        .ArraySize = 1,
        .Format = format,
        .SampleDesc = { .Count = 1 },
        .Usage = D3D11_USAGE_DEFAULT,
        .BindFlags = bind_flags,
    };
    HRESULT hr;

    hr = s->device->lpVtbl->CreateTexture2D(s->device, &desc, NULL, tex);
    return SUCCEEDED(hr) ? 0 : AVERROR_EXTERNAL;
}

static int vp_create(AVFilterContext *ctx, NVVFXContext *s, int width, int height,
                     ID3D11Texture2D *target,
                     ID3D11VideoProcessorEnumerator **vp_enum,
                     ID3D11VideoProcessor **vp,
                     ID3D11VideoProcessorOutputView **vp_out_view)
{
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content = {
        .InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE,
        .InputWidth = width,
        .InputHeight = height,
        .OutputWidth = width,
        .OutputHeight = height,
        .Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL,
    };
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC out_desc = {
        .ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D,
        .Texture2D = { .MipSlice = 0 },
    };
    RECT src_rect = {
        .right = width,
        .bottom = height,
    };
    HRESULT hr;

    hr = s->video_device->lpVtbl->CreateVideoProcessorEnumerator(s->video_device,
                                                                 &content, vp_enum);
    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx: failed to create the video processor "
               "enumerator (HRESULT 0x%lX).\n", hr);
        return AVERROR_EXTERNAL;
    }

    hr = s->video_device->lpVtbl->CreateVideoProcessor(s->video_device, *vp_enum,
                                                       0, vp);
    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx: failed to create the video processor "
               "(HRESULT 0x%lX).\n", hr);
        return AVERROR_EXTERNAL;
    }

    hr = s->video_device->lpVtbl->CreateVideoProcessorOutputView(s->video_device,
                                                                 (ID3D11Resource *)target,
                                                                 *vp_enum, &out_desc,
                                                                 vp_out_view);
    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx: failed to create the video processor "
               "output view (HRESULT 0x%lX).\n", hr);
        return AVERROR_EXTERNAL;
    }

    s->video_context->lpVtbl->VideoProcessorSetStreamSourceRect(s->video_context,
                                                                *vp, 0, TRUE,
                                                                &src_rect);
    s->video_context->lpVtbl->VideoProcessorSetStreamAutoProcessingMode(s->video_context,
                                                                        *vp, 0, FALSE);

    {
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE csp = {
            .Usage = 0,
            .RGB_Range = 0,
            .Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255,
        };
        s->video_context->lpVtbl->VideoProcessorSetOutputColorSpace(s->video_context,
                                                                    *vp, &csp);
    }

    return 0;
}

static void vp_set_stream_colorspace(NVVFXContext *s, const AVFrame *frame, int hdr)
{
    int matrix_601;

    if (s->in_sw_format == AV_PIX_FMT_X2BGR10) {
        /* RGB source: full-range RGB is the only meaningful interpretation. */
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE csp = {
            .Usage = 0,
            .RGB_Range = 0,
            .Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255,
        };
        s->video_context->lpVtbl->VideoProcessorSetStreamColorSpace(s->video_context,
                                                                    s->vp, 0, &csp);
        return;
    }

    if (hdr && s->video_context1) {
        /* PQ/BT.2020 input. The legacy D3D11_VIDEO_PROCESSOR_COLOR_SPACE
         * cannot express ST 2084, so use the ID3D11VideoContext1 API: the
         * matrix/range conversion keeps the PQ code values (both sides use
         * the same transfer function). Do not fall back to BT.709 here. */
        s->video_context1->lpVtbl->VideoProcessorSetStreamColorSpace1(
            s->video_context1, s->vp, 0,
            DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020);
        s->video_context1->lpVtbl->VideoProcessorSetOutputColorSpace1(
            s->video_context1, s->vp,
            DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
        return;
    }

    switch (frame->colorspace) {
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M:
        matrix_601 = 1;
        break;
    case AVCOL_SPC_BT709:
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL:
        matrix_601 = 0;
        break;
    default:
        /* 10-bit SDR sources are treated as BT.709 (the usual HD case); keep
         * the existing SD heuristic for the 8-bit path. */
        matrix_601 = nvvfx_10bit_mode(s) ? 0 : frame->height <= 576;
        break;
    }

    {
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE csp = {
            .Usage = 0,
            .RGB_Range = 0,
            .YCbCr_Matrix = !matrix_601,
            .Nominal_Range = frame->color_range == AVCOL_RANGE_JPEG ?
                             D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255 :
                             D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235,
        };
        s->video_context->lpVtbl->VideoProcessorSetStreamColorSpace(s->video_context,
                                                                    s->vp, 0, &csp);
    }
}

static int vfx_setup(AVFilterContext *ctx, NVVFXContext *s, int width, int height,
                     DXGI_FORMAT in_format, int pixfmt, int vp_needed)
{
    int is_artifact = s->mode == MODE_ARTIFACT;
    int is_hdr = nvvfx_hdr_mode(s);
    int hdr_out = s->hdr_out;
    int packed_out = s->packed_out;
    int use_10bit = nvvfx_10bit_mode(s);
    int bridge_out = use_10bit || is_hdr;
    int quality_level = 0;
    int in_pf = use_10bit ? NVCV_RGB10A2 : pixfmt;
    int in_type = use_10bit ? NVCV_P32 : NVCV_U8;
    int out_pf = (use_10bit || is_hdr) ? NVCV_RGB10A2 :
                 is_artifact ? NVCV_BGRA : pixfmt;
    int out_type = (use_10bit || is_hdr) ? NVCV_P32 : NVCV_U8;
    int out_width, out_height;
    DXGI_FORMAT tex_format = vp_needed ? DXGI_FORMAT_B8G8R8A8_UNORM : in_format;
    DXGI_FORMAT out_tex_format = is_artifact ? DXGI_FORMAT_B8G8R8A8_UNORM : tex_format;
    char model_dir[NVVFX_MAX_PATH] = "";
    D3D11_QUERY_DESC query_desc = { .Query = D3D11_QUERY_EVENT };
    NvCV_Status st;
    HRESULT hr;
    int ret;

    nvvfx_output_dims(s, width, height, &out_width, &out_height);
    ret = nvvfx_validate_output_dims(ctx, s, width, height,
                                     out_width, out_height);
    if (ret < 0)
        return ret;

    if (is_artifact) {
        if (width > 1920 || height > 1080) {
            av_log(ctx, AV_LOG_ERROR, "nvvfx: ArtifactReduction supports at most "
                   "1920x1080 (got %dx%d).\n", width, height);
            return AVERROR(EINVAL);
        }
        quality_level = s->quality ? 1 : 0;
        snprintf(model_dir, sizeof(model_dir), "%s\\models",
                 s->dep_dir && s->dep_dir[0] ? s->dep_dir :
                 "C:\\Program Files\\NVIDIA Corporation\\NVIDIA Video Effects");
    } else if (is_hdr) {
        quality_level = 0;
    } else if (s->mode == MODE_SUPERRES) {
        quality_level = 1 + s->quality;
    } else if (s->mode == MODE_DENOISE) {
        quality_level = 8 + s->quality;
    } else {
        quality_level = 12 + s->quality;
    }

    if (s->ready && s->width == width && s->height == height &&
        s->out_width == out_width && s->out_height == out_height &&
        s->pixfmt == pixfmt && s->quality_level == quality_level &&
        s->vp_needed == vp_needed && s->in_format == in_format)
        return 0;

    vfx_destroy(s);
    s->in_format = in_format;

    if (!use_10bit &&
        (ret = create_texture(s, width, height, tex_format,
                              D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
                              &s->in_tex)) < 0) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx: failed to create the input texture.\n");
        goto fail;
    }
    if (!bridge_out &&
        (ret = create_texture(s, out_width, out_height, out_tex_format,
                              D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
                              &s->out_tex)) < 0) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx: failed to create the output texture.\n");
        goto fail;
    }

    /* True RGB10A2: the video processor writes an R10G10B10A2 texture, a
     * pixel shader repacks it into RGBA16_UNORM (the only 10-bit-capable
     * format NvCVImage can import from D3D11), then NvCVImage transfers it
     * into a GPU RGB10A2 image. */
    if (use_10bit &&
        ((ret = create_texture(s, width, height, DXGI_FORMAT_R10G10B10A2_UNORM,
                               D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
                               &s->in_r10_tex)) < 0 ||
         (ret = create_texture(s, width, height, DXGI_FORMAT_R16G16B16A16_UNORM,
                               D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
                               &s->in_u16_tex)) < 0)) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx: failed to create the 10-bit input "
               "bridge textures.\n");
        goto fail;
    }
    if (bridge_out &&
        ((ret = create_texture(s, out_width, out_height,
                               DXGI_FORMAT_R16G16B16A16_UNORM,
                               D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
                               &s->out_u16_tex)) < 0 ||
         (ret = create_texture(s, out_width, out_height,
                               DXGI_FORMAT_R10G10B10A2_UNORM,
                               D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
                               &s->out_r10_tex)) < 0)) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx: failed to create the 10-bit output "
               "bridge textures.\n");
        goto fail;
    }
    if (use_10bit && !packed_out &&
        (ret = create_texture(s, out_width, out_height, DXGI_FORMAT_P010,
                              D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
                              &s->out_p010_tex)) < 0) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx: failed to create the P010 output "
               "texture.\n");
        goto fail;
    }

    /* Normalize the input into a texture we own: decoder textures are padded
     * arrays, and hwupload textures carry no video-processor bind flags. */
    if (vp_needed) {
        UINT yuv_bind = (in_format == DXGI_FORMAT_NV12 ||
                         in_format == DXGI_FORMAT_P010) ?
                        (D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_DECODER) :
                        (D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);

        ret = create_texture(s, width, height, in_format, yuv_bind,
                             &s->in_yuv_tex);
        if (ret < 0) {
            av_log(ctx, AV_LOG_ERROR, "nvvfx: failed to create the staging textures.\n");
            goto fail;
        }
        if (!use_10bit) {
            ret = create_texture(s, width, height, tex_format,
                                 D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
                                 &s->in_copy_tex);
            if (ret < 0) {
                av_log(ctx, AV_LOG_ERROR, "nvvfx: failed to create the staging "
                       "textures.\n");
                goto fail;
            }
        }
    }

    hr = s->device->lpVtbl->CreateQuery(s->device, &query_desc, &s->sync_query);
    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx: failed to create the sync query "
               "(HRESULT 0x%lX).\n", hr);
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    if (s->in_tex) {
        st = s->InitFromD3D11Texture(&s->in_d3d, s->in_tex);
        if (st) {
            av_log(ctx, AV_LOG_ERROR, "nvvfx: failed to register the input "
                   "texture (%s).\n", vfx_err(s, st));
            ret = AVERROR_EXTERNAL;
            goto fail;
        }
    }
    if (s->out_tex) {
        st = s->InitFromD3D11Texture(&s->out_d3d, s->out_tex);
        if (st) {
            av_log(ctx, AV_LOG_ERROR, "nvvfx: failed to register the output "
                   "texture (%s).\n", vfx_err(s, st));
            ret = AVERROR_EXTERNAL;
            goto fail;
        }
    }
    if (s->in_copy_tex) {
        st = s->InitFromD3D11Texture(&s->in_copy_d3d, s->in_copy_tex);
        if (st) {
            av_log(ctx, AV_LOG_ERROR, "nvvfx: failed to register the copy "
                   "texture (%s).\n", vfx_err(s, st));
            ret = AVERROR_EXTERNAL;
            goto fail;
        }
    }
    if (s->in_u16_tex) {
        st = s->InitFromD3D11Texture(&s->in_u16_d3d, s->in_u16_tex);
        if (st) {
            av_log(ctx, AV_LOG_ERROR, "nvvfx: failed to register the U16 input "
                   "texture (%s).\n", vfx_err(s, st));
            ret = AVERROR_EXTERNAL;
            goto fail;
        }
    }
    if (s->out_u16_tex) {
        st = s->InitFromD3D11Texture(&s->out_u16_d3d, s->out_u16_tex);
        if (st) {
            av_log(ctx, AV_LOG_ERROR, "nvvfx: failed to register the U16 output "
                   "texture (%s).\n", vfx_err(s, st));
            ret = AVERROR_EXTERNAL;
            goto fail;
        }
    }

    st = s->Alloc(&s->in_gpu, width, height, in_pf, in_type,
                  NVCV_CHUNKY, NVCV_GPU, 32);
    if (st) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx: failed to allocate the input image (%s).\n",
               vfx_err(s, st));
        if (use_10bit || is_hdr)
            av_log(ctx, AV_LOG_ERROR, "nvvfx: 10-bit and TrueHDR require the "
                   "NVCVImage.dll from the NVIDIA VFX 0.2.0.0 wheel.\n");
        ret = AVERROR_EXTERNAL;
        goto fail;
    }
    st = s->Alloc(&s->out_gpu, out_width, out_height, out_pf, out_type,
                  NVCV_CHUNKY, NVCV_GPU, 32);
    if (st) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx: failed to allocate the output image (%s).\n",
               vfx_err(s, st));
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    if (use_10bit) {
        st = s->Alloc(&s->in_u16_gpu, width, height, NVCV_RGBA, NVCV_U16,
                      NVCV_CHUNKY, NVCV_GPU, 32);
        if (st) {
            av_log(ctx, AV_LOG_ERROR, "nvvfx: failed to allocate the U16 input "
                   "image (%s).\n", vfx_err(s, st));
            ret = AVERROR_EXTERNAL;
            goto fail;
        }
    }
    if (bridge_out) {
        st = s->Alloc(&s->out_u16_gpu, out_width, out_height, NVCV_RGBA, NVCV_U16,
                      NVCV_CHUNKY, NVCV_GPU, 32);
        if (st) {
            av_log(ctx, AV_LOG_ERROR, "nvvfx: failed to allocate the U16 output "
                   "image (%s).\n", vfx_err(s, st));
            ret = AVERROR_EXTERNAL;
            goto fail;
        }
    }
    if (is_hdr) {
        /* TrueHDR rejects BGRA input; keep an RGBA U8 conversion target. */
        st = s->Alloc(&s->hdr_in_gpu, width, height, NVCV_RGBA, NVCV_U8,
                      NVCV_CHUNKY, NVCV_GPU, 32);
        if (st) {
            av_log(ctx, AV_LOG_ERROR, "nvvfx: failed to allocate the RGBA input "
                   "image (%s).\n", vfx_err(s, st));
            ret = AVERROR_EXTERNAL;
            goto fail;
        }
    }

    if (is_artifact) {
        st = s->Alloc(&s->in_f32, width, height, NVCV_BGR, NVCV_F32,
                      NVCV_PLANAR, NVCV_GPU, 32);
        if (st) {
            av_log(ctx, AV_LOG_ERROR, "nvvfx: failed to allocate the BGR F32 input "
                   "(%s).\n", vfx_err(s, st));
            ret = AVERROR_EXTERNAL;
            goto fail;
        }
        st = s->Alloc(&s->out_f32, width, height, NVCV_BGR, NVCV_F32,
                      NVCV_PLANAR, NVCV_GPU, 32);
        if (st) {
            av_log(ctx, AV_LOG_ERROR, "nvvfx: failed to allocate the BGR F32 output "
                   "(%s).\n", vfx_err(s, st));
            ret = AVERROR_EXTERNAL;
            goto fail;
        }
    }

    if (vp_needed) {
        ret = vp_create(ctx, s, width, height,
                        use_10bit ? s->in_r10_tex : s->in_tex,
                        &s->vp_enum, &s->vp, &s->vp_out_view);
        if (ret < 0)
            goto fail;
    }
    if (use_10bit && !is_hdr && !packed_out) {
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE stream_csp = {
            .Usage = 0,
            .RGB_Range = 0,
            .Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255,
        };
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE out_csp = {
            .Usage = 0,
            .RGB_Range = 0,
            .YCbCr_Matrix = 1, /* BT.709 */
            .Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235,
        };

        ret = vp_create(ctx, s, out_width, out_height, s->out_p010_tex,
                        &s->vp_out_enum, &s->vp_out, &s->vp_out_p010_view);
        if (ret < 0)
            goto fail;
        s->video_context->lpVtbl->VideoProcessorSetStreamColorSpace(s->video_context,
                                                                    s->vp_out, 0,
                                                                    &stream_csp);
        s->video_context->lpVtbl->VideoProcessorSetOutputColorSpace(s->video_context,
                                                                    s->vp_out,
                                                                    &out_csp);
    }

    if (bridge_out) {
        if ((ret = bridge_ensure(ctx, s)) < 0)
            goto fail;
        if ((ret = bridge_views(s)) < 0) {
            av_log(ctx, AV_LOG_ERROR, "nvvfx: failed to create the bridge views.\n");
            ret = AVERROR_EXTERNAL;
            goto fail;
        }
    }

    if (is_artifact) {
        st = s->CreateEffect("ArtifactReduction", &s->effect);
        if (st) {
            av_log(ctx, AV_LOG_ERROR, "nvvfx: failed to create the "
                   "ArtifactReduction effect (%s).\n", vfx_err(s, st));
            ret = AVERROR_EXTERNAL;
            goto fail;
        }

        st = s->SetU32(s->effect, NVVFX_MODE, quality_level);
        if (st) {
            av_log(ctx, AV_LOG_ERROR, "nvvfx: failed to set Mode=%d (%s).\n",
                   quality_level, vfx_err(s, st));
            ret = AVERROR_EXTERNAL;
            goto fail;
        }
    } else if (is_hdr) {
        st = s->CreateEffect("TrueHDR", &s->effect);
        if (st) {
            av_log(ctx, AV_LOG_ERROR, "nvvfx: failed to create the TrueHDR "
                   "effect (%s).\n", vfx_err(s, st));
            ret = AVERROR_EXTERNAL;
            goto fail;
        }

        st = s->SetU32(s->effect, NVVFX_CONTRAST, s->contrast);
        if (!st)
            st = s->SetU32(s->effect, NVVFX_SATURATION, s->saturation);
        if (!st)
            st = s->SetU32(s->effect, NVVFX_MIDDLE_GRAY, s->middle_gray);
        if (!st)
            st = s->SetU32(s->effect, NVVFX_LUMINANCE, s->luminance);
        if (!st)
            st = s->SetU32(s->effect, NVVFX_DEBANDING_OFF, s->debanding_off);
        if (st) {
            av_log(ctx, AV_LOG_ERROR, "nvvfx: failed to set the TrueHDR "
                   "parameters (%s).\n", vfx_err(s, st));
            ret = AVERROR_EXTERNAL;
            goto fail;
        }
    } else {
        st = s->CreateEffect("VideoSuperRes", &s->effect);
        if (st) {
            av_log(ctx, AV_LOG_ERROR, "nvvfx: failed to create the VideoSuperRes "
                   "effect (%s).\n", vfx_err(s, st));
            ret = AVERROR_EXTERNAL;
            goto fail;
        }

        st = s->SetU32(s->effect, NVVFX_QUALITY, quality_level);
        if (st) {
            av_log(ctx, AV_LOG_ERROR, "nvvfx: failed to set QualityLevel=%d (%s).\n",
                   quality_level, vfx_err(s, st));
            ret = AVERROR_EXTERNAL;
            goto fail;
        }

        st = s->SetF32(s->effect, NVVFX_STRENGTH, s->strength);
        if (st) {
            /* The 1.3 VideoSuperRes feature DLL as shipped in the older split
             * runtime returns -2 (unimplemented) for Strength; its default is
             * already 1.0, so that combination keeps working. */
            if (s->strength == 1.0f) {
                av_log(ctx, AV_LOG_VERBOSE, "nvvfx: runtime rejected the default "
                       "Strength selector (%s); using its default.\n",
                       vfx_err(s, st));
            } else {
                av_log(ctx, AV_LOG_ERROR, "nvvfx: failed to set Strength=%.3f (%s).\n",
                       s->strength, vfx_err(s, st));
                ret = AVERROR_EXTERNAL;
                goto fail;
            }
        }

        if (use_10bit) {
            /* Must be set before SetImage/Load and match both images. */
            st = s->SetU32(s->effect, NVVFX_ENCODING, 1);
            if (st) {
                av_log(ctx, AV_LOG_ERROR, "nvvfx: failed to set "
                       "ImageEncodingMode=1 (%s).\n", vfx_err(s, st));
                ret = AVERROR_EXTERNAL;
                goto fail;
            }
        }
    }

    if (!s->stream) {
        st = s->CudaStreamCreate(&s->stream);
        if (st) {
            av_log(ctx, AV_LOG_ERROR, "nvvfx: failed to create the CUDA stream (%s).\n",
                   vfx_err(s, st));
            ret = AVERROR_EXTERNAL;
            goto fail;
        }
    }

    st = s->SetCudaStream(s->effect, NVVFX_CUDA_STREAM, s->stream);
    if (st) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx: failed to set the CUDA stream (%s).\n",
               vfx_err(s, st));
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    if (is_artifact) {
        st = s->SetString(s->effect, NVVFX_MODEL_DIR, model_dir);
        if (st) {
            av_log(ctx, AV_LOG_ERROR, "nvvfx: failed to set ModelDir=%s (%s).\n",
                   model_dir, vfx_err(s, st));
            ret = AVERROR_EXTERNAL;
            goto fail;
        }
        st = s->SetImage(s->effect, NVVFX_INPUT_IMAGE, &s->in_f32);
        if (st) {
            av_log(ctx, AV_LOG_ERROR, "nvvfx: failed to set the input image (%s).\n",
                   vfx_err(s, st));
            ret = AVERROR_EXTERNAL;
            goto fail;
        }
        st = s->SetImage(s->effect, NVVFX_OUTPUT_IMAGE, &s->out_f32);
        if (st) {
            av_log(ctx, AV_LOG_ERROR, "nvvfx: failed to set the output image (%s).\n",
                   vfx_err(s, st));
            ret = AVERROR_EXTERNAL;
            goto fail;
        }
    } else {
        NvCVImage *src = is_hdr && pixfmt != NVCV_RGBA ?
                         &s->hdr_in_gpu : &s->in_gpu;

        st = s->SetImage(s->effect, NVVFX_INPUT_IMAGE, src);
        if (st) {
            av_log(ctx, AV_LOG_ERROR, "nvvfx: failed to set the input image (%s).\n",
                   vfx_err(s, st));
            ret = AVERROR_EXTERNAL;
            goto fail;
        }
        st = s->SetImage(s->effect, NVVFX_OUTPUT_IMAGE, &s->out_gpu);
        if (st) {
            av_log(ctx, AV_LOG_ERROR, "nvvfx: failed to set the output image (%s).\n",
                   vfx_err(s, st));
            ret = AVERROR_EXTERNAL;
            goto fail;
        }
    }

    st = s->Load(s->effect);
    if (st) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx: failed to load the %s model (%s).\n",
               is_artifact ? "ArtifactReduction" :
               is_hdr ? "TrueHDR" : "VideoSuperRes", vfx_err(s, st));
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    s->width = width;
    s->height = height;
    s->out_width = out_width;
    s->out_height = out_height;
    s->pixfmt = pixfmt;
    s->quality_level = quality_level;
    s->vp_needed = vp_needed;
    s->in_format = in_format;
    s->ready = 1;

    av_log(ctx, AV_LOG_VERBOSE, "nvvfx: %s ready (%dx%d -> %dx%d, quality=%d, "
           "mode=%d, %s)\n", is_artifact ? "ArtifactReduction" :
           is_hdr ? "TrueHDR" : "VideoSuperRes", width, height,
           out_width, out_height, quality_level, s->mode,
           is_hdr ? "RGBA8 in, RGB10A2 out" :
           hdr_out ? "10-bit RGB10A2 in/out, PQ/BT.2020" :
           use_10bit ? "10-bit RGB10A2" : "8-bit RGBA/BGRA");
    return 0;

fail:
    vfx_destroy(s);
    return ret;
}

static int d3d11_sync(AVFilterContext *ctx, NVVFXContext *s)
{
    HRESULT hr;

    s->device_ctx->lpVtbl->End(s->device_ctx, (ID3D11Asynchronous *)s->sync_query);

    do {
        hr = s->device_ctx->lpVtbl->GetData(s->device_ctx,
                                            (ID3D11Asynchronous *)s->sync_query,
                                            NULL, 0, 0);
        if (hr == S_FALSE)
            Sleep(0);
    } while (hr == S_FALSE);

    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx: D3D11 sync query failed (HRESULT 0x%lX).\n",
               hr);
        return AVERROR_EXTERNAL;
    }
    return 0;
}

static int vp_blt(AVFilterContext *ctx, NVVFXContext *s,
                  ID3D11VideoProcessorEnumerator *vp_enum,
                  ID3D11VideoProcessor *vp,
                  ID3D11VideoProcessorOutputView *out_view,
                  ID3D11Texture2D *src_tex, DXGI_FORMAT fourcc)
{
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC view_desc = {
        .FourCC = fourcc,
        .ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D,
        .Texture2D.ArraySlice = 0,
    };
    ID3D11VideoProcessorInputView *view = NULL;
    D3D11_VIDEO_PROCESSOR_STREAM stream = {
        .Enable = TRUE,
        .OutputIndex = 0,
    };
    HRESULT hr;

    hr = s->video_device->lpVtbl->CreateVideoProcessorInputView(s->video_device,
                                                                (ID3D11Resource *)src_tex,
                                                                vp_enum, &view_desc,
                                                                &view);
    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx: failed to create the input view "
               "(HRESULT 0x%lX).\n", hr);
        return AVERROR_EXTERNAL;
    }

    stream.pInputSurface = view;

    hr = s->video_context->lpVtbl->VideoProcessorBlt(s->video_context, vp,
                                                     out_view, 0, 1, &stream);
    view->lpVtbl->Release(view);

    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx: VideoProcessorBlt failed (HRESULT 0x%lX).\n",
               hr);
        return AVERROR_EXTERNAL;
    }
    return 0;
}

static int vfx_process(AVFilterContext *ctx, NVVFXContext *s, const AVFrame *frame,
                       ID3D11Texture2D *in_tex, UINT in_slice,
                       ID3D11Texture2D *out_tex, UINT out_slice)
{
    int is_hdr = nvvfx_hdr_mode(s);
    int hdr_out = s->hdr_out;
    int use_10bit = nvvfx_10bit_mode(s);
    int bridge_out = use_10bit || is_hdr;
    NvCV_Status st;
    int ret;

    if (!s->ready)
        return AVERROR(EINVAL);

    if (use_10bit) {
        if (s->vp_needed) {
            vp_set_stream_colorspace(s, frame, hdr_out);
            s->device_ctx->lpVtbl->CopySubresourceRegion(s->device_ctx,
                                                         (ID3D11Resource *)s->in_yuv_tex, 0,
                                                         0, 0, 0,
                                                         (ID3D11Resource *)in_tex,
                                                         in_slice, NULL);
            if ((ret = vp_blt(ctx, s, s->vp_enum, s->vp, s->vp_out_view,
                              s->in_yuv_tex, s->in_format)) < 0)
                return ret;
        } else {
            /* X2BGR10 already is an R10G10B10A2 texture. */
            s->device_ctx->lpVtbl->CopySubresourceRegion(s->device_ctx,
                                                         (ID3D11Resource *)s->in_r10_tex, 0,
                                                         0, 0, 0,
                                                         (ID3D11Resource *)in_tex,
                                                         in_slice, NULL);
        }

        if ((ret = d3d11_sync(ctx, s)) < 0)
            return ret;

        /* R10G10B10A2 -> RGBA16_UNORM, the only 10-bit format NvCVImage can
         * import from a D3D11 texture. */
        bridge_draw(s, s->in_r10_srv, s->in_u16_rtv, s->width, s->height);
        if ((ret = d3d11_sync(ctx, s)) < 0)
            return ret;

        st = s->MapResource(&s->in_u16_d3d, s->stream);
        if (st == 0) {
            NvCV_Status transfer_st = s->Transfer(&s->in_u16_d3d, &s->in_u16_gpu,
                                                  1.0f, s->stream, NULL);
            NvCV_Status unmap_st = s->UnmapResource(&s->in_u16_d3d, s->stream);
            st = transfer_st ? transfer_st : unmap_st;
        }
        if (st) {
            av_log(ctx, AV_LOG_ERROR, "nvvfx: 10-bit input transfer failed (%s).\n",
                   vfx_err(s, st));
            return AVERROR_EXTERNAL;
        }

        st = s->Transfer(&s->in_u16_gpu, &s->in_gpu, 1.0f, s->stream, NULL);
        if (st) {
            av_log(ctx, AV_LOG_ERROR, "nvvfx: 10-bit input repack failed (%s).\n",
                   vfx_err(s, st));
            return AVERROR_EXTERNAL;
        }
    } else {
        if (s->vp_needed) {
            vp_set_stream_colorspace(s, frame, 0);
            s->device_ctx->lpVtbl->CopySubresourceRegion(s->device_ctx,
                                                         (ID3D11Resource *)s->in_yuv_tex, 0,
                                                         0, 0, 0,
                                                         (ID3D11Resource *)in_tex,
                                                         in_slice, NULL);
            if ((ret = vp_blt(ctx, s, s->vp_enum, s->vp, s->vp_out_view,
                              s->in_yuv_tex, s->in_format)) < 0)
                return ret;
            /* CUDA can see stale data when it maps a texture the video processor
             * just wrote, so map a copy made by the D3D11 context instead. */
            s->device_ctx->lpVtbl->CopySubresourceRegion(s->device_ctx,
                                                         (ID3D11Resource *)s->in_copy_tex, 0,
                                                         0, 0, 0,
                                                         (ID3D11Resource *)s->in_tex, 0, NULL);
        } else {
            s->device_ctx->lpVtbl->CopySubresourceRegion(s->device_ctx,
                                                         (ID3D11Resource *)s->in_tex, 0,
                                                         0, 0, 0,
                                                         (ID3D11Resource *)in_tex,
                                                         in_slice, NULL);
        }

        /* The copy/Blt is asynchronous; CUDA must not map the texture before the
         * GPU has finished writing it. */
        if ((ret = d3d11_sync(ctx, s)) < 0)
            return ret;

        {
            NvCVImage *in_d3d = s->vp_needed ? &s->in_copy_d3d : &s->in_d3d;
            st = s->MapResource(in_d3d, s->stream);
            if (st == 0) {
                NvCV_Status transfer_st = s->Transfer(in_d3d, &s->in_gpu, 1.0f,
                                                      s->stream, NULL);
                NvCV_Status unmap_st = s->UnmapResource(in_d3d, s->stream);
                st = transfer_st ? transfer_st : unmap_st;
            }
        }
        if (st) {
            av_log(ctx, AV_LOG_ERROR, "nvvfx: input transfer failed (%s).\n",
                   vfx_err(s, st));
            return AVERROR_EXTERNAL;
        }
    }

    if (s->mode == MODE_ARTIFACT) {
        /* ArtifactReduction expects normalized BGR F32 planar (0..1). */
        st = s->Transfer(&s->in_gpu, &s->in_f32, 1.0f / 255.0f,
                         s->stream, NULL);
        if (st) {
            av_log(ctx, AV_LOG_ERROR, "nvvfx: input format conversion failed (%s).\n",
                   vfx_err(s, st));
            return AVERROR_EXTERNAL;
        }
    }

    if (is_hdr && s->pixfmt != NVCV_RGBA) {
        /* TrueHDR explicitly rejects BGRA input. */
        st = s->Transfer(&s->in_gpu, &s->hdr_in_gpu, 1.0f, s->stream, NULL);
        if (st) {
            av_log(ctx, AV_LOG_ERROR, "nvvfx: BGRA to RGBA conversion failed (%s).\n",
                   vfx_err(s, st));
            return AVERROR_EXTERNAL;
        }
    }

    st = s->Run(s->effect, 0);
    if (st) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx: inference failed (%s).\n", vfx_err(s, st));
        return AVERROR_EXTERNAL;
    }

    if (s->mode == MODE_ARTIFACT) {
        st = s->Transfer(&s->out_f32, &s->out_gpu, 255.0f,
                         s->stream, NULL);
        if (st) {
            av_log(ctx, AV_LOG_ERROR, "nvvfx: output format conversion failed (%s).\n",
                   vfx_err(s, st));
            return AVERROR_EXTERNAL;
        }
    }

    if (bridge_out) {
        /* RGB10A2 -> RGBA16_UNORM -> D3D11 shader -> R10G10B10A2. */
        st = s->Transfer(&s->out_gpu, &s->out_u16_gpu, 1.0f, s->stream, NULL);
        if (st) {
            av_log(ctx, AV_LOG_ERROR, "nvvfx: 10-bit output repack failed (%s).\n",
                   vfx_err(s, st));
            return AVERROR_EXTERNAL;
        }

        st = s->MapResource(&s->out_u16_d3d, s->stream);
        if (st == 0) {
            NvCV_Status transfer_st = s->Transfer(&s->out_u16_gpu, &s->out_u16_d3d,
                                                  1.0f, s->stream, NULL);
            NvCV_Status unmap_st = s->UnmapResource(&s->out_u16_d3d, s->stream);
            st = transfer_st ? transfer_st : unmap_st;
        }
        if (st) {
            av_log(ctx, AV_LOG_ERROR, "nvvfx: 10-bit output transfer failed (%s).\n",
                   vfx_err(s, st));
            return AVERROR_EXTERNAL;
        }

        if (s->CudaStreamSynchronize)
            s->CudaStreamSynchronize(s->stream);

        bridge_draw(s, s->out_u16_srv, s->out_r10_rtv, s->out_width, s->out_height);
        if ((ret = d3d11_sync(ctx, s)) < 0)
            return ret;

        if (is_hdr || (use_10bit && s->packed_out)) {
            /* TrueHDR, PQ VideoSuperRes and odd-sized SDR VideoSuperRes output
             * packed RGB10A2; copy it straight into the X2BGR10 output frame
             * (no VP color conversion to P010). The bridge pixel shader writes
             * alpha 1.0, so the packed word's alpha bits are forced to 3
             * (word |= 0xC0000000). */
            s->device_ctx->lpVtbl->CopySubresourceRegion(s->device_ctx,
                                                         (ID3D11Resource *)out_tex, out_slice,
                                                         0, 0, 0,
                                                         (ID3D11Resource *)s->out_r10_tex,
                                                         0, NULL);
        } else {
            /* SDR RGB10A2 -> P010, BT.709 limited. */
            if ((ret = vp_blt(ctx, s, s->vp_out_enum, s->vp_out,
                              s->vp_out_p010_view, s->out_r10_tex,
                              DXGI_FORMAT_UNKNOWN)) < 0)
                return ret;
            if ((ret = d3d11_sync(ctx, s)) < 0)
                return ret;
            s->device_ctx->lpVtbl->CopySubresourceRegion(s->device_ctx,
                                                         (ID3D11Resource *)out_tex, out_slice,
                                                         0, 0, 0,
                                                         (ID3D11Resource *)s->out_p010_tex,
                                                         0, NULL);
        }
    } else {
        st = s->MapResource(&s->out_d3d, s->stream);
        if (st == 0) {
            NvCV_Status transfer_st = s->Transfer(&s->out_gpu, &s->out_d3d, 1.0f,
                                                  s->stream, NULL);
            NvCV_Status unmap_st = s->UnmapResource(&s->out_d3d, s->stream);
            st = transfer_st ? transfer_st : unmap_st;
        }
        if (st) {
            av_log(ctx, AV_LOG_ERROR, "nvvfx: output transfer failed (%s).\n",
                   vfx_err(s, st));
            return AVERROR_EXTERNAL;
        }

        if (s->CudaStreamSynchronize)
            s->CudaStreamSynchronize(s->stream);

        s->device_ctx->lpVtbl->CopySubresourceRegion(s->device_ctx,
                                                     (ID3D11Resource *)out_tex, out_slice,
                                                     0, 0, 0,
                                                     (ID3D11Resource *)s->out_tex,
                                                     0, NULL);
    }

    s->device_ctx->lpVtbl->Flush(s->device_ctx);
    return 0;
}

static int nvvfx_set_output_hdr10_metadata(NVVFXContext *s, AVFilterLink *outlink);

static int nvvfx_config_props(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    NVVFXContext *s = ctx->priv;
    AVFilterLink *inlink = ctx->inputs[0];
    FilterLink *inl = ff_filter_link(inlink);
    FilterLink *outl = ff_filter_link(outlink);
    AVHWFramesContext *in_frames;
    AVHWFramesContext *out_frames;
    AVD3D11VAFramesContext *out_frames_hwctx;
    AVHWDeviceContext *hw_device;
    AVD3D11VADeviceContext *d3d11_hwctx;
    int out_width, out_height;
    int vp_needed;
    HRESULT hr;
    int ret;

    av_buffer_unref(&s->hw_frames_ctx_out);
    vfx_destroy(s);
    if (s->video_context1) {
        s->video_context1->lpVtbl->Release(s->video_context1);
        s->video_context1 = NULL;
    }
    if (s->video_context) {
        s->video_context->lpVtbl->Release(s->video_context);
        s->video_context = NULL;
    }
    if (s->video_device) {
        s->video_device->lpVtbl->Release(s->video_device);
        s->video_device = NULL;
    }

    if (!inl->hw_frames_ctx) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx: no input hw_frames_ctx.\n");
        return AVERROR(EINVAL);
    }

    in_frames = (AVHWFramesContext *)inl->hw_frames_ctx->data;
    s->in_sw_format = in_frames->sw_format;
    s->hdr_out = 0;
    s->packed_out = 0;

    if (s->mode == MODE_ARTIFACT && s->bit_depth != 8) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx: bit_depth is only supported for "
               "superres/denoise/deblur and hdr.\n");
        return AVERROR(EINVAL);
    }

    nvvfx_output_dims(s, inlink->w, inlink->h, &out_width, &out_height);
    ret = nvvfx_validate_output_dims(ctx, s, inlink->w, inlink->h,
                                     out_width, out_height);
    if (ret < 0)
        return ret;

    if (s->mode == MODE_HDR) {
        switch (s->in_sw_format) {
        case AV_PIX_FMT_NV12:
        case AV_PIX_FMT_P010:
        case AV_PIX_FMT_BGRA:
        case AV_PIX_FMT_RGBA:
        case AV_PIX_FMT_X2BGR10:
            s->out_sw_format = AV_PIX_FMT_X2BGR10;
            break;
        default:
            av_log(ctx, AV_LOG_ERROR, "nvvfx: TrueHDR requires NV12, P010, BGRA, "
                   "RGBA or X2BGR10 input, got %s.\n",
                   av_get_pix_fmt_name(s->in_sw_format));
            return AVERROR(EINVAL);
        }
    } else if (nvvfx_10bit_mode(s)) {
        if (s->in_sw_format != AV_PIX_FMT_P010 &&
            s->in_sw_format != AV_PIX_FMT_X2BGR10) {
            av_log(ctx, AV_LOG_ERROR, "nvvfx: bit_depth=10 requires P010 or "
                   "X2BGR10 input, got %s.\n",
                   av_get_pix_fmt_name(s->in_sw_format));
            return AVERROR(EINVAL);
        }
        s->hdr_out = s->hdr == HDR_ON ||
                     (s->hdr == HDR_AUTO && nvvfx_link_is_hdr(inlink));
        /* P010 requires even dimensions. SuperRes accepts odd/tiny sizes, so
         * fall back to packed X2BGR10 for odd SDR output (the VSR effect itself
         * has no even-dimension constraint). */
        s->packed_out = s->hdr_out || (out_width & 1) || (out_height & 1);
        if (s->packed_out) {
            /* VSR preserves the PQ code values; emit packed RGB10A2 so the
             * graph can convert to P010 with swscale (the D3D11 VP cannot do
             * PQ/BT.2020 -> P010 correctly). */
            s->out_sw_format = AV_PIX_FMT_X2BGR10;
            if (!s->hdr_out)
                av_log(ctx, AV_LOG_VERBOSE, "nvvfx: odd output %dx%d cannot be "
                       "represented as P010; using packed SDR X2BGR10.\n",
                       out_width, out_height);
        } else {
            s->out_sw_format = AV_PIX_FMT_P010;
        }
        av_log(ctx, AV_LOG_VERBOSE, "nvvfx: 10-bit path: hdr=%d (link colorspace "
               "%s, range %s, side data %d) -> %s\n", s->hdr,
               (const char *)av_x_if_null(av_color_space_name(inlink->colorspace), "unknown"),
               (const char *)av_x_if_null(av_color_range_name(inlink->color_range), "unknown"),
               inlink->nb_side_data,
               s->hdr_out ? "X2BGR10 PQ/BT.2020" :
               s->packed_out ? "X2BGR10 SDR" : "P010 SDR");
    } else {
        switch (s->in_sw_format) {
        case AV_PIX_FMT_NV12:
        case AV_PIX_FMT_P010:
            s->out_sw_format = AV_PIX_FMT_BGRA;
            break;
        case AV_PIX_FMT_BGRA:
        case AV_PIX_FMT_RGBA:
            s->out_sw_format = s->in_sw_format;
            break;
        default:
            av_log(ctx, AV_LOG_ERROR, "nvvfx: unsupported input format %s, expected "
                   "NV12, P010, BGRA or RGBA (P010/X2BGR10 with bit_depth=10).\n",
                   av_get_pix_fmt_name(s->in_sw_format));
            return AVERROR(EINVAL);
        }
    }

    if (s->mode == MODE_ARTIFACT) {
        if (inlink->w > 1920 || inlink->h > 1080) {
            av_log(ctx, AV_LOG_ERROR, "nvvfx: ArtifactReduction supports at most "
                   "1920x1080 (got %dx%d).\n", inlink->w, inlink->h);
            return AVERROR(EINVAL);
        }
        s->out_sw_format = AV_PIX_FMT_BGRA;
    }

    vp_needed = nvvfx_input_needs_vp(s);

    av_buffer_unref(&s->hw_device_ctx);
    s->hw_device_ctx = av_buffer_ref(in_frames->device_ref);
    if (!s->hw_device_ctx)
        return AVERROR(ENOMEM);
    hw_device = (AVHWDeviceContext *)s->hw_device_ctx->data;
    d3d11_hwctx = hw_device->hwctx;

    s->device = d3d11_hwctx->device;
    s->device_ctx = d3d11_hwctx->device_context;
    if (!s->device || !s->device_ctx) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx: invalid D3D11 device or context.\n");
        return AVERROR(EINVAL);
    }

    /* 10-bit SDR always needs the video processor for the RGB10A2 -> P010
     * output stage, even when the input is already an X2BGR10 texture. */
    if (vp_needed || nvvfx_10bit_mode(s)) {
        hr = s->device->lpVtbl->QueryInterface(s->device, &IID_ID3D11VideoDevice,
                                               (void **)&s->video_device);
        if (FAILED(hr)) {
            av_log(ctx, AV_LOG_ERROR, "nvvfx: failed to get the D3D11 video device "
                   "(HRESULT 0x%lX).\n", hr);
            return AVERROR_EXTERNAL;
        }
        hr = s->device_ctx->lpVtbl->QueryInterface(s->device_ctx, &IID_ID3D11VideoContext,
                                                   (void **)&s->video_context);
        if (FAILED(hr)) {
            av_log(ctx, AV_LOG_ERROR, "nvvfx: failed to get the D3D11 video context "
                   "(HRESULT 0x%lX).\n", hr);
            return AVERROR_EXTERNAL;
        }
    }

    if (s->hdr_out && vp_needed) {
        hr = s->device_ctx->lpVtbl->QueryInterface(s->device_ctx,
                                                   &IID_ID3D11VideoContext1,
                                                   (void **)&s->video_context1);
        if (FAILED(hr)) {
            av_log(ctx, AV_LOG_ERROR, "nvvfx: HDR VideoSuperRes requires "
                   "ID3D11VideoContext1 for the PQ/BT.2020 conversion "
                   "(HRESULT 0x%lX).\n", hr);
            return AVERROR_EXTERNAL;
        }
    }

    outlink->w = out_width;
    outlink->h = out_height;

    if (s->mode == MODE_HDR ||
        (nvvfx_10bit_mode(s) && s->hdr_out)) {
        ret = nvvfx_set_output_hdr10_metadata(s, outlink);
        if (ret < 0) {
            av_log(ctx, AV_LOG_ERROR, "nvvfx: failed to set the HDR10 output "
                   "metadata.\n");
            return ret;
        }
    }

    s->hw_frames_ctx_out = av_hwframe_ctx_alloc(s->hw_device_ctx);
    if (!s->hw_frames_ctx_out)
        return AVERROR(ENOMEM);

    out_frames = (AVHWFramesContext *)s->hw_frames_ctx_out->data;
    out_frames->format = AV_PIX_FMT_D3D11;
    out_frames->sw_format = s->out_sw_format;
    out_frames->width = out_width;
    out_frames->height = out_height;
    /* D3D11 rejects static array textures with RENDER_TARGET for NV12/P010
     * (and the 10-bit RGB formats), so let the pool allocate on demand. */
    if (s->out_sw_format == AV_PIX_FMT_P010 ||
        s->out_sw_format == AV_PIX_FMT_X2BGR10) {
        out_frames->initial_pool_size = 0;
    } else {
        out_frames->initial_pool_size = 10;
        if (ctx->extra_hw_frames > 0)
            out_frames->initial_pool_size += ctx->extra_hw_frames;
    }

    out_frames_hwctx = out_frames->hwctx;
    out_frames_hwctx->BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    out_frames_hwctx->MiscFlags = 0;

    ret = av_hwframe_ctx_init(s->hw_frames_ctx_out);
    if (ret < 0) {
        av_buffer_unref(&s->hw_frames_ctx_out);
        return ret;
    }

    outl->hw_frames_ctx = av_buffer_ref(s->hw_frames_ctx_out);
    if (!outl->hw_frames_ctx)
        return AVERROR(ENOMEM);

    ret = vfx_load_libs(ctx, s);
    if (ret < 0)
        return ret;

    av_log(ctx, AV_LOG_VERBOSE, "nvvfx: configured %dx%d %s -> %dx%d %s (mode=%d)\n",
           inlink->w, inlink->h, av_get_pix_fmt_name(s->in_sw_format),
           out_width, out_height, av_get_pix_fmt_name(s->out_sw_format), s->mode);
    return 0;
}

static void nvvfx_fill_hdr10_metadata(NVVFXContext *s,
                                      AVMasteringDisplayMetadata *mdm,
                                      AVContentLightMetadata *cll)
{
    memset(mdm, 0, sizeof(*mdm));
    mdm->display_primaries[0][0] = (AVRational){ 35400, 50000 }; /* R x */
    mdm->display_primaries[0][1] = (AVRational){ 14600, 50000 }; /* R y */
    mdm->display_primaries[1][0] = (AVRational){  8500, 50000 }; /* G x */
    mdm->display_primaries[1][1] = (AVRational){ 39850, 50000 }; /* G y */
    mdm->display_primaries[2][0] = (AVRational){  6550, 50000 }; /* B x */
    mdm->display_primaries[2][1] = (AVRational){  2300, 50000 }; /* B y */
    mdm->white_point[0] = (AVRational){ 15635, 50000 };          /* D65 x */
    mdm->white_point[1] = (AVRational){ 16450, 50000 };          /* D65 y */
    mdm->has_primaries = 1;

    mdm->max_luminance = (AVRational){ s->luminance * 10000, 10000 };
    mdm->min_luminance = (AVRational){ 1, 10000 };               /* 0.0001 nits */
    mdm->has_luminance = 1;

    memset(cll, 0, sizeof(*cll));
    cll->MaxCLL  = 0; /* unknown */
    cll->MaxFALL = 0; /* unknown */
}

/* Global (link) copy: lets the FFmpeg CLI initialise the encoder with HDR10
 * static metadata, which is what makes NVENC emit the ST 2086/CLL SEI. */
static int nvvfx_set_output_hdr10_metadata(NVVFXContext *s, AVFilterLink *outlink)
{
    AVFrameSideData *sd;
    AVMasteringDisplayMetadata *mdm;
    AVContentLightMetadata *cll;

    av_frame_side_data_remove_by_props(&outlink->side_data, &outlink->nb_side_data,
                                       AV_SIDE_DATA_PROP_COLOR_DEPENDENT);

    sd = av_frame_side_data_new(&outlink->side_data, &outlink->nb_side_data,
                                AV_FRAME_DATA_MASTERING_DISPLAY_METADATA,
                                sizeof(*mdm), 0);
    if (!sd)
        return AVERROR(ENOMEM);
    mdm = (AVMasteringDisplayMetadata *)sd->data;

    sd = av_frame_side_data_new(&outlink->side_data, &outlink->nb_side_data,
                                AV_FRAME_DATA_CONTENT_LIGHT_LEVEL,
                                sizeof(*cll), 0);
    if (!sd)
        return AVERROR(ENOMEM);
    cll = (AVContentLightMetadata *)sd->data;

    nvvfx_fill_hdr10_metadata(s, mdm, cll);
    return 0;
}

static int nvvfx_attach_hdr10_metadata(NVVFXContext *s, AVFrame *frame)
{
    AVMasteringDisplayMetadata *mdm;
    AVContentLightMetadata *cll;

    frame->colorspace      = AVCOL_SPC_RGB;
    frame->color_range     = AVCOL_RANGE_JPEG;
    frame->color_trc       = AVCOL_TRC_SMPTE2084;
    frame->color_primaries = AVCOL_PRI_BT2020;
    frame->chroma_location = AVCHROMA_LOC_UNSPECIFIED;

    /* The SDR input may have carried colour-dependent side data; it is not
     * valid for the converted PQ/BT.2020 frame. */
    av_frame_side_data_remove_by_props(&frame->side_data, &frame->nb_side_data,
                                       AV_SIDE_DATA_PROP_COLOR_DEPENDENT);

    mdm = av_mastering_display_metadata_create_side_data(frame);
    if (!mdm)
        return AVERROR(ENOMEM);

    cll = av_content_light_metadata_create_side_data(frame);
    if (!cll)
        return AVERROR(ENOMEM);

    nvvfx_fill_hdr10_metadata(s, mdm, cll);
    return 0;
}

static int nvvfx_filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext *ctx = inlink->dst;
    NVVFXContext *s = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];
    ID3D11Texture2D *in_tex;
    ID3D11Texture2D *out_tex;
    D3D11_TEXTURE2D_DESC desc;
    AVFrame *out = NULL;
    UINT in_slice, out_slice;
    int width, height;
    int pixfmt;
    int vp_needed;
    int ret;

    if (in->format != AV_PIX_FMT_D3D11 || !in->hw_frames_ctx) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx: input is not a D3D11 hardware frame.\n");
        av_frame_free(&in);
        return AVERROR(EINVAL);
    }
    if (!s->hw_frames_ctx_out) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx: output frames context is not configured.\n");
        av_frame_free(&in);
        return AVERROR(EINVAL);
    }

    in_tex = (ID3D11Texture2D *)in->data[0];
    in_slice = (UINT)(intptr_t)in->data[1];
    in_tex->lpVtbl->GetDesc(in_tex, &desc);

    /* Decoder textures can be padded to a coded size; the filter graph and
     * the output pool are sized from the logical frame dimensions, and copies
     * use a NULL source box, which clips to the valid region. */
    width = in->width > 0 ? in->width : (int)desc.Width;
    height = in->height > 0 ? in->height : (int)desc.Height;
    if (width > (int)desc.Width || height > (int)desc.Height) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx: frame size %dx%d exceeds the texture "
               "size %ux%u.\n", width, height, desc.Width, desc.Height);
        av_frame_free(&in);
        return AVERROR(EINVAL);
    }

    pixfmt = s->in_sw_format == AV_PIX_FMT_RGBA ? NVCV_RGBA : NVCV_BGRA;
    vp_needed = nvvfx_input_needs_vp(s);

    if (nvvfx_10bit_mode(s) && s->hdr == HDR_AUTO &&
        in->color_trc == AVCOL_TRC_SMPTE2084 && !s->hdr_out) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx: the input is PQ/BT.2020 but the HDR10 "
               "output path was not selected at configuration time (the input "
               "link carried no HDR hint); set hdr=on.\n");
        ret = AVERROR(EINVAL);
        goto fail;
    }

    ret = vfx_setup(ctx, s, width, height, desc.Format, pixfmt, vp_needed);
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx: failed to set up the effect.\n");
        goto fail;
    }

    out = av_frame_alloc();
    if (!out) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    ret = av_hwframe_get_buffer(s->hw_frames_ctx_out, out, 0);
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx: failed to allocate the output frame.\n");
        goto fail;
    }

    out_tex = (ID3D11Texture2D *)out->data[0];
    out_slice = (UINT)(intptr_t)out->data[1];

    ret = vfx_process(ctx, s, in, in_tex, in_slice, out_tex, out_slice);
    if (ret < 0)
        goto fail;

    ret = av_frame_copy_props(out, in);
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx: failed to copy frame properties.\n");
        goto fail;
    }

    if (nvvfx_hdr_mode(s) || (nvvfx_10bit_mode(s) && s->hdr_out)) {
        ret = nvvfx_attach_hdr10_metadata(s, out);
        if (ret < 0) {
            av_log(ctx, AV_LOG_ERROR, "nvvfx: failed to attach HDR10 metadata.\n");
            goto fail;
        }
        av_log(ctx, AV_LOG_VERBOSE, "nvvfx: HDR10 output %dx%d X2BGR10 "
               "(trc=%d, pri=%d, spc=%d, range=%d, side data %d)\n",
               s->out_width, s->out_height, out->color_trc,
               out->color_primaries, out->colorspace, out->color_range,
               out->nb_side_data);
    } else if (nvvfx_10bit_mode(s) && s->packed_out) {
        /* Odd SDR output is packed full-range RGB (P010 cannot represent odd
         * dimensions). Keep the input's SDR transfer/primaries. */
        out->colorspace = AVCOL_SPC_RGB;
        out->color_range = AVCOL_RANGE_JPEG;
        if (out->color_primaries == AVCOL_PRI_UNSPECIFIED)
            out->color_primaries = AVCOL_PRI_BT709;
        if (out->color_trc == AVCOL_TRC_UNSPECIFIED)
            out->color_trc = AVCOL_TRC_BT709;
        out->chroma_location = AVCHROMA_LOC_UNSPECIFIED;
        av_log(ctx, AV_LOG_VERBOSE, "nvvfx: SDR 10-bit packed output %dx%d "
               "X2BGR10 (trc=%d, pri=%d)\n", s->out_width, s->out_height,
               out->color_trc, out->color_primaries);
    } else if (nvvfx_10bit_mode(s)) {
        /* SDR 10-bit: the video processor writes BT.709 limited-range P010. */
        out->color_range = AVCOL_RANGE_MPEG;
        if (out->colorspace == AVCOL_SPC_UNSPECIFIED ||
            out->colorspace == AVCOL_SPC_RGB)
            out->colorspace = AVCOL_SPC_BT709;
        if (out->color_primaries == AVCOL_PRI_UNSPECIFIED)
            out->color_primaries = AVCOL_PRI_BT709;
        if (out->color_trc == AVCOL_TRC_UNSPECIFIED)
            out->color_trc = AVCOL_TRC_BT709;
        out->chroma_location = AVCHROMA_LOC_LEFT;
    } else if (s->vp_needed && (s->out_sw_format == AV_PIX_FMT_BGRA ||
                                s->out_sw_format == AV_PIX_FMT_RGBA)) {
        /* The video processor writes full-range RGB. */
        out->color_range = AVCOL_RANGE_JPEG;
    }

    out->width = s->out_width;
    out->height = s->out_height;

    av_frame_free(&in);
    return ff_filter_frame(outlink, out);

fail:
    av_frame_free(&in);
    av_frame_free(&out);
    return ret;
}

static av_cold void nvvfx_uninit(AVFilterContext *ctx)
{
    NVVFXContext *s = ctx->priv;

    vfx_destroy(s);

    if (s->video_context1) {
        s->video_context1->lpVtbl->Release(s->video_context1);
        s->video_context1 = NULL;
    }
    if (s->video_context) {
        s->video_context->lpVtbl->Release(s->video_context);
        s->video_context = NULL;
    }
    if (s->video_device) {
        s->video_device->lpVtbl->Release(s->video_device);
        s->video_device = NULL;
    }

    av_buffer_unref(&s->hw_frames_ctx_out);
    av_buffer_unref(&s->hw_device_ctx);

    av_freep(&s->lib_dir);
    av_freep(&s->dep_dir);
}

static const AVFilterPad nvvfx_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = nvvfx_filter_frame,
    },
};

static const AVFilterPad nvvfx_outputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = nvvfx_config_props,
    },
};

#define OFFSET(x) offsetof(NVVFXContext, x)
#define FLAGS (AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_VIDEO_PARAM)

static const AVOption nvvfx_options[] = {
    { "mode", "processing mode", OFFSET(mode), AV_OPT_TYPE_INT,
      { .i64 = MODE_SUPERRES }, 0, MODE_NB - 1, FLAGS, "mode" },
    { "superres", "super resolution", 0, AV_OPT_TYPE_CONST,
      { .i64 = MODE_SUPERRES }, 0, 0, FLAGS, "mode" },
    { "denoise", "denoise", 0, AV_OPT_TYPE_CONST,
      { .i64 = MODE_DENOISE }, 0, 0, FLAGS, "mode" },
    { "deblur", "deblur", 0, AV_OPT_TYPE_CONST,
      { .i64 = MODE_DEBLUR }, 0, 0, FLAGS, "mode" },
    { "artifact", "compression artifact reduction", 0, AV_OPT_TYPE_CONST,
      { .i64 = MODE_ARTIFACT }, 0, 0, FLAGS, "mode" },
    { "hdr", "SDR to HDR10 conversion (TrueHDR)", 0, AV_OPT_TYPE_CONST,
      { .i64 = MODE_HDR }, 0, 0, FLAGS, "mode" },
    { "quality", "quality level", OFFSET(quality), AV_OPT_TYPE_INT,
      { .i64 = QUALITY_HIGH }, 0, QUALITY_NB - 1, FLAGS, "quality" },
    { "low", "low quality", 0, AV_OPT_TYPE_CONST,
      { .i64 = QUALITY_LOW }, 0, 0, FLAGS, "quality" },
    { "medium", "medium quality", 0, AV_OPT_TYPE_CONST,
      { .i64 = QUALITY_MEDIUM }, 0, 0, FLAGS, "quality" },
    { "high", "high quality", 0, AV_OPT_TYPE_CONST,
      { .i64 = QUALITY_HIGH }, 0, 0, FLAGS, "quality" },
    { "ultra", "ultra quality", 0, AV_OPT_TYPE_CONST,
      { .i64 = QUALITY_ULTRA }, 0, 0, FLAGS, "quality" },
    { "conservative", "conservative artifact reduction", 0, AV_OPT_TYPE_CONST,
      { .i64 = QUALITY_LOW }, 0, 0, FLAGS, "quality" },
    { "aggressive", "aggressive artifact reduction", 0, AV_OPT_TYPE_CONST,
      { .i64 = QUALITY_MEDIUM }, 0, 0, FLAGS, "quality" },
    { "scale", "super resolution scale factor", OFFSET(scale), AV_OPT_TYPE_INT,
      { .i64 = 2 }, 2, 4, FLAGS },
    { "out_w", "super resolution output width (0 = width * scale)", OFFSET(out_w),
      AV_OPT_TYPE_INT, { .i64 = 0 }, 0, NVVFX_MAX_DIM, FLAGS },
    { "out_h", "super resolution output height (0 = height * scale)", OFFSET(out_h),
      AV_OPT_TYPE_INT, { .i64 = 0 }, 0, NVVFX_MAX_DIM, FLAGS },
    { "strength", "super resolution strength", OFFSET(strength), AV_OPT_TYPE_FLOAT,
      { .dbl = 1.0 }, 0.0, 1.0, FLAGS },
    { "bit_depth", "input/output bit depth for VideoSuperRes", OFFSET(bit_depth),
      AV_OPT_TYPE_INT, { .i64 = 8 }, 8, 10, FLAGS, "bit_depth" },
    { "8", "8-bit RGBA/BGRA", 0, AV_OPT_TYPE_CONST,
      { .i64 = 8 }, 0, 0, FLAGS, "bit_depth" },
    { "10", "10-bit RGB10A2", 0, AV_OPT_TYPE_CONST,
      { .i64 = 10 }, 0, 0, FLAGS, "bit_depth" },
    { "hdr", "HDR/PQ handling for 10-bit VideoSuperRes", OFFSET(hdr),
      AV_OPT_TYPE_INT, { .i64 = HDR_AUTO }, 0, HDR_NB - 1, FLAGS, "hdr" },
    { "auto", "detect PQ/BT.2020 from the link's HDR hints", 0, AV_OPT_TYPE_CONST,
      { .i64 = HDR_AUTO }, 0, 0, FLAGS, "hdr" },
    { "on", "force the PQ/BT.2020 X2BGR10 output path", 0, AV_OPT_TYPE_CONST,
      { .i64 = HDR_ON }, 0, 0, FLAGS, "hdr" },
    { "off", "keep the SDR P010 output path", 0, AV_OPT_TYPE_CONST,
      { .i64 = HDR_OFF }, 0, 0, FLAGS, "hdr" },
    { "contrast", "TrueHDR contrast", OFFSET(contrast), AV_OPT_TYPE_INT,
      { .i64 = 100 }, 0, 200, FLAGS },
    { "saturation", "TrueHDR saturation", OFFSET(saturation), AV_OPT_TYPE_INT,
      { .i64 = 100 }, 0, 200, FLAGS },
    { "middle_gray", "TrueHDR middle gray", OFFSET(middle_gray), AV_OPT_TYPE_INT,
      { .i64 = 50 }, 10, 100, FLAGS },
    { "luminance", "TrueHDR target peak luminance in nits", OFFSET(luminance),
      AV_OPT_TYPE_INT, { .i64 = 650 }, 400, 2000, FLAGS },
    { "debanding_off", "TrueHDR: skip the DL-debanding pass", OFFSET(debanding_off),
      AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 1, FLAGS },
    { "lib_dir", "directory containing the split NVVFX runtime", OFFSET(lib_dir),
      AV_OPT_TYPE_STRING, { .str = "" }, 0, 0, FLAGS },
    { "dep_dir", "directory containing the legacy NVIDIA Video Effects runtime",
      OFFSET(dep_dir), AV_OPT_TYPE_STRING, { .str = "" }, 0, 0, FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(nvvfx);

const FFFilter ff_vf_nvvfx = {
    .p.name           = "nvvfx",
    .p.description    = NULL_IF_CONFIG_SMALL("NVIDIA Video Effects"),
    .priv_size        = sizeof(NVVFXContext),
    .p.priv_class     = &nvvfx_class,
    .init             = nvvfx_init,
    .uninit           = nvvfx_uninit,
    FILTER_INPUTS(nvvfx_inputs),
    FILTER_OUTPUTS(nvvfx_outputs),
    FILTER_SINGLE_PIXFMT(AV_PIX_FMT_D3D11),
    .p.flags          = AVFILTER_FLAG_HWDEVICE,
    .flags_internal   = FF_FILTER_FLAG_HWFRAME_AWARE,
};
