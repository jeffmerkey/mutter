/*
 * Cogl
 *
 * A Low Level GPU Graphics and Utilities API
 *
 * Copyright (C) 2009,2010,2011 Intel Corporation.
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 *
 *
 * Authors:
 *  Neil Roberts   <neil@linux.intel.com>
 */

#include "config.h"

#include "cogl/cogl-debug.h"
#include "cogl/cogl-util.h"
#include "cogl/cogl-texture-private.h"
#include "cogl/cogl-atlas-texture-private.h"
#include "cogl/cogl-texture-2d-private.h"
#include "cogl/cogl-sub-texture-private.h"
#include "cogl/cogl-context-private.h"
#include "cogl/cogl-texture-driver.h"
#include "cogl/cogl-rectangle-map.h"
#include "cogl/cogl-journal-private.h"
#include "cogl/cogl-atlas-private.h"
#include "cogl/cogl-sub-texture.h"
#include "cogl/driver/gl/cogl-texture-gl-private.h"

#include <stdlib.h>

G_DEFINE_FINAL_TYPE (CoglAtlasTexture, cogl_atlas_texture, COGL_TYPE_TEXTURE)

static void
_cogl_atlas_texture_remove_from_atlas (CoglAtlasTexture *atlas_tex)
{
  if (atlas_tex->atlas)
    {
      _cogl_atlas_remove (atlas_tex->atlas,
                          &atlas_tex->rectangle);

      g_object_unref (atlas_tex->atlas);
      atlas_tex->atlas = NULL;
    }
}

static void
cogl_atlas_texture_dispose (GObject *object)
{
  CoglAtlasTexture *atlas_tex = COGL_ATLAS_TEXTURE (object);

  _cogl_atlas_texture_remove_from_atlas (atlas_tex);
  if (atlas_tex->sub_texture)
    g_object_unref (atlas_tex->sub_texture);

  G_OBJECT_CLASS (cogl_atlas_texture_parent_class)->dispose (object);
}

static CoglTexture *
_cogl_atlas_texture_create_sub_texture (CoglTexture        *full_texture,
                                        const MtkRectangle *rectangle)
{
  CoglContext *ctx = cogl_texture_get_context (full_texture);
  /* Create a subtexture for the given rectangle not including the
     1-pixel border */
  return cogl_sub_texture_new (ctx,
                               full_texture,
                               rectangle->x + 1,
                               rectangle->y + 1,
                               rectangle->width - 2,
                               rectangle->height - 2);
}

static void
_cogl_atlas_texture_update_position_cb (void               *user_data,
                                        CoglTexture        *new_texture,
                                        const MtkRectangle *rectangle)
{
  CoglAtlasTexture *atlas_tex = user_data;

  /* Update the sub texture */
  if (atlas_tex->sub_texture)
    g_object_unref (atlas_tex->sub_texture);
  atlas_tex->sub_texture =
    _cogl_atlas_texture_create_sub_texture (new_texture, rectangle);

  /* Update the position */
  atlas_tex->rectangle = *rectangle;
}

static void
_cogl_atlas_texture_pre_reorganize_foreach_cb
                                         (const MtkRectangle *entry,
                                          void *rectangle_data,
                                          void *user_data)
{
  CoglAtlasTexture *atlas_tex = rectangle_data;

  /* Keep a reference to the texture because we don't want it to be
     destroyed during the reorganization */
  g_object_ref (atlas_tex);

  /* Notify cogl-pipeline.c that the texture's underlying GL texture
   * storage is changing so it knows it may need to bind a new texture
   * if the CoglTexture is reused with the same texture unit. */
  _cogl_pipeline_texture_storage_change_notify (COGL_TEXTURE (atlas_tex));
}

static void
_cogl_atlas_texture_pre_reorganize_cb (void *data)
{
  CoglAtlas *atlas = data;

  /* We don't know if any journal entries currently depend on OpenGL
   * texture coordinates that would be invalidated by reorganizing
   * this atlas so we flush all journals before migrating.
   *
   * We are assuming that texture atlas migration never happens
   * during a flush so we don't have to consider recursion here.
   */
  cogl_context_flush (atlas->context);

  if (atlas->map)
    _cogl_rectangle_map_foreach (atlas->map,
                                 _cogl_atlas_texture_pre_reorganize_foreach_cb,
                                 NULL);
}

typedef struct
{
  CoglAtlasTexture **textures;
  /* Number of textures found so far */
  unsigned int n_textures;
} CoglAtlasTextureGetRectanglesData;

static void
_cogl_atlas_texture_get_rectangles_cb (const MtkRectangle *entry,
                                       void *rectangle_data,
                                       void *user_data)
{
  CoglAtlasTextureGetRectanglesData *data = user_data;

  data->textures[data->n_textures++] = rectangle_data;
}

static void
_cogl_atlas_texture_post_reorganize_cb (void *user_data)
{
  CoglAtlas *atlas = user_data;

  if (atlas->map)
    {
      CoglAtlasTextureGetRectanglesData data;
      unsigned int i;

      data.textures = g_new (CoglAtlasTexture *,
                             _cogl_rectangle_map_get_n_rectangles (atlas->map));
      data.n_textures = 0;

      /* We need to remove all of the references that we took during
         the preorganize callback. We have to get a separate array of
         the textures because CoglRectangleMap doesn't support
         removing rectangles during iteration */
      _cogl_rectangle_map_foreach (atlas->map,
                                   _cogl_atlas_texture_get_rectangles_cb,
                                   &data);

      for (i = 0; i < data.n_textures; i++)
        {
          /* Ignore textures that don't have an atlas yet. This will
             happen when a new texture is added because we allocate
             the structure for the texture so that it can get stored
             in the atlas but it isn't a valid object yet */
          if (data.textures[i]->atlas)
            g_object_unref (data.textures[i]);
        }

      g_free (data.textures);
    }

  /* Notify any listeners that an atlas has changed */
  g_hook_list_invoke (&atlas->context->atlas_reorganize_callbacks, FALSE);
}

static CoglAtlas *
_cogl_atlas_texture_create_atlas (CoglContext *ctx)
{
  CoglAtlas *atlas = cogl_atlas_new (ctx, COGL_PIXEL_FORMAT_RGBA_8888,
                                     0,
                                     _cogl_atlas_texture_update_position_cb);

  cogl_atlas_add_reorganize_callback (atlas,
                                      _cogl_atlas_texture_pre_reorganize_cb,
                                      _cogl_atlas_texture_post_reorganize_cb,
                                      atlas);

  ctx->atlases = g_slist_prepend (ctx->atlases, atlas);
  return atlas;
}

static void
_cogl_atlas_texture_foreach_sub_texture_in_region (
                                       CoglTexture                *tex,
                                       float                       virtual_tx_1,
                                       float                       virtual_ty_1,
                                       float                       virtual_tx_2,
                                       float                       virtual_ty_2,
                                       CoglTextureForeachCallback  callback,
                                       void                       *user_data)
{
  CoglAtlasTexture *atlas_tex = COGL_ATLAS_TEXTURE (tex);

  /* Forward on to the sub texture */
  cogl_texture_foreach_in_region (atlas_tex->sub_texture,
                                  virtual_tx_1,
                                  virtual_ty_1,
                                  virtual_tx_2,
                                  virtual_ty_2,
                                  COGL_PIPELINE_WRAP_MODE_REPEAT,
                                  COGL_PIPELINE_WRAP_MODE_REPEAT,
                                  callback,
                                  user_data);
}

static void
_cogl_atlas_texture_gl_flush_legacy_texobj_wrap_modes (CoglTexture *tex,
                                                       GLenum wrap_mode_s,
                                                       GLenum wrap_mode_t)
{
  CoglAtlasTexture *atlas_tex = COGL_ATLAS_TEXTURE (tex);

  /* Forward on to the sub texture */
  _cogl_texture_gl_flush_legacy_texobj_wrap_modes (atlas_tex->sub_texture,
                                                   wrap_mode_s,
                                                   wrap_mode_t);
}

static gboolean
_cogl_atlas_texture_is_sliced (CoglTexture *tex)
{
  CoglAtlasTexture *atlas_tex = COGL_ATLAS_TEXTURE (tex);

  /* Forward on to the sub texture */
  return cogl_texture_is_sliced (atlas_tex->sub_texture);
}

static gboolean
_cogl_atlas_texture_can_hardware_repeat (CoglTexture *tex)
{
  CoglAtlasTexture *atlas_tex = COGL_ATLAS_TEXTURE (tex);

  /* Forward on to the sub texture */
  return _cogl_texture_can_hardware_repeat (atlas_tex->sub_texture);
}

static void
_cogl_atlas_texture_transform_coords_to_gl (CoglTexture *tex,
                                            float *s,
                                            float *t)
{
  CoglAtlasTexture *atlas_tex = COGL_ATLAS_TEXTURE (tex);
  CoglTextureClass *klass = COGL_TEXTURE_GET_CLASS (atlas_tex->sub_texture);

  /* Forward on to the sub texture */
  klass->transform_coords_to_gl (atlas_tex->sub_texture, s, t);
}

static CoglTransformResult
_cogl_atlas_texture_transform_quad_coords_to_gl (CoglTexture *tex,
                                                 float *coords)
{
  CoglAtlasTexture *atlas_tex = COGL_ATLAS_TEXTURE (tex);
  CoglTextureClass *klass = COGL_TEXTURE_GET_CLASS (atlas_tex->sub_texture);

  /* Forward on to the sub texture */
  return klass->transform_quad_coords_to_gl (atlas_tex->sub_texture, coords);
}

static gboolean
_cogl_atlas_texture_get_gl_texture (CoglTexture *tex,
                                    GLuint *out_gl_handle,
                                    GLenum *out_gl_target)
{
  CoglAtlasTexture *atlas_tex = COGL_ATLAS_TEXTURE (tex);

  /* Forward on to the sub texture */
  return cogl_texture_get_gl_texture (atlas_tex->sub_texture,
                                      out_gl_handle,
                                      out_gl_target);
}

static void
_cogl_atlas_texture_gl_flush_legacy_texobj_filters (CoglTexture *tex,
                                                    GLenum min_filter,
                                                    GLenum mag_filter)
{
  CoglAtlasTexture *atlas_tex = COGL_ATLAS_TEXTURE (tex);

  /* Forward on to the sub texture */
  _cogl_texture_gl_flush_legacy_texobj_filters (atlas_tex->sub_texture,
                                                min_filter, mag_filter);
}

static void
_cogl_atlas_texture_migrate_out_of_atlas (CoglAtlasTexture *atlas_tex)
{
  CoglTexture *standalone_tex;
  CoglContext *ctx;

  /* Make sure this texture is not in the atlas */
  if (!atlas_tex->atlas)
    return;

  ctx = cogl_texture_get_context (COGL_TEXTURE (atlas_tex));
  COGL_NOTE (ATLAS, "Migrating texture out of the atlas");

  /* We don't know if any journal entries currently depend on
   * OpenGL texture coordinates that would be invalidated by
   * migrating textures in this atlas so we flush all journals
   * before migrating.
   *
   * We are assuming that texture atlas migration never happens
   * during a flush so we don't have to consider recursion here.
   */
  cogl_context_flush (ctx);

  standalone_tex =
    _cogl_atlas_copy_rectangle (atlas_tex->atlas,
                                atlas_tex->rectangle.x + 1,
                                atlas_tex->rectangle.y + 1,
                                atlas_tex->rectangle.width - 2,
                                atlas_tex->rectangle.height - 2,
                                atlas_tex->internal_format);
  /* Note: we simply silently ignore failures to migrate a texture
   * out (most likely due to lack of memory) and hope for the
   * best.
   *
   * Maybe we should find a way to report the problem back to the
   * app.
   */
  if (!standalone_tex)
    return;

  /* Notify cogl-pipeline.c that the texture's underlying GL texture
   * storage is changing so it knows it may need to bind a new texture
   * if the CoglTexture is reused with the same texture unit. */
  _cogl_pipeline_texture_storage_change_notify (COGL_TEXTURE (atlas_tex));

  /* We need to unref the sub texture after doing the copy because
     the copy can involve rendering which might cause the texture
     to be used if it is used from a layer that is left in a
     texture unit */
  g_object_unref (atlas_tex->sub_texture);
  atlas_tex->sub_texture = standalone_tex;

  _cogl_atlas_texture_remove_from_atlas (atlas_tex);
}

static void
_cogl_atlas_texture_pre_paint (CoglTexture *tex, CoglTexturePrePaintFlags flags)
{
  CoglAtlasTexture *atlas_tex = COGL_ATLAS_TEXTURE (tex);

  if ((flags & COGL_TEXTURE_NEEDS_MIPMAP))
    /* Mipmaps do not work well with the current atlas so instead
       we'll just migrate the texture out and use a regular texture */
    _cogl_atlas_texture_migrate_out_of_atlas (atlas_tex);

  /* Forward on to the sub texture */
  _cogl_texture_pre_paint (atlas_tex->sub_texture, flags);
}

static void
_cogl_atlas_texture_ensure_non_quad_rendering (CoglTexture *tex)
{
  CoglAtlasTexture *atlas_tex = COGL_ATLAS_TEXTURE (tex);
  CoglTextureClass *klass = COGL_TEXTURE_GET_CLASS (atlas_tex->sub_texture);

  /* Sub textures can't support non-quad rendering so we'll just
     migrate the texture out */
  _cogl_atlas_texture_migrate_out_of_atlas (atlas_tex);

  /* Forward on to the sub texture */
  klass->ensure_non_quad_rendering (atlas_tex->sub_texture);
}

static gboolean
_cogl_atlas_texture_set_region_with_border (CoglAtlasTexture *atlas_tex,
                                            int src_x,
                                            int src_y,
                                            int dst_x,
                                            int dst_y,
                                            int dst_width,
                                            int dst_height,
                                            CoglBitmap *bmp,
                                            GError **error)
{
  CoglAtlas *atlas = atlas_tex->atlas;

  /* Copy the central data */
  if (!_cogl_texture_set_region_from_bitmap (atlas->texture,
                                             src_x, src_y,
                                             dst_width,
                                             dst_height,
                                             bmp,
                                             dst_x + atlas_tex->rectangle.x + 1,
                                             dst_y + atlas_tex->rectangle.y + 1,
                                             0, /* level 0 */
                                             error))
    return FALSE;

  /* Update the left edge pixels */
  if (dst_x == 0 &&
      !_cogl_texture_set_region_from_bitmap (atlas->texture,
                                             src_x, src_y,
                                             1, dst_height,
                                             bmp,
                                             atlas_tex->rectangle.x,
                                             dst_y + atlas_tex->rectangle.y + 1,
                                             0, /* level 0 */
                                             error))
    return FALSE;
  /* Update the right edge pixels */
  if (dst_x + dst_width == atlas_tex->rectangle.width - 2 &&
      !_cogl_texture_set_region_from_bitmap (atlas->texture,
                                             src_x + dst_width - 1, src_y,
                                             1, dst_height,
                                             bmp,
                                             atlas_tex->rectangle.x +
                                             atlas_tex->rectangle.width - 1,
                                             dst_y + atlas_tex->rectangle.y + 1,
                                             0, /* level 0 */
                                             error))
    return FALSE;
  /* Update the top edge pixels */
  if (dst_y == 0 &&
      !_cogl_texture_set_region_from_bitmap (atlas->texture,
                                             src_x, src_y,
                                             dst_width, 1,
                                             bmp,
                                             dst_x + atlas_tex->rectangle.x + 1,
                                             atlas_tex->rectangle.y,
                                             0, /* level 0 */
                                             error))
    return FALSE;
  /* Update the bottom edge pixels */
  if (dst_y + dst_height == atlas_tex->rectangle.height - 2 &&
      !_cogl_texture_set_region_from_bitmap (atlas->texture,
                                             src_x, src_y + dst_height - 1,
                                             dst_width, 1,
                                             bmp,
                                             dst_x + atlas_tex->rectangle.x + 1,
                                             atlas_tex->rectangle.y +
                                             atlas_tex->rectangle.height - 1,
                                             0, /* level 0 */
                                             error))
    return FALSE;

  return TRUE;
}

static CoglBitmap *
_cogl_atlas_texture_convert_bitmap_for_upload (CoglAtlasTexture *atlas_tex,
                                               CoglBitmap *bmp,
                                               CoglPixelFormat internal_format,
                                               GError **error)
{
  CoglBitmap *upload_bmp;
  CoglBitmap *override_bmp;

  /* We'll prepare to upload using the format of the actual texture of
     the atlas texture instead of the format reported by
     cogl_texture_get_format which would be the original internal
     format specified when the texture was created. However we'll
     preserve the premult status of the internal format because the
     images are all stored in the original premult format of the
     original format so we do need to trigger the conversion */

  internal_format = (COGL_PIXEL_FORMAT_RGBA_8888 |
                     (internal_format & COGL_PREMULT_BIT));

  upload_bmp = _cogl_bitmap_convert_for_upload (bmp,
                                                internal_format,
                                                error);
  if (upload_bmp == NULL)
    return NULL;

  /* We'll create another bitmap which uses the same data but
     overrides the format to remove the premult flag so that uploads
     to the atlas texture won't trigger the conversion again */

  override_bmp =
    _cogl_bitmap_new_shared (upload_bmp,
                             cogl_bitmap_get_format (upload_bmp) &
                             ~COGL_PREMULT_BIT,
                             cogl_bitmap_get_width (upload_bmp),
                             cogl_bitmap_get_height (upload_bmp),
                             cogl_bitmap_get_rowstride (upload_bmp));

  g_object_unref (upload_bmp);

  return override_bmp;
}

static gboolean
_cogl_atlas_texture_set_region (CoglTexture *tex,
                                int src_x,
                                int src_y,
                                int dst_x,
                                int dst_y,
                                int dst_width,
                                int dst_height,
                                int level,
                                CoglBitmap *bmp,
                                GError **error)
{
  CoglAtlasTexture  *atlas_tex = COGL_ATLAS_TEXTURE (tex);

  if (level != 0 && atlas_tex->atlas)
    _cogl_atlas_texture_migrate_out_of_atlas (atlas_tex);

  /* If the texture is in the atlas then we need to copy the edge
     pixels to the border */
  if (atlas_tex->atlas)
    {
      gboolean ret;
      CoglBitmap *upload_bmp =
        _cogl_atlas_texture_convert_bitmap_for_upload (atlas_tex,
                                                       bmp,
                                                       atlas_tex->internal_format,
                                                       error);
      if (!upload_bmp)
        return FALSE;

      /* Upload the data ignoring the premult bit */
      ret = _cogl_atlas_texture_set_region_with_border (atlas_tex,
                                                        src_x, src_y,
                                                        dst_x, dst_y,
                                                        dst_width, dst_height,
                                                        upload_bmp,
                                                        error);

      g_object_unref (upload_bmp);

      return ret;
    }
  else
    /* Otherwise we can just forward on to the sub texture */
    return _cogl_texture_set_region_from_bitmap (atlas_tex->sub_texture,
                                                 src_x, src_y,
                                                 dst_width, dst_height,
                                                 bmp,
                                                 dst_x, dst_y,
                                                 level,
                                                 error);
}

static CoglPixelFormat
_cogl_atlas_texture_get_format (CoglTexture *tex)
{
  CoglAtlasTexture *atlas_tex = COGL_ATLAS_TEXTURE (tex);

  /* We don't want to forward this on the sub-texture because it isn't
     the necessarily the same format. This will happen if the texture
     isn't pre-multiplied */
  return atlas_tex->internal_format;
}

static GLenum
_cogl_atlas_texture_get_gl_format (CoglTexture *tex)
{
  CoglAtlasTexture *atlas_tex = COGL_ATLAS_TEXTURE (tex);

  /* Forward on to the sub texture */
  return _cogl_texture_gl_get_format (atlas_tex->sub_texture);
}

static gboolean
_cogl_atlas_texture_can_use_format (CoglPixelFormat format)
{
  /* We don't care about the ordering or the premult status and we can
     accept RGBA or RGB textures. Although we could also accept
     luminance and alpha only textures or 16-bit formats it seems that
     if the application is explicitly using these formats then they've
     got a reason to want the lower memory requirements so putting
     them in the atlas might not be a good idea */
  format &= ~(COGL_PREMULT_BIT | COGL_BGR_BIT | COGL_AFIRST_BIT);
  return (format == COGL_PIXEL_FORMAT_RGB_888 ||
          format == COGL_PIXEL_FORMAT_RGBA_8888);
}

static gboolean
allocate_space (CoglAtlasTexture *atlas_tex,
                int width,
                int height,
                CoglPixelFormat internal_format,
                GError **error)
{
  CoglTexture *tex = COGL_TEXTURE (atlas_tex);
  CoglContext *ctx = cogl_texture_get_context (tex);
  CoglAtlas *atlas;
  GSList *l;

  /* If the texture is in a strange format then we won't use it */
  if (!_cogl_atlas_texture_can_use_format (internal_format))
    {
      COGL_NOTE (ATLAS, "Texture can not be added because the "
                 "format is unsupported");
      g_set_error_literal (error,
                           COGL_TEXTURE_ERROR,
                           COGL_TEXTURE_ERROR_FORMAT,
                           "Texture format unsuitable for atlasing");
      return FALSE;
    }

  /* Look for an existing atlas that can hold the texture */
  for (l = ctx->atlases; l; l = l->next)
    {
      /* We need to take a reference on the atlas before trying to
       * reserve space because in some circumstances atlas migration
       * can cause the atlas to be freed */
      atlas = g_object_ref (l->data);
      /* Try to make some space in the atlas for the texture */
      if (cogl_atlas_reserve_space (atlas,
                                    /* Add two pixels for the border */
                                    width + 2, height + 2,
                                    atlas_tex))
        {
          /* keep the atlas reference */
          break;
        }
      else
        {
          g_object_unref (atlas);
        }
    }

  /* If we couldn't find a suitable atlas then start another */
  if (l == NULL)
    {
      atlas = _cogl_atlas_texture_create_atlas (ctx);
      COGL_NOTE (ATLAS, "Created new atlas for textures: %p", atlas);
      if (!cogl_atlas_reserve_space (atlas,
                                     /* Add two pixels for the border */
                                     width + 2, height + 2,
                                     atlas_tex))
        {
          /* Ok, this means we really can't add it to the atlas */
          g_object_unref (atlas);

          g_set_error_literal (error,
                               COGL_SYSTEM_ERROR,
                               COGL_SYSTEM_ERROR_NO_MEMORY,
                               "Not enough memory to atlas texture");
          return FALSE;
        }
    }

  atlas_tex->internal_format = internal_format;

  atlas_tex->atlas = atlas;

  return TRUE;
}

static gboolean
allocate_with_size (CoglAtlasTexture *atlas_tex,
                    CoglTextureLoader *loader,
                    GError **error)
{
  CoglTexture *tex = COGL_TEXTURE (atlas_tex);
  CoglPixelFormat internal_format;

  g_warn_if_fail (loader->src.sized.format == COGL_PIXEL_FORMAT_ANY);

  internal_format =
    _cogl_texture_determine_internal_format (tex, COGL_PIXEL_FORMAT_ANY);

  if (allocate_space (atlas_tex,
                      loader->src.sized.width,
                      loader->src.sized.height,
                      internal_format,
                      error))
    {
      _cogl_texture_set_allocated (tex,
                                   internal_format,
                                   loader->src.sized.width,
                                   loader->src.sized.height);
      return TRUE;
    }
  else
    return FALSE;
}

static gboolean
allocate_from_bitmap (CoglAtlasTexture *atlas_tex,
                      CoglTextureLoader *loader,
                      GError **error)
{
  CoglTexture *tex = COGL_TEXTURE (atlas_tex);
  CoglBitmap *bmp = loader->src.bitmap.bitmap;
  CoglPixelFormat bmp_format = cogl_bitmap_get_format (bmp);
  int width = cogl_bitmap_get_width (bmp);
  int height = cogl_bitmap_get_height (bmp);
  CoglPixelFormat internal_format;
  CoglBitmap *upload_bmp;

  g_return_val_if_fail (atlas_tex->atlas == NULL, FALSE);

  internal_format = _cogl_texture_determine_internal_format (tex, bmp_format);

  upload_bmp =
    _cogl_atlas_texture_convert_bitmap_for_upload (atlas_tex,
                                                   bmp,
                                                   internal_format,
                                                   error);
  if (upload_bmp == NULL)
    return FALSE;

  if (!allocate_space (atlas_tex,
                       width,
                       height,
                       internal_format,
                       error))
    {
      g_object_unref (upload_bmp);
      return FALSE;
    }

  /* Defer to set_region so that we can share the code for copying the
     edge pixels to the border. */
  if (!_cogl_atlas_texture_set_region_with_border (atlas_tex,
                                                   0, /* src_x */
                                                   0, /* src_y */
                                                   0, /* dst_x */
                                                   0, /* dst_y */
                                                   width, /* dst_width */
                                                   height, /* dst_height */
                                                   upload_bmp,
                                                   error))
    {
      _cogl_atlas_texture_remove_from_atlas (atlas_tex);
      g_object_unref (upload_bmp);
      return FALSE;
    }

  g_object_unref (upload_bmp);

  _cogl_texture_set_allocated (tex, internal_format, width, height);

  return TRUE;
}

static gboolean
_cogl_atlas_texture_allocate (CoglTexture *tex,
                              GError **error)
{
  CoglAtlasTexture *atlas_tex = COGL_ATLAS_TEXTURE (tex);
  CoglTextureLoader *loader = cogl_texture_get_loader (tex);

  g_return_val_if_fail (loader, FALSE);

  switch (loader->src_type)
    {
    case COGL_TEXTURE_SOURCE_TYPE_SIZE:
      return allocate_with_size (atlas_tex, loader, error);
    case COGL_TEXTURE_SOURCE_TYPE_BITMAP:
      return allocate_from_bitmap (atlas_tex, loader, error);
    default:
      break;
    }

  g_return_val_if_reached (FALSE);
}

static void
cogl_atlas_texture_class_init (CoglAtlasTextureClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  CoglTextureClass *texture_class = COGL_TEXTURE_CLASS (klass);

  object_class->dispose = cogl_atlas_texture_dispose;

  texture_class->allocate = _cogl_atlas_texture_allocate;
  texture_class->set_region = _cogl_atlas_texture_set_region;
  texture_class->foreach_sub_texture_in_region = _cogl_atlas_texture_foreach_sub_texture_in_region;
  texture_class->is_sliced = _cogl_atlas_texture_is_sliced;
  texture_class->can_hardware_repeat = _cogl_atlas_texture_can_hardware_repeat;

  texture_class->transform_coords_to_gl = _cogl_atlas_texture_transform_coords_to_gl;
  texture_class->transform_quad_coords_to_gl = _cogl_atlas_texture_transform_quad_coords_to_gl;
  texture_class->get_gl_texture = _cogl_atlas_texture_get_gl_texture;
  texture_class->gl_flush_legacy_texobj_filters = _cogl_atlas_texture_gl_flush_legacy_texobj_filters;
  texture_class->pre_paint = _cogl_atlas_texture_pre_paint;
  texture_class->ensure_non_quad_rendering = _cogl_atlas_texture_ensure_non_quad_rendering;
  texture_class->gl_flush_legacy_texobj_wrap_modes = _cogl_atlas_texture_gl_flush_legacy_texobj_wrap_modes;
  texture_class->get_format = _cogl_atlas_texture_get_format;
  texture_class->get_gl_format = _cogl_atlas_texture_get_gl_format;
}

static void
cogl_atlas_texture_init (CoglAtlasTexture *self)
{
}

static CoglTexture *
_cogl_atlas_texture_create_base (CoglContext *ctx,
                                 int width,
                                 int height,
                                 CoglPixelFormat internal_format,
                                 CoglTextureLoader *loader)
{
  CoglAtlasTexture *atlas_tex;
  CoglDriver *driver = cogl_context_get_driver (ctx);
  COGL_NOTE (ATLAS, "Adding texture of size %ix%i", width, height);

  /* We need to allocate the texture now because we need the pointer
     to set as the data for the rectangle in the atlas */
  atlas_tex = g_object_new (COGL_TYPE_ATLAS_TEXTURE,
                            "context", ctx,
                            "texture-driver", cogl_driver_create_texture_driver (driver),
                            "width", width,
                            "height", height,
                            "loader", loader,
                            "format", internal_format,
                            NULL);
  /* Mark it as having no atlas so we don't try to unref it in
     _cogl_atlas_texture_post_reorganize_cb */
  atlas_tex->atlas = NULL;
  atlas_tex->sub_texture = NULL;
  atlas_tex->atlas = NULL;

  return COGL_TEXTURE (atlas_tex);
}

CoglTexture *
cogl_atlas_texture_new_with_size (CoglContext *ctx,
                                  int width,
                                  int height)
{
  CoglTextureLoader *loader;

  /* We can't atlas zero-sized textures because it breaks the atlas
   * data structure */
  g_return_val_if_fail (width > 0 && height > 0, NULL);

  loader = cogl_texture_loader_new (COGL_TEXTURE_SOURCE_TYPE_SIZE);
  loader->src.sized.width = width;
  loader->src.sized.height = height;
  loader->src.sized.format = COGL_PIXEL_FORMAT_ANY;

  return _cogl_atlas_texture_create_base (ctx, width, height,
                                          COGL_PIXEL_FORMAT_RGBA_8888_PRE,
                                          loader);
}

CoglTexture *
cogl_atlas_texture_new_from_bitmap (CoglBitmap *bmp)
{
  CoglTextureLoader *loader;

  g_return_val_if_fail (COGL_IS_BITMAP (bmp), NULL);

  loader = cogl_texture_loader_new (COGL_TEXTURE_SOURCE_TYPE_BITMAP);
  loader->src.bitmap.bitmap = g_object_ref (bmp);

  return _cogl_atlas_texture_create_base (_cogl_bitmap_get_context (bmp),
                                          cogl_bitmap_get_width (bmp),
                                          cogl_bitmap_get_height (bmp),
                                          cogl_bitmap_get_format (bmp),
                                          loader);
}

void
cogl_atlas_texture_add_reorganize_callback (CoglContext *ctx,
                                            GHookFunc callback,
                                            void *user_data)
{
  GHook *hook = g_hook_alloc (&ctx->atlas_reorganize_callbacks);
  hook->func = callback;
  hook->data = user_data;
  g_hook_prepend (&ctx->atlas_reorganize_callbacks, hook);
}

void
cogl_atlas_texture_remove_reorganize_callback (CoglContext *ctx,
                                                GHookFunc callback,
                                                void *user_data)
{
  GHook *hook = g_hook_find_func_data (&ctx->atlas_reorganize_callbacks,
                                       FALSE,
                                       callback,
                                       user_data);

  if (hook)
    g_hook_destroy_link (&ctx->atlas_reorganize_callbacks, hook);
}
