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
 * NVIDIA Video Effects frame generation (VideoFrameGeneration).
 *
 * The effect takes two frames (previous and current) and produces one
 * intermediate frame.  Both inputs must already be D3D11 hardware frames
 * with the same dimensions and software format.  A typical graph feeds the
 * filter from a split stream, e.g.
 *
 *   [in]split[prev][cur];
 *   [prev]trim=end_frame=N-1,setpts=PTS-STARTPTS[p];
 *   [cur]trim=start_frame=1,setpts=PTS-STARTPTS[c];
 *   [p][c]nvvfx_framegen
 *
 * The output is the generated frame; the original frames are not forwarded.
 *
 * Timestamp: the output PTS is
 *   previous_pts + step * (current_pts - previous_pts)
 * where step is the "timestep" option in timestep mode, or
 * frame_index / frame_multiplier in index mode.  If the paired frames carry
 * the same timestamp (the usual case for two PTS-aligned trimmed streams),
 * the previous frame's timestamp is used unchanged.
 *
 * Option selection: method=time (default) uses timestep; method=index uses
 * frame_multiplier/frame_index.
 *
 * RGB8 (BGRA/RGBA + U8) is implemented.  The 10-bit RGB10A2/P32 path only
 * needs the image encoding fields in NVVFXFGContext to be filled in; the
 * effect auto-detects the encoding from the NvCVImage pixel format, so no
 * "ImageEncodingMode" selector is used for VideoFrameGeneration.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>

#include <initguid.h>
#include <d3d11.h>
#include <d3d11_1.h>

#include "libavutil/avstring.h"
#include "libavutil/error.h"
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_d3d11va.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"

#include "filters.h"
#include "framesync.h"
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
    NVCV_U32    = 5,
    NVCV_F32    = 7,
    NVCV_CHUNKY = 0,
    NVCV_PLANAR = 1,
    NVCV_GPU    = 1,
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

#define NVVFX_EFFECT          "VideoFrameGeneration"
#define NVVFX_PREV_IMAGE      "SrcImage0"
#define NVVFX_CUR_IMAGE       "SrcImage1"
#define NVVFX_OUTPUT_IMAGE    "DstImage0"
#define NVVFX_CUDA_STREAM     "CudaStream"
#define NVVFX_INPUT_WIDTH     "InputWidth"
#define NVVFX_INPUT_HEIGHT    "InputHeight"
#define NVVFX_MODE            "Mode"
#define NVVFX_AUTO_SHOT       "AutomaticShotChangeDetectionEnabled"
#define NVVFX_FRAME_MULT      "FrameMultiplier"
#define NVVFX_FRAME_INDEX     "FrameIndex"
#define NVVFX_TIMESTEP        "Timestep"
#define NVVFX_SHOT_CHANGE     "ShotChange"

enum NVVFXFGMode {
    FG_MODE_LOW,
    FG_MODE_MEDIUM,
    FG_MODE_HIGH,
    FG_MODE_NB
};

enum NVVFXFGEncoding {
    FG_ENC_RGB8,
    FG_ENC_RGB10A2,
    FG_ENC_NB
};

enum NVVFXFGMethod {
    FG_METHOD_TIME,
    FG_METHOD_INDEX,
    FG_METHOD_NB
};

typedef struct NVVFXFGContext {
    const AVClass *class;

    int mode;
    int method;
    float timestep;
    int frame_multiplier;
    int frame_index;
    int shot_change;
    int auto_shot_change;
    int encoding;
    char *lib_dir;
    char *dep_dir;

    int use_timestep;

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
    NvCVImage prev_gpu;
    NvCVImage cur_gpu;
    NvCVImage out_gpu;

    int width;
    int height;
    int pixfmt;
    int comp_type;
    int vp_needed;
    DXGI_FORMAT in_format;
    int ready;

    FFFrameSync fs;
    int fs_inited;
} NVVFXFGContext;

static const char *fg_err(NVVFXFGContext *s, NvCV_Status status)
{
    return s->ErrorString ? s->ErrorString(status) : "unknown error";
}

static av_cold int nvvfx_framegen_preinit(AVFilterContext *ctx)
{
    NVVFXFGContext *s = ctx->priv;

    ff_framesync_preinit(&s->fs);
    return 0;
}

static av_cold int nvvfx_framegen_init(AVFilterContext *ctx)
{
    NVVFXFGContext *s = ctx->priv;

    /* The API accepts either an explicit timestep in the open interval
     * (0, 1) or a FrameMultiplier/FrameIndex pair. */
    s->use_timestep = s->method == FG_METHOD_TIME;

    if (s->use_timestep) {
        if (!(s->timestep > 0.0f && s->timestep < 1.0f)) {
            av_log(ctx, AV_LOG_ERROR, "nvvfx_framegen: timestep must be in "
                   "the open interval (0, 1).\n");
            return AVERROR(EINVAL);
        }
    } else {
        if (s->frame_multiplier < 2 || s->frame_multiplier > 8) {
            av_log(ctx, AV_LOG_ERROR, "nvvfx_framegen: frame_multiplier must "
                   "be between 2 and 8.\n");
            return AVERROR(EINVAL);
        }
        if (s->frame_index < 1 || s->frame_index >= s->frame_multiplier) {
            av_log(ctx, AV_LOG_ERROR, "nvvfx_framegen: frame_index must be "
                   "between 1 and frame_multiplier - 1.\n");
            return AVERROR(EINVAL);
        }
    }

    s->fs.on_event = NULL;
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

static int fg_load_libs(AVFilterContext *ctx, NVVFXFGContext *s)
{
    static const char *const deps[] = {
        "nppc64_12.dll", "nppial64_12.dll", "nppicc64_12.dll",
        "nppidei64_12.dll", "nppif64_12.dll", "nppig64_12.dll",
        "nppim64_12.dll", "nppist64_12.dll", "nppitc64_12.dll",
        "cudart64_12.dll", "nvinfer_10.dll", "nvinfer_plugin_10.dll",
        "nvonnxparser_10.dll",
    };
    char old_dir[NVVFX_MAX_PATH];
    char dirs[5][NVVFX_MAX_PATH] = { 0 };
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
             "C:\\Program Files\\NVIDIA Corporation\\NVIDIA Video Effects");

    for (i = 0; i < nb_dirs; i++) {
        if (dir_has_file(dirs[i], "nvVFXVideoFrameGeneration.dll")) {
            lib_dir = dirs[i];
            break;
        }
    }
    if (!lib_dir) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_framegen: could not find "
               "nvVFXVideoFrameGeneration.dll. Install the NVIDIA Video "
               "Effects SDK or set the lib_dir option.\n");
        return AVERROR_EXTERNAL;
    }

    have_old = dir_has_file(old_dir, "NVVideoEffects.dll");
    dep_source = have_old ? old_dir : lib_dir;

    av_log(ctx, AV_LOG_VERBOSE, "nvvfx_framegen: runtime %s, legacy runtime "
           "%s\n", lib_dir, have_old ? old_dir : "not found");

    /* The split runtime needs the old NPP/CUDA/TRT partner DLLs, which live
     * in the Program Files SDK.  Missing ones are not fatal; not every
     * effect uses every dependency. */
    for (i = 0; i < FF_ARRAY_ELEMS(deps); i++) {
        if (!load_dll(dep_source, deps[i]))
            av_log(ctx, AV_LOG_VERBOSE, "nvvfx_framegen: could not load %s\n",
                   deps[i]);
    }

    /* The 0.2.0.0 runtime is built against the matching NVCVImage ABI, but
     * the legacy Program Files copy of NVCVImage works for the RGB8 path
     * and keeps the DLL search path simple, so prefer it like the VSR
     * filter does. */
    s->h_nvcv = load_dll(dir_has_file(old_dir, "NVCVImage.dll") ? old_dir : lib_dir,
                         "NVCVImage.dll");
    s->h_ngxruntime = load_dll(lib_dir, "nvngxruntime.dll");
    s->h_vfx = load_dll(lib_dir, "NVVideoEffects.dll");
    s->h_feature = load_dll(lib_dir, "nvVFXVideoFrameGeneration.dll");

    if (!s->h_nvcv || !s->h_vfx || !s->h_ngxruntime || !s->h_feature) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_framegen: failed to load the NVIDIA "
               "Video Effects runtime from %s (error %lu).\n", lib_dir,
               GetLastError());
        return AVERROR_EXTERNAL;
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

    if (!s->CreateEffect || !s->DestroyEffect || !s->SetU32 || !s->SetF32 ||
        !s->SetImage || !s->SetCudaStream || !s->Load || !s->Run ||
        !s->CudaStreamCreate || !s->CudaStreamDestroy ||
        !s->CudaStreamSynchronize || !s->Alloc || !s->Dealloc ||
        !s->Transfer || !s->InitFromD3D11Texture || !s->MapResource ||
        !s->UnmapResource) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_framegen: runtime is missing "
               "required symbols.\n");
        return AVERROR_EXTERNAL;
    }

    s->libs_loaded = 1;
    return 0;
}

static void release_video_processor(NVVFXFGContext *s)
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

static void fg_destroy(NVVFXFGContext *s)
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
        if (s->prev_gpu.pixels || s->prev_gpu.deletePtr)
            s->Dealloc(&s->prev_gpu);
        if (s->cur_gpu.pixels || s->cur_gpu.deletePtr)
            s->Dealloc(&s->cur_gpu);
        if (s->out_gpu.pixels || s->out_gpu.deletePtr)
            s->Dealloc(&s->out_gpu);
    }
    memset(&s->in_d3d, 0, sizeof(s->in_d3d));
    memset(&s->in_copy_d3d, 0, sizeof(s->in_copy_d3d));
    memset(&s->out_d3d, 0, sizeof(s->out_d3d));
    memset(&s->prev_gpu, 0, sizeof(s->prev_gpu));
    memset(&s->cur_gpu, 0, sizeof(s->cur_gpu));
    memset(&s->out_gpu, 0, sizeof(s->out_gpu));

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
    s->pixfmt = 0;
    s->comp_type = 0;
    s->vp_needed = 0;
    s->in_format = DXGI_FORMAT_UNKNOWN;
}

static int create_texture(NVVFXFGContext *s, int width, int height,
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

static int vp_setup(AVFilterContext *ctx, NVVFXFGContext *s, int width, int height)
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
        av_log(ctx, AV_LOG_ERROR, "nvvfx_framegen: failed to create the video "
               "processor enumerator (HRESULT 0x%lX).\n", hr);
        return AVERROR_EXTERNAL;
    }

    hr = s->video_device->lpVtbl->CreateVideoProcessor(s->video_device, s->vp_enum,
                                                       0, &s->vp);
    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_framegen: failed to create the video "
               "processor (HRESULT 0x%lX).\n", hr);
        return AVERROR_EXTERNAL;
    }

    hr = s->video_device->lpVtbl->CreateVideoProcessorOutputView(s->video_device,
                                                                 (ID3D11Resource *)s->in_tex,
                                                                 s->vp_enum, &out_desc,
                                                                 &s->vp_out_view);
    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_framegen: failed to create the video "
               "processor output view (HRESULT 0x%lX).\n", hr);
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

static void vp_set_stream_colorspace(NVVFXFGContext *s, const AVFrame *frame)
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

static int fg_setup(AVFilterContext *ctx, NVVFXFGContext *s, int width, int height,
                    DXGI_FORMAT in_format, int pixfmt, int comp_type, int vp_needed)
{
    DXGI_FORMAT tex_format = vp_needed ? DXGI_FORMAT_B8G8R8A8_UNORM : in_format;
    D3D11_QUERY_DESC query_desc = { .Query = D3D11_QUERY_EVENT };
    NvCV_Status st;
    HRESULT hr;
    int ret;

    if (s->ready && s->width == width && s->height == height &&
        s->pixfmt == pixfmt && s->comp_type == comp_type &&
        s->vp_needed == vp_needed && s->in_format == in_format)
        return 0;

    fg_destroy(s);
    s->in_format = in_format;

    if ((ret = create_texture(s, width, height, tex_format,
                              D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
                              &s->in_tex)) < 0 ||
        (ret = create_texture(s, width, height, tex_format,
                              D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
                              &s->out_tex)) < 0) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_framegen: failed to create D3D11 "
               "textures.\n");
        goto fail;
    }

    /* Normalize both inputs into a texture we own: decoder textures are
     * padded arrays, and hwupload textures carry no video-processor bind
     * flags. */
    if (vp_needed &&
        ((ret = create_texture(s, width, height, in_format,
                               D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_DECODER,
                               &s->in_yuv_tex)) < 0 ||
         (ret = create_texture(s, width, height, tex_format,
                               D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
                               &s->in_copy_tex)) < 0)) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_framegen: failed to create the "
               "staging textures.\n");
        goto fail;
    }

    hr = s->device->lpVtbl->CreateQuery(s->device, &query_desc, &s->sync_query);
    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_framegen: failed to create the sync "
               "query (HRESULT 0x%lX).\n", hr);
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    st = s->InitFromD3D11Texture(&s->in_d3d, s->in_tex);
    if (st) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_framegen: failed to register the "
               "input texture (%s).\n", fg_err(s, st));
        ret = AVERROR_EXTERNAL;
        goto fail;
    }
    st = s->InitFromD3D11Texture(&s->out_d3d, s->out_tex);
    if (st) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_framegen: failed to register the "
               "output texture (%s).\n", fg_err(s, st));
        ret = AVERROR_EXTERNAL;
        goto fail;
    }
    if (vp_needed) {
        st = s->InitFromD3D11Texture(&s->in_copy_d3d, s->in_copy_tex);
        if (st) {
            av_log(ctx, AV_LOG_ERROR, "nvvfx_framegen: failed to register the "
                   "copy texture (%s).\n", fg_err(s, st));
            ret = AVERROR_EXTERNAL;
            goto fail;
        }
    }

    st = s->Alloc(&s->prev_gpu, width, height, pixfmt, comp_type,
                  NVCV_CHUNKY, NVCV_GPU, 32);
    if (st) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_framegen: failed to allocate the "
               "previous-frame image (%s).\n", fg_err(s, st));
        ret = AVERROR_EXTERNAL;
        goto fail;
    }
    st = s->Alloc(&s->cur_gpu, width, height, pixfmt, comp_type,
                  NVCV_CHUNKY, NVCV_GPU, 32);
    if (st) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_framegen: failed to allocate the "
               "current-frame image (%s).\n", fg_err(s, st));
        ret = AVERROR_EXTERNAL;
        goto fail;
    }
    st = s->Alloc(&s->out_gpu, width, height, pixfmt, comp_type,
                  NVCV_CHUNKY, NVCV_GPU, 32);
    if (st) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_framegen: failed to allocate the "
               "output image (%s).\n", fg_err(s, st));
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    if (vp_needed) {
        if ((ret = vp_setup(ctx, s, width, height)) < 0)
            goto fail;
    }

    st = s->CreateEffect(NVVFX_EFFECT, &s->effect);
    if (st) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_framegen: failed to create the "
               "VideoFrameGeneration effect (%s).\n", fg_err(s, st));
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    /* InputWidth/InputHeight are required before NvVFX_Load(). */
    st = s->SetU32(s->effect, NVVFX_INPUT_WIDTH, width);
    if (st) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_framegen: failed to set InputWidth=%d "
               "(%s).\n", width, fg_err(s, st));
        ret = AVERROR_EXTERNAL;
        goto fail;
    }
    st = s->SetU32(s->effect, NVVFX_INPUT_HEIGHT, height);
    if (st) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_framegen: failed to set InputHeight=%d "
               "(%s).\n", height, fg_err(s, st));
        ret = AVERROR_EXTERNAL;
        goto fail;
    }
    st = s->SetU32(s->effect, NVVFX_MODE, s->mode);
    if (st) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_framegen: failed to set Mode=%d "
               "(%s).\n", s->mode, fg_err(s, st));
        ret = AVERROR_EXTERNAL;
        goto fail;
    }
    st = s->SetU32(s->effect, NVVFX_AUTO_SHOT, s->auto_shot_change ? 1 : 0);
    if (st) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_framegen: failed to set "
               "AutomaticShotChangeDetectionEnabled=%d (%s).\n",
               s->auto_shot_change ? 1 : 0, fg_err(s, st));
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    st = s->SetImage(s->effect, NVVFX_PREV_IMAGE, &s->prev_gpu);
    if (st) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_framegen: failed to set the previous "
               "frame image (%s).\n", fg_err(s, st));
        ret = AVERROR_EXTERNAL;
        goto fail;
    }
    st = s->SetImage(s->effect, NVVFX_CUR_IMAGE, &s->cur_gpu);
    if (st) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_framegen: failed to set the current "
               "frame image (%s).\n", fg_err(s, st));
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    if (!s->stream) {
        st = s->CudaStreamCreate(&s->stream);
        if (st) {
            av_log(ctx, AV_LOG_ERROR, "nvvfx_framegen: failed to create the "
                   "CUDA stream (%s).\n", fg_err(s, st));
            ret = AVERROR_EXTERNAL;
            goto fail;
        }
    }

    st = s->SetCudaStream(s->effect, NVVFX_CUDA_STREAM, s->stream);
    if (st) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_framegen: failed to set the CUDA "
               "stream (%s).\n", fg_err(s, st));
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    st = s->Load(s->effect);
    if (st) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_framegen: failed to load the "
               "VideoFrameGeneration model (%s).\n", fg_err(s, st));
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    /* The reference implementation binds the output image after Load(). */
    st = s->SetImage(s->effect, NVVFX_OUTPUT_IMAGE, &s->out_gpu);
    if (st) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_framegen: failed to set the output "
               "image (%s).\n", fg_err(s, st));
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    /* Writing FrameMultiplier resets both FrameIndex and Timestep, so this
     * has to happen after Load() and before the per-frame parameters. */
    st = s->SetU32(s->effect, NVVFX_FRAME_MULT,
                   s->use_timestep ? 0 : s->frame_multiplier);
    if (st) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_framegen: failed to set "
               "FrameMultiplier=%d (%s).\n",
               s->use_timestep ? 0 : s->frame_multiplier, fg_err(s, st));
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    s->width = width;
    s->height = height;
    s->pixfmt = pixfmt;
    s->comp_type = comp_type;
    s->vp_needed = vp_needed;
    s->in_format = in_format;
    s->ready = 1;

    if (s->use_timestep)
        av_log(ctx, AV_LOG_VERBOSE, "nvvfx_framegen: VideoFrameGeneration "
               "ready (%dx%d, mode=%d, timestep=%g)\n", width, height,
               s->mode, s->timestep);
    else
        av_log(ctx, AV_LOG_VERBOSE, "nvvfx_framegen: VideoFrameGeneration "
               "ready (%dx%d, mode=%d, frame_index=%d/%d)\n", width, height,
               s->mode, s->frame_index, s->frame_multiplier);
    return 0;

fail:
    fg_destroy(s);
    return ret;
}

static int d3d11_sync(AVFilterContext *ctx, NVVFXFGContext *s)
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
        av_log(ctx, AV_LOG_ERROR, "nvvfx_framegen: D3D11 sync query failed "
               "(HRESULT 0x%lX).\n", hr);
        return AVERROR_EXTERNAL;
    }
    return 0;
}

static int vp_convert(AVFilterContext *ctx, NVVFXFGContext *s)
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
        av_log(ctx, AV_LOG_ERROR, "nvvfx_framegen: failed to create the input "
               "view (HRESULT 0x%lX).\n", hr);
        return AVERROR_EXTERNAL;
    }

    stream.pInputSurface = view;

    hr = s->video_context->lpVtbl->VideoProcessorBlt(s->video_context, s->vp,
                                                     s->vp_out_view, 0, 1, &stream);
    view->lpVtbl->Release(view);

    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_framegen: VideoProcessorBlt failed "
               "(HRESULT 0x%lX).\n", hr);
        return AVERROR_EXTERNAL;
    }
    return 0;
}

/* Copies and converts one D3D11 frame into the effect's NvCVImage.  The same
 * staging textures are reused for the previous and current frames, so the
 * caller must not have a transfer outstanding on need_sync (this helper
 * synchronizes the CUDA stream after every transfer). */
static int fg_load_frame(AVFilterContext *ctx, NVVFXFGContext *s,
                         const AVFrame *frame, NvCVImage *dst_gpu,
                         int need_sync)
{
    ID3D11Texture2D *in_tex;
    D3D11_TEXTURE2D_DESC desc;
    NvCVImage *wrapper;
    NvCV_Status st;
    UINT in_slice;
    int width, height;
    int ret;

    if (frame->format != AV_PIX_FMT_D3D11 || !frame->hw_frames_ctx) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_framegen: input is not a D3D11 "
               "hardware frame.\n");
        return AVERROR(EINVAL);
    }

    in_tex = (ID3D11Texture2D *)frame->data[0];
    in_slice = (UINT)(intptr_t)frame->data[1];
    in_tex->lpVtbl->GetDesc(in_tex, &desc);

    width = frame->width > 0 ? frame->width : (int)desc.Width;
    height = frame->height > 0 ? frame->height : (int)desc.Height;
    if (width != s->width || height != s->height ||
        desc.Format != s->in_format) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_framegen: frame does not match the "
               "configured %dx%d effect (%dx%d, DXGI format %d vs %d).\n",
               s->width, s->height, width, height,
               (int)desc.Format, (int)s->in_format);
        return AVERROR(EINVAL);
    }
    if (width > (int)desc.Width || height > (int)desc.Height) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_framegen: frame size %dx%d exceeds "
               "the texture size %ux%u.\n", width, height, desc.Width,
               desc.Height);
        return AVERROR(EINVAL);
    }

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
        wrapper = &s->in_copy_d3d;
    } else {
        s->device_ctx->lpVtbl->CopySubresourceRegion(s->device_ctx,
                                                     (ID3D11Resource *)s->in_tex, 0,
                                                     0, 0, 0,
                                                     (ID3D11Resource *)in_tex,
                                                     in_slice, NULL);
        wrapper = &s->in_d3d;
    }

    if ((ret = d3d11_sync(ctx, s)) < 0)
        return ret;

    st = s->MapResource(wrapper, s->stream);
    if (st == 0) {
        NvCV_Status transfer_st = s->Transfer(wrapper, dst_gpu, 1.0f,
                                              s->stream, NULL);
        NvCV_Status unmap_st = s->UnmapResource(wrapper, s->stream);
        st = transfer_st ? transfer_st : unmap_st;
    }
    if (st) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_framegen: input transfer failed "
               "(%s).\n", fg_err(s, st));
        return AVERROR_EXTERNAL;
    }

    if (need_sync && s->CudaStreamSynchronize)
        s->CudaStreamSynchronize(s->stream);

    return 0;
}

static int fg_process(AVFilterContext *ctx, NVVFXFGContext *s,
                      const AVFrame *prev, const AVFrame *cur,
                      ID3D11Texture2D *out_tex, UINT out_slice)
{
    NvCV_Status st;
    int ret;

    if (!s->ready)
        return AVERROR(EINVAL);

    if ((ret = fg_load_frame(ctx, s, prev, &s->prev_gpu, 1)) < 0)
        return ret;
    if ((ret = fg_load_frame(ctx, s, cur, &s->cur_gpu, 0)) < 0)
        return ret;

    st = s->SetU32(s->effect, NVVFX_SHOT_CHANGE, s->shot_change ? 1 : 0);
    if (st) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_framegen: failed to set ShotChange=%d "
               "(%s).\n", s->shot_change ? 1 : 0, fg_err(s, st));
        return AVERROR_EXTERNAL;
    }

    if (s->use_timestep) {
        st = s->SetF32(s->effect, NVVFX_TIMESTEP, s->timestep);
        if (st) {
            av_log(ctx, AV_LOG_ERROR, "nvvfx_framegen: failed to set "
                   "Timestep=%g (%s).\n", s->timestep, fg_err(s, st));
            return AVERROR_EXTERNAL;
        }
    } else {
        st = s->SetU32(s->effect, NVVFX_FRAME_INDEX, s->frame_index);
        if (st) {
            av_log(ctx, AV_LOG_ERROR, "nvvfx_framegen: failed to set "
                   "FrameIndex=%d (%s).\n", s->frame_index, fg_err(s, st));
            return AVERROR_EXTERNAL;
        }
    }

    st = s->Run(s->effect, 0);
    if (st) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_framegen: inference failed (%s).\n",
               fg_err(s, st));
        return AVERROR_EXTERNAL;
    }

    st = s->MapResource(&s->out_d3d, s->stream);
    if (st == 0) {
        NvCV_Status transfer_st = s->Transfer(&s->out_gpu, &s->out_d3d, 1.0f,
                                              s->stream, NULL);
        NvCV_Status unmap_st = s->UnmapResource(&s->out_d3d, s->stream);
        st = transfer_st ? transfer_st : unmap_st;
    }
    if (st) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_framegen: output transfer failed "
               "(%s).\n", fg_err(s, st));
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

static int64_t fg_frame_pts(NVVFXFGContext *s,
                            const AVFrame *prev, const AVFrame *cur)
{
    int64_t pts = prev->pts;
    int64_t delta;

    if (prev->pts == AV_NOPTS_VALUE || cur->pts == AV_NOPTS_VALUE)
        return pts;

    delta = cur->pts - prev->pts;
    if (delta <= 0)
        return pts;

    if (s->use_timestep)
        pts += (int64_t)(delta * (double)s->timestep + 0.5);
    else
        pts += av_rescale(delta, s->frame_index, s->frame_multiplier);
    return pts;
}

static int fg_do_filter(FFFrameSync *fs)
{
    AVFilterContext *ctx = fs->parent;
    NVVFXFGContext *s = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];
    ID3D11Texture2D *prev_tex;
    ID3D11Texture2D *out_tex;
    D3D11_TEXTURE2D_DESC prev_desc;
    D3D11_TEXTURE2D_DESC cur_desc;
    AVFrame *prev = NULL, *cur = NULL, *out = NULL;
    UINT out_slice;
    int pixfmt, comp_type;
    int vp_needed;
    int ret;

    ret = ff_framesync_dualinput_get(fs, &prev, &cur);
    if (ret < 0)
        return ret;
    if (!cur)
        return ff_filter_frame(outlink, prev);

    if (prev->format != AV_PIX_FMT_D3D11 || cur->format != AV_PIX_FMT_D3D11) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_framegen: inputs are not D3D11 "
               "hardware frames.\n");
        ret = AVERROR(EINVAL);
        goto fail;
    }
    if (prev->data[0] == cur->data[0] &&
        prev->data[1] == cur->data[1]) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_framegen: previous and current "
               "frames are the same frame.\n");
        ret = AVERROR(EINVAL);
        goto fail;
    }

    prev_tex = (ID3D11Texture2D *)prev->data[0];
    prev_tex->lpVtbl->GetDesc(prev_tex, &prev_desc);
    {
        ID3D11Texture2D *cur_tex = (ID3D11Texture2D *)cur->data[0];

        cur_tex->lpVtbl->GetDesc(cur_tex, &cur_desc);
    }
    if (prev_desc.Format != cur_desc.Format) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_framegen: input DXGI formats differ "
               "(%d vs %d).\n", (int)prev_desc.Format, (int)cur_desc.Format);
        ret = AVERROR(EINVAL);
        goto fail;
    }
    if (!s->hw_frames_ctx_out) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_framegen: output frames context is "
               "not configured.\n");
        ret = AVERROR(EINVAL);
        goto fail;
    }

    /* Use the logical frame dimensions, not the possibly padded texture
     * size: InputWidth/InputHeight must match the NvCVImage dimensions. */
    {
        int width = prev->width > 0 ? prev->width : (int)prev_desc.Width;
        int height = prev->height > 0 ? prev->height : (int)prev_desc.Height;

        if (cur->width != width || cur->height != height) {
            av_log(ctx, AV_LOG_ERROR, "nvvfx_framegen: input dimensions "
                   "differ (%dx%d vs %dx%d).\n", width, height,
                   cur->width, cur->height);
            ret = AVERROR(EINVAL);
            goto fail;
        }
        if (s->encoding == FG_ENC_RGB10A2) {
            av_log(ctx, AV_LOG_ERROR, "nvvfx_framegen: encoding=rgb10a2 is not "
                   "implemented yet; the 0.2.0.0 NvCVImage RGB10A2/P32 path is "
                   "the hook for it. Use encoding=rgb8.\n");
            ret = AVERROR(ENOSYS);
            goto fail;
        }

        pixfmt = s->in_sw_format == AV_PIX_FMT_RGBA ? NVCV_RGBA : NVCV_BGRA;
        comp_type = NVCV_U8;
        vp_needed = s->in_sw_format == AV_PIX_FMT_NV12 ||
                    s->in_sw_format == AV_PIX_FMT_P010;

        ret = fg_setup(ctx, s, width, height, prev_desc.Format,
                       pixfmt, comp_type, vp_needed);
        if (ret < 0) {
            av_log(ctx, AV_LOG_ERROR, "nvvfx_framegen: failed to set up the "
                   "effect.\n");
            goto fail;
        }
    }

    out = av_frame_alloc();
    if (!out) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    ret = av_hwframe_get_buffer(s->hw_frames_ctx_out, out, 0);
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_framegen: failed to allocate the "
               "output frame.\n");
        goto fail;
    }

    out_tex = (ID3D11Texture2D *)out->data[0];
    out_slice = (UINT)(intptr_t)out->data[1];

    ret = fg_process(ctx, s, prev, cur, out_tex, out_slice);
    if (ret < 0)
        goto fail;

    ret = av_frame_copy_props(out, prev);
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_framegen: failed to copy frame "
               "properties.\n");
        goto fail;
    }

    /* The generated frame is full-range RGB. */
    if (s->out_sw_format == AV_PIX_FMT_BGRA ||
        s->out_sw_format == AV_PIX_FMT_RGBA)
        out->color_range = AVCOL_RANGE_JPEG;

    out->pts = fg_frame_pts(s, prev, cur);
    out->duration = cur->pts != AV_NOPTS_VALUE && prev->pts != AV_NOPTS_VALUE &&
                    cur->pts > prev->pts ? cur->pts - prev->pts :
                    prev->duration;

    av_frame_free(&prev);
    return ff_filter_frame(outlink, out);

fail:
    av_frame_free(&prev);
    av_frame_free(&out);
    return ret;
}

static int nvvfx_framegen_config_props(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    NVVFXFGContext *s = ctx->priv;
    AVFilterLink *inlink = ctx->inputs[0];
    AVFilterLink *inlink1 = ctx->inputs[1];
    FilterLink *inl = ff_filter_link(inlink);
    FilterLink *inl1 = ff_filter_link(inlink1);
    FilterLink *outl = ff_filter_link(outlink);
    AVHWFramesContext *in_frames;
    AVHWFramesContext *in_frames1;
    AVHWFramesContext *out_frames;
    AVD3D11VAFramesContext *out_frames_hwctx;
    AVHWDeviceContext *hw_device;
    AVD3D11VADeviceContext *d3d11_hwctx;
    int vp_needed;
    HRESULT hr;
    int ret;

    av_buffer_unref(&s->hw_frames_ctx_out);
    fg_destroy(s);
    if (s->video_context) {
        s->video_context->lpVtbl->Release(s->video_context);
        s->video_context = NULL;
    }
    if (s->video_device) {
        s->video_device->lpVtbl->Release(s->video_device);
        s->video_device = NULL;
    }

    if (!inl->hw_frames_ctx || !inl1->hw_frames_ctx) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_framegen: both inputs must be D3D11 "
               "hardware frames.\n");
        return AVERROR(EINVAL);
    }

    in_frames = (AVHWFramesContext *)inl->hw_frames_ctx->data;
    in_frames1 = (AVHWFramesContext *)inl1->hw_frames_ctx->data;
    if (in_frames->device_ref->data != in_frames1->device_ref->data) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_framegen: inputs must use the same "
               "D3D11 device.\n");
        return AVERROR(EINVAL);
    }
    if (in_frames->sw_format != in_frames1->sw_format) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_framegen: inputs must have the same "
               "software format (%s vs %s).\n",
               av_get_pix_fmt_name(in_frames->sw_format),
               av_get_pix_fmt_name(in_frames1->sw_format));
        return AVERROR(EINVAL);
    }
    if (inlink->w != inlink1->w || inlink->h != inlink1->h) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_framegen: inputs must have the same "
               "dimensions (%dx%d vs %dx%d).\n", inlink->w, inlink->h,
               inlink1->w, inlink1->h);
        return AVERROR(EINVAL);
    }

    if (s->encoding == FG_ENC_RGB10A2) {
        av_log(ctx, AV_LOG_ERROR, "nvvfx_framegen: encoding=rgb10a2 is not "
               "implemented yet; the 0.2.0.0 NvCVImage RGB10A2/P32 path is "
               "the hook for it. Use encoding=rgb8.\n");
        return AVERROR(ENOSYS);
    }

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
        av_log(ctx, AV_LOG_ERROR, "nvvfx_framegen: unsupported input format "
               "%s, expected NV12, P010, BGRA or RGBA.\n",
               av_get_pix_fmt_name(s->in_sw_format));
        return AVERROR(EINVAL);
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
        av_log(ctx, AV_LOG_ERROR, "nvvfx_framegen: invalid D3D11 device or "
               "context.\n");
        return AVERROR(EINVAL);
    }

    if (vp_needed) {
        hr = s->device->lpVtbl->QueryInterface(s->device, &IID_ID3D11VideoDevice,
                                               (void **)&s->video_device);
        if (FAILED(hr)) {
            av_log(ctx, AV_LOG_ERROR, "nvvfx_framegen: failed to get the D3D11 "
                   "video device (HRESULT 0x%lX).\n", hr);
            return AVERROR_EXTERNAL;
        }
        hr = s->device_ctx->lpVtbl->QueryInterface(s->device_ctx,
                                                   &IID_ID3D11VideoContext,
                                                   (void **)&s->video_context);
        if (FAILED(hr)) {
            av_log(ctx, AV_LOG_ERROR, "nvvfx_framegen: failed to get the D3D11 "
                   "video context (HRESULT 0x%lX).\n", hr);
            return AVERROR_EXTERNAL;
        }
    }

    outlink->w = inlink->w;
    outlink->h = inlink->h;
    outlink->sample_aspect_ratio = inlink->sample_aspect_ratio;
    outl->frame_rate = inl->frame_rate;

    s->hw_frames_ctx_out = av_hwframe_ctx_alloc(s->hw_device_ctx);
    if (!s->hw_frames_ctx_out)
        return AVERROR(ENOMEM);

    out_frames = (AVHWFramesContext *)s->hw_frames_ctx_out->data;
    out_frames->format = AV_PIX_FMT_D3D11;
    out_frames->sw_format = s->out_sw_format;
    out_frames->width = inlink->w;
    out_frames->height = inlink->h;
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

    ret = fg_load_libs(ctx, s);
    if (ret < 0)
        return ret;

    if (s->fs_inited) {
        ff_framesync_uninit(&s->fs);
        s->fs.nb_in = 0;
        s->fs.in = NULL;
        s->fs.time_base = (AVRational){ 0, 1 };
        s->fs.eof = 0;
        s->fs.frame_ready = 0;
        s->fs.sync_level = 0;
        s->fs.pts = AV_NOPTS_VALUE;
    }
    ret = ff_framesync_init_dualinput(&s->fs, ctx);
    if (ret < 0)
        return ret;
    s->fs_inited = 1;
    s->fs.on_event = fg_do_filter;
    /* Do not keep pairing trailing frames with a repeated last frame when
     * the two trimmed inputs have different lengths. */
    s->fs.opt_shortest = 1;

    ret = ff_framesync_configure(&s->fs);
    if (ret < 0)
        return ret;
    outlink->time_base = s->fs.time_base;

    av_log(ctx, AV_LOG_VERBOSE, "nvvfx_framegen: configured %dx%d %s -> %dx%d "
           "%s\n", inlink->w, inlink->h,
           av_get_pix_fmt_name(s->in_sw_format), outlink->w, outlink->h,
           av_get_pix_fmt_name(s->out_sw_format));
    return 0;
}

static int nvvfx_framegen_activate(AVFilterContext *ctx)
{
    NVVFXFGContext *s = ctx->priv;

    return ff_framesync_activate(&s->fs);
}

static av_cold void nvvfx_framegen_uninit(AVFilterContext *ctx)
{
    NVVFXFGContext *s = ctx->priv;

    ff_framesync_uninit(&s->fs);

    fg_destroy(s);

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

static const AVFilterPad nvvfx_framegen_inputs[] = {
    {
        .name = "previous",
        .type = AVMEDIA_TYPE_VIDEO,
    },
    {
        .name = "current",
        .type = AVMEDIA_TYPE_VIDEO,
    },
};

static const AVFilterPad nvvfx_framegen_outputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = nvvfx_framegen_config_props,
    },
};

#define OFFSET(x) offsetof(NVVFXFGContext, x)
#define FLAGS (AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_VIDEO_PARAM)

static const AVOption nvvfx_framegen_options[] = {
    { "mode", "generation quality mode", OFFSET(mode), AV_OPT_TYPE_INT,
      { .i64 = FG_MODE_MEDIUM }, 0, FG_MODE_NB - 1, FLAGS, "mode" },
    { "low", "low quality", 0, AV_OPT_TYPE_CONST,
      { .i64 = FG_MODE_LOW }, 0, 0, FLAGS, "mode" },
    { "medium", "medium quality", 0, AV_OPT_TYPE_CONST,
      { .i64 = FG_MODE_MEDIUM }, 0, 0, FLAGS, "mode" },
    { "high", "high quality", 0, AV_OPT_TYPE_CONST,
      { .i64 = FG_MODE_HIGH }, 0, 0, FLAGS, "mode" },
    { "method", "frame selection method", OFFSET(method), AV_OPT_TYPE_INT,
      { .i64 = FG_METHOD_TIME }, 0, FG_METHOD_NB - 1, FLAGS, "method" },
    { "time", "use an explicit timestep", 0, AV_OPT_TYPE_CONST,
      { .i64 = FG_METHOD_TIME }, 0, 0, FLAGS, "method" },
    { "index", "use frame_multiplier and frame_index", 0, AV_OPT_TYPE_CONST,
      { .i64 = FG_METHOD_INDEX }, 0, 0, FLAGS, "method" },
    { "timestep", "temporal position of the generated frame in (0,1)", OFFSET(timestep),
      AV_OPT_TYPE_FLOAT, { .dbl = 0.5 }, 0.0, 1.0, FLAGS },
    { "frame_multiplier", "frame-rate multiplier for index mode", OFFSET(frame_multiplier),
      AV_OPT_TYPE_INT, { .i64 = 2 }, 2, 8, FLAGS },
    { "frame_index", "one-based intermediate frame index in index mode", OFFSET(frame_index),
      AV_OPT_TYPE_INT, { .i64 = 1 }, 1, 7, FLAGS },
    { "shot_change", "mark the pair as a shot change", OFFSET(shot_change),
      AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1, FLAGS },
    { "auto_shot_change", "enable automatic shot-change detection", OFFSET(auto_shot_change),
      AV_OPT_TYPE_BOOL, { .i64 = 1 }, 0, 1, FLAGS },
    { "encoding", "input/output image encoding", OFFSET(encoding), AV_OPT_TYPE_INT,
      { .i64 = FG_ENC_RGB8 }, 0, FG_ENC_NB - 1, FLAGS, "encoding" },
    { "rgb8", "8-bit RGBA/BGRA", 0, AV_OPT_TYPE_CONST,
      { .i64 = FG_ENC_RGB8 }, 0, 0, FLAGS, "encoding" },
    { "rgb10a2", "10-bit packed RGB10A2 (not implemented yet)", 0, AV_OPT_TYPE_CONST,
      { .i64 = FG_ENC_RGB10A2 }, 0, 0, FLAGS, "encoding" },
    { "lib_dir", "directory containing the split NVVFX runtime", OFFSET(lib_dir),
      AV_OPT_TYPE_STRING, { .str = "" }, 0, 0, FLAGS },
    { "dep_dir", "directory containing the legacy NVIDIA Video Effects runtime",
      OFFSET(dep_dir), AV_OPT_TYPE_STRING, { .str = "" }, 0, 0, FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(nvvfx_framegen);

const FFFilter ff_vf_nvvfx_framegen = {
    .p.name           = "nvvfx_framegen",
    .p.description    = NULL_IF_CONFIG_SMALL("NVIDIA Video Effects frame generation"),
    .priv_size        = sizeof(NVVFXFGContext),
    .p.priv_class     = &nvvfx_framegen_class,
    .preinit          = nvvfx_framegen_preinit,
    .init             = nvvfx_framegen_init,
    .uninit           = nvvfx_framegen_uninit,
    .activate         = nvvfx_framegen_activate,
    FILTER_INPUTS(nvvfx_framegen_inputs),
    FILTER_OUTPUTS(nvvfx_framegen_outputs),
    FILTER_SINGLE_PIXFMT(AV_PIX_FMT_D3D11),
    .p.flags          = AVFILTER_FLAG_HWDEVICE,
    .flags_internal   = FF_FILTER_FLAG_HWFRAME_AWARE,
};
