/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* Apple's 8×8 tile encoding, the picture of Screen Sharing's Adaptive quality. */
#define DR_APPLE_ADAPTIVE_ENCODING 0x3f3

typedef struct _DrAppleAdaptive DrAppleAdaptive;

DrAppleAdaptive *dr_apple_adaptive_new    (void);
void             dr_apple_adaptive_free   (DrAppleAdaptive *self);

/* Decodes one rectangle's body (the bytes after its u32 length) onto pixels,
 * a framebuffer of width × height 32-bit pixels, 0xAARRGGBB. Each rectangle
 * draws on what the ones before it left, so pass the same framebuffer every
 * time; a new size forgets what each tile was coded as. Returns FALSE with
 * error set for a rectangle that does not decode, whose pixels stay as far as
 * they were drawn. */
gboolean         dr_apple_adaptive_decode (DrAppleAdaptive *self,
                                           guint32         *pixels,
                                           int              width,
                                           int              height,
                                           int              x,
                                           int              y,
                                           int              w,
                                           int              h,
                                           const guint8    *body,
                                           gsize            len,
                                           GError         **error);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (DrAppleAdaptive, dr_apple_adaptive_free)

G_END_DECLS
