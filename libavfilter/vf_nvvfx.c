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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include <initguid.h>
#include <d3d11.h>
#include <d3d11_1.h>

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
    NVCV_BGR    = 5,
    NVCV_RGBA   = 6,
    NVCV_BGRA   = 7,
    NVCV_U8     = 1,
    NVCV_F32    = 7,
    NVCV_CHUNKY = 0,
    NVCV_PLANAR = 1,
    NVCV_GPU    = 1,
};

typedef NvCV_Status (__cdecl *PFN_NvVFX_CreateEffect)(const char *, NvVFX_Handle *);
typedef void (__cdecl *PFN_NvVFX_DestroyEffect)(NvVFX_Handle);
typedef NvCV_Status (__cdecl *PFN_NvVFX_SetU32)(NvVFX_Handle, const char *, unsigned int);
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

#define NVVFX_INPUT_IMAGE  "SrcImage0"
#define NVVFX_OUTPUT_IMAGE "DstImage0"
#define NVVFX_CUDA_STREAM  "CudaStream"
#define NVVFX_QUALITY      "QualityLevel"
#define NVVFX_MODE         "Mode"
#define NVVFX_MODEL_DIR    "ModelDir"

enum NVVFXMode {
    MODE_SUPERRES,
    MODE_DENOISE,
    MODE_DEBLUR,
    MODE_ARTIFACT,
    MODE_NB
};

enum NVVFXQuality {
    QUALITY_LOW,
    QUALITY_MEDIUM,
    QUALITY_HIGH,
    QUALITY_ULTRA,
    QUALITY_NB
};

typedef struct NVVFXContext {
    const AVClass *class;

    int mode;
    int quality;
    int scale;
    char *lib_dir;
    char *dep_dir;

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
    HMODULE h_vsr;

    PFN_NvVFX_CreateEffect CreateEffect;
    PFN_NvVFX_DestroyEffect DestroyEffect;
    PFN_NvVFX_SetU32 SetU32;
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
    ID3D11Query *sync_query;
    NvCVImage in_d3d;
    NvCVImage in_copy_d3d;
    NvCVImage out_d3d;
    NvCVImage in_gpu;
    NvCVImage out_gpu;
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
    return 0;
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
        char dirs[5][NVVFX_MAX_PATH] = { 0 };
        char exe_dir[MAX_PATH];
        const char *lib_dir = NULL;
        const char *dep_source;
        const char *env_dir;
        int nb_dirs = 0;
        int have_old;

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
            if (dir_has_file(dirs[i], "nvVFXVideoSuperRes.dll")) {
                lib_dir = dirs[i];
                break;
            }
        }
        if (!lib_dir) {
            av_log(ctx, AV_LOG_ERROR, "nvvfx: could not find nvVFXVideoSuperRes.dll. "
                   "Install the NVIDIA Video Effects SDK or set the lib_dir option.\n");
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

        s->h_nvcv = load_dll(dir_has_file(old_dir, "NVCVImage.dll") ? old_dir : lib_dir,
                             "NVCVImage.dll");
        s->h_ngxruntime = load_dll(lib_dir, "nvngxruntime.dll");
        s->h_vfx = load_dll(lib_dir, "NVVideoEffects.dll");
        s->h_feature = load_dll(lib_dir, "nvVFXVideoSuperRes.dll");
        s->h_vsr = load_dll(lib_dir, "nvngx_vsr.dll");

        if (!s->h_nvcv || !s->h_vfx || !s->h_ngxruntime || !s->h_feature || !s->h_vsr) {
            av_log(ctx, AV_LOG_ERROR, "nvvfx: failed to load the NVIDIA Video Effects "
                   "runtime from %s (error %lu).\n", lib_dir, GetLastError());
            return AVERROR_EXTERNAL;
        }
    }

    s->CreateEffect = (PFN_NvVFX_CreateEffect)GetProcAddress(s->h_vfx, "NvVFX_CreateEffect");
    s->DestroyEffect = (PFN_NvVFX_DestroyEffect)GetProcAddress(s->h_vfx, "NvVFX_DestroyEffect");
    s->SetU32 = (PFN_NvVFX_SetU32)GetProcAddress(s->h_vfx, "NvVFX_SetU32");
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
        (s->mode == MODE_ARTIFACT && !s->SetString)) {
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
        if (s->in_gpu.pixels || s->in_gpu.deletePtr)
            s->Dealloc(&s->in_gpu);
        if (s->out_gpu.pixels || s->out_gpu.deletePtr)
            s->Dealloc(&s->out_gpu);
        if (s->in_f32.pixels || s->in_f32.deletePtr)
            s->Dealloc(&s->in_f32);
        if (s->out_f32.pixels || s->out_f32.deletePtr)
            s->Dealloc(&s->out_f32);
    }
    memset(&s->in_d3d, 0, sizeof(s->in_d3d));
    memset(&s->in_copy_d3d, 0, sizeof(s->in_copy_d3d));
    memset(&s->out_d3d, 0, sizeof(s->out_d3d));
    memset(&s->in_gpu, 0, sizeof(s->in_gpu));
    memset(&s->out_gpu, 0, sizeof(s->out_gpu));
    memset(&s->in_f32, 0, sizeof(s->in_f32));
    memset(&s->out_f32, 0, sizeof(s->out_f32));

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

static int vp_setup(AVFilterContext *ctx, NVVFXContext *s, int width, int height)
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
                                                                 &content, &s->vp_enum);
    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx: failed to create the video processor "
               "enumerator (HRESULT 0x%lX).\n", hr);
        return AVERROR_EXTERNAL;
    }

    hr = s->video_device->lpVtbl->CreateVideoProcessor(s->video_device, s->vp_enum,
                                                       0, &s->vp);
    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx: failed to create the video processor "
               "(HRESULT 0x%lX).\n", hr);
        return AVERROR_EXTERNAL;
    }

    hr = s->video_device->lpVtbl->CreateVideoProcessorOutputView(s->video_device,
                                                                 (ID3D11Resource *)s->in_tex,
                                                                 s->vp_enum, &out_desc,
                                                                 &s->vp_out_view);
    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx: failed to create the video processor "
               "output view (HRESULT 0x%lX).\n", hr);
        return AVERROR_EXTERNAL;
    }

    s->video_context->lpVtbl->VideoProcessorSetStreamSourceRect(s->video_context,
                                                                s->vp, 0, TRUE,
                                                                &src_rect);
    s->video_context->lpVtbl->VideoProcessorSetStreamAutoProcessingMode(s->video_context,
                                                                        s->vp, 0, FALSE);

    {
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE csp = {
            .Usage = 0,
            .RGB_Range = 0,
            .Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255,
        };
        s->video_context->lpVtbl->VideoProcessorSetOutputColorSpace(s->video_context,
                                                                    s->vp, &csp);
    }

    return 0;
}

static void vp_set_stream_colorspace(NVVFXContext *s, const AVFrame *frame)
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

static int vfx_setup(AVFilterContext *ctx, NVVFXContext *s, int width, int height,
                     DXGI_FORMAT in_format, int pixfmt, int vp_needed)
{
    int quality_level;
    int out_width = s->mode == MODE_SUPERRES ? width * s->scale : width;
    int out_height = s->mode == MODE_SUPERRES ? height * s->scale : height;
    int out_pixfmt = s->mode == MODE_ARTIFACT ? NVCV_BGRA : pixfmt;
    DXGI_FORMAT tex_format = vp_needed ? DXGI_FORMAT_B8G8R8A8_UNORM : in_format;
    DXGI_FORMAT out_tex_format = s->mode == MODE_ARTIFACT ?
                                 DXGI_FORMAT_B8G8R8A8_UNORM : tex_format;
    char model_dir[NVVFX_MAX_PATH] = "";
    D3D11_QUERY_DESC query_desc = { .Query = D3D11_QUERY_EVENT };
    NvCV_Status st;
    HRESULT hr;
    int ret;

    if (s->mode == MODE_ARTIFACT) {
        if (width > 1920 || height > 1080) {
            av_log(ctx, AV_LOG_ERROR, "nvvfx: ArtifactReduction supports at most "
                   "1920x1080 (got %dx%d).\n", width, height);
            return AVERROR(EINVAL);
        }
        quality_level = s->quality ? 1 : 0;
        snprintf(model_dir, sizeof(model_dir), "%s\\models",
                 s->dep_dir && s->dep_dir[0] ? s->dep_dir :
                 "C:\\Program Files\\NVIDIA Corporation\\NVIDIA Video Effects");
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

    if ((ret = create_texture(s, width, height, tex_format,
                              D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
                              &s->in_tex)) < 0 ||
        (ret = create_texture(s, out_width, out_height, out_tex_format,
                              D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
                              &s->out_tex)) < 0) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx: failed to create D3D11 textures.\n");
        goto fail;
    }

    /* Normalize the input into a texture we own: decoder textures are padded
     * arrays, and hwupload textures carry no video-processor bind flags. */
    if (vp_needed &&
        ((ret = create_texture(s, width, height, in_format,
                               D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_DECODER,
                               &s->in_yuv_tex)) < 0 ||
         (ret = create_texture(s, width, height, tex_format,
                               D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
                               &s->in_copy_tex)) < 0)) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx: failed to create the staging textures.\n");
        goto fail;
    }

    hr = s->device->lpVtbl->CreateQuery(s->device, &query_desc, &s->sync_query);
    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx: failed to create the sync query "
               "(HRESULT 0x%lX).\n", hr);
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    st = s->InitFromD3D11Texture(&s->in_d3d, s->in_tex);
    if (st) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx: failed to register the input texture (%s).\n",
               vfx_err(s, st));
        ret = AVERROR_EXTERNAL;
        goto fail;
    }
    st = s->InitFromD3D11Texture(&s->out_d3d, s->out_tex);
    if (st) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx: failed to register the output texture (%s).\n",
               vfx_err(s, st));
        ret = AVERROR_EXTERNAL;
        goto fail;
    }
    if (vp_needed) {
        st = s->InitFromD3D11Texture(&s->in_copy_d3d, s->in_copy_tex);
        if (st) {
            av_log(ctx, AV_LOG_ERROR, "nvvfx: failed to register the copy texture (%s).\n",
                   vfx_err(s, st));
            ret = AVERROR_EXTERNAL;
            goto fail;
        }
    }

    st = s->Alloc(&s->in_gpu, width, height, pixfmt, NVCV_U8,
                  NVCV_CHUNKY, NVCV_GPU, 32);
    if (st) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx: failed to allocate the input image (%s).\n",
               vfx_err(s, st));
        ret = AVERROR_EXTERNAL;
        goto fail;
    }
    st = s->Alloc(&s->out_gpu, out_width, out_height, out_pixfmt, NVCV_U8,
                  NVCV_CHUNKY, NVCV_GPU, 32);
    if (st) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx: failed to allocate the output image (%s).\n",
               vfx_err(s, st));
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    if (s->mode == MODE_ARTIFACT) {
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
        if ((ret = vp_setup(ctx, s, width, height)) < 0)
            goto fail;
    }

    if (s->mode == MODE_ARTIFACT) {
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

    if (s->mode == MODE_ARTIFACT) {
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
        st = s->SetImage(s->effect, NVVFX_INPUT_IMAGE, &s->in_gpu);
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
               s->mode == MODE_ARTIFACT ? "ArtifactReduction" : "VideoSuperRes",
               vfx_err(s, st));
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
           "mode=%d)\n", s->mode == MODE_ARTIFACT ? "ArtifactReduction" :
           "VideoSuperRes", width, height, out_width, out_height,
           quality_level, s->mode);
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

static int vp_convert(AVFilterContext *ctx, NVVFXContext *s)
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
        av_log(ctx, AV_LOG_ERROR, "nvvfx: failed to create the input view "
               "(HRESULT 0x%lX).\n", hr);
        return AVERROR_EXTERNAL;
    }

    stream.pInputSurface = view;

    hr = s->video_context->lpVtbl->VideoProcessorBlt(s->video_context, s->vp,
                                                     s->vp_out_view, 0, 1, &stream);
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
        if ((ret = vp_convert(ctx, s)) < 0)
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
    s->device_ctx->lpVtbl->Flush(s->device_ctx);
    return 0;
}

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

    switch (s->in_sw_format) {
    case AV_PIX_FMT_NV12:
    case AV_PIX_FMT_P010:
        s->out_sw_format = AV_PIX_FMT_BGRA;
        vp_needed = 1;
        break;
    case AV_PIX_FMT_BGRA:
    case AV_PIX_FMT_RGBA:
        s->out_sw_format = s->in_sw_format;
        vp_needed = 0;
        break;
    default:
        av_log(ctx, AV_LOG_ERROR, "nvvfx: unsupported input format %s, expected "
               "NV12, P010, BGRA or RGBA.\n",
               av_get_pix_fmt_name(s->in_sw_format));
        return AVERROR(EINVAL);
    }

    if (s->mode == MODE_ARTIFACT) {
        if (inlink->w > 1920 || inlink->h > 1080) {
            av_log(ctx, AV_LOG_ERROR, "nvvfx: ArtifactReduction supports at most "
                   "1920x1080 (got %dx%d).\n", inlink->w, inlink->h);
            return AVERROR(EINVAL);
        }
        s->out_sw_format = AV_PIX_FMT_BGRA;
    }

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

    if (vp_needed) {
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

    out_width = s->mode == MODE_SUPERRES ? inlink->w * s->scale : inlink->w;
    out_height = s->mode == MODE_SUPERRES ? inlink->h * s->scale : inlink->h;
    outlink->w = out_width;
    outlink->h = out_height;

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
    vp_needed = s->in_sw_format == AV_PIX_FMT_NV12 ||
                s->in_sw_format == AV_PIX_FMT_P010;

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

    /* The video processor writes full-range RGB. */
    if (s->vp_needed && (s->out_sw_format == AV_PIX_FMT_BGRA ||
                         s->out_sw_format == AV_PIX_FMT_RGBA))
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

static av_cold void nvvfx_uninit(AVFilterContext *ctx)
{
    NVVFXContext *s = ctx->priv;

    vfx_destroy(s);

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
