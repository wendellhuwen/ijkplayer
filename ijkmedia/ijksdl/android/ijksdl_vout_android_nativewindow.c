/*****************************************************************************
 * ijksdl_vout_android_nativewindow.c
 *****************************************************************************
 *
 * copyright (c) 2013 Zhang Rui <bbcallen@gmail.com>
 *
 * This file is part of ijkPlayer.
 *
 * ijkPlayer is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * ijkPlayer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with ijkPlayer; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "ijksdl_vout_android_nativewindow.h"

#include <assert.h>
#include <android/native_window.h>
#include "ijksdl/ijksdl_vout.h"
#include "ijksdl/ijksdl_vout_internal.h"
#include "ijksdl/ijksdl_container.h"
#include "ijksdl/ffmpeg/ijksdl_vout_overlay_ffmpeg.h"
#include "ijksdl_codec_android_mediacodec.h"
#include "ijksdl_inc_internal_android.h"
#include "ijksdl_vout_overlay_android_mediacodec.h"
#include "android_nativewindow.h"

#ifndef AMCTRACE
#define AMCTRACE(...)
#endif

struct SDL_AMediaCodecBufferProxy
{
    int                 buffer_index;
    SDL_AMediaCodec    *weak_acodec;    // only used for compare
};

static void SDL_AMediaCodecBufferProxy_reset(SDL_AMediaCodecBufferProxy *proxy)
{
    proxy->buffer_index = -1;
    proxy->weak_acodec  = NULL;
}

static void SDL_AMediaCodecBufferProxy_init(SDL_AMediaCodecBufferProxy *proxy)
{
    assert(proxy);
    SDL_AMediaCodecBufferProxy_reset(proxy);
}

static void SDL_AMediaCodecBufferProxy_destroy(SDL_AMediaCodecBufferProxy *proxy)
{
    if (!proxy)
        return;
    SDL_AMediaCodecBufferProxy_reset(proxy);
}

static void SDL_AMediaCodecBufferProxy_destroyP(SDL_AMediaCodecBufferProxy **proxy)
{
    if (!proxy)
        return;
    SDL_AMediaCodecBufferProxy_destroy(*proxy);
    *proxy = NULL;
}

static void SDL_AMediaCodecBufferProxy_invalidate(SDL_AMediaCodecBufferProxy *proxy)
{
    SDL_AMediaCodecBufferProxy_reset(proxy);
}

typedef struct SDL_Vout_Opaque {
    ANativeWindow   *native_window;
    SDL_AMediaCodec *acodec;
    int              null_native_window_warned; // reduce log for null window

    ISDL_Array       overlay_manager;
    ISDL_Array       overlay_pool;
} SDL_Vout_Opaque;

static SDL_VoutOverlay *func_create_overlay_l(int width, int height, Uint32 format, SDL_Vout *vout)
{
    switch (format) {
    case SDL_FCC__AMC:
        return SDL_VoutAMediaCodec_CreateOverlay(width, height, format, vout);
    default:
        return SDL_VoutFFmpeg_CreateOverlay(width, height, format, vout);
    }
}

static SDL_VoutOverlay *func_create_overlay(int width, int height, Uint32 format, SDL_Vout *vout)
{
    SDL_LockMutex(vout->mutex);
    SDL_VoutOverlay *overlay = func_create_overlay_l(width, height, format, vout);
    SDL_UnlockMutex(vout->mutex);
    return overlay;
}

static void func_free_l(SDL_Vout *vout)
{
    if (!vout)
        return;

    SDL_Vout_Opaque *opaque = vout->opaque;
    if (opaque) {
        SDL_AMediaCodecBufferProxy **begin = (SDL_AMediaCodecBufferProxy **)ISDL_Array__begin(&opaque->overlay_manager);
        SDL_AMediaCodecBufferProxy **end   = (SDL_AMediaCodecBufferProxy **)ISDL_Array__end(&opaque->overlay_manager);
        for (; begin < end; ++begin) {
            SDL_AMediaCodecBufferProxy_destroyP(begin);
        }
        ISDL_Array__clear(&opaque->overlay_pool);
        ISDL_Array__clear(&opaque->overlay_manager);

        if (opaque->native_window) {
            ANativeWindow_release(opaque->native_window);
            opaque->native_window = NULL;
        }
    }

    SDL_Vout_FreeInternal(vout);
}

static int func_display_overlay_l(SDL_Vout *vout, SDL_VoutOverlay *overlay)
{
    SDL_Vout_Opaque *opaque = vout->opaque;
    ANativeWindow *native_window = opaque->native_window;

    if (!native_window && !opaque->null_native_window_warned) {
        opaque->null_native_window_warned = 1;
        ALOGW("voud_display_overlay_l: NULL native_window");
        return -1;
    }

    if (!overlay) {
        ALOGE("voud_display_overlay_l: NULL overlay");
        return -1;
    }

    if (overlay->w <= 0 || overlay->h <= 0) {
        ALOGE("voud_display_overlay_l: invalid overlay dimensions(%d, %d)", overlay->w, overlay->h);
        return -1;
    }

    switch(overlay->format) {
    case SDL_FCC__AMC:
        return SDL_VoutOverlayAMediaCodec_releaseFrame_l(overlay, NULL, true);
    default:
        return SDL_Android_NativeWindow_display_l(native_window, overlay);
    }
}

static int func_display_overlay(SDL_Vout *vout, SDL_VoutOverlay *overlay)
{
    SDL_LockMutex(vout->mutex);
    int retval = func_display_overlay_l(vout, overlay);
    SDL_UnlockMutex(vout->mutex);
    return retval;
}

static SDL_Class g_nativewindow_class = {
    .name = "ANativeWindow_Vout",
};

SDL_Vout *SDL_VoutAndroid_CreateForANativeWindow()
{
    SDL_Vout *vout = SDL_Vout_CreateInternal(sizeof(SDL_Vout_Opaque));
    if (!vout)
        return NULL;

    SDL_Vout_Opaque *opaque = vout->opaque;
    opaque->native_window = NULL;
    if (ISDL_Array__init(&opaque->overlay_manager, 32))
        goto fail;
    if (ISDL_Array__init(&opaque->overlay_pool, 32))
        goto fail;

    vout->opaque_class    = &g_nativewindow_class;
    vout->create_overlay  = func_create_overlay;
    vout->free_l          = func_free_l;
    vout->display_overlay = func_display_overlay;

    return vout;
fail:
    func_free_l(vout);
    return NULL;
}

static void SDL_VoutAndroid_InvalidateAllBuffers_l(SDL_Vout *vout)
{
    AMCTRACE("%s\n", __func__);
    SDL_Vout_Opaque *opaque = vout->opaque;

    SDL_AMediaCodecBufferProxy **begin = (SDL_AMediaCodecBufferProxy **)ISDL_Array__begin(&opaque->overlay_manager);
    SDL_AMediaCodecBufferProxy **end   = (SDL_AMediaCodecBufferProxy **)ISDL_Array__end(&opaque->overlay_manager);
    for (; begin < end; ++begin) {
        SDL_AMediaCodecBufferProxy_invalidate(*begin);
    }
}

void SDL_VoutAndroid_InvalidateAllBuffers(SDL_Vout *vout)
{
    SDL_LockMutex(vout->mutex);
    SDL_VoutAndroid_InvalidateAllBuffers_l(vout);
    SDL_UnlockMutex(vout->mutex);
}

static void SDL_VoutAndroid_SetNativeWindow_l(SDL_Vout *vout, ANativeWindow *native_window)
{
    AMCTRACE("%s(%p, %p)\n", __func__, vout, native_window);
    SDL_Vout_Opaque *opaque = vout->opaque;

    if (opaque->native_window == native_window) {
        if (native_window == NULL) {
            // always invalidate buffers, if native_window is changed
            SDL_VoutAndroid_InvalidateAllBuffers_l(vout);
        }
        return;
    }

    SDL_VoutAndroid_InvalidateAllBuffers_l(vout);

    if (opaque->native_window)
        ANativeWindow_release(opaque->native_window);

    if (native_window)
        ANativeWindow_acquire(native_window);

    opaque->native_window = native_window;
    opaque->null_native_window_warned = 0;
}

void SDL_VoutAndroid_SetNativeWindow(SDL_Vout *vout, ANativeWindow *native_window)
{
    SDL_LockMutex(vout->mutex);
    SDL_VoutAndroid_SetNativeWindow_l(vout, native_window);
    SDL_UnlockMutex(vout->mutex);
}

static void SDL_VoutAndroid_setAMediaCodec_l(SDL_Vout *vout, SDL_AMediaCodec *acodec)
{
    AMCTRACE("%s(%p)\n", __func__, acodec);
    SDL_Vout_Opaque *opaque = vout->opaque;
    if (opaque->acodec == acodec)
        return;

    SDL_VoutAndroid_InvalidateAllBuffers_l(vout);

    SDL_AMediaCodec_decreaseReferenceP(&opaque->acodec);
    opaque->acodec = acodec;
    if (opaque->acodec)
        SDL_AMediaCodec_increaseReference(opaque->acodec);
}

void SDL_VoutAndroid_setAMediaCodec(SDL_Vout *vout, SDL_AMediaCodec *acodec)
{
    SDL_LockMutex(vout->mutex);
    SDL_VoutAndroid_setAMediaCodec_l(vout, acodec);
    SDL_UnlockMutex(vout->mutex);
}

SDL_AMediaCodec *SDL_VoutAndroid_peekAMediaCodec(SDL_Vout *vout)
{
    SDL_Vout_Opaque *opaque = vout->opaque;
    SDL_AMediaCodec *acodec = NULL;

    SDL_LockMutex(vout->mutex);
    acodec = opaque->acodec;
    SDL_UnlockMutex(vout->mutex);
    return acodec;
}

static SDL_AMediaCodecBufferProxy *SDL_VoutAndroid_obtainBufferProxy_l(SDL_Vout *vout, int buffer_index)
{
    SDL_Vout_Opaque *opaque = vout->opaque;
    SDL_AMediaCodecBufferProxy *proxy = NULL;

    if (ISDL_Array__size(&opaque->overlay_pool) > 0) {
        proxy = ISDL_Array__pop_back(&opaque->overlay_pool);
        SDL_AMediaCodecBufferProxy_reset(proxy);
    } else {
        proxy = (SDL_AMediaCodecBufferProxy *)mallocz(sizeof(SDL_AMediaCodecBufferProxy));
        if (!proxy)
            return NULL;
        SDL_AMediaCodecBufferProxy_init(proxy);
        ISDL_Array__push_back(&opaque->overlay_manager, proxy);
    }

    proxy->weak_acodec  = opaque->acodec;
    proxy->buffer_index = buffer_index;
    return proxy;
}

SDL_AMediaCodecBufferProxy *SDL_VoutAndroid_obtainBufferProxy(SDL_Vout *vout, int buffer_index)
{
    SDL_AMediaCodecBufferProxy *proxy = NULL;
    SDL_LockMutex(vout->mutex);
    proxy = SDL_VoutAndroid_obtainBufferProxy_l(vout, buffer_index);
    SDL_UnlockMutex(vout->mutex);
    return proxy;
}

static int SDL_VoutAndroid_releaseBufferProxy_l(SDL_Vout *vout, SDL_AMediaCodecBufferProxy *proxy, bool render)
{
    SDL_Vout_Opaque *opaque = vout->opaque;

    if (!proxy)
        return 0;

    ISDL_Array__push_back(&opaque->overlay_pool, proxy);

    if (proxy->weak_acodec != opaque->acodec || opaque->acodec == NULL) {
        ALOGE("%s: obselete AMediaCodec %p: current: %p\n", __func__, proxy->weak_acodec, opaque->acodec);
        return 0;
    }

    if (proxy->buffer_index < 0) {
        ALOGE("%s: invalid AMediaCodec buffer index %d\n", __func__, proxy->buffer_index);
        return 0;
    }

    sdl_amedia_status_t amc_ret = SDL_AMediaCodec_releaseOutputBuffer(opaque->acodec, proxy->buffer_index, render);
    proxy->buffer_index = -1;
    if (amc_ret != SDL_AMEDIA_OK) {
        ALOGE("%s: SDL_VoutAndroid_renderBufferProxy: failed (%d)\n", __func__, (int)amc_ret);
        return -1;
    }

    return 0;
}

static int SDL_VoutAndroid_releaseBufferProxy(SDL_Vout *vout, SDL_AMediaCodecBufferProxy *proxy, bool render)
{
    int ret = 0;

    if (!proxy)
        return 0;

    SDL_LockMutex(vout->mutex);
    ret = SDL_VoutAndroid_releaseBufferProxy_l(vout, proxy, render);
    SDL_UnlockMutex(vout->mutex);
    return ret;
}

int SDL_VoutAndroid_releaseBufferProxyP(SDL_Vout *vout, SDL_AMediaCodecBufferProxy **proxy, bool render)
{
    int ret = 0;

    if (!proxy)
        return 0;

    ret = SDL_VoutAndroid_releaseBufferProxy(vout, *proxy, render);
    *proxy = NULL;
    return ret;
}

int SDL_VoutAndroid_releaseBufferProxyP_l(SDL_Vout *vout, SDL_AMediaCodecBufferProxy **proxy, bool render)
{
    int ret = 0;

    if (!proxy)
        return 0;

    ret = SDL_VoutAndroid_releaseBufferProxy_l(vout, *proxy, render);
    *proxy = NULL;
    return ret;
}
