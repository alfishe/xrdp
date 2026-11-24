/**
 * xrdp: A Remote Desktop Protocol server.
 *
 * Copyright (C) 2026 alfishe
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * H.264 encoder for the EGFX pipeline based on NVIDIA NVENC.
 *
 * The encoder keeps a persistent NV12 copy of the screen (the "shadow"),
 * updates it from the damaged rectangles supplied by the caller, uploads
 * it to an NVENC input buffer and returns the Annex B bitstream. The CUDA
 * and NVENC libraries are loaded at run time, so the binary has no hard
 * dependency on the NVIDIA driver.
 */

#if defined(HAVE_CONFIG_H)
#include <config_ac.h>
#endif

#include <stdint.h>
#include <string.h>

#include "xrdp.h"
#include "arch.h"
#include "os_calls.h"
#include "xrdp_encoder_nvenc.h"
#include "xrdp_tconfig.h"

/* The ffnvcodec loader needs these before it is included */
#define FFNV_LOG_FUNC(logctx, msg, ...) \
    LOG(LOG_LEVEL_WARNING, msg, __VA_ARGS__)
#define FFNV_DEBUG_LOG_FUNC(logctx, msg, ...) \
    LOG_DEVEL(LOG_LEVEL_TRACE, msg, __VA_ARGS__)
#include <ffnvcodec/dynlink_loader.h>

#define NVENC_MAX_ENCODERS 16
#define NVENC_DEFAULT_QP 23
/* The encoder has no notion of time: every submitted frame is coded at a
 * fixed QP. NVENC still wants a nominal rate; with constant QP it has no
 * effect on the output. Frame pacing and bandwidth adaptation belong to
 * the layer that decides when to submit frames. */
#define NVENC_NOMINAL_FPS 60

struct nvenc_encoder
{
    void *enc;                    /* NVENC session handle */
    NV_ENC_INPUT_PTR in_buf;
    NV_ENC_OUTPUT_PTR out_buf;
    uint8_t *shadow;              /* NV12, pad_w x pad_h */
    int width;                    /* size requested by the caller */
    int height;
    int pad_w;                    /* size rounded up to the macroblock */
    int pad_h;
    uint64_t frame_count;
};

struct nvenc_global
{
    CudaFunctions *cu;
    NvencFunctions *nv;
    NV_ENCODE_API_FUNCTION_LIST fl;
    CUcontext ctx;
    struct nvenc_encoder encoders[NVENC_MAX_ENCODERS];
    struct xrdp_tconfig_gfx_nvenc_param param[NUM_CONNECTION_TYPES];
};

/*****************************************************************************/
static const GUID *
nvenc_preset_guid(const char *name)
{
    static const GUID *const presets[] =
    {
        &NV_ENC_PRESET_P1_GUID, &NV_ENC_PRESET_P2_GUID,
        &NV_ENC_PRESET_P3_GUID, &NV_ENC_PRESET_P4_GUID,
        &NV_ENC_PRESET_P5_GUID, &NV_ENC_PRESET_P6_GUID,
        &NV_ENC_PRESET_P7_GUID
    };

    if (name != NULL && (name[0] == 'p' || name[0] == 'P') &&
            name[1] >= '1' && name[1] <= '7' && name[2] == '\0')
    {
        return presets[name[1] - '1'];
    }
    return &NV_ENC_PRESET_P2_GUID;
}


/*****************************************************************************/
/**
 * Loads CUDA and NVENC and creates a CUDA context. The context is
 * detached from the calling thread before returning.
 */
static int
nvenc_runtime_open(struct nvenc_global *g)
{
    CUdevice dev;
    uint32_t max_ver = 0;
    uint32_t our_ver = (NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION;

    if (cuda_load_functions(&g->cu, NULL) < 0)
    {
        LOG(LOG_LEVEL_ERROR, "NVENC: cannot load the CUDA driver library");
        return 1;
    }
    if (nvenc_load_functions(&g->nv, NULL) < 0)
    {
        LOG(LOG_LEVEL_ERROR, "NVENC: cannot load the NVENC library");
        return 1;
    }
    if (g->cu->cuInit(0) != CUDA_SUCCESS ||
            g->cu->cuDeviceGet(&dev, 0) != CUDA_SUCCESS ||
            g->cu->cuCtxCreate(&g->ctx, 0, dev) != CUDA_SUCCESS)
    {
        LOG(LOG_LEVEL_ERROR, "NVENC: cannot create a CUDA context");
        g->ctx = NULL;
        return 1;
    }
    g->cu->cuCtxPopCurrent(NULL);

    if (g->nv->NvEncodeAPIGetMaxSupportedVersion(&max_ver) != NV_ENC_SUCCESS ||
            max_ver < our_ver)
    {
        LOG(LOG_LEVEL_ERROR, "NVENC: driver supports API 0x%x, need 0x%x",
            max_ver, our_ver);
        return 1;
    }

    g->fl.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    if (g->nv->NvEncodeAPICreateInstance(&g->fl) != NV_ENC_SUCCESS)
    {
        LOG(LOG_LEVEL_ERROR, "NVENC: NvEncodeAPICreateInstance failed");
        return 1;
    }
    return 0;
}

/*****************************************************************************/
static void
nvenc_runtime_close(struct nvenc_global *g)
{
    if (g->ctx != NULL && g->cu != NULL)
    {
        g->cu->cuCtxDestroy(g->ctx);
        g->ctx = NULL;
    }
    nvenc_free_functions(&g->nv);
    cuda_free_functions(&g->cu);
}

/*****************************************************************************/
static int
nvenc_open_session(struct nvenc_global *g, void **enc)
{
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS op;

    memset(&op, 0, sizeof(op));
    op.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    op.deviceType = NV_ENC_DEVICE_TYPE_CUDA;
    op.device = g->ctx;
    op.apiVersion = NVENCAPI_VERSION;
    *enc = NULL;
    return g->fl.nvEncOpenEncodeSessionEx(&op, enc) != NV_ENC_SUCCESS;
}

/*****************************************************************************/
int
xrdp_encoder_nvenc_install_ok(void)
{
    static int result = -1;
    struct nvenc_global g;
    void *enc;

    if (result >= 0)
    {
        return result;
    }
    memset(&g, 0, sizeof(g));
    result = 0;
    if (nvenc_runtime_open(&g) == 0)
    {
        /* Make sure there is a GPU that can actually open a session */
        if (g.cu->cuCtxPushCurrent(g.ctx) == CUDA_SUCCESS)
        {
            if (nvenc_open_session(&g, &enc) == 0)
            {
                g.fl.nvEncDestroyEncoder(enc);
                result = 1;
            }
            else
            {
                LOG(LOG_LEVEL_ERROR, "NVENC: cannot open an encode session");
            }
            g.cu->cuCtxPopCurrent(NULL);
        }
    }
    nvenc_runtime_close(&g);
    return result;
}

/*****************************************************************************/
void *
xrdp_encoder_nvenc_create(void)
{
    struct nvenc_global *g;
    struct xrdp_tconfig_gfx gfxconfig;

    LOG_DEVEL(LOG_LEVEL_TRACE, "xrdp_encoder_nvenc_create:");
    g = g_new0(struct nvenc_global, 1);
    if (g == NULL)
    {
        return NULL;
    }
    if (nvenc_runtime_open(g) != 0)
    {
        nvenc_runtime_close(g);
        g_free(g);
        return NULL;
    }
    tconfig_load_gfx(GFX_CONF, &gfxconfig);
    memcpy(&g->param, &gfxconfig.nvenc_param, sizeof(g->param));
    return g;
}

/*****************************************************************************/
static void
nvenc_encoder_close(struct nvenc_global *g, struct nvenc_encoder *e)
{
    if (e->enc != NULL)
    {
        if (e->in_buf != NULL)
        {
            g->fl.nvEncDestroyInputBuffer(e->enc, e->in_buf);
        }
        if (e->out_buf != NULL)
        {
            g->fl.nvEncDestroyBitstreamBuffer(e->enc, e->out_buf);
        }
        g->fl.nvEncDestroyEncoder(e->enc);
    }
    g_free(e->shadow);
    memset(e, 0, sizeof(*e));
}

/*****************************************************************************/
int
xrdp_encoder_nvenc_delete(void *handle)
{
    struct nvenc_global *g = (struct nvenc_global *) handle;
    int index;

    if (g == NULL)
    {
        return 0;
    }
    if (g->cu->cuCtxPushCurrent(g->ctx) == CUDA_SUCCESS)
    {
        for (index = 0; index < NVENC_MAX_ENCODERS; index++)
        {
            nvenc_encoder_close(g, &g->encoders[index]);
        }
        g->cu->cuCtxPopCurrent(NULL);
    }
    nvenc_runtime_close(g);
    g_free(g);
    return 0;
}

/*****************************************************************************/
static int
nvenc_encoder_open(struct nvenc_global *g, struct nvenc_encoder *e,
                   int width, int height, int ct)
{
    const struct xrdp_tconfig_gfx_nvenc_param *p = &g->param[ct];
    const GUID *preset = nvenc_preset_guid(p->preset);
    const NV_ENC_TUNING_INFO tuning = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
    NV_ENC_PRESET_CONFIG pc;
    NV_ENC_CONFIG cfg;
    NV_ENC_INITIALIZE_PARAMS ip;
    NV_ENC_CREATE_INPUT_BUFFER cib;
    NV_ENC_CREATE_BITSTREAM_BUFFER cbb;
    NVENCSTATUS st;
    int qp;

    memset(e, 0, sizeof(*e));
    e->width = width;
    e->height = height;
    e->pad_w = (width + 15) & ~15;
    e->pad_h = (height + 15) & ~15;

    qp = (p->Qp >= 1 && p->Qp <= 51) ? p->Qp : NVENC_DEFAULT_QP;

    if (nvenc_open_session(g, &e->enc) != 0)
    {
        LOG(LOG_LEVEL_ERROR, "NVENC: nvEncOpenEncodeSessionEx failed");
        e->enc = NULL;
        return 1;
    }

    memset(&pc, 0, sizeof(pc));
    pc.version = NV_ENC_PRESET_CONFIG_VER;
    pc.presetCfg.version = NV_ENC_CONFIG_VER;
    st = g->fl.nvEncGetEncodePresetConfigEx(e->enc, NV_ENC_CODEC_H264_GUID,
                                            *preset, tuning, &pc);
    if (st != NV_ENC_SUCCESS)
    {
        LOG(LOG_LEVEL_ERROR, "NVENC: nvEncGetEncodePresetConfigEx failed %d: %s",
            (int) st, g->fl.nvEncGetLastErrorString(e->enc));
        nvenc_encoder_close(g, e);
        return 2;
    }
    cfg = pc.presetCfg;

    /* Low latency, no B frames, a single IDR at the start. P frames only,
     * the transport is reliable so there is no need for periodic IDRs. */
    cfg.gopLength = NVENC_INFINITE_GOPLENGTH;
    cfg.frameIntervalP = 1;
    cfg.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CONSTQP;
    cfg.rcParams.constQP.qpInterP = (uint32_t) qp;
    cfg.rcParams.constQP.qpInterB = (uint32_t) qp;
    cfg.rcParams.constQP.qpIntra = (uint32_t) qp;
    cfg.rcParams.zeroReorderDelay = 1;
    cfg.encodeCodecConfig.h264Config.idrPeriod = NVENC_INFINITE_GOPLENGTH;
    cfg.encodeCodecConfig.h264Config.repeatSPSPPS = 1;
    cfg.encodeCodecConfig.h264Config.chromaFormatIDC = 1;

    memset(&ip, 0, sizeof(ip));
    ip.version = NV_ENC_INITIALIZE_PARAMS_VER;
    ip.encodeGUID = NV_ENC_CODEC_H264_GUID;
    ip.presetGUID = *preset;
    ip.tuningInfo = tuning;
    ip.encodeWidth = (uint32_t) e->pad_w;
    ip.encodeHeight = (uint32_t) e->pad_h;
    ip.darWidth = (uint32_t) e->pad_w;
    ip.darHeight = (uint32_t) e->pad_h;
    ip.maxEncodeWidth = (uint32_t) e->pad_w;
    ip.maxEncodeHeight = (uint32_t) e->pad_h;
    ip.frameRateNum = NVENC_NOMINAL_FPS;
    ip.frameRateDen = 1;
    ip.enablePTD = 1;
    ip.encodeConfig = &cfg;
    st = g->fl.nvEncInitializeEncoder(e->enc, &ip);
    if (st != NV_ENC_SUCCESS)
    {
        LOG(LOG_LEVEL_ERROR, "NVENC: nvEncInitializeEncoder %dx%d failed %d: %s",
            e->pad_w, e->pad_h, (int) st, g->fl.nvEncGetLastErrorString(e->enc));
        nvenc_encoder_close(g, e);
        return 3;
    }

    memset(&cib, 0, sizeof(cib));
    cib.version = NV_ENC_CREATE_INPUT_BUFFER_VER;
    cib.width = (uint32_t) e->pad_w;
    cib.height = (uint32_t) e->pad_h;
    cib.bufferFmt = NV_ENC_BUFFER_FORMAT_NV12;
    st = g->fl.nvEncCreateInputBuffer(e->enc, &cib);
    if (st != NV_ENC_SUCCESS)
    {
        LOG(LOG_LEVEL_ERROR, "NVENC: nvEncCreateInputBuffer failed %d", (int) st);
        nvenc_encoder_close(g, e);
        return 4;
    }
    e->in_buf = cib.inputBuffer;

    memset(&cbb, 0, sizeof(cbb));
    cbb.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
    st = g->fl.nvEncCreateBitstreamBuffer(e->enc, &cbb);
    if (st != NV_ENC_SUCCESS)
    {
        LOG(LOG_LEVEL_ERROR, "NVENC: nvEncCreateBitstreamBuffer failed %d", (int) st);
        nvenc_encoder_close(g, e);
        return 5;
    }
    e->out_buf = cbb.bitstreamBuffer;

    e->shadow = (uint8_t *) g_malloc(e->pad_w * e->pad_h * 3 / 2, 0);
    if (e->shadow == NULL)
    {
        nvenc_encoder_close(g, e);
        return 6;
    }
    /* black: Y = 16, U = V = 128 */
    memset(e->shadow, 16, (size_t) e->pad_w * e->pad_h);
    memset(e->shadow + (size_t) e->pad_w * e->pad_h, 128,
           (size_t) e->pad_w * e->pad_h / 2);

    LOG(LOG_LEVEL_INFO, "NVENC: encoder opened %dx%d (coded %dx%d) "
        "preset %s qp %d", width, height, e->pad_w, e->pad_h,
        (p->preset[0] != '\0') ? p->preset : "p2", qp);
    return 0;
}

/*****************************************************************************/
/**
 * Copies the damaged rectangles from the caller's NV12 surface into the
 * shadow copy. Same layout rules as the x264 and OpenH264 encoders.
 */
static void
nvenc_update_shadow(struct nvenc_encoder *e, int left, int top,
                    int twidth, int theight, const char *data,
                    const short *crects, int num_crects)
{
    const uint8_t *src_y = (const uint8_t *) data;
    const uint8_t *src_uv = src_y + (size_t) twidth * theight;
    uint8_t *dst_y = e->shadow;
    uint8_t *dst_uv = e->shadow + (size_t) e->pad_w * e->pad_h;
    int index;
    int x;
    int y;
    int cx;
    int cy;
    int row;

    for (index = 0; index < num_crects; index++)
    {
        x = crects[index * 4 + 0];
        y = crects[index * 4 + 1];
        cx = crects[index * 4 + 2];
        cy = crects[index * 4 + 3];
        if (x < left || y < top || cx <= 0 || cy <= 0 ||
                x - left + cx > e->pad_w || y - top + cy > e->pad_h ||
                x + cx > twidth || y + cy > theight)
        {
            continue;
        }
        for (row = 0; row < cy; row++)
        {
            memcpy(dst_y + (size_t) e->pad_w * (y - top + row) + (x - left),
                   src_y + (size_t) twidth * (y + row) + x, (size_t) cx);
        }
        /* chroma is subsampled 2x vertically, one NV12 UV row per 2 luma rows */
        for (row = 0; row < cy; row += 2)
        {
            memcpy(dst_uv + (size_t) e->pad_w * ((y - top) / 2 + row / 2) +
                   (x - left),
                   src_uv + (size_t) twidth * (y / 2 + row / 2) + x,
                   (size_t) cx);
        }
    }
}

/*****************************************************************************/
static int
nvenc_encode_frame(struct nvenc_global *g, struct nvenc_encoder *e,
                   char *cdata, int *cdata_bytes)
{
    NV_ENC_LOCK_INPUT_BUFFER li;
    NV_ENC_PIC_PARAMS pp;
    NV_ENC_LOCK_BITSTREAM lb;
    NVENCSTATUS st;
    uint8_t *dst;
    int row;
    int rc = 0;

    memset(&li, 0, sizeof(li));
    li.version = NV_ENC_LOCK_INPUT_BUFFER_VER;
    li.inputBuffer = e->in_buf;
    st = g->fl.nvEncLockInputBuffer(e->enc, &li);
    if (st != NV_ENC_SUCCESS)
    {
        LOG(LOG_LEVEL_ERROR, "NVENC: nvEncLockInputBuffer failed %d", (int) st);
        return 10;
    }
    dst = (uint8_t *) li.bufferDataPtr;
    for (row = 0; row < e->pad_h; row++)
    {
        memcpy(dst + (size_t) li.pitch * row,
               e->shadow + (size_t) e->pad_w * row, (size_t) e->pad_w);
    }
    dst += (size_t) li.pitch * e->pad_h;
    for (row = 0; row < e->pad_h / 2; row++)
    {
        memcpy(dst + (size_t) li.pitch * row,
               e->shadow + (size_t) e->pad_w * e->pad_h +
               (size_t) e->pad_w * row, (size_t) e->pad_w);
    }
    g->fl.nvEncUnlockInputBuffer(e->enc, e->in_buf);

    memset(&pp, 0, sizeof(pp));
    pp.version = NV_ENC_PIC_PARAMS_VER;
    pp.inputBuffer = e->in_buf;
    pp.bufferFmt = NV_ENC_BUFFER_FORMAT_NV12;
    pp.inputWidth = (uint32_t) e->pad_w;
    pp.inputHeight = (uint32_t) e->pad_h;
    pp.inputPitch = li.pitch;
    pp.outputBitstream = e->out_buf;
    pp.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    pp.inputTimeStamp = e->frame_count;
    if (e->frame_count == 0)
    {
        pp.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR |
                            NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
    }
    st = g->fl.nvEncEncodePicture(e->enc, &pp);
    if (st != NV_ENC_SUCCESS)
    {
        LOG(LOG_LEVEL_ERROR, "NVENC: nvEncEncodePicture failed %d: %s",
            (int) st, g->fl.nvEncGetLastErrorString(e->enc));
        return 11;
    }

    memset(&lb, 0, sizeof(lb));
    lb.version = NV_ENC_LOCK_BITSTREAM_VER;
    lb.outputBitstream = e->out_buf;
    st = g->fl.nvEncLockBitstream(e->enc, &lb);
    if (st != NV_ENC_SUCCESS)
    {
        LOG(LOG_LEVEL_ERROR, "NVENC: nvEncLockBitstream failed %d", (int) st);
        return 12;
    }
    if (lb.bitstreamSizeInBytes == 0)
    {
        rc = 13;
    }
    else if ((uint32_t) *cdata_bytes < lb.bitstreamSizeInBytes)
    {
        LOG(LOG_LEVEL_ERROR, "NVENC: output buffer too small, need %u have %d",
            lb.bitstreamSizeInBytes, *cdata_bytes);
        rc = 14;
    }
    else
    {
        memcpy(cdata, lb.bitstreamBufferPtr, lb.bitstreamSizeInBytes);
        *cdata_bytes = (int) lb.bitstreamSizeInBytes;
    }
    g->fl.nvEncUnlockBitstream(e->enc, e->out_buf);
    e->frame_count++;
    return rc;
}

/*****************************************************************************/
int
xrdp_encoder_nvenc_encode(void *handle, int session, int left, int top,
                          int width, int height, int twidth, int theight,
                          int format, const char *data,
                          short *crects, int num_crects,
                          char *cdata, int *cdata_bytes, int connection_type,
                          int *flags_ptr)
{
    struct nvenc_global *g = (struct nvenc_global *) handle;
    struct nvenc_encoder *e;
    int ct;
    int flags = 0;
    int rv = 0;

    (void) format;
    if (g == NULL)
    {
        return 1;
    }
    e = &g->encoders[session % NVENC_MAX_ENCODERS];

    ct = connection_type;
    if (ct > CONNECTION_TYPE_LAN || ct < CONNECTION_TYPE_MODEM)
    {
        ct = CONNECTION_TYPE_LAN;
    }

    if (g->cu->cuCtxPushCurrent(g->ctx) != CUDA_SUCCESS)
    {
        LOG(LOG_LEVEL_ERROR, "NVENC: cuCtxPushCurrent failed");
        return 2;
    }

    if (e->enc == NULL || e->width != width || e->height != height)
    {
        if (e->enc != NULL)
        {
            nvenc_encoder_close(g, e);
            flags |= 2;
        }
        if (width > 0 && height > 0)
        {
            if (nvenc_encoder_open(g, e, width, height, ct) != 0)
            {
                g->cu->cuCtxPopCurrent(NULL);
                return 3;
            }
            flags |= 1;
        }
    }

    if (data != NULL && e->enc != NULL)
    {
        nvenc_update_shadow(e, left, top, twidth, theight, data,
                            crects, num_crects);
        rv = nvenc_encode_frame(g, e, cdata, cdata_bytes);
    }
    g->cu->cuCtxPopCurrent(NULL);

    if (flags_ptr != NULL)
    {
        *flags_ptr = flags;
    }
    return rv;
}
