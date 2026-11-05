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

/**
 * @file
 * NVIDIA Video Effects Upscale.
 *
 * The effect takes one image and produces a sharpened/enhanced image whose
 * dimensions are an exact rational multiple of the input.  The ratios
 * accepted by the NVIDIA runtime are 4:3, 3:2, 2:1, 3:1 and 4:1; the output
 * width and height must be integers and both must use the same ratio.
 *
 * Input is one D3D11 hardware frame (NV12, P010, BGRA or RGBA), output is a
 * D3D11 BGRA hardware frame.  NV12/P010 is converted to BGRA by the D3D11
 * video processor; P010 is 10-bit and is therefore downconverted to 8 bits
 * here, because the Upscale effect only accepts 8-bit RGBA images.
 *
 * The effect accepts only chunky RGBA U8 NvCVImages, so the D3D11 BGRA data
 * is transferred into a GPU BGRA image, converted GPU-to-GPU to RGBA, run
 * through the effect, converted back to BGRA and transferred into the output
 * D3D11 texture.
 *
 * The effect derives the scale factor from the source and destination image
 * dimensions.  NvVFX_Load() does not validate the pair: an unsupported
 * ratio or output size only fails later, from NvVFX_Run(), with -16
 * ("image resolution is not supported").  The filter validates the ratio and
 * the exact integer output dimensions while configuring the links, so a bad
 * combination is reported before any frame is processed.
 */

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>

#include <initguid.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <d3dcommon.h>

#include "libavutil/avstring.h"
#include "libavutil/error.h"
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_d3d11va.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"

#include "filters.h"
#include "video.h"

/* Minimal NVVFX ABI, copied from the MIT-licensed nvCVImage.h and
 * nvVideoEffects.h.  Keeping a local copy avoids an SDK build dependency. */

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
    NVCV_RGBA   = 6,
    NVCV_BGRA   = 7,
    NVCV_U8     = 1,
    NVCV_CHUNKY = 0,
    NVCV_GPU    = 1,
};

typedef NvCV_Status (__cdecl *PFN_NvVFX_CreateEffect)(const char *, NvVFX_Handle *);
typedef void (__cdecl *PFN_NvVFX_DestroyEffect)(NvVFX_Handle);
typedef NvCV_Status (__cdecl *PFN_NvVFX_SetU32)(NvVFX_Handle, const char *, unsigned int);
typedef NvCV_Status (__cdecl *PFN_NvVFX_SetF32)(NvVFX_Handle, const char *, float);
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

#define NVVFX_EFFECT       "Upscale"
#define NVVFX_SRC_IMAGE    "SrcImage0"
#define NVVFX_DST_IMAGE    "DstImage0"
#define NVVFX_STRENGTH     "Strength"
#define NVVFX_MODEL_BATCH  "ModelBatch"
#define NVVFX_CUDA_STREAM  "CudaStream"

typedef struct NVVFXUPContext {
    const AVClass *class;

    char *ratio;
    float strength;
    char *lib_dir;
    char *dep_dir;

    int ratio_num;
    int ratio_den;

    ID3D11Device *device;
    ID3D11DeviceContext *device_ctx;
    ID3D11VideoDevice *video_device;
    ID3D11VideoContext *video_context;
    ID3D11VideoProcessorEnumerator *vp_enum;
    ID3D11VideoProcessor *vp;
    ID3D11VideoProcessorOutputView *vp_out_view;

    AVBufferRef *hw_device_ctx;
    AVBufferRef *hw_frames_ctx_out;

    enum AVPixelFormat in_sw_format;
    enum AVPixelFormat out_sw_format;

    HMODULE h_nvcv;
    HMODULE h_vfx;
    HMODULE h_ngxruntime;
    HMODULE h_feature;

    PFN_NvVFX_CreateEffect CreateEffect;
    PFN_NvVFX_DestroyEffect DestroyEffect;
    PFN_NvVFX_SetU32 SetU32;
    PFN_NvVFX_SetF32 SetF32;
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
    ID3D11Query *sync_query;

    NvCVImage in_d3d;
    NvCVImage in_copy_d3d;
    NvCVImage out_d3d;
    NvCVImage in_gpu;
    NvCVImage src_rgba;
    NvCVImage out_rgba;
    NvCVImage out_bgra;

    int width;
    int height;
    int out_width;
    int out_height;
    int pixfmt;
    int vp_needed;
    DXGI_FORMAT in_format;
    int ready;
} NVVFXUPContext;

static const char *up_err(NVVFXUPContext *s, NvCV_Status status)
{
    return s->ErrorString ? s->ErrorString(status) : "unknown error";
}

static const struct {
    const char *name;
    int num;
    int den;
} up_ratios[] = {
    { "4/3", 4, 3 },
    { "3/2", 3, 2 },
    { "2",   2, 1 },
    { "3",   3, 1 },
    { "4",   4, 1 },
};

static av_cold int nvvfx_upscale_init(AVFilterContext *ctx)
{
    NVVFXUPContext *s = ctx->priv;
    int i;

    for (i = 0; i < FF_ARRAY_ELEMS(up_ratios); i++) {
        if (!strcmp(s->ratio, up_ratios[i].name)) {
            s->ratio_num = up_ratios[i].num;
            s->ratio_den = up_ratios[i].den;
            break;
        }
    }

    if (!s->ratio_num) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_upscale: unsupported ratio \"%s\"; "
               "expected one of 4/3, 3/2, 2, 3 or 4.\n", s->ratio);
        return AVERROR(EINVAL);
    }

    if (!(s->strength >= 0.0f && s->strength <= 1.0f)) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_upscale: strength must be in the "
               "range [0, 1].\n");
        return AVERROR(EINVAL);
    }

    s->out_sw_format = AV_PIX_FMT_BGRA;
    return 0;
}

/* The NVIDIA runtime checks ow/iw == oh/ih and both being exact integers, but
 * only from NvVFX_Run().  Do the same checks while configuring the graph and
 * return a clear error instead of failing on the first frame. */
static int up_validate_dims(AVFilterContext *ctx, NVVFXUPContext *s,
                            int width, int height,
                            int *out_width, int *out_height)
{
    int64_t scaled_w = (int64_t)width * s->ratio_num;
    int64_t scaled_h = (int64_t)height * s->ratio_num;
    int64_t ow, oh;

    if (width <= 0 || height <= 0) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_upscale: invalid input dimensions "
               "%dx%d.\n", width, height);
        return AVERROR(EINVAL);
    }

    if (scaled_w % s->ratio_den || scaled_h % s->ratio_den) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_upscale: ratio %s does not produce "
               "integer output dimensions for %dx%d (got %"PRId64"x%"PRId64" "
               "at 1/%d resolution); adjust the input size or use another "
               "ratio.\n", s->ratio, width, height,
               scaled_w / s->ratio_den, scaled_h / s->ratio_den,
               s->ratio_den);
        return AVERROR(EINVAL);
    }

    ow = scaled_w / s->ratio_den;
    oh = scaled_h / s->ratio_den;

    /* Redundant with the division above, but these are the two conditions the
     * effect documents: exact integer output and identical w/h ratio. */
    if (ow * s->ratio_den != (int64_t)width * s->ratio_num ||
        oh * s->ratio_den != (int64_t)height * s->ratio_num) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_upscale: output %"PRId64"x%"PRId64" "
               "does not have the exact ratio %s for input %dx%d.\n",
               ow, oh, s->ratio, width, height);
        return AVERROR(EINVAL);
    }

    if (ow <= width || oh <= height) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_upscale: ratio %s is not an upscale "
               "for %dx%d (output %"PRId64"x%"PRId64").\n",
               s->ratio, width, height, ow, oh);
        return AVERROR(EINVAL);
    }

    if (ow > INT_MAX || oh > INT_MAX) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_upscale: output dimensions %"PRId64
               "x%"PRId64" are too large.\n", ow, oh);
        return AVERROR(EINVAL);
    }

    *out_width = (int)ow;
    *out_height = (int)oh;
    return 0;
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

static int up_load_libs(AVFilterContext *ctx, NVVFXUPContext *s)
{
    /* The split runtime needs the old NPP/CUDA/TRT partner DLLs, which live
     * in the Program Files SDK.  The Upscale effect itself uses NGX models
     * supplied by the driver, so no ModelDir is required. */
    static const char *const deps[] = {
        "nppc64_12.dll", "nppial64_12.dll", "nppicc64_12.dll",
        "nppidei64_12.dll", "nppif64_12.dll", "nppig64_12.dll",
        "nppim64_12.dll", "nppist64_12.dll", "nppitc64_12.dll",
        "cudart64_12.dll", "nvinfer_10.dll", "nvinfer_plugin_10.dll",
        "nvonnxparser_10.dll",
    };
    char old_dir[NVVFX_MAX_PATH];
    char dirs[8][NVVFX_MAX_PATH] = { 0 };
    char exe_dir[MAX_PATH];
    const char *lib_dir = NULL;
    const char *dep_source;
    const char *env_dir;
    int nb_dirs = 0;
    int have_old;
    int i;

    if (s->libs_loaded)
        return 0;

    snprintf(old_dir, sizeof(old_dir), "%s",
             s->dep_dir && s->dep_dir[0] ? s->dep_dir :
             "C:\\Program Files\\NVIDIA Corporation\\NVIDIA Video Effects");

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
             "C:\\kilo\\NVIDIA_VFX_SDK_1.3.0\\flat");
    snprintf(dirs[nb_dirs++], NVVFX_MAX_PATH,
             "C:\\kilo\\NVIDIA_VFX_SDK_1.3.0\\sdk\\VideoFX\\bin");
    snprintf(dirs[nb_dirs++], NVVFX_MAX_PATH,
             "C:\\Program Files\\NVIDIA Corporation\\NVIDIA Video Effects");

    for (i = 0; i < nb_dirs; i++) {
        if (dir_has_file(dirs[i], "nvVFXUpscale.dll")) {
            lib_dir = dirs[i];
            break;
        }
    }
    if (!lib_dir) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_upscale: could not find "
               "nvVFXUpscale.dll. Install the NVIDIA Video Effects SDK or set "
               "the lib_dir option.\n");
        return AVERROR_EXTERNAL;
    }

    have_old = dir_has_file(old_dir, "NVVideoEffects.dll");
    dep_source = have_old ? old_dir : lib_dir;

    av_log(ctx, AV_LOG_VERBOSE, "nvvfx_upscale: runtime %s, legacy runtime "
           "%s\n", lib_dir, have_old ? old_dir : "not found");

    /* Missing partner DLLs are not fatal; not every effect needs every
     * dependency. */
    for (i = 0; i < FF_ARRAY_ELEMS(deps); i++) {
        if (!load_dll(dep_source, deps[i]))
            av_log(ctx, AV_LOG_VERBOSE, "nvvfx_upscale: could not load %s\n",
                   deps[i]);
    }

    /* Upscale only needs RGBA/BGRA U8, so the legacy NVCVImage is preferred;
     * fall back to the one shipped next to the split runtime. */
    s->h_nvcv = load_dll(dir_has_file(old_dir, "NVCVImage.dll") ?
                         old_dir : lib_dir, "NVCVImage.dll");
    s->h_ngxruntime = load_dll(lib_dir, "nvngxruntime.dll");
    s->h_vfx = load_dll(lib_dir, "NVVideoEffects.dll");
    s->h_feature = load_dll(lib_dir, "nvVFXUpscale.dll");

    if (!s->h_nvcv || !s->h_vfx || !s->h_ngxruntime || !s->h_feature) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_upscale: failed to load the NVIDIA "
               "Video Effects runtime from %s (error %lu).\n", lib_dir,
               GetLastError());
        return AVERROR_EXTERNAL;
    }

    s->CreateEffect = (PFN_NvVFX_CreateEffect)GetProcAddress(s->h_vfx, "NvVFX_CreateEffect");
    s->DestroyEffect = (PFN_NvVFX_DestroyEffect)GetProcAddress(s->h_vfx, "NvVFX_DestroyEffect");
    s->SetU32 = (PFN_NvVFX_SetU32)GetProcAddress(s->h_vfx, "NvVFX_SetU32");
    s->SetF32 = (PFN_NvVFX_SetF32)GetProcAddress(s->h_vfx, "NvVFX_SetF32");
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

    if (!s->CreateEffect || !s->DestroyEffect || !s->SetU32 || !s->SetF32 ||
        !s->SetImage || !s->SetCudaStream || !s->Load || !s->Run ||
        !s->CudaStreamCreate || !s->CudaStreamDestroy ||
        !s->CudaStreamSynchronize || !s->Alloc || !s->Dealloc ||
        !s->Transfer || !s->InitFromD3D11Texture || !s->MapResource ||
        !s->UnmapResource) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_upscale: runtime is missing "
               "required symbols.\n");
        return AVERROR_EXTERNAL;
    }

    s->libs_loaded = 1;
    return 0;
}

static void release_video_processor(NVVFXUPContext *s)
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
}

static void up_destroy(NVVFXUPContext *s)
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
        if (s->in_gpu.pixels || s->in_gpu.deletePtr)
            s->Dealloc(&s->in_gpu);
        if (s->src_rgba.pixels || s->src_rgba.deletePtr)
            s->Dealloc(&s->src_rgba);
        if (s->out_rgba.pixels || s->out_rgba.deletePtr)
            s->Dealloc(&s->out_rgba);
        if (s->out_bgra.pixels || s->out_bgra.deletePtr)
            s->Dealloc(&s->out_bgra);
    }
    memset(&s->in_d3d, 0, sizeof(s->in_d3d));
    memset(&s->in_copy_d3d, 0, sizeof(s->in_copy_d3d));
    memset(&s->out_d3d, 0, sizeof(s->out_d3d));
    memset(&s->in_gpu, 0, sizeof(s->in_gpu));
    memset(&s->src_rgba, 0, sizeof(s->src_rgba));
    memset(&s->out_rgba, 0, sizeof(s->out_rgba));
    memset(&s->out_bgra, 0, sizeof(s->out_bgra));

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
    s->vp_needed = 0;
    s->in_format = DXGI_FORMAT_UNKNOWN;
}

static int create_texture(NVVFXUPContext *s, int width, int height,
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

static int vp_create(AVFilterContext *ctx, NVVFXUPContext *s, int width, int height,
                     ID3D11Texture2D *target)
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
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE csp = {
        .Usage = 0,
        .RGB_Range = 0,
        .Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255,
    };
    RECT src_rect = {
        .right = width,
        .bottom = height,
    };
    HRESULT hr;

    hr = s->video_device->lpVtbl->CreateVideoProcessorEnumerator(s->video_device,
                                                                 &content,
                                                                 &s->vp_enum);
    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_upscale: failed to create the video "
               "processor enumerator (HRESULT 0x%lX).\n", hr);
        return AVERROR_EXTERNAL;
    }

    hr = s->video_device->lpVtbl->CreateVideoProcessor(s->video_device,
                                                        s->vp_enum, 0, &s->vp);
    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_upscale: failed to create the video "
               "processor (HRESULT 0x%lX).\n", hr);
        return AVERROR_EXTERNAL;
    }

    hr = s->video_device->lpVtbl->CreateVideoProcessorOutputView(s->video_device,
                                                                 (ID3D11Resource *)target,
                                                                 s->vp_enum, &out_desc,
                                                                 &s->vp_out_view);
    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_upscale: failed to create the video "
               "processor output view (HRESULT 0x%lX).\n", hr);
        return AVERROR_EXTERNAL;
    }

    s->video_context->lpVtbl->VideoProcessorSetStreamSourceRect(s->video_context,
                                                                s->vp, 0, TRUE,
                                                                &src_rect);
    s->video_context->lpVtbl->VideoProcessorSetStreamAutoProcessingMode(s->video_context,
                                                                        s->vp, 0, FALSE);
    s->video_context->lpVtbl->VideoProcessorSetOutputColorSpace(s->video_context,
                                                                s->vp, &csp);
    return 0;
}

static void vp_set_stream_colorspace(NVVFXUPContext *s, const AVFrame *frame)
{
    int matrix_601;

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
        matrix_601 = frame->height <= 576;
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

static int up_setup(AVFilterContext *ctx, NVVFXUPContext *s,
                    int width, int height, int out_width, int out_height,
                    DXGI_FORMAT in_format, int pixfmt, int vp_needed)
{
    int expect_w, expect_h;
    DXGI_FORMAT tex_format = vp_needed ? DXGI_FORMAT_B8G8R8A8_UNORM : in_format;
    D3D11_QUERY_DESC query_desc = { .Query = D3D11_QUERY_EVENT };
    NvCV_Status st;
    HRESULT hr;
    int ret;

    ret = up_validate_dims(ctx, s, width, height, &expect_w, &expect_h);
    if (ret < 0)
        return ret;
    if (expect_w != out_width || expect_h != out_height) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_upscale: frame %dx%d would produce "
               "%dx%d, but the output pool is %dx%d; the input changed "
               "resolution without a graph reconfiguration.\n",
               width, height, expect_w, expect_h, out_width, out_height);
        return AVERROR(EINVAL);
    }

    if (s->ready && s->width == width && s->height == height &&
        s->out_width == out_width && s->out_height == out_height &&
        s->pixfmt == pixfmt && s->vp_needed == vp_needed &&
        s->in_format == in_format)
        return 0;

    up_destroy(s);
    s->in_format = in_format;

    if ((ret = create_texture(s, width, height, tex_format,
                              D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
                              &s->in_tex)) < 0 ||
        (ret = create_texture(s, out_width, out_height, DXGI_FORMAT_B8G8R8A8_UNORM,
                              D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
                              &s->out_tex)) < 0) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_upscale: failed to create D3D11 "
               "textures.\n");
        goto fail;
    }

    /* Normalize the input into a texture we own: decoder textures are padded
     * arrays, and hwupload textures carry no video-processor bind flags. */
    if (vp_needed) {
        UINT yuv_bind = (in_format == DXGI_FORMAT_NV12 ||
                         in_format == DXGI_FORMAT_P010) ?
                        (D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_DECODER) :
                        (D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);

        if ((ret = create_texture(s, width, height, in_format, yuv_bind,
                                  &s->in_yuv_tex)) < 0 ||
            (ret = create_texture(s, width, height, DXGI_FORMAT_B8G8R8A8_UNORM,
                                  D3D11_BIND_SHADER_RESOURCE |
                                  D3D11_BIND_RENDER_TARGET,
                                  &s->in_copy_tex)) < 0) {
            av_log(ctx, AV_LOG_ERROR, "nvvfx_upscale: failed to create the "
                   "staging textures.\n");
            goto fail;
        }
    }

    hr = s->device->lpVtbl->CreateQuery(s->device, &query_desc, &s->sync_query);
    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_upscale: failed to create the sync "
               "query (HRESULT 0x%lX).\n", hr);
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    st = s->InitFromD3D11Texture(&s->in_d3d, s->in_tex);
    if (st) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_upscale: failed to register the input "
               "texture (%s).\n", up_err(s, st));
        ret = AVERROR_EXTERNAL;
        goto fail;
    }
    st = s->InitFromD3D11Texture(&s->out_d3d, s->out_tex);
    if (st) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_upscale: failed to register the output "
               "texture (%s).\n", up_err(s, st));
        ret = AVERROR_EXTERNAL;
        goto fail;
    }
    if (s->in_copy_tex) {
        st = s->InitFromD3D11Texture(&s->in_copy_d3d, s->in_copy_tex);
        if (st) {
            av_log(ctx, AV_LOG_ERROR, "nvvfx_upscale: failed to register the "
                   "copy texture (%s).\n", up_err(s, st));
            ret = AVERROR_EXTERNAL;
            goto fail;
        }
    }

    st = s->Alloc(&s->in_gpu, width, height, pixfmt, NVCV_U8,
                  NVCV_CHUNKY, NVCV_GPU, 32);
    if (st) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_upscale: failed to allocate the input "
               "image (%s).\n", up_err(s, st));
        ret = AVERROR_EXTERNAL;
        goto fail;
    }
    st = s->Alloc(&s->src_rgba, width, height, NVCV_RGBA, NVCV_U8,
                  NVCV_CHUNKY, NVCV_GPU, 32);
    if (st) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_upscale: failed to allocate the RGBA "
               "input image (%s).\n", up_err(s, st));
        ret = AVERROR_EXTERNAL;
        goto fail;
    }
    st = s->Alloc(&s->out_rgba, out_width, out_height, NVCV_RGBA, NVCV_U8,
                  NVCV_CHUNKY, NVCV_GPU, 32);
    if (st) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_upscale: failed to allocate the RGBA "
               "output image (%s).\n", up_err(s, st));
        ret = AVERROR_EXTERNAL;
        goto fail;
    }
    st = s->Alloc(&s->out_bgra, out_width, out_height, NVCV_BGRA, NVCV_U8,
                  NVCV_CHUNKY, NVCV_GPU, 32);
    if (st) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_upscale: failed to allocate the BGRA "
               "output image (%s).\n", up_err(s, st));
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    if (vp_needed) {
        ret = vp_create(ctx, s, width, height, s->in_tex);
        if (ret < 0)
            goto fail;
    }

    st = s->CreateEffect(NVVFX_EFFECT, &s->effect);
    if (st) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_upscale: failed to create the Upscale "
               "effect (%s).\n", up_err(s, st));
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    st = s->SetF32(s->effect, NVVFX_STRENGTH, s->strength);
    if (st) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_upscale: failed to set Strength=%g "
               "(%s).\n", s->strength, up_err(s, st));
        ret = AVERROR_EXTERNAL;
        goto fail;
    }
    st = s->SetU32(s->effect, NVVFX_MODEL_BATCH, 1);
    if (st) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_upscale: failed to set ModelBatch=1 "
               "(%s).\n", up_err(s, st));
        ret = AVERROR_EXTERNAL;
        goto fail;
    }
    st = s->SetImage(s->effect, NVVFX_SRC_IMAGE, &s->src_rgba);
    if (st) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_upscale: failed to set the source "
               "image (%s).\n", up_err(s, st));
        ret = AVERROR_EXTERNAL;
        goto fail;
    }
    st = s->SetImage(s->effect, NVVFX_DST_IMAGE, &s->out_rgba);
    if (st) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_upscale: failed to set the destination "
               "image (%s).\n", up_err(s, st));
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    if (!s->stream) {
        st = s->CudaStreamCreate(&s->stream);
        if (st) {
            av_log(ctx, AV_LOG_ERROR, "nvvfx_upscale: failed to create the "
                   "CUDA stream (%s).\n", up_err(s, st));
            ret = AVERROR_EXTERNAL;
            goto fail;
        }
    }

    st = s->SetCudaStream(s->effect, NVVFX_CUDA_STREAM, s->stream);
    if (st) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_upscale: failed to set the CUDA "
               "stream (%s).\n", up_err(s, st));
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    st = s->Load(s->effect);
    if (st) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_upscale: failed to load the Upscale "
               "model (%s).\n", up_err(s, st));
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    s->width = width;
    s->height = height;
    s->out_width = out_width;
    s->out_height = out_height;
    s->pixfmt = pixfmt;
    s->vp_needed = vp_needed;
    s->in_format = in_format;
    s->ready = 1;

    av_log(ctx, AV_LOG_VERBOSE, "nvvfx_upscale: Upscale ready (%dx%d -> %dx%d, "
           "ratio=%s, strength=%g)\n", width, height, out_width, out_height,
           s->ratio, s->strength);
    return 0;

fail:
    up_destroy(s);
    return ret;
}

static int d3d11_sync(AVFilterContext *ctx, NVVFXUPContext *s)
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
        av_log(ctx, AV_LOG_ERROR, "nvvfx_upscale: D3D11 sync query failed "
               "(HRESULT 0x%lX).\n", hr);
        return AVERROR_EXTERNAL;
    }
    return 0;
}

static int vp_blt(AVFilterContext *ctx, NVVFXUPContext *s)
{
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC view_desc = {
        .FourCC = s->in_format,
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
                                                                (ID3D11Resource *)s->in_yuv_tex,
                                                                s->vp_enum, &view_desc,
                                                                &view);
    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_upscale: failed to create the video "
               "processor input view (HRESULT 0x%lX).\n", hr);
        return AVERROR_EXTERNAL;
    }

    stream.pInputSurface = view;

    hr = s->video_context->lpVtbl->VideoProcessorBlt(s->video_context, s->vp,
                                                     s->vp_out_view, 0, 1, &stream);
    view->lpVtbl->Release(view);

    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_upscale: VideoProcessorBlt failed "
               "(HRESULT 0x%lX).\n", hr);
        return AVERROR_EXTERNAL;
    }
    return 0;
}

static int up_process(AVFilterContext *ctx, NVVFXUPContext *s, const AVFrame *frame,
                      ID3D11Texture2D *in_tex, UINT in_slice,
                      ID3D11Texture2D *out_tex, UINT out_slice)
{
    NvCVImage *wrapper;
    NvCV_Status st;
    int ret;

    if (!s->ready)
        return AVERROR(EINVAL);

    if (s->vp_needed) {
        vp_set_stream_colorspace(s, frame);
        s->device_ctx->lpVtbl->CopySubresourceRegion(s->device_ctx,
                                                     (ID3D11Resource *)s->in_yuv_tex, 0,
                                                     0, 0, 0,
                                                     (ID3D11Resource *)in_tex,
                                                     in_slice, NULL);
        if ((ret = vp_blt(ctx, s)) < 0)
            return ret;
        /* CUDA can see stale data when it maps a texture the video processor
         * just wrote, so map a copy made by the D3D11 context instead. */
        s->device_ctx->lpVtbl->CopySubresourceRegion(s->device_ctx,
                                                     (ID3D11Resource *)s->in_copy_tex, 0,
                                                     0, 0, 0,
                                                     (ID3D11Resource *)s->in_tex, 0, NULL);
        wrapper = &s->in_copy_d3d;
    } else {
        s->device_ctx->lpVtbl->CopySubresourceRegion(s->device_ctx,
                                                     (ID3D11Resource *)s->in_tex, 0,
                                                     0, 0, 0,
                                                     (ID3D11Resource *)in_tex,
                                                     in_slice, NULL);
        wrapper = &s->in_d3d;
    }

    /* The copy/Blt is asynchronous; CUDA must not map the texture before the
     * GPU has finished writing it. */
    if ((ret = d3d11_sync(ctx, s)) < 0)
        return ret;

    st = s->MapResource(wrapper, s->stream);
    if (st == 0) {
        NvCV_Status transfer_st = s->Transfer(wrapper, &s->in_gpu, 1.0f,
                                              s->stream, NULL);
        NvCV_Status unmap_st = s->UnmapResource(wrapper, s->stream);
        st = transfer_st ? transfer_st : unmap_st;
    }
    if (st) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_upscale: input transfer failed (%s).\n",
               up_err(s, st));
        return AVERROR_EXTERNAL;
    }

    /* The effect accepts chunky RGBA U8 only; BGRA input is converted
     * GPU-to-GPU.  For RGBA input this is a same-format copy. */
    st = s->Transfer(&s->in_gpu, &s->src_rgba, 1.0f, s->stream, NULL);
    if (st) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_upscale: RGBA conversion failed (%s).\n",
               up_err(s, st));
        return AVERROR_EXTERNAL;
    }

    st = s->Run(s->effect, 0);
    if (st) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_upscale: inference failed (%s).\n",
               up_err(s, st));
        return AVERROR_EXTERNAL;
    }

    st = s->Transfer(&s->out_rgba, &s->out_bgra, 1.0f, s->stream, NULL);
    if (st) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_upscale: BGRA conversion failed (%s).\n",
               up_err(s, st));
        return AVERROR_EXTERNAL;
    }

    st = s->MapResource(&s->out_d3d, s->stream);
    if (st == 0) {
        NvCV_Status transfer_st = s->Transfer(&s->out_bgra, &s->out_d3d, 1.0f,
                                              s->stream, NULL);
        NvCV_Status unmap_st = s->UnmapResource(&s->out_d3d, s->stream);
        st = transfer_st ? transfer_st : unmap_st;
    }
    if (st) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_upscale: output transfer failed (%s).\n",
               up_err(s, st));
        return AVERROR_EXTERNAL;
    }

    if (s->CudaStreamSynchronize)
        s->CudaStreamSynchronize(s->stream);

    s->device_ctx->lpVtbl->CopySubresourceRegion(s->device_ctx,
                                                 (ID3D11Resource *)out_tex, out_slice,
                                                 0, 0, 0,
                                                 (ID3D11Resource *)s->out_tex,
                                                 0, NULL);
    s->device_ctx->lpVtbl->Flush(s->device_ctx);
    return 0;
}

static int nvvfx_upscale_config_props(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    NVVFXUPContext *s = ctx->priv;
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
    up_destroy(s);
    if (s->video_context) {
        s->video_context->lpVtbl->Release(s->video_context);
        s->video_context = NULL;
    }
    if (s->video_device) {
        s->video_device->lpVtbl->Release(s->video_device);
        s->video_device = NULL;
    }

    if (!inl->hw_frames_ctx) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_upscale: no input hw_frames_ctx.\n");
        return AVERROR(EINVAL);
    }

    in_frames = (AVHWFramesContext *)inl->hw_frames_ctx->data;
    s->in_sw_format = in_frames->sw_format;

    switch (s->in_sw_format) {
    case AV_PIX_FMT_NV12:
    case AV_PIX_FMT_P010:
        vp_needed = 1;
        break;
    case AV_PIX_FMT_BGRA:
    case AV_PIX_FMT_RGBA:
        vp_needed = 0;
        break;
    default:
        av_log(ctx, AV_LOG_ERROR, "nvvfx_upscale: unsupported input format "
               "%s, expected NV12, P010, BGRA or RGBA (8-bit; P010 is "
               "downconverted to 8-bit by the video processor).\n",
               av_get_pix_fmt_name(s->in_sw_format));
        return AVERROR(EINVAL);
    }

    if (s->in_sw_format == AV_PIX_FMT_P010)
        av_log(ctx, AV_LOG_VERBOSE, "nvvfx_upscale: P010 input is converted "
               "to 8-bit BGRA before the Upscale effect (8-bit only).\n");

    ret = up_validate_dims(ctx, s, inlink->w, inlink->h,
                           &out_width, &out_height);
    if (ret < 0)
        return ret;
    s->out_width = out_width;
    s->out_height = out_height;

    av_buffer_unref(&s->hw_device_ctx);
    s->hw_device_ctx = av_buffer_ref(in_frames->device_ref);
    if (!s->hw_device_ctx)
        return AVERROR(ENOMEM);
    hw_device = (AVHWDeviceContext *)s->hw_device_ctx->data;
    d3d11_hwctx = hw_device->hwctx;

    s->device = d3d11_hwctx->device;
    s->device_ctx = d3d11_hwctx->device_context;
    if (!s->device || !s->device_ctx) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_upscale: invalid D3D11 device or "
               "context.\n");
        return AVERROR(EINVAL);
    }

    if (vp_needed) {
        hr = s->device->lpVtbl->QueryInterface(s->device, &IID_ID3D11VideoDevice,
                                               (void **)&s->video_device);
        if (FAILED(hr)) {
            av_log(ctx, AV_LOG_ERROR, "nvvfx_upscale: failed to get the D3D11 "
                   "video device (HRESULT 0x%lX).\n", hr);
            return AVERROR_EXTERNAL;
        }
        hr = s->device_ctx->lpVtbl->QueryInterface(s->device_ctx,
                                                   &IID_ID3D11VideoContext,
                                                   (void **)&s->video_context);
        if (FAILED(hr)) {
            av_log(ctx, AV_LOG_ERROR, "nvvfx_upscale: failed to get the D3D11 "
                   "video context (HRESULT 0x%lX).\n", hr);
            return AVERROR_EXTERNAL;
        }
    }

    outlink->w = out_width;
    outlink->h = out_height;
    outlink->sample_aspect_ratio = inlink->sample_aspect_ratio;
    outl->frame_rate = inl->frame_rate;

    s->hw_frames_ctx_out = av_hwframe_ctx_alloc(s->hw_device_ctx);
    if (!s->hw_frames_ctx_out)
        return AVERROR(ENOMEM);

    out_frames = (AVHWFramesContext *)s->hw_frames_ctx_out->data;
    out_frames->format = AV_PIX_FMT_D3D11;
    out_frames->sw_format = s->out_sw_format;
    out_frames->width = out_width;
    out_frames->height = out_height;
    out_frames->initial_pool_size = 10;
    if (ctx->extra_hw_frames > 0)
        out_frames->initial_pool_size += ctx->extra_hw_frames;

    out_frames_hwctx = out_frames->hwctx;
    out_frames_hwctx->BindFlags = D3D11_BIND_RENDER_TARGET |
                                  D3D11_BIND_SHADER_RESOURCE;
    out_frames_hwctx->MiscFlags = 0;

    ret = av_hwframe_ctx_init(s->hw_frames_ctx_out);
    if (ret < 0) {
        av_buffer_unref(&s->hw_frames_ctx_out);
        return ret;
    }

    outl->hw_frames_ctx = av_buffer_ref(s->hw_frames_ctx_out);
    if (!outl->hw_frames_ctx)
        return AVERROR(ENOMEM);

    ret = up_load_libs(ctx, s);
    if (ret < 0)
        return ret;

    av_log(ctx, AV_LOG_VERBOSE, "nvvfx_upscale: configured %dx%d %s -> %dx%d "
           "%s (ratio=%s)\n", inlink->w, inlink->h,
           av_get_pix_fmt_name(s->in_sw_format), out_width, out_height,
           av_get_pix_fmt_name(s->out_sw_format), s->ratio);
    return 0;
}

static int nvvfx_upscale_filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext *ctx = inlink->dst;
    NVVFXUPContext *s = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];
    ID3D11Texture2D *in_tex;
    ID3D11Texture2D *out_tex;
    D3D11_TEXTURE2D_DESC desc;
    AVFrame *out = NULL;
    UINT in_slice, out_slice;
    int width, height;
    int pixfmt, vp_needed;
    int ret;

    if (in->format != AV_PIX_FMT_D3D11 || !in->hw_frames_ctx) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_upscale: input is not a D3D11 "
               "hardware frame.\n");
        av_frame_free(&in);
        return AVERROR(EINVAL);
    }
    if (!s->hw_frames_ctx_out) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_upscale: output frames context is "
               "not configured.\n");
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
        av_log(ctx, AV_LOG_ERROR, "nvvfx_upscale: frame size %dx%d exceeds the "
               "texture size %ux%u.\n", width, height, desc.Width, desc.Height);
        av_frame_free(&in);
        return AVERROR(EINVAL);
    }

    pixfmt = s->in_sw_format == AV_PIX_FMT_RGBA ? NVCV_RGBA : NVCV_BGRA;
    vp_needed = s->in_sw_format == AV_PIX_FMT_NV12 ||
                s->in_sw_format == AV_PIX_FMT_P010;

    ret = up_setup(ctx, s, width, height, s->out_width, s->out_height,
                   desc.Format, pixfmt, vp_needed);
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_upscale: failed to set up the "
               "effect.\n");
        goto fail;
    }

    out = av_frame_alloc();
    if (!out) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    ret = av_hwframe_get_buffer(s->hw_frames_ctx_out, out, 0);
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_upscale: failed to allocate the "
               "output frame.\n");
        goto fail;
    }

    out_tex = (ID3D11Texture2D *)out->data[0];
    out_slice = (UINT)(intptr_t)out->data[1];

    ret = up_process(ctx, s, in, in_tex, in_slice, out_tex, out_slice);
    if (ret < 0)
        goto fail;

    ret = av_frame_copy_props(out, in);
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_upscale: failed to copy frame "
               "properties.\n");
        goto fail;
    }

    /* av_frame_copy_props() adopts the source hw_frames_ctx, but the output
     * texture uses a different size and software format. */
    av_buffer_unref(&out->hw_frames_ctx);
    out->hw_frames_ctx = av_buffer_ref(s->hw_frames_ctx_out);
    if (!out->hw_frames_ctx) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    /* The D3D11 video processor and the effect both produce full-range RGB. */
    out->color_range = AVCOL_RANGE_JPEG;
    out->width = s->out_width;
    out->height = s->out_height;

    av_frame_free(&in);
    return ff_filter_frame(outlink, out);

fail:
    av_frame_free(&in);
    av_frame_free(&out);
    return ret;
}

static av_cold void nvvfx_upscale_uninit(AVFilterContext *ctx)
{
    NVVFXUPContext *s = ctx->priv;

    up_destroy(s);

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

    av_freep(&s->ratio);
    av_freep(&s->lib_dir);
    av_freep(&s->dep_dir);
}

static const AVFilterPad nvvfx_upscale_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = nvvfx_upscale_filter_frame,
    },
};

static const AVFilterPad nvvfx_upscale_outputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = nvvfx_upscale_config_props,
    },
};

#define OFFSET(x) offsetof(NVVFXUPContext, x)
#define FLAGS (AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_VIDEO_PARAM)

static const AVOption nvvfx_upscale_options[] = {
    { "ratio", "upscale ratio (4/3, 3/2, 2, 3 or 4)", OFFSET(ratio),
      AV_OPT_TYPE_STRING, { .str = "2" }, 0, 0, FLAGS },
    { "strength", "sharpening/enhancement strength", OFFSET(strength),
      AV_OPT_TYPE_FLOAT, { .dbl = 0.4 }, 0.0, 1.0, FLAGS },
    { "lib_dir", "directory containing the split NVVFX runtime", OFFSET(lib_dir),
      AV_OPT_TYPE_STRING, { .str = "" }, 0, 0, FLAGS },
    { "dep_dir", "directory containing the legacy NVIDIA Video Effects runtime",
      OFFSET(dep_dir), AV_OPT_TYPE_STRING, { .str = "" }, 0, 0, FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(nvvfx_upscale);

const FFFilter ff_vf_nvvfx_upscale = {
    .p.name           = "nvvfx_upscale",
    .p.description    = NULL_IF_CONFIG_SMALL("NVIDIA Video Effects Upscale"),
    .priv_size        = sizeof(NVVFXUPContext),
    .p.priv_class     = &nvvfx_upscale_class,
    .init             = nvvfx_upscale_init,
    .uninit           = nvvfx_upscale_uninit,
    FILTER_INPUTS(nvvfx_upscale_inputs),
    FILTER_OUTPUTS(nvvfx_upscale_outputs),
    FILTER_SINGLE_PIXFMT(AV_PIX_FMT_D3D11),
    .p.flags          = AVFILTER_FLAG_HWDEVICE,
    .flags_internal   = FF_FILTER_FLAG_HWFRAME_AWARE,
};
