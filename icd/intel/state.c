/*
 * Vulkan
 *
 * Copyright (C) 2014 LunarG, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *   Chia-I Wu <olv@lunarg.com>
 */

#include <math.h>
#include "genhw/genhw.h"
#include "dev.h"
#include "state.h"
#include "cmd.h"

static void
viewport_get_guardband(const struct intel_gpu *gpu,
                       int center_x, int center_y,
                       int *min_gbx, int *max_gbx,
                       int *min_gby, int *max_gby)
{
   /*
    * From the Sandy Bridge PRM, volume 2 part 1, page 234:
    *
    *     "Per-Device Guardband Extents
    *
    *       - Supported X,Y ScreenSpace "Guardband" Extent: [-16K,16K-1]
    *       - Maximum Post-Clamp Delta (X or Y): 16K"
    *
    *     "In addition, in order to be correctly rendered, objects must have a
    *      screenspace bounding box not exceeding 8K in the X or Y direction.
    *      This additional restriction must also be comprehended by software,
    *      i.e., enforced by use of clipping."
    *
    * From the Ivy Bridge PRM, volume 2 part 1, page 248:
    *
    *     "Per-Device Guardband Extents
    *
    *       - Supported X,Y ScreenSpace "Guardband" Extent: [-32K,32K-1]
    *       - Maximum Post-Clamp Delta (X or Y): N/A"
    *
    *     "In addition, in order to be correctly rendered, objects must have a
    *      screenspace bounding box not exceeding 8K in the X or Y direction.
    *      This additional restriction must also be comprehended by software,
    *      i.e., enforced by use of clipping."
    *
    * Combined, the bounding box of any object can not exceed 8K in both
    * width and height.
    *
    * Below we set the guardband as a squre of length 8K, centered at where
    * the viewport is.  This makes sure all objects passing the GB test are
    * valid to the renderer, and those failing the XY clipping have a
    * better chance of passing the GB test.
    */
   const int max_extent = (intel_gpu_gen(gpu) >= INTEL_GEN(7)) ? 32768 : 16384;
   const int half_len = 8192 / 2;

   /* make sure the guardband is within the valid range */
   if (center_x - half_len < -max_extent)
      center_x = -max_extent + half_len;
   else if (center_x + half_len > max_extent - 1)
      center_x = max_extent - half_len;

   if (center_y - half_len < -max_extent)
      center_y = -max_extent + half_len;
   else if (center_y + half_len > max_extent - 1)
      center_y = max_extent - half_len;

   *min_gbx = (float) (center_x - half_len);
   *max_gbx = (float) (center_x + half_len);
   *min_gby = (float) (center_y - half_len);
   *max_gby = (float) (center_y + half_len);
}

static void
viewport_state_cmd(struct intel_dynamic_viewport *state,
                   const struct intel_gpu *gpu,
                   uint32_t count)
{
    INTEL_GPU_ASSERT(gpu, 6, 7.5);

    state->viewport_count = count;

    assert(count <= INTEL_MAX_VIEWPORTS);

    if (intel_gpu_gen(gpu) >= INTEL_GEN(7)) {
        state->cmd_len = 16 * count;

        state->cmd_clip_pos = 8;
    } else {
        state->cmd_len = 8 * count;

        state->cmd_clip_pos = state->cmd_len;
        state->cmd_len += 4 * count;
    }

    state->cmd_cc_pos = state->cmd_len;
    state->cmd_len += 2 * count;

    state->cmd_scissor_rect_pos = state->cmd_len;
    state->cmd_len += 2 * count;

    assert(sizeof(uint32_t) * state->cmd_len <= sizeof(state->cmd));
}

static void
set_viewport_state(
        struct intel_cmd*               cmd,
        uint32_t                        count,
        const VkViewport*               viewports,
        const VkRect2D*                 scissors)
{
    const struct intel_gpu *gpu = cmd->dev->gpu;
    struct intel_dynamic_viewport *state = &cmd->bind.state.viewport;
    const uint32_t sf_stride = (intel_gpu_gen(gpu) >= INTEL_GEN(7)) ? 16 : 8;
    const uint32_t clip_stride = (intel_gpu_gen(gpu) >= INTEL_GEN(7)) ? 16 : 4;
    uint32_t *sf_viewport, *clip_viewport, *cc_viewport, *scissor_rect;
    uint32_t i;

    INTEL_GPU_ASSERT(gpu, 6, 7.5);

    viewport_state_cmd(state, gpu, count);

    sf_viewport = state->cmd;
    clip_viewport = state->cmd + state->cmd_clip_pos;
    cc_viewport = state->cmd + state->cmd_cc_pos;
    scissor_rect = state->cmd + state->cmd_scissor_rect_pos;

    for (i = 0; i < count; i++) {
        const VkViewport *viewport = &viewports[i];
        uint32_t *dw = NULL;
        float translate[3], scale[3];
        int min_gbx, max_gbx, min_gby, max_gby;

        scale[0] = viewport->width / 2.0f;
        scale[1] = viewport->height / 2.0f;
        scale[2] = viewport->maxDepth - viewport->minDepth;
        translate[0] = viewport->originX + scale[0];
        translate[1] = viewport->originY + scale[1];
        translate[2] = viewport->minDepth;

        viewport_get_guardband(gpu, (int) translate[0], (int) translate[1],
                &min_gbx, &max_gbx, &min_gby, &max_gby);

        /* SF_VIEWPORT */
        dw = sf_viewport;
        dw[0] = u_fui(scale[0]);
        dw[1] = u_fui(scale[1]);
        dw[2] = u_fui(scale[2]);
        dw[3] = u_fui(translate[0]);
        dw[4] = u_fui(translate[1]);
        dw[5] = u_fui(translate[2]);
        dw[6] = 0;
        dw[7] = 0;
        sf_viewport += sf_stride;

        /* CLIP_VIEWPORT */
        dw = clip_viewport;
        dw[0] = u_fui(((float) min_gbx - translate[0]) / fabsf(scale[0]));
        dw[1] = u_fui(((float) max_gbx - translate[0]) / fabsf(scale[0]));
        dw[2] = u_fui(((float) min_gby - translate[1]) / fabsf(scale[1]));
        dw[3] = u_fui(((float) max_gby - translate[1]) / fabsf(scale[1]));
        clip_viewport += clip_stride;

        /* CC_VIEWPORT */
        dw = cc_viewport;
        dw[0] = u_fui(viewport->minDepth);
        dw[1] = u_fui(viewport->maxDepth);
        cc_viewport += 2;
    }

    for (i = 0; i < count; i++) {
        const VkRect2D *scissor = &scissors[i];
        /* SCISSOR_RECT */
        int16_t max_x, max_y;
        uint32_t *dw = NULL;

        max_x = (scissor->offset.x + scissor->extent.width - 1) & 0xffff;
        max_y = (scissor->offset.y + scissor->extent.height - 1) & 0xffff;

        dw = scissor_rect;
        if (scissor->extent.width && scissor->extent.height) {
            dw[0] = (scissor->offset.y & 0xffff) << 16 |
                                                    (scissor->offset.x & 0xffff);
            dw[1] = max_y << 16 | max_x;
        } else {
            dw[0] = 1 << 16 | 1;
            dw[1] = 0;
        }
        scissor_rect += 2;
    }
}

ICD_EXPORT void VKAPI vkCmdSetViewport(
    VkCmdBuffer                         cmdBuffer,
    uint32_t                            viewportAndScissorCount,
    const VkViewport*                   pViewports,
    const VkRect2D*                     pScissors)
{
    struct intel_cmd *cmd = intel_cmd(cmdBuffer);

    set_viewport_state(cmd, viewportAndScissorCount, pViewports, pScissors);
}

ICD_EXPORT void VKAPI vkCmdSetLineWidth(
    VkCmdBuffer                              cmdBuffer,
    float                                    line_width)
{
    struct intel_cmd *cmd = intel_cmd(cmdBuffer);

    cmd->bind.state.line_width.line_width = line_width;
}

ICD_EXPORT void VKAPI vkCmdSetDepthBias(
    VkCmdBuffer                         cmdBuffer,
    float                               depthBias,
    float                               depthBiasClamp,
    float                               slopeScaledDepthBias)
{
    struct intel_cmd *cmd = intel_cmd(cmdBuffer);

    cmd->bind.state.depth_bias.depth_bias = depthBias;
    cmd->bind.state.depth_bias.depth_bias_clamp = depthBiasClamp;
    cmd->bind.state.depth_bias.slope_scaled_depth_bias = slopeScaledDepthBias;
}

ICD_EXPORT void VKAPI vkCmdSetBlendConstants(
    VkCmdBuffer                         cmdBuffer,
    const float                         blendConst[4])
{
    struct intel_cmd *cmd = intel_cmd(cmdBuffer);

    cmd->bind.state.blend.blend_const[0] = blendConst[0];
    cmd->bind.state.blend.blend_const[1] = blendConst[1];
    cmd->bind.state.blend.blend_const[2] = blendConst[2];
    cmd->bind.state.blend.blend_const[3] = blendConst[3];
}

ICD_EXPORT void VKAPI vkCmdSetDepthBounds(
    VkCmdBuffer                         cmdBuffer,
    float                               minDepthBounds,
    float                               maxDepthBounds)
{
    struct intel_cmd *cmd = intel_cmd(cmdBuffer);

    /*
     * From the Sandy Bridge PRM, volume 2 part 1, page 359:
     *
     *     "If the Depth Buffer is either undefined or does not have a surface
     *      format of D32_FLOAT_S8X24_UINT or D24_UNORM_S8_UINT and separate
     *      stencil buffer is disabled, Stencil Test Enable must be DISABLED"
     *
     * From the Sandy Bridge PRM, volume 2 part 1, page 370:
     *
     *     "This field (Stencil Test Enable) cannot be enabled if
     *      Surface Format in 3DSTATE_DEPTH_BUFFER is set to D16_UNORM."
     *
     * TODO We do not check these yet.
     */
    cmd->bind.state.depth_bounds.min_depth_bounds = minDepthBounds;
    cmd->bind.state.depth_bounds.max_depth_bounds = maxDepthBounds;
}

ICD_EXPORT void VKAPI vkCmdSetStencilCompareMask(
    VkCmdBuffer                         cmdBuffer,
    VkStencilFaceFlags                  faceMask,
    uint32_t                            stencilCompareMask)
{
    struct intel_cmd *cmd = intel_cmd(cmdBuffer);

    /* TODO: enable back facing stencil state */
    /* Some plumbing needs to be done if we want to support info_back.
     * In the meantime, catch that back facing info has been submitted. */

    /*
     * From the Sandy Bridge PRM, volume 2 part 1, page 359:
     *
     *     "If the Depth Buffer is either undefined or does not have a surface
     *      format of D32_FLOAT_S8X24_UINT or D24_UNORM_S8_UINT and separate
     *      stencil buffer is disabled, Stencil Test Enable must be DISABLED"
     *
     * From the Sandy Bridge PRM, volume 2 part 1, page 370:
     *
     *     "This field (Stencil Test Enable) cannot be enabled if
     *      Surface Format in 3DSTATE_DEPTH_BUFFER is set to D16_UNORM."
     *
     * TODO We do not check these yet.
     */
    if (faceMask & VK_STENCIL_FACE_FRONT_BIT) {
        cmd->bind.state.stencil.front.stencil_compare_mask = stencilCompareMask;
    }
    if (faceMask & VK_STENCIL_FACE_BACK_BIT) {
        cmd->bind.state.stencil.back.stencil_compare_mask = stencilCompareMask;
    }
}

ICD_EXPORT void VKAPI vkCmdSetStencilWriteMask(
    VkCmdBuffer                         cmdBuffer,
    VkStencilFaceFlags                  faceMask,
    uint32_t                            stencilWriteMask)
{
    struct intel_cmd *cmd = intel_cmd(cmdBuffer);

    if (faceMask & VK_STENCIL_FACE_FRONT_BIT) {
        cmd->bind.state.stencil.front.stencil_write_mask = stencilWriteMask;
    }
    if (faceMask & VK_STENCIL_FACE_BACK_BIT) {
        cmd->bind.state.stencil.back.stencil_write_mask = stencilWriteMask;
    }
}
ICD_EXPORT void VKAPI vkCmdSetStencilReference(
    VkCmdBuffer                         cmdBuffer,
    VkStencilFaceFlags                  faceMask,
    uint32_t                            stencilReference)
{
    struct intel_cmd *cmd = intel_cmd(cmdBuffer);

    if (faceMask & VK_STENCIL_FACE_FRONT_BIT) {
        cmd->bind.state.stencil.front.stencil_reference = stencilReference;
    }
    if (faceMask & VK_STENCIL_FACE_BACK_BIT) {
        cmd->bind.state.stencil.back.stencil_reference = stencilReference;
    }
}
