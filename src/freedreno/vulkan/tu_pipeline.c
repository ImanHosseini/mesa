/*
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 *
 * based in part on anv driver which is:
 * Copyright © 2015 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "tu_private.h"

#include "ir3/ir3_nir.h"
#include "main/menums.h"
#include "nir/nir.h"
#include "nir/nir_builder.h"
#include "spirv/nir_spirv.h"
#include "util/debug.h"
#include "util/mesa-sha1.h"
#include "util/u_atomic.h"
#include "vk_format.h"
#include "vk_util.h"

#include "tu_cs.h"

/* Emit IB that preloads the descriptors that the shader uses */

static void
emit_load_state(struct tu_cs *cs, unsigned opcode, enum a6xx_state_type st,
                enum a6xx_state_block sb, unsigned base, unsigned offset,
                unsigned count)
{
   /* Note: just emit one packet, even if count overflows NUM_UNIT. It's not
    * clear if emitting more packets will even help anything. Presumably the
    * descriptor cache is relatively small, and these packets stop doing
    * anything when there are too many descriptors.
    */
   tu_cs_emit_pkt7(cs, opcode, 3);
   tu_cs_emit(cs,
              CP_LOAD_STATE6_0_STATE_TYPE(st) |
              CP_LOAD_STATE6_0_STATE_SRC(SS6_BINDLESS) |
              CP_LOAD_STATE6_0_STATE_BLOCK(sb) |
              CP_LOAD_STATE6_0_NUM_UNIT(MIN2(count, 1024-1)));
   tu_cs_emit_qw(cs, offset | (base << 28));
}

static unsigned
tu6_load_state_size(struct tu_pipeline_layout *layout, bool compute)
{
   const unsigned load_state_size = 4;
   unsigned size = 0;
   for (unsigned i = 0; i < layout->num_sets; i++) {
      struct tu_descriptor_set_layout *set_layout = layout->set[i].layout;
      for (unsigned j = 0; j < set_layout->binding_count; j++) {
         struct tu_descriptor_set_binding_layout *binding = &set_layout->binding[j];
         unsigned count = 0;
         /* Note: some users, like amber for example, pass in
          * VK_SHADER_STAGE_ALL which includes a bunch of extra bits, so
          * filter these out by using VK_SHADER_STAGE_ALL_GRAPHICS explicitly.
          */
         VkShaderStageFlags stages = compute ?
            binding->shader_stages & VK_SHADER_STAGE_COMPUTE_BIT :
            binding->shader_stages & VK_SHADER_STAGE_ALL_GRAPHICS;
         unsigned stage_count = util_bitcount(stages);
         switch (binding->type) {
         case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
         case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
         case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
         case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
            /* IBO-backed resources only need one packet for all graphics stages */
            if (stages & ~VK_SHADER_STAGE_COMPUTE_BIT)
               count += 1;
            if (stages & VK_SHADER_STAGE_COMPUTE_BIT)
               count += 1;
            break;
         case VK_DESCRIPTOR_TYPE_SAMPLER:
         case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
         case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
         case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
         case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
         case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
            /* Textures and UBO's needs a packet for each stage */
            count = stage_count;
            break;
         case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
            /* Because of how we pack combined images and samplers, we
             * currently can't use one packet for the whole array.
             */
            count = stage_count * binding->array_size * 2;
            break;
         default:
            unreachable("bad descriptor type");
         }
         size += count * load_state_size;
      }
   }
   return size;
}

static void
tu6_emit_load_state(struct tu_pipeline *pipeline, bool compute)
{
   unsigned size = tu6_load_state_size(pipeline->layout, compute);
   if (size == 0)
      return;

   struct tu_cs cs;
   tu_cs_begin_sub_stream(&pipeline->cs, size, &cs);

   struct tu_pipeline_layout *layout = pipeline->layout;
   for (unsigned i = 0; i < layout->num_sets; i++) {
      /* From 13.2.7. Descriptor Set Binding:
       *
       *    A compatible descriptor set must be bound for all set numbers that
       *    any shaders in a pipeline access, at the time that a draw or
       *    dispatch command is recorded to execute using that pipeline.
       *    However, if none of the shaders in a pipeline statically use any
       *    bindings with a particular set number, then no descriptor set need
       *    be bound for that set number, even if the pipeline layout includes
       *    a non-trivial descriptor set layout for that set number.
       *
       * This means that descriptor sets unused by the pipeline may have a
       * garbage or 0 BINDLESS_BASE register, which will cause context faults
       * when prefetching descriptors from these sets. Skip prefetching for
       * descriptors from them to avoid this. This is also an optimization,
       * since these prefetches would be useless.
       */
      if (!(pipeline->active_desc_sets & (1u << i)))
         continue;

      struct tu_descriptor_set_layout *set_layout = layout->set[i].layout;
      for (unsigned j = 0; j < set_layout->binding_count; j++) {
         struct tu_descriptor_set_binding_layout *binding = &set_layout->binding[j];
         unsigned base = i;
         unsigned offset = binding->offset / 4;
         /* Note: some users, like amber for example, pass in
          * VK_SHADER_STAGE_ALL which includes a bunch of extra bits, so
          * filter these out by using VK_SHADER_STAGE_ALL_GRAPHICS explicitly.
          */
         VkShaderStageFlags stages = compute ?
            binding->shader_stages & VK_SHADER_STAGE_COMPUTE_BIT :
            binding->shader_stages & VK_SHADER_STAGE_ALL_GRAPHICS;
         unsigned count = binding->array_size;
         if (count == 0 || stages == 0)
            continue;
         switch (binding->type) {
         case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
            base = MAX_SETS;
            offset = (layout->set[i].dynamic_offset_start +
                      binding->dynamic_offset_offset) * A6XX_TEX_CONST_DWORDS;
            /* fallthrough */
         case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
         case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
         case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
            /* IBO-backed resources only need one packet for all graphics stages */
            if (stages & ~VK_SHADER_STAGE_COMPUTE_BIT) {
               emit_load_state(&cs, CP_LOAD_STATE6, ST6_SHADER, SB6_IBO,
                               base, offset, count);
            }
            if (stages & VK_SHADER_STAGE_COMPUTE_BIT) {
               emit_load_state(&cs, CP_LOAD_STATE6_FRAG, ST6_IBO, SB6_CS_SHADER,
                               base, offset, count);
            }
            break;
         case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
            /* nothing - input attachment doesn't use bindless */
            break;
         case VK_DESCRIPTOR_TYPE_SAMPLER:
         case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
         case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER: {
            tu_foreach_stage(stage, stages) {
               emit_load_state(&cs, tu6_stage2opcode(stage),
                               binding->type == VK_DESCRIPTOR_TYPE_SAMPLER ?
                               ST6_SHADER : ST6_CONSTANTS,
                               tu6_stage2texsb(stage), base, offset, count);
            }
            break;
         }
         case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
            base = MAX_SETS;
            offset = (layout->set[i].dynamic_offset_start +
                      binding->dynamic_offset_offset) * A6XX_TEX_CONST_DWORDS;
            /* fallthrough */
         case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER: {
            tu_foreach_stage(stage, stages) {
               emit_load_state(&cs, tu6_stage2opcode(stage), ST6_UBO,
                               tu6_stage2shadersb(stage), base, offset, count);
            }
            break;
         }
         case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: {
            tu_foreach_stage(stage, stages) {
               /* TODO: We could emit less CP_LOAD_STATE6 if we used
                * struct-of-arrays instead of array-of-structs.
                */
               for (unsigned i = 0; i < count; i++) {
                  unsigned tex_offset = offset + 2 * i * A6XX_TEX_CONST_DWORDS;
                  unsigned sam_offset = offset + (2 * i + 1) * A6XX_TEX_CONST_DWORDS;
                  emit_load_state(&cs, tu6_stage2opcode(stage),
                                  ST6_CONSTANTS, tu6_stage2texsb(stage),
                                  base, tex_offset, 1);
                  emit_load_state(&cs, tu6_stage2opcode(stage),
                                  ST6_SHADER, tu6_stage2texsb(stage),
                                  base, sam_offset, 1);
               }
            }
            break;
         }
         default:
            unreachable("bad descriptor type");
         }
      }
   }

   pipeline->load_state.state_ib = tu_cs_end_sub_stream(&pipeline->cs, &cs);
}

struct tu_pipeline_builder
{
   struct tu_device *device;
   struct tu_pipeline_cache *cache;
   struct tu_pipeline_layout *layout;
   const VkAllocationCallbacks *alloc;
   const VkGraphicsPipelineCreateInfo *create_info;

   struct tu_shader *shaders[MESA_SHADER_STAGES];
   struct ir3_shader_variant *variants[MESA_SHADER_STAGES];
   struct ir3_shader_variant *binning_variant;
   uint32_t shader_offsets[MESA_SHADER_STAGES];
   uint32_t binning_vs_offset;
   uint32_t shader_total_size;

   bool rasterizer_discard;
   /* these states are affectd by rasterizer_discard */
   VkSampleCountFlagBits samples;
   bool use_color_attachments;
   bool use_dual_src_blend;
   uint32_t color_attachment_count;
   VkFormat color_attachment_formats[MAX_RTS];
   VkFormat depth_attachment_format;
   uint32_t render_components;
};

static bool
tu_logic_op_reads_dst(VkLogicOp op)
{
   switch (op) {
   case VK_LOGIC_OP_CLEAR:
   case VK_LOGIC_OP_COPY:
   case VK_LOGIC_OP_COPY_INVERTED:
   case VK_LOGIC_OP_SET:
      return false;
   default:
      return true;
   }
}

static VkBlendFactor
tu_blend_factor_no_dst_alpha(VkBlendFactor factor)
{
   /* treat dst alpha as 1.0 and avoid reading it */
   switch (factor) {
   case VK_BLEND_FACTOR_DST_ALPHA:
      return VK_BLEND_FACTOR_ONE;
   case VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA:
      return VK_BLEND_FACTOR_ZERO;
   default:
      return factor;
   }
}

static bool tu_blend_factor_is_dual_src(VkBlendFactor factor)
{
   switch (factor) {
   case VK_BLEND_FACTOR_SRC1_COLOR:
   case VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR:
   case VK_BLEND_FACTOR_SRC1_ALPHA:
   case VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA:
      return true;
   default:
      return false;
   }
}

static bool
tu_blend_state_is_dual_src(const VkPipelineColorBlendStateCreateInfo *info)
{
   if (!info)
      return false;

   for (unsigned i = 0; i < info->attachmentCount; i++) {
      const VkPipelineColorBlendAttachmentState *blend = &info->pAttachments[i];
      if (tu_blend_factor_is_dual_src(blend->srcColorBlendFactor) ||
          tu_blend_factor_is_dual_src(blend->dstColorBlendFactor) ||
          tu_blend_factor_is_dual_src(blend->srcAlphaBlendFactor) ||
          tu_blend_factor_is_dual_src(blend->dstAlphaBlendFactor))
         return true;
   }

   return false;
}

void
tu6_emit_xs_config(struct tu_cs *cs,
                   gl_shader_stage stage, /* xs->type, but xs may be NULL */
                   const struct ir3_shader_variant *xs,
                   uint64_t binary_iova)
{
   static const struct xs_config {
      uint16_t reg_sp_xs_ctrl;
      uint16_t reg_sp_xs_config;
      uint16_t reg_hlsq_xs_ctrl;
      uint16_t reg_sp_vs_obj_start;
   } xs_config[] = {
      [MESA_SHADER_VERTEX] = {
         REG_A6XX_SP_VS_CTRL_REG0,
         REG_A6XX_SP_VS_CONFIG,
         REG_A6XX_HLSQ_VS_CNTL,
         REG_A6XX_SP_VS_OBJ_START_LO,
      },
      [MESA_SHADER_TESS_CTRL] = {
         REG_A6XX_SP_HS_CTRL_REG0,
         REG_A6XX_SP_HS_CONFIG,
         REG_A6XX_HLSQ_HS_CNTL,
         REG_A6XX_SP_HS_OBJ_START_LO,
      },
      [MESA_SHADER_TESS_EVAL] = {
         REG_A6XX_SP_DS_CTRL_REG0,
         REG_A6XX_SP_DS_CONFIG,
         REG_A6XX_HLSQ_DS_CNTL,
         REG_A6XX_SP_DS_OBJ_START_LO,
      },
      [MESA_SHADER_GEOMETRY] = {
         REG_A6XX_SP_GS_CTRL_REG0,
         REG_A6XX_SP_GS_CONFIG,
         REG_A6XX_HLSQ_GS_CNTL,
         REG_A6XX_SP_GS_OBJ_START_LO,
      },
      [MESA_SHADER_FRAGMENT] = {
         REG_A6XX_SP_FS_CTRL_REG0,
         REG_A6XX_SP_FS_CONFIG,
         REG_A6XX_HLSQ_FS_CNTL,
         REG_A6XX_SP_FS_OBJ_START_LO,
      },
      [MESA_SHADER_COMPUTE] = {
         REG_A6XX_SP_CS_CTRL_REG0,
         REG_A6XX_SP_CS_CONFIG,
         REG_A6XX_HLSQ_CS_CNTL,
         REG_A6XX_SP_CS_OBJ_START_LO,
      },
   };
   const struct xs_config *cfg = &xs_config[stage];

   if (!xs) {
      /* shader stage disabled */
      tu_cs_emit_pkt4(cs, cfg->reg_sp_xs_config, 1);
      tu_cs_emit(cs, 0);

      tu_cs_emit_pkt4(cs, cfg->reg_hlsq_xs_ctrl, 1);
      tu_cs_emit(cs, 0);
      return;
   }

   bool is_fs = xs->type == MESA_SHADER_FRAGMENT;
   enum a3xx_threadsize threadsize = FOUR_QUADS;

   /* TODO:
    * the "threadsize" field may have nothing to do with threadsize,
    * use a value that matches the blob until it is figured out
    */
   if (xs->type == MESA_SHADER_GEOMETRY)
      threadsize = TWO_QUADS;

   tu_cs_emit_pkt4(cs, cfg->reg_sp_xs_ctrl, 1);
   tu_cs_emit(cs,
              A6XX_SP_VS_CTRL_REG0_THREADSIZE(threadsize) |
              A6XX_SP_VS_CTRL_REG0_FULLREGFOOTPRINT(xs->info.max_reg + 1) |
              A6XX_SP_VS_CTRL_REG0_HALFREGFOOTPRINT(xs->info.max_half_reg + 1) |
              COND(xs->mergedregs, A6XX_SP_VS_CTRL_REG0_MERGEDREGS) |
              A6XX_SP_VS_CTRL_REG0_BRANCHSTACK(xs->branchstack) |
              COND(xs->need_pixlod, A6XX_SP_VS_CTRL_REG0_PIXLODENABLE) |
              COND(xs->need_fine_derivatives, A6XX_SP_VS_CTRL_REG0_DIFF_FINE) |
              /* only fragment shader sets VARYING bit */
              COND(xs->total_in && is_fs, A6XX_SP_FS_CTRL_REG0_VARYING) |
              /* unknown bit, seems unnecessary */
              COND(is_fs, 0x1000000));

   tu_cs_emit_pkt4(cs, cfg->reg_sp_xs_config, 2);
   tu_cs_emit(cs, A6XX_SP_VS_CONFIG_ENABLED |
                  COND(xs->bindless_tex, A6XX_SP_VS_CONFIG_BINDLESS_TEX) |
                  COND(xs->bindless_samp, A6XX_SP_VS_CONFIG_BINDLESS_SAMP) |
                  COND(xs->bindless_ibo, A6XX_SP_VS_CONFIG_BINDLESS_IBO) |
                  COND(xs->bindless_ubo, A6XX_SP_VS_CONFIG_BINDLESS_UBO) |
                  A6XX_SP_VS_CONFIG_NTEX(xs->num_samp) |
                  A6XX_SP_VS_CONFIG_NSAMP(xs->num_samp));
   tu_cs_emit(cs, xs->instrlen);

   tu_cs_emit_pkt4(cs, cfg->reg_hlsq_xs_ctrl, 1);
   tu_cs_emit(cs, A6XX_HLSQ_VS_CNTL_CONSTLEN(align(xs->constlen, 4)) |
                  A6XX_HLSQ_VS_CNTL_ENABLED);

   /* emit program binary
    * binary_iova should be aligned to 1 instrlen unit (128 bytes)
    */

   assert((binary_iova & 0x7f) == 0);

   tu_cs_emit_pkt4(cs, cfg->reg_sp_vs_obj_start, 2);
   tu_cs_emit_qw(cs, binary_iova);

   tu_cs_emit_pkt7(cs, tu6_stage2opcode(stage), 3);
   tu_cs_emit(cs, CP_LOAD_STATE6_0_DST_OFF(0) |
                  CP_LOAD_STATE6_0_STATE_TYPE(ST6_SHADER) |
                  CP_LOAD_STATE6_0_STATE_SRC(SS6_INDIRECT) |
                  CP_LOAD_STATE6_0_STATE_BLOCK(tu6_stage2shadersb(stage)) |
                  CP_LOAD_STATE6_0_NUM_UNIT(xs->instrlen));
   tu_cs_emit_qw(cs, binary_iova);

   /* emit immediates */

   const struct ir3_const_state *const_state = ir3_const_state(xs);
   uint32_t base = const_state->offsets.immediate;
   int size = const_state->immediates_count;

   /* truncate size to avoid writing constants that shader
    * does not use:
    */
   size = MIN2(size + base, xs->constlen) - base;

   if (size <= 0)
      return;

   tu_cs_emit_pkt7(cs, tu6_stage2opcode(stage), 3 + size * 4);
   tu_cs_emit(cs, CP_LOAD_STATE6_0_DST_OFF(base) |
                  CP_LOAD_STATE6_0_STATE_TYPE(ST6_CONSTANTS) |
                  CP_LOAD_STATE6_0_STATE_SRC(SS6_DIRECT) |
                  CP_LOAD_STATE6_0_STATE_BLOCK(tu6_stage2shadersb(stage)) |
                  CP_LOAD_STATE6_0_NUM_UNIT(size));
   tu_cs_emit(cs, CP_LOAD_STATE6_1_EXT_SRC_ADDR(0));
   tu_cs_emit(cs, CP_LOAD_STATE6_2_EXT_SRC_ADDR_HI(0));

   for (unsigned i = 0; i < size; i++) {
      tu_cs_emit(cs, const_state->immediates[i].val[0]);
      tu_cs_emit(cs, const_state->immediates[i].val[1]);
      tu_cs_emit(cs, const_state->immediates[i].val[2]);
      tu_cs_emit(cs, const_state->immediates[i].val[3]);
   }
}

static void
tu6_emit_cs_config(struct tu_cs *cs, const struct tu_shader *shader,
                   const struct ir3_shader_variant *v,
                   uint32_t binary_iova)
{
   tu_cs_emit_pkt4(cs, REG_A6XX_HLSQ_UPDATE_CNTL, 1);
   tu_cs_emit(cs, 0xff);

   tu6_emit_xs_config(cs, MESA_SHADER_COMPUTE, v, binary_iova);

   tu_cs_emit_pkt4(cs, REG_A6XX_SP_CS_UNKNOWN_A9B1, 1);
   tu_cs_emit(cs, 0x41);

   uint32_t local_invocation_id =
      ir3_find_sysval_regid(v, SYSTEM_VALUE_LOCAL_INVOCATION_ID);
   uint32_t work_group_id =
      ir3_find_sysval_regid(v, SYSTEM_VALUE_WORK_GROUP_ID);

   tu_cs_emit_pkt4(cs, REG_A6XX_HLSQ_CS_CNTL_0, 2);
   tu_cs_emit(cs,
              A6XX_HLSQ_CS_CNTL_0_WGIDCONSTID(work_group_id) |
              A6XX_HLSQ_CS_CNTL_0_UNK0(regid(63, 0)) |
              A6XX_HLSQ_CS_CNTL_0_UNK1(regid(63, 0)) |
              A6XX_HLSQ_CS_CNTL_0_LOCALIDREGID(local_invocation_id));
   tu_cs_emit(cs, 0x2fc);             /* HLSQ_CS_UNKNOWN_B998 */
}

static void
tu6_emit_vs_system_values(struct tu_cs *cs,
                          const struct ir3_shader_variant *vs,
                          const struct ir3_shader_variant *gs,
                          bool primid_passthru)
{
   const uint32_t vertexid_regid =
         ir3_find_sysval_regid(vs, SYSTEM_VALUE_VERTEX_ID);
   const uint32_t instanceid_regid =
         ir3_find_sysval_regid(vs, SYSTEM_VALUE_INSTANCE_ID);
   const uint32_t primitiveid_regid = gs ?
         ir3_find_sysval_regid(gs, SYSTEM_VALUE_PRIMITIVE_ID) :
         regid(63, 0);
   const uint32_t gsheader_regid = gs ?
         ir3_find_sysval_regid(gs, SYSTEM_VALUE_GS_HEADER_IR3) :
         regid(63, 0);

   tu_cs_emit_pkt4(cs, REG_A6XX_VFD_CONTROL_1, 6);
   tu_cs_emit(cs, A6XX_VFD_CONTROL_1_REGID4VTX(vertexid_regid) |
                  A6XX_VFD_CONTROL_1_REGID4INST(instanceid_regid) |
                  A6XX_VFD_CONTROL_1_REGID4PRIMID(primitiveid_regid) |
                  0xfc000000);
   tu_cs_emit(cs, 0x0000fcfc); /* VFD_CONTROL_2 */
   tu_cs_emit(cs, 0xfcfcfcfc); /* VFD_CONTROL_3 */
   tu_cs_emit(cs, 0x000000fc); /* VFD_CONTROL_4 */
   tu_cs_emit(cs, A6XX_VFD_CONTROL_5_REGID_GSHEADER(gsheader_regid) |
                  0xfc00); /* VFD_CONTROL_5 */
   tu_cs_emit(cs, COND(primid_passthru, A6XX_VFD_CONTROL_6_PRIMID_PASSTHRU)); /* VFD_CONTROL_6 */
}

/* Add any missing varyings needed for stream-out. Otherwise varyings not
 * used by fragment shader will be stripped out.
 */
static void
tu6_link_streamout(struct ir3_shader_linkage *l,
                     const struct ir3_shader_variant *v)
{
   const struct ir3_stream_output_info *info = &v->shader->stream_output;

   /*
    * First, any stream-out varyings not already in linkage map (ie. also
    * consumed by frag shader) need to be added:
    */
   for (unsigned i = 0; i < info->num_outputs; i++) {
      const struct ir3_stream_output *out = &info->output[i];
      unsigned compmask =
                  (1 << (out->num_components + out->start_component)) - 1;
      unsigned k = out->register_index;
      unsigned idx, nextloc = 0;

      /* psize/pos need to be the last entries in linkage map, and will
       * get added link_stream_out, so skip over them:
       */
      if (v->outputs[k].slot == VARYING_SLOT_PSIZ ||
            v->outputs[k].slot == VARYING_SLOT_POS)
         continue;

      for (idx = 0; idx < l->cnt; idx++) {
         if (l->var[idx].regid == v->outputs[k].regid)
            break;
         nextloc = MAX2(nextloc, l->var[idx].loc + 4);
      }

      /* add if not already in linkage map: */
      if (idx == l->cnt)
         ir3_link_add(l, v->outputs[k].regid, compmask, nextloc);

      /* expand component-mask if needed, ie streaming out all components
       * but frag shader doesn't consume all components:
       */
      if (compmask & ~l->var[idx].compmask) {
         l->var[idx].compmask |= compmask;
         l->max_loc = MAX2(l->max_loc, l->var[idx].loc +
                           util_last_bit(l->var[idx].compmask));
      }
   }
}

static void
tu6_setup_streamout(const struct ir3_shader_variant *v,
            struct ir3_shader_linkage *l, struct tu_streamout_state *tf)
{
   const struct ir3_stream_output_info *info = &v->shader->stream_output;

   memset(tf, 0, sizeof(*tf));

   tf->prog_count = align(l->max_loc, 2) / 2;

   debug_assert(tf->prog_count < ARRAY_SIZE(tf->prog));

   /* set stride info to the streamout state */
   for (unsigned i = 0; i < IR3_MAX_SO_BUFFERS; i++)
      tf->stride[i] = info->stride[i];

   for (unsigned i = 0; i < info->num_outputs; i++) {
      const struct ir3_stream_output *out = &info->output[i];
      unsigned k = out->register_index;
      unsigned idx;

      /* Skip it, if there's an unused reg in the middle of outputs. */
      if (v->outputs[k].regid == INVALID_REG)
         continue;

      tf->ncomp[out->output_buffer] += out->num_components;

      /* linkage map sorted by order frag shader wants things, so
       * a bit less ideal here..
       */
      for (idx = 0; idx < l->cnt; idx++)
         if (l->var[idx].regid == v->outputs[k].regid)
            break;

      debug_assert(idx < l->cnt);

      for (unsigned j = 0; j < out->num_components; j++) {
         unsigned c   = j + out->start_component;
         unsigned loc = l->var[idx].loc + c;
         unsigned off = j + out->dst_offset;  /* in dwords */

         if (loc & 1) {
            tf->prog[loc/2] |= A6XX_VPC_SO_PROG_B_EN |
                        A6XX_VPC_SO_PROG_B_BUF(out->output_buffer) |
                        A6XX_VPC_SO_PROG_B_OFF(off * 4);
         } else {
            tf->prog[loc/2] |= A6XX_VPC_SO_PROG_A_EN |
                        A6XX_VPC_SO_PROG_A_BUF(out->output_buffer) |
                        A6XX_VPC_SO_PROG_A_OFF(off * 4);
         }
      }
   }

   tf->vpc_so_buf_cntl = A6XX_VPC_SO_BUF_CNTL_ENABLE |
               COND(tf->ncomp[0] > 0, A6XX_VPC_SO_BUF_CNTL_BUF0) |
               COND(tf->ncomp[1] > 0, A6XX_VPC_SO_BUF_CNTL_BUF1) |
               COND(tf->ncomp[2] > 0, A6XX_VPC_SO_BUF_CNTL_BUF2) |
               COND(tf->ncomp[3] > 0, A6XX_VPC_SO_BUF_CNTL_BUF3);
}

static void
tu6_emit_const(struct tu_cs *cs, uint32_t opcode, uint32_t base,
               enum a6xx_state_block block, uint32_t offset,
               uint32_t size, uint32_t *dwords) {
   assert(size % 4 == 0);

   tu_cs_emit_pkt7(cs, opcode, 3 + size);
   tu_cs_emit(cs, CP_LOAD_STATE6_0_DST_OFF(base) |
         CP_LOAD_STATE6_0_STATE_TYPE(ST6_CONSTANTS) |
         CP_LOAD_STATE6_0_STATE_SRC(SS6_DIRECT) |
         CP_LOAD_STATE6_0_STATE_BLOCK(block) |
         CP_LOAD_STATE6_0_NUM_UNIT(size / 4));

   tu_cs_emit(cs, CP_LOAD_STATE6_1_EXT_SRC_ADDR(0));
   tu_cs_emit(cs, CP_LOAD_STATE6_2_EXT_SRC_ADDR_HI(0));
   dwords = (uint32_t *)&((uint8_t *)dwords)[offset];

   tu_cs_emit_array(cs, dwords, size);
}

static void
tu6_emit_link_map(struct tu_cs *cs,
                  const struct ir3_shader_variant *producer,
                  const struct ir3_shader_variant *consumer) {
   const struct ir3_const_state *const_state = ir3_const_state(consumer);
   uint32_t base = const_state->offsets.primitive_map;
   uint32_t patch_locs[MAX_VARYING] = { }, num_loc;
   num_loc = ir3_link_geometry_stages(producer, consumer, patch_locs);
   int size = DIV_ROUND_UP(num_loc, 4);

   size = (MIN2(size + base, consumer->constlen) - base) * 4;
   if (size <= 0)
      return;

   tu6_emit_const(cs, CP_LOAD_STATE6_GEOM, base, SB6_GS_SHADER, 0, size,
                  patch_locs);
}

static uint16_t
gl_primitive_to_tess(uint16_t primitive) {
   switch (primitive) {
   case GL_POINTS:
      return TESS_POINTS;
   case GL_LINE_STRIP:
      return TESS_LINES;
   case GL_TRIANGLE_STRIP:
      return TESS_CW_TRIS;
   default:
      unreachable("");
   }
}

void
tu6_emit_vpc(struct tu_cs *cs,
             const struct ir3_shader_variant *vs,
             const struct ir3_shader_variant *gs,
             const struct ir3_shader_variant *fs,
             struct tu_streamout_state *tf)
{
   const struct ir3_shader_variant *last_shader = gs ?: vs;
   struct ir3_shader_linkage linkage = { .primid_loc = 0xff };
   if (fs)
      ir3_link_shaders(&linkage, last_shader, fs, true);

   if (last_shader->shader->stream_output.num_outputs)
      tu6_link_streamout(&linkage, last_shader);

   /* We do this after linking shaders in order to know whether PrimID
    * passthrough needs to be enabled.
    */
   bool primid_passthru = linkage.primid_loc != 0xff;
   tu6_emit_vs_system_values(cs, vs, gs, primid_passthru);

   tu_cs_emit_pkt4(cs, REG_A6XX_VPC_VAR_DISABLE(0), 4);
   tu_cs_emit(cs, ~linkage.varmask[0]);
   tu_cs_emit(cs, ~linkage.varmask[1]);
   tu_cs_emit(cs, ~linkage.varmask[2]);
   tu_cs_emit(cs, ~linkage.varmask[3]);

   /* a6xx finds position/pointsize at the end */
   const uint32_t position_regid =
      ir3_find_output_regid(last_shader, VARYING_SLOT_POS);
   const uint32_t pointsize_regid =
      ir3_find_output_regid(last_shader, VARYING_SLOT_PSIZ);
   const uint32_t layer_regid = gs ?
      ir3_find_output_regid(gs, VARYING_SLOT_LAYER) : regid(63, 0);

   uint32_t pointsize_loc = 0xff, position_loc = 0xff, layer_loc = 0xff;
   if (layer_regid != regid(63, 0)) {
      layer_loc = linkage.max_loc;
      ir3_link_add(&linkage, layer_regid, 0x1, linkage.max_loc);
   }
   if (position_regid != regid(63, 0)) {
      position_loc = linkage.max_loc;
      ir3_link_add(&linkage, position_regid, 0xf, linkage.max_loc);
   }
   if (pointsize_regid != regid(63, 0)) {
      pointsize_loc = linkage.max_loc;
      ir3_link_add(&linkage, pointsize_regid, 0x1, linkage.max_loc);
   }

   if (last_shader->shader->stream_output.num_outputs)
      tu6_setup_streamout(last_shader, &linkage, tf);

   /* map outputs of the last shader to VPC */
   assert(linkage.cnt <= 32);
   const uint32_t sp_out_count = DIV_ROUND_UP(linkage.cnt, 2);
   const uint32_t sp_vpc_dst_count = DIV_ROUND_UP(linkage.cnt, 4);
   uint32_t sp_out[16];
   uint32_t sp_vpc_dst[8];
   for (uint32_t i = 0; i < linkage.cnt; i++) {
      ((uint16_t *) sp_out)[i] =
         A6XX_SP_VS_OUT_REG_A_REGID(linkage.var[i].regid) |
         A6XX_SP_VS_OUT_REG_A_COMPMASK(linkage.var[i].compmask);
      ((uint8_t *) sp_vpc_dst)[i] =
         A6XX_SP_VS_VPC_DST_REG_OUTLOC0(linkage.var[i].loc);
   }

   if (gs)
      tu_cs_emit_pkt4(cs, REG_A6XX_SP_GS_OUT_REG(0), sp_out_count);
   else
      tu_cs_emit_pkt4(cs, REG_A6XX_SP_VS_OUT_REG(0), sp_out_count);
   tu_cs_emit_array(cs, sp_out, sp_out_count);

   if (gs)
      tu_cs_emit_pkt4(cs, REG_A6XX_SP_GS_VPC_DST_REG(0), sp_vpc_dst_count);
   else
      tu_cs_emit_pkt4(cs, REG_A6XX_SP_VS_VPC_DST_REG(0), sp_vpc_dst_count);
   tu_cs_emit_array(cs, sp_vpc_dst, sp_vpc_dst_count);

   tu_cs_emit_pkt4(cs, REG_A6XX_PC_PRIMID_CNTL, 1);
   tu_cs_emit(cs, COND(primid_passthru, A6XX_PC_PRIMID_CNTL_PRIMID_PASSTHRU));

   tu_cs_emit_pkt4(cs, REG_A6XX_VPC_CNTL_0, 1);
   tu_cs_emit(cs, A6XX_VPC_CNTL_0_NUMNONPOSVAR(fs ? fs->total_in : 0) |
                  COND(fs && fs->total_in, A6XX_VPC_CNTL_0_VARYING) |
                  A6XX_VPC_CNTL_0_PRIMIDLOC(linkage.primid_loc) |
                  A6XX_VPC_CNTL_0_UNKLOC(0xff));

   tu_cs_emit_pkt4(cs, REG_A6XX_VPC_PACK, 1);
   tu_cs_emit(cs, A6XX_VPC_PACK_POSITIONLOC(position_loc) |
                     A6XX_VPC_PACK_PSIZELOC(pointsize_loc) |
                     A6XX_VPC_PACK_STRIDE_IN_VPC(linkage.max_loc));

   if (gs) {
      uint32_t vertices_out, invocations, output, vec4_size;
      /* this detects the tu_clear_blit path, which doesn't set ->nir */
      if (gs->shader->nir) {
         tu6_emit_link_map(cs, vs, gs);
         vertices_out = gs->shader->nir->info.gs.vertices_out - 1;
         output = gl_primitive_to_tess(gs->shader->nir->info.gs.output_primitive);
         invocations = gs->shader->nir->info.gs.invocations - 1;
         /* Size of per-primitive alloction in ldlw memory in vec4s. */
         vec4_size = gs->shader->nir->info.gs.vertices_in *
                     DIV_ROUND_UP(vs->output_size, 4);
      } else {
         vertices_out = 3;
         output = TESS_CW_TRIS;
         invocations = 0;
         vec4_size = 0;
      }

      uint32_t primitive_regid =
            ir3_find_sysval_regid(gs, SYSTEM_VALUE_PRIMITIVE_ID);
      tu_cs_emit_pkt4(cs, REG_A6XX_VPC_PACK_GS, 1);
      tu_cs_emit(cs, A6XX_VPC_PACK_GS_POSITIONLOC(position_loc) |
             A6XX_VPC_PACK_GS_PSIZELOC(pointsize_loc) |
             A6XX_VPC_PACK_GS_STRIDE_IN_VPC(linkage.max_loc));

      tu_cs_emit_pkt4(cs, REG_A6XX_VPC_UNKNOWN_9105, 1);
      tu_cs_emit(cs, A6XX_VPC_UNKNOWN_9105_LAYERLOC(layer_loc) | 0xff00);

      tu_cs_emit_pkt4(cs, REG_A6XX_GRAS_UNKNOWN_809C, 1);
      tu_cs_emit(cs, CONDREG(layer_regid,
            A6XX_GRAS_UNKNOWN_809C_GS_WRITES_LAYER));

      uint32_t flags_regid = ir3_find_output_regid(gs,
            VARYING_SLOT_GS_VERTEX_FLAGS_IR3);

      tu_cs_emit_pkt4(cs, REG_A6XX_SP_PRIMITIVE_CNTL_GS, 1);
      tu_cs_emit(cs, A6XX_SP_PRIMITIVE_CNTL_GS_GSOUT(linkage.cnt) |
            A6XX_SP_PRIMITIVE_CNTL_GS_FLAGS_REGID(flags_regid));

      tu_cs_emit_pkt4(cs, REG_A6XX_PC_PRIMITIVE_CNTL_2, 1);
      tu_cs_emit(cs, A6XX_PC_PRIMITIVE_CNTL_2_STRIDE_IN_VPC(linkage.max_loc) |
            CONDREG(pointsize_regid, A6XX_PC_PRIMITIVE_CNTL_2_PSIZE) |
            CONDREG(layer_regid, A6XX_PC_PRIMITIVE_CNTL_2_LAYER) |
            CONDREG(primitive_regid, A6XX_PC_PRIMITIVE_CNTL_2_PRIMITIVE_ID));

      tu_cs_emit_pkt4(cs, REG_A6XX_PC_PRIMITIVE_CNTL_5, 1);
      tu_cs_emit(cs,
            A6XX_PC_PRIMITIVE_CNTL_5_GS_VERTICES_OUT(vertices_out) |
            A6XX_PC_PRIMITIVE_CNTL_5_GS_OUTPUT(output) |
            A6XX_PC_PRIMITIVE_CNTL_5_GS_INVOCATIONS(invocations));

      tu_cs_emit_pkt4(cs, REG_A6XX_PC_PRIMITIVE_CNTL_3, 1);
      tu_cs_emit(cs, 0);

      tu_cs_emit_pkt4(cs, REG_A6XX_GRAS_UNKNOWN_8003, 1);
      tu_cs_emit(cs, 0);

      tu_cs_emit_pkt4(cs, REG_A6XX_VPC_UNKNOWN_9100, 1);
      tu_cs_emit(cs, 0xff);

      tu_cs_emit_pkt4(cs, REG_A6XX_VPC_UNKNOWN_9102, 1);
      tu_cs_emit(cs, 0xffff00);

      tu_cs_emit_pkt4(cs, REG_A6XX_PC_PRIMITIVE_CNTL_6, 1);
      tu_cs_emit(cs, A6XX_PC_PRIMITIVE_CNTL_6_STRIDE_IN_VPC(vec4_size));

      tu_cs_emit_pkt4(cs, REG_A6XX_PC_UNKNOWN_9B07, 1);
      tu_cs_emit(cs, 0);

      tu_cs_emit_pkt4(cs, REG_A6XX_SP_GS_PRIM_SIZE, 1);
      tu_cs_emit(cs, vs->output_size);
   }

   tu_cs_emit_pkt4(cs, REG_A6XX_SP_PRIMITIVE_CNTL, 1);
   tu_cs_emit(cs, A6XX_SP_PRIMITIVE_CNTL_VSOUT(linkage.cnt));

   tu_cs_emit_pkt4(cs, REG_A6XX_PC_PRIMITIVE_CNTL_1, 1);
   tu_cs_emit(cs, A6XX_PC_PRIMITIVE_CNTL_1_STRIDE_IN_VPC(linkage.max_loc) |
         (last_shader->writes_psize ? A6XX_PC_PRIMITIVE_CNTL_1_PSIZE : 0));
}

static int
tu6_vpc_varying_mode(const struct ir3_shader_variant *fs,
                     uint32_t index,
                     uint8_t *interp_mode,
                     uint8_t *ps_repl_mode)
{
   enum
   {
      INTERP_SMOOTH = 0,
      INTERP_FLAT = 1,
      INTERP_ZERO = 2,
      INTERP_ONE = 3,
   };
   enum
   {
      PS_REPL_NONE = 0,
      PS_REPL_S = 1,
      PS_REPL_T = 2,
      PS_REPL_ONE_MINUS_T = 3,
   };

   const uint32_t compmask = fs->inputs[index].compmask;

   /* NOTE: varyings are packed, so if compmask is 0xb then first, second, and
    * fourth component occupy three consecutive varying slots
    */
   int shift = 0;
   *interp_mode = 0;
   *ps_repl_mode = 0;
   if (fs->inputs[index].slot == VARYING_SLOT_PNTC) {
      if (compmask & 0x1) {
         *ps_repl_mode |= PS_REPL_S << shift;
         shift += 2;
      }
      if (compmask & 0x2) {
         *ps_repl_mode |= PS_REPL_T << shift;
         shift += 2;
      }
      if (compmask & 0x4) {
         *interp_mode |= INTERP_ZERO << shift;
         shift += 2;
      }
      if (compmask & 0x8) {
         *interp_mode |= INTERP_ONE << 6;
         shift += 2;
      }
   } else if ((fs->inputs[index].interpolate == INTERP_MODE_FLAT) ||
              fs->inputs[index].rasterflat) {
      for (int i = 0; i < 4; i++) {
         if (compmask & (1 << i)) {
            *interp_mode |= INTERP_FLAT << shift;
            shift += 2;
         }
      }
   }

   return shift;
}

static void
tu6_emit_vpc_varying_modes(struct tu_cs *cs,
                           const struct ir3_shader_variant *fs)
{
   uint32_t interp_modes[8] = { 0 };
   uint32_t ps_repl_modes[8] = { 0 };

   if (fs) {
      for (int i = -1;
           (i = ir3_next_varying(fs, i)) < (int) fs->inputs_count;) {

         /* get the mode for input i */
         uint8_t interp_mode;
         uint8_t ps_repl_mode;
         const int bits =
            tu6_vpc_varying_mode(fs, i, &interp_mode, &ps_repl_mode);

         /* OR the mode into the array */
         const uint32_t inloc = fs->inputs[i].inloc * 2;
         uint32_t n = inloc / 32;
         uint32_t shift = inloc % 32;
         interp_modes[n] |= interp_mode << shift;
         ps_repl_modes[n] |= ps_repl_mode << shift;
         if (shift + bits > 32) {
            n++;
            shift = 32 - shift;

            interp_modes[n] |= interp_mode >> shift;
            ps_repl_modes[n] |= ps_repl_mode >> shift;
         }
      }
   }

   tu_cs_emit_pkt4(cs, REG_A6XX_VPC_VARYING_INTERP_MODE(0), 8);
   tu_cs_emit_array(cs, interp_modes, 8);

   tu_cs_emit_pkt4(cs, REG_A6XX_VPC_VARYING_PS_REPL_MODE(0), 8);
   tu_cs_emit_array(cs, ps_repl_modes, 8);
}

void
tu6_emit_fs_inputs(struct tu_cs *cs, const struct ir3_shader_variant *fs)
{
   uint32_t face_regid, coord_regid, zwcoord_regid, samp_id_regid;
   uint32_t ij_pix_regid, ij_samp_regid, ij_cent_regid, ij_size_regid;
   uint32_t smask_in_regid;

   bool sample_shading = fs->per_samp | fs->key.sample_shading;
   bool enable_varyings = fs->total_in > 0;

   samp_id_regid   = ir3_find_sysval_regid(fs, SYSTEM_VALUE_SAMPLE_ID);
   smask_in_regid  = ir3_find_sysval_regid(fs, SYSTEM_VALUE_SAMPLE_MASK_IN);
   face_regid      = ir3_find_sysval_regid(fs, SYSTEM_VALUE_FRONT_FACE);
   coord_regid     = ir3_find_sysval_regid(fs, SYSTEM_VALUE_FRAG_COORD);
   zwcoord_regid   = VALIDREG(coord_regid) ? coord_regid + 2 : regid(63, 0);
   ij_pix_regid    = ir3_find_sysval_regid(fs, SYSTEM_VALUE_BARYCENTRIC_PERSP_PIXEL);
   ij_samp_regid   = ir3_find_sysval_regid(fs, SYSTEM_VALUE_BARYCENTRIC_PERSP_SAMPLE);
   ij_cent_regid   = ir3_find_sysval_regid(fs, SYSTEM_VALUE_BARYCENTRIC_PERSP_CENTROID);
   ij_size_regid   = ir3_find_sysval_regid(fs, SYSTEM_VALUE_BARYCENTRIC_PERSP_SIZE);

   if (fs->num_sampler_prefetch > 0) {
      assert(VALIDREG(ij_pix_regid));
      /* also, it seems like ij_pix is *required* to be r0.x */
      assert(ij_pix_regid == regid(0, 0));
   }

   tu_cs_emit_pkt4(cs, REG_A6XX_SP_FS_PREFETCH_CNTL, 1 + fs->num_sampler_prefetch);
   tu_cs_emit(cs, A6XX_SP_FS_PREFETCH_CNTL_COUNT(fs->num_sampler_prefetch) |
         A6XX_SP_FS_PREFETCH_CNTL_UNK4(regid(63, 0)) |
         0x7000);    // XXX);
   for (int i = 0; i < fs->num_sampler_prefetch; i++) {
      const struct ir3_sampler_prefetch *prefetch = &fs->sampler_prefetch[i];
      tu_cs_emit(cs, A6XX_SP_FS_PREFETCH_CMD_SRC(prefetch->src) |
                     A6XX_SP_FS_PREFETCH_CMD_SAMP_ID(prefetch->samp_id) |
                     A6XX_SP_FS_PREFETCH_CMD_TEX_ID(prefetch->tex_id) |
                     A6XX_SP_FS_PREFETCH_CMD_DST(prefetch->dst) |
                     A6XX_SP_FS_PREFETCH_CMD_WRMASK(prefetch->wrmask) |
                     COND(prefetch->half_precision, A6XX_SP_FS_PREFETCH_CMD_HALF) |
                     A6XX_SP_FS_PREFETCH_CMD_CMD(prefetch->cmd));
   }

   if (fs->num_sampler_prefetch > 0) {
      tu_cs_emit_pkt4(cs, REG_A6XX_SP_FS_BINDLESS_PREFETCH_CMD(0), fs->num_sampler_prefetch);
      for (int i = 0; i < fs->num_sampler_prefetch; i++) {
         const struct ir3_sampler_prefetch *prefetch = &fs->sampler_prefetch[i];
         tu_cs_emit(cs,
                    A6XX_SP_FS_BINDLESS_PREFETCH_CMD_SAMP_ID(prefetch->samp_bindless_id) |
                    A6XX_SP_FS_BINDLESS_PREFETCH_CMD_TEX_ID(prefetch->tex_bindless_id));
      }
   }

   tu_cs_emit_pkt4(cs, REG_A6XX_HLSQ_CONTROL_1_REG, 5);
   tu_cs_emit(cs, 0x7);
   tu_cs_emit(cs, A6XX_HLSQ_CONTROL_2_REG_FACEREGID(face_regid) |
                  A6XX_HLSQ_CONTROL_2_REG_SAMPLEID(samp_id_regid) |
                  A6XX_HLSQ_CONTROL_2_REG_SAMPLEMASK(smask_in_regid) |
                  A6XX_HLSQ_CONTROL_2_REG_SIZE(ij_size_regid));
   tu_cs_emit(cs, A6XX_HLSQ_CONTROL_3_REG_BARY_IJ_PIXEL(ij_pix_regid) |
                  A6XX_HLSQ_CONTROL_3_REG_BARY_IJ_CENTROID(ij_cent_regid) |
                  0xfc00fc00);
   tu_cs_emit(cs, A6XX_HLSQ_CONTROL_4_REG_XYCOORDREGID(coord_regid) |
                  A6XX_HLSQ_CONTROL_4_REG_ZWCOORDREGID(zwcoord_regid) |
                  A6XX_HLSQ_CONTROL_4_REG_BARY_IJ_PIXEL_PERSAMP(ij_samp_regid) |
                  0x0000fc00);
   tu_cs_emit(cs, 0xfc);

   tu_cs_emit_pkt4(cs, REG_A6XX_HLSQ_UNKNOWN_B980, 1);
   tu_cs_emit(cs, enable_varyings ? 3 : 1);

   tu_cs_emit_pkt4(cs, REG_A6XX_GRAS_CNTL, 1);
   tu_cs_emit(cs,
         CONDREG(ij_pix_regid, A6XX_GRAS_CNTL_VARYING) |
         CONDREG(ij_cent_regid, A6XX_GRAS_CNTL_CENTROID) |
         CONDREG(ij_samp_regid, A6XX_GRAS_CNTL_PERSAMP_VARYING) |
         COND(VALIDREG(ij_size_regid) && !sample_shading, A6XX_GRAS_CNTL_SIZE) |
         COND(VALIDREG(ij_size_regid) &&  sample_shading, A6XX_GRAS_CNTL_SIZE_PERSAMP) |
         COND(fs->fragcoord_compmask != 0, A6XX_GRAS_CNTL_SIZE |
                              A6XX_GRAS_CNTL_COORD_MASK(fs->fragcoord_compmask)) |
         COND(fs->frag_face, A6XX_GRAS_CNTL_SIZE));

   tu_cs_emit_pkt4(cs, REG_A6XX_RB_RENDER_CONTROL0, 2);
   tu_cs_emit(cs,
         CONDREG(ij_pix_regid, A6XX_RB_RENDER_CONTROL0_VARYING) |
         CONDREG(ij_cent_regid, A6XX_RB_RENDER_CONTROL0_CENTROID) |
         CONDREG(ij_samp_regid, A6XX_RB_RENDER_CONTROL0_PERSAMP_VARYING) |
         COND(enable_varyings, A6XX_RB_RENDER_CONTROL0_UNK10) |
         COND(VALIDREG(ij_size_regid) && !sample_shading, A6XX_RB_RENDER_CONTROL0_SIZE) |
         COND(VALIDREG(ij_size_regid) &&  sample_shading, A6XX_RB_RENDER_CONTROL0_SIZE_PERSAMP) |
         COND(fs->fragcoord_compmask != 0, A6XX_RB_RENDER_CONTROL0_SIZE |
                              A6XX_RB_RENDER_CONTROL0_COORD_MASK(fs->fragcoord_compmask)) |
         COND(fs->frag_face, A6XX_RB_RENDER_CONTROL0_SIZE));
   tu_cs_emit(cs,
         /* these two bits (UNK4/UNK5) relate to fragcoord
          * without them, fragcoord is the same for all samples
          */
         COND(sample_shading, A6XX_RB_RENDER_CONTROL1_UNK4) |
         COND(sample_shading, A6XX_RB_RENDER_CONTROL1_UNK5) |
         CONDREG(smask_in_regid, A6XX_RB_RENDER_CONTROL1_SAMPLEMASK) |
         CONDREG(samp_id_regid, A6XX_RB_RENDER_CONTROL1_SAMPLEID) |
         CONDREG(ij_size_regid, A6XX_RB_RENDER_CONTROL1_SIZE) |
         COND(fs->frag_face, A6XX_RB_RENDER_CONTROL1_FACENESS));

   tu_cs_emit_pkt4(cs, REG_A6XX_RB_SAMPLE_CNTL, 1);
   tu_cs_emit(cs, COND(sample_shading, A6XX_RB_SAMPLE_CNTL_PER_SAMP_MODE));

   tu_cs_emit_pkt4(cs, REG_A6XX_GRAS_UNKNOWN_8101, 1);
   tu_cs_emit(cs, COND(sample_shading, 0x6));  // XXX

   tu_cs_emit_pkt4(cs, REG_A6XX_GRAS_SAMPLE_CNTL, 1);
   tu_cs_emit(cs, COND(sample_shading, A6XX_GRAS_SAMPLE_CNTL_PER_SAMP_MODE));
}

static void
tu6_emit_fs_outputs(struct tu_cs *cs,
                    const struct ir3_shader_variant *fs,
                    uint32_t mrt_count, bool dual_src_blend,
                    uint32_t render_components)
{
   uint32_t smask_regid, posz_regid;

   posz_regid      = ir3_find_output_regid(fs, FRAG_RESULT_DEPTH);
   smask_regid     = ir3_find_output_regid(fs, FRAG_RESULT_SAMPLE_MASK);

   uint32_t fragdata_regid[8];
   if (fs->color0_mrt) {
      fragdata_regid[0] = ir3_find_output_regid(fs, FRAG_RESULT_COLOR);
      for (uint32_t i = 1; i < ARRAY_SIZE(fragdata_regid); i++)
         fragdata_regid[i] = fragdata_regid[0];
   } else {
      for (uint32_t i = 0; i < ARRAY_SIZE(fragdata_regid); i++)
         fragdata_regid[i] = ir3_find_output_regid(fs, FRAG_RESULT_DATA0 + i);
   }

   tu_cs_emit_pkt4(cs, REG_A6XX_SP_FS_OUTPUT_CNTL0, 2);
   tu_cs_emit(cs, A6XX_SP_FS_OUTPUT_CNTL0_DEPTH_REGID(posz_regid) |
                  A6XX_SP_FS_OUTPUT_CNTL0_SAMPMASK_REGID(smask_regid) |
                  COND(dual_src_blend, A6XX_SP_FS_OUTPUT_CNTL0_DUAL_COLOR_IN_ENABLE) |
                  0xfc000000);
   tu_cs_emit(cs, A6XX_SP_FS_OUTPUT_CNTL1_MRT(mrt_count));

   tu_cs_emit_pkt4(cs, REG_A6XX_SP_FS_OUTPUT_REG(0), 8);
   for (uint32_t i = 0; i < ARRAY_SIZE(fragdata_regid); i++) {
      // TODO we could have a mix of half and full precision outputs,
      // we really need to figure out half-precision from IR3_REG_HALF
      tu_cs_emit(cs, A6XX_SP_FS_OUTPUT_REG_REGID(fragdata_regid[i]) |
                        (false ? A6XX_SP_FS_OUTPUT_REG_HALF_PRECISION : 0));
   }

   tu_cs_emit_regs(cs,
                   A6XX_SP_FS_RENDER_COMPONENTS(.dword = render_components));

   tu_cs_emit_pkt4(cs, REG_A6XX_RB_FS_OUTPUT_CNTL0, 2);
   tu_cs_emit(cs, COND(fs->writes_pos, A6XX_RB_FS_OUTPUT_CNTL0_FRAG_WRITES_Z) |
                  COND(fs->writes_smask, A6XX_RB_FS_OUTPUT_CNTL0_FRAG_WRITES_SAMPMASK) |
                  COND(dual_src_blend, A6XX_RB_FS_OUTPUT_CNTL0_DUAL_COLOR_IN_ENABLE));
   tu_cs_emit(cs, A6XX_RB_FS_OUTPUT_CNTL1_MRT(mrt_count));

   tu_cs_emit_regs(cs,
                   A6XX_RB_RENDER_COMPONENTS(.dword = render_components));

   enum a6xx_ztest_mode zmode;

   if (fs->no_earlyz || fs->has_kill || fs->writes_pos) {
      zmode = A6XX_LATE_Z;
   } else {
      zmode = A6XX_EARLY_Z;
   }

   tu_cs_emit_pkt4(cs, REG_A6XX_GRAS_SU_DEPTH_PLANE_CNTL, 1);
   tu_cs_emit(cs, A6XX_GRAS_SU_DEPTH_PLANE_CNTL_Z_MODE(zmode));

   tu_cs_emit_pkt4(cs, REG_A6XX_RB_DEPTH_PLANE_CNTL, 1);
   tu_cs_emit(cs, A6XX_RB_DEPTH_PLANE_CNTL_Z_MODE(zmode));
}

static void
tu6_emit_geometry_consts(struct tu_cs *cs,
                         const struct ir3_shader_variant *vs,
                         const struct ir3_shader_variant *gs) {
   unsigned num_vertices = gs->shader->nir->info.gs.vertices_in;

   uint32_t params[4] = {
      vs->output_size * num_vertices * 4,  /* primitive stride */
      vs->output_size * 4,                 /* vertex stride */
      0,
      0,
   };
   uint32_t vs_base = ir3_const_state(vs)->offsets.primitive_param;
   tu6_emit_const(cs, CP_LOAD_STATE6_GEOM, vs_base, SB6_VS_SHADER, 0,
                  ARRAY_SIZE(params), params);

   uint32_t gs_base = ir3_const_state(gs)->offsets.primitive_param;
   tu6_emit_const(cs, CP_LOAD_STATE6_GEOM, gs_base, SB6_GS_SHADER, 0,
                  ARRAY_SIZE(params), params);
}

static void
tu6_emit_program(struct tu_cs *cs,
                 struct tu_pipeline_builder *builder,
                 const struct tu_bo *binary_bo,
                 bool binning_pass,
                 struct tu_streamout_state *tf)
{
   const struct ir3_shader_variant *vs = builder->variants[MESA_SHADER_VERTEX];
   const struct ir3_shader_variant *bs = builder->binning_variant;
   const struct ir3_shader_variant *gs = builder->variants[MESA_SHADER_GEOMETRY];
   const struct ir3_shader_variant *fs = builder->variants[MESA_SHADER_FRAGMENT];
   gl_shader_stage stage = MESA_SHADER_VERTEX;

   STATIC_ASSERT(MESA_SHADER_VERTEX == 0);

   tu_cs_emit_pkt4(cs, REG_A6XX_HLSQ_UPDATE_CNTL, 1);
   tu_cs_emit(cs, 0xff); /* XXX */

  /* Don't use the binning pass variant when GS is present because we don't
   * support compiling correct binning pass variants with GS.
   */
   if (binning_pass && !gs) {
      vs = bs;
      tu6_emit_xs_config(cs, stage, bs,
                         binary_bo->iova + builder->binning_vs_offset);
      stage++;
   }

   for (; stage < ARRAY_SIZE(builder->shaders); stage++) {
      const struct ir3_shader_variant *xs = builder->variants[stage];

      if (stage == MESA_SHADER_FRAGMENT && binning_pass)
         fs = xs = NULL;

      tu6_emit_xs_config(cs, stage, xs,
                         binary_bo->iova + builder->shader_offsets[stage]);
   }

   tu_cs_emit_pkt4(cs, REG_A6XX_SP_HS_UNKNOWN_A831, 1);
   tu_cs_emit(cs, 0);

   tu6_emit_vpc(cs, vs, gs, fs, tf);
   tu6_emit_vpc_varying_modes(cs, fs);

   if (fs) {
      tu6_emit_fs_inputs(cs, fs);
      tu6_emit_fs_outputs(cs, fs, builder->color_attachment_count,
                          builder->use_dual_src_blend,
                          builder->render_components);
   } else {
      /* TODO: check if these can be skipped if fs is disabled */
      struct ir3_shader_variant dummy_variant = {};
      tu6_emit_fs_inputs(cs, &dummy_variant);
      tu6_emit_fs_outputs(cs, &dummy_variant, builder->color_attachment_count,
                          builder->use_dual_src_blend,
                          builder->render_components);
   }

   if (gs)
      tu6_emit_geometry_consts(cs, vs, gs);
}

static void
tu6_emit_vertex_input(struct tu_cs *cs,
                      const struct ir3_shader_variant *vs,
                      const VkPipelineVertexInputStateCreateInfo *info,
                      uint32_t *bindings_used)
{
   uint32_t vfd_decode_idx = 0;
   uint32_t binding_instanced = 0; /* bitmask of instanced bindings */

   for (uint32_t i = 0; i < info->vertexBindingDescriptionCount; i++) {
      const VkVertexInputBindingDescription *binding =
         &info->pVertexBindingDescriptions[i];

      tu_cs_emit_regs(cs,
                      A6XX_VFD_FETCH_STRIDE(binding->binding, binding->stride));

      if (binding->inputRate == VK_VERTEX_INPUT_RATE_INSTANCE)
         binding_instanced |= 1 << binding->binding;

      *bindings_used |= 1 << binding->binding;
   }

   /* TODO: emit all VFD_DECODE/VFD_DEST_CNTL in same (two) pkt4 */

   for (uint32_t i = 0; i < info->vertexAttributeDescriptionCount; i++) {
      const VkVertexInputAttributeDescription *attr =
         &info->pVertexAttributeDescriptions[i];
      uint32_t input_idx;

      for (input_idx = 0; input_idx < vs->inputs_count; input_idx++) {
         if ((vs->inputs[input_idx].slot - VERT_ATTRIB_GENERIC0) == attr->location)
            break;
      }

      /* attribute not used, skip it */
      if (input_idx == vs->inputs_count)
         continue;

      const struct tu_native_format format = tu6_format_vtx(attr->format);
      tu_cs_emit_regs(cs,
                      A6XX_VFD_DECODE_INSTR(vfd_decode_idx,
                        .idx = attr->binding,
                        .offset = attr->offset,
                        .instanced = binding_instanced & (1 << attr->binding),
                        .format = format.fmt,
                        .swap = format.swap,
                        .unk30 = 1,
                        ._float = !vk_format_is_int(attr->format)),
                      A6XX_VFD_DECODE_STEP_RATE(vfd_decode_idx, 1));

      tu_cs_emit_regs(cs,
                      A6XX_VFD_DEST_CNTL_INSTR(vfd_decode_idx,
                        .writemask = vs->inputs[input_idx].compmask,
                        .regid = vs->inputs[input_idx].regid));

      vfd_decode_idx++;
   }

   tu_cs_emit_regs(cs,
                   A6XX_VFD_CONTROL_0(
                     .fetch_cnt = vfd_decode_idx, /* decode_cnt for binning pass ? */
                     .decode_cnt = vfd_decode_idx));
}

static uint32_t
tu6_guardband_adj(uint32_t v)
{
   if (v > 256)
      return (uint32_t)(511.0 - 65.0 * (log2(v) - 8.0));
   else
      return 511;
}

void
tu6_emit_viewport(struct tu_cs *cs, const VkViewport *viewport)
{
   float offsets[3];
   float scales[3];
   scales[0] = viewport->width / 2.0f;
   scales[1] = viewport->height / 2.0f;
   scales[2] = viewport->maxDepth - viewport->minDepth;
   offsets[0] = viewport->x + scales[0];
   offsets[1] = viewport->y + scales[1];
   offsets[2] = viewport->minDepth;

   VkOffset2D min;
   VkOffset2D max;
   min.x = (int32_t) viewport->x;
   max.x = (int32_t) ceilf(viewport->x + viewport->width);
   if (viewport->height >= 0.0f) {
      min.y = (int32_t) viewport->y;
      max.y = (int32_t) ceilf(viewport->y + viewport->height);
   } else {
      min.y = (int32_t)(viewport->y + viewport->height);
      max.y = (int32_t) ceilf(viewport->y);
   }
   /* the spec allows viewport->height to be 0.0f */
   if (min.y == max.y)
      max.y++;
   assert(min.x >= 0 && min.x < max.x);
   assert(min.y >= 0 && min.y < max.y);

   VkExtent2D guardband_adj;
   guardband_adj.width = tu6_guardband_adj(max.x - min.x);
   guardband_adj.height = tu6_guardband_adj(max.y - min.y);

   tu_cs_emit_pkt4(cs, REG_A6XX_GRAS_CL_VPORT_XOFFSET_0, 6);
   tu_cs_emit(cs, A6XX_GRAS_CL_VPORT_XOFFSET_0(offsets[0]).value);
   tu_cs_emit(cs, A6XX_GRAS_CL_VPORT_XSCALE_0(scales[0]).value);
   tu_cs_emit(cs, A6XX_GRAS_CL_VPORT_YOFFSET_0(offsets[1]).value);
   tu_cs_emit(cs, A6XX_GRAS_CL_VPORT_YSCALE_0(scales[1]).value);
   tu_cs_emit(cs, A6XX_GRAS_CL_VPORT_ZOFFSET_0(offsets[2]).value);
   tu_cs_emit(cs, A6XX_GRAS_CL_VPORT_ZSCALE_0(scales[2]).value);

   tu_cs_emit_pkt4(cs, REG_A6XX_GRAS_SC_VIEWPORT_SCISSOR_TL_0, 2);
   tu_cs_emit(cs, A6XX_GRAS_SC_VIEWPORT_SCISSOR_TL_0_X(min.x) |
                     A6XX_GRAS_SC_VIEWPORT_SCISSOR_TL_0_Y(min.y));
   tu_cs_emit(cs, A6XX_GRAS_SC_VIEWPORT_SCISSOR_TL_0_X(max.x - 1) |
                     A6XX_GRAS_SC_VIEWPORT_SCISSOR_TL_0_Y(max.y - 1));

   tu_cs_emit_pkt4(cs, REG_A6XX_GRAS_CL_GUARDBAND_CLIP_ADJ, 1);
   tu_cs_emit(cs,
              A6XX_GRAS_CL_GUARDBAND_CLIP_ADJ_HORZ(guardband_adj.width) |
                 A6XX_GRAS_CL_GUARDBAND_CLIP_ADJ_VERT(guardband_adj.height));

   float z_clamp_min = MIN2(viewport->minDepth, viewport->maxDepth);
   float z_clamp_max = MAX2(viewport->minDepth, viewport->maxDepth);

   tu_cs_emit_regs(cs,
                   A6XX_GRAS_CL_Z_CLAMP_MIN(z_clamp_min),
                   A6XX_GRAS_CL_Z_CLAMP_MAX(z_clamp_max));

   tu_cs_emit_regs(cs,
                   A6XX_RB_Z_CLAMP_MIN(z_clamp_min),
                   A6XX_RB_Z_CLAMP_MAX(z_clamp_max));
}

void
tu6_emit_scissor(struct tu_cs *cs, const VkRect2D *scissor)
{
   const VkOffset2D min = scissor->offset;
   const VkOffset2D max = {
      scissor->offset.x + scissor->extent.width,
      scissor->offset.y + scissor->extent.height,
   };

   tu_cs_emit_pkt4(cs, REG_A6XX_GRAS_SC_SCREEN_SCISSOR_TL_0, 2);
   tu_cs_emit(cs, A6XX_GRAS_SC_SCREEN_SCISSOR_TL_0_X(min.x) |
                     A6XX_GRAS_SC_SCREEN_SCISSOR_TL_0_Y(min.y));
   tu_cs_emit(cs, A6XX_GRAS_SC_SCREEN_SCISSOR_TL_0_X(max.x - 1) |
                     A6XX_GRAS_SC_SCREEN_SCISSOR_TL_0_Y(max.y - 1));
}

void
tu6_emit_sample_locations(struct tu_cs *cs, const VkSampleLocationsInfoEXT *samp_loc)
{
   if (!samp_loc) {
      tu_cs_emit_pkt4(cs, REG_A6XX_GRAS_SAMPLE_CONFIG, 1);
      tu_cs_emit(cs, 0);

      tu_cs_emit_pkt4(cs, REG_A6XX_RB_SAMPLE_CONFIG, 1);
      tu_cs_emit(cs, 0);

      tu_cs_emit_pkt4(cs, REG_A6XX_SP_TP_SAMPLE_CONFIG, 1);
      tu_cs_emit(cs, 0);
      return;
   }

   assert(samp_loc->sampleLocationsPerPixel == samp_loc->sampleLocationsCount);
   assert(samp_loc->sampleLocationGridSize.width == 1);
   assert(samp_loc->sampleLocationGridSize.height == 1);

   uint32_t sample_config =
      A6XX_RB_SAMPLE_CONFIG_LOCATION_ENABLE;
   uint32_t sample_locations = 0;
   for (uint32_t i = 0; i < samp_loc->sampleLocationsCount; i++) {
      sample_locations |=
         (A6XX_RB_SAMPLE_LOCATION_0_SAMPLE_0_X(samp_loc->pSampleLocations[i].x) |
          A6XX_RB_SAMPLE_LOCATION_0_SAMPLE_0_Y(samp_loc->pSampleLocations[i].y)) << i*8;
   }

   tu_cs_emit_pkt4(cs, REG_A6XX_GRAS_SAMPLE_CONFIG, 2);
   tu_cs_emit(cs, sample_config);
   tu_cs_emit(cs, sample_locations);

   tu_cs_emit_pkt4(cs, REG_A6XX_RB_SAMPLE_CONFIG, 2);
   tu_cs_emit(cs, sample_config);
   tu_cs_emit(cs, sample_locations);

   tu_cs_emit_pkt4(cs, REG_A6XX_SP_TP_SAMPLE_CONFIG, 2);
   tu_cs_emit(cs, sample_config);
   tu_cs_emit(cs, sample_locations);
}

static uint32_t
tu6_gras_su_cntl(const VkPipelineRasterizationStateCreateInfo *rast_info,
                 VkSampleCountFlagBits samples)
{
   uint32_t gras_su_cntl = 0;

   if (rast_info->cullMode & VK_CULL_MODE_FRONT_BIT)
      gras_su_cntl |= A6XX_GRAS_SU_CNTL_CULL_FRONT;
   if (rast_info->cullMode & VK_CULL_MODE_BACK_BIT)
      gras_su_cntl |= A6XX_GRAS_SU_CNTL_CULL_BACK;

   if (rast_info->frontFace == VK_FRONT_FACE_CLOCKWISE)
      gras_su_cntl |= A6XX_GRAS_SU_CNTL_FRONT_CW;

   /* don't set A6XX_GRAS_SU_CNTL_LINEHALFWIDTH */

   if (rast_info->depthBiasEnable)
      gras_su_cntl |= A6XX_GRAS_SU_CNTL_POLY_OFFSET;

   if (samples > VK_SAMPLE_COUNT_1_BIT)
      gras_su_cntl |= A6XX_GRAS_SU_CNTL_MSAA_ENABLE;

   return gras_su_cntl;
}

void
tu6_emit_depth_bias(struct tu_cs *cs,
                    float constant_factor,
                    float clamp,
                    float slope_factor)
{
   tu_cs_emit_pkt4(cs, REG_A6XX_GRAS_SU_POLY_OFFSET_SCALE, 3);
   tu_cs_emit(cs, A6XX_GRAS_SU_POLY_OFFSET_SCALE(slope_factor).value);
   tu_cs_emit(cs, A6XX_GRAS_SU_POLY_OFFSET_OFFSET(constant_factor).value);
   tu_cs_emit(cs, A6XX_GRAS_SU_POLY_OFFSET_OFFSET_CLAMP(clamp).value);
}

static void
tu6_emit_depth_control(struct tu_cs *cs,
                       const VkPipelineDepthStencilStateCreateInfo *ds_info,
                       const VkPipelineRasterizationStateCreateInfo *rast_info)
{
   assert(!ds_info->depthBoundsTestEnable);

   uint32_t rb_depth_cntl = 0;
   if (ds_info->depthTestEnable) {
      rb_depth_cntl |=
         A6XX_RB_DEPTH_CNTL_Z_ENABLE |
         A6XX_RB_DEPTH_CNTL_ZFUNC(tu6_compare_func(ds_info->depthCompareOp)) |
         A6XX_RB_DEPTH_CNTL_Z_TEST_ENABLE;

      if (rast_info->depthClampEnable)
         rb_depth_cntl |= A6XX_RB_DEPTH_CNTL_Z_CLAMP_ENABLE;

      if (ds_info->depthWriteEnable)
         rb_depth_cntl |= A6XX_RB_DEPTH_CNTL_Z_WRITE_ENABLE;
   }

   tu_cs_emit_pkt4(cs, REG_A6XX_RB_DEPTH_CNTL, 1);
   tu_cs_emit(cs, rb_depth_cntl);
}

static void
tu6_emit_stencil_control(struct tu_cs *cs,
                         const VkPipelineDepthStencilStateCreateInfo *ds_info)
{
   uint32_t rb_stencil_control = 0;
   if (ds_info->stencilTestEnable) {
      const VkStencilOpState *front = &ds_info->front;
      const VkStencilOpState *back = &ds_info->back;
      rb_stencil_control |=
         A6XX_RB_STENCIL_CONTROL_STENCIL_ENABLE |
         A6XX_RB_STENCIL_CONTROL_STENCIL_ENABLE_BF |
         A6XX_RB_STENCIL_CONTROL_STENCIL_READ |
         A6XX_RB_STENCIL_CONTROL_FUNC(tu6_compare_func(front->compareOp)) |
         A6XX_RB_STENCIL_CONTROL_FAIL(tu6_stencil_op(front->failOp)) |
         A6XX_RB_STENCIL_CONTROL_ZPASS(tu6_stencil_op(front->passOp)) |
         A6XX_RB_STENCIL_CONTROL_ZFAIL(tu6_stencil_op(front->depthFailOp)) |
         A6XX_RB_STENCIL_CONTROL_FUNC_BF(tu6_compare_func(back->compareOp)) |
         A6XX_RB_STENCIL_CONTROL_FAIL_BF(tu6_stencil_op(back->failOp)) |
         A6XX_RB_STENCIL_CONTROL_ZPASS_BF(tu6_stencil_op(back->passOp)) |
         A6XX_RB_STENCIL_CONTROL_ZFAIL_BF(tu6_stencil_op(back->depthFailOp));
   }

   tu_cs_emit_pkt4(cs, REG_A6XX_RB_STENCIL_CONTROL, 1);
   tu_cs_emit(cs, rb_stencil_control);
}

static uint32_t
tu6_rb_mrt_blend_control(const VkPipelineColorBlendAttachmentState *att,
                         bool has_alpha)
{
   const enum a3xx_rb_blend_opcode color_op = tu6_blend_op(att->colorBlendOp);
   const enum adreno_rb_blend_factor src_color_factor = tu6_blend_factor(
      has_alpha ? att->srcColorBlendFactor
                : tu_blend_factor_no_dst_alpha(att->srcColorBlendFactor));
   const enum adreno_rb_blend_factor dst_color_factor = tu6_blend_factor(
      has_alpha ? att->dstColorBlendFactor
                : tu_blend_factor_no_dst_alpha(att->dstColorBlendFactor));
   const enum a3xx_rb_blend_opcode alpha_op = tu6_blend_op(att->alphaBlendOp);
   const enum adreno_rb_blend_factor src_alpha_factor =
      tu6_blend_factor(att->srcAlphaBlendFactor);
   const enum adreno_rb_blend_factor dst_alpha_factor =
      tu6_blend_factor(att->dstAlphaBlendFactor);

   return A6XX_RB_MRT_BLEND_CONTROL_RGB_SRC_FACTOR(src_color_factor) |
          A6XX_RB_MRT_BLEND_CONTROL_RGB_BLEND_OPCODE(color_op) |
          A6XX_RB_MRT_BLEND_CONTROL_RGB_DEST_FACTOR(dst_color_factor) |
          A6XX_RB_MRT_BLEND_CONTROL_ALPHA_SRC_FACTOR(src_alpha_factor) |
          A6XX_RB_MRT_BLEND_CONTROL_ALPHA_BLEND_OPCODE(alpha_op) |
          A6XX_RB_MRT_BLEND_CONTROL_ALPHA_DEST_FACTOR(dst_alpha_factor);
}

static uint32_t
tu6_rb_mrt_control(const VkPipelineColorBlendAttachmentState *att,
                   uint32_t rb_mrt_control_rop,
                   bool is_int,
                   bool has_alpha)
{
   uint32_t rb_mrt_control =
      A6XX_RB_MRT_CONTROL_COMPONENT_ENABLE(att->colorWriteMask);

   /* ignore blending and logic op for integer attachments */
   if (is_int) {
      rb_mrt_control |= A6XX_RB_MRT_CONTROL_ROP_CODE(ROP_COPY);
      return rb_mrt_control;
   }

   rb_mrt_control |= rb_mrt_control_rop;

   if (att->blendEnable) {
      rb_mrt_control |= A6XX_RB_MRT_CONTROL_BLEND;

      if (has_alpha)
         rb_mrt_control |= A6XX_RB_MRT_CONTROL_BLEND2;
   }

   return rb_mrt_control;
}

static void
tu6_emit_rb_mrt_controls(struct tu_cs *cs,
                         const VkPipelineColorBlendStateCreateInfo *blend_info,
                         const VkFormat attachment_formats[MAX_RTS],
                         uint32_t *blend_enable_mask)
{
   *blend_enable_mask = 0;

   bool rop_reads_dst = false;
   uint32_t rb_mrt_control_rop = 0;
   if (blend_info->logicOpEnable) {
      rop_reads_dst = tu_logic_op_reads_dst(blend_info->logicOp);
      rb_mrt_control_rop =
         A6XX_RB_MRT_CONTROL_ROP_ENABLE |
         A6XX_RB_MRT_CONTROL_ROP_CODE(tu6_rop(blend_info->logicOp));
   }

   for (uint32_t i = 0; i < blend_info->attachmentCount; i++) {
      const VkPipelineColorBlendAttachmentState *att =
         &blend_info->pAttachments[i];
      const VkFormat format = attachment_formats[i];

      uint32_t rb_mrt_control = 0;
      uint32_t rb_mrt_blend_control = 0;
      if (format != VK_FORMAT_UNDEFINED) {
         const bool is_int = vk_format_is_int(format);
         const bool has_alpha = vk_format_has_alpha(format);

         rb_mrt_control =
            tu6_rb_mrt_control(att, rb_mrt_control_rop, is_int, has_alpha);
         rb_mrt_blend_control = tu6_rb_mrt_blend_control(att, has_alpha);

         if (att->blendEnable || rop_reads_dst)
            *blend_enable_mask |= 1 << i;
      }

      tu_cs_emit_pkt4(cs, REG_A6XX_RB_MRT_CONTROL(i), 2);
      tu_cs_emit(cs, rb_mrt_control);
      tu_cs_emit(cs, rb_mrt_blend_control);
   }
}

static void
tu6_emit_blend_control(struct tu_cs *cs,
                       uint32_t blend_enable_mask,
                       bool dual_src_blend,
                       const VkPipelineMultisampleStateCreateInfo *msaa_info)
{
   const uint32_t sample_mask =
      msaa_info->pSampleMask ? (*msaa_info->pSampleMask & 0xffff)
                             : ((1 << msaa_info->rasterizationSamples) - 1);

   tu_cs_emit_regs(cs,
                   A6XX_SP_BLEND_CNTL(.enabled = blend_enable_mask,
                                      .dual_color_in_enable = dual_src_blend,
                                      .alpha_to_coverage = msaa_info->alphaToCoverageEnable,
                                      .unk8 = true));

   /* set A6XX_RB_BLEND_CNTL_INDEPENDENT_BLEND only when enabled? */
   tu_cs_emit_regs(cs,
                   A6XX_RB_BLEND_CNTL(.enable_blend = blend_enable_mask,
                                      .independent_blend = true,
                                      .sample_mask = sample_mask,
                                      .dual_color_in_enable = dual_src_blend,
                                      .alpha_to_coverage = msaa_info->alphaToCoverageEnable,
                                      .alpha_to_one = msaa_info->alphaToOneEnable));
}

static VkResult
tu_pipeline_create(struct tu_device *dev,
                   struct tu_pipeline_layout *layout,
                   bool compute,
                   const VkAllocationCallbacks *pAllocator,
                   struct tu_pipeline **out_pipeline)
{
   struct tu_pipeline *pipeline =
      vk_zalloc2(&dev->alloc, pAllocator, sizeof(*pipeline), 8,
                 VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!pipeline)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   tu_cs_init(&pipeline->cs, dev, TU_CS_MODE_SUB_STREAM, 2048);

   /* Reserve the space now such that tu_cs_begin_sub_stream never fails. Note
    * that LOAD_STATE can potentially take up a large amount of space so we
    * calculate its size explicitly.
   */
   unsigned load_state_size = tu6_load_state_size(layout, compute);
   VkResult result = tu_cs_reserve_space(&pipeline->cs, 2048 + load_state_size);
   if (result != VK_SUCCESS) {
      vk_free2(&dev->alloc, pAllocator, pipeline);
      return result;
   }

   *out_pipeline = pipeline;

   return VK_SUCCESS;
}

static void
tu_pipeline_shader_key_init(struct ir3_shader_key *key,
                            const VkGraphicsPipelineCreateInfo *pipeline_info)
{
   for (uint32_t i = 0; i < pipeline_info->stageCount; i++) {
      if (pipeline_info->pStages[i].stage == VK_SHADER_STAGE_GEOMETRY_BIT) {
         key->has_gs = true;
         break;
      }
   }

   if (pipeline_info->pRasterizationState->rasterizerDiscardEnable)
      return;

   const VkPipelineMultisampleStateCreateInfo *msaa_info = pipeline_info->pMultisampleState;
   const struct VkPipelineSampleLocationsStateCreateInfoEXT *sample_locations =
      vk_find_struct_const(msaa_info->pNext, PIPELINE_SAMPLE_LOCATIONS_STATE_CREATE_INFO_EXT);
   if (msaa_info->rasterizationSamples > 1 ||
       /* also set msaa key when sample location is not the default
        * since this affects varying interpolation */
       (sample_locations && sample_locations->sampleLocationsEnable)) {
      key->msaa = true;
   }

   /* note: not actually used by ir3, just checked in tu6_emit_fs_inputs */
   if (msaa_info->sampleShadingEnable)
      key->sample_shading = true;

   /* TODO: Populate the remaining fields of ir3_shader_key. */
}

static VkResult
tu_pipeline_builder_compile_shaders(struct tu_pipeline_builder *builder)
{
   const VkPipelineShaderStageCreateInfo *stage_infos[MESA_SHADER_STAGES] = {
      NULL
   };
   for (uint32_t i = 0; i < builder->create_info->stageCount; i++) {
      gl_shader_stage stage =
         vk_to_mesa_shader_stage(builder->create_info->pStages[i].stage);
      stage_infos[stage] = &builder->create_info->pStages[i];
   }

   struct ir3_shader_key key = {};
   tu_pipeline_shader_key_init(&key, builder->create_info);

   for (gl_shader_stage stage = MESA_SHADER_VERTEX;
        stage < MESA_SHADER_STAGES; stage++) {
      const VkPipelineShaderStageCreateInfo *stage_info = stage_infos[stage];
      if (!stage_info && stage != MESA_SHADER_FRAGMENT)
         continue;

      struct tu_shader *shader =
         tu_shader_create(builder->device, stage, stage_info, builder->layout,
                          builder->alloc);
      if (!shader)
         return VK_ERROR_OUT_OF_HOST_MEMORY;

      builder->shaders[stage] = shader;
   }

   for (gl_shader_stage stage = MESA_SHADER_STAGES - 1;
        stage > MESA_SHADER_NONE; stage--) {
      if (!builder->shaders[stage])
         continue;
      
      bool created;
      builder->variants[stage] =
         ir3_shader_get_variant(builder->shaders[stage]->ir3_shader,
                                &key, false, &created);
      if (!builder->variants[stage])
         return VK_ERROR_OUT_OF_HOST_MEMORY;

      builder->shader_offsets[stage] = builder->shader_total_size;
      builder->shader_total_size +=
         sizeof(uint32_t) * builder->variants[stage]->info.sizedwords;
   }

   const struct tu_shader *vs = builder->shaders[MESA_SHADER_VERTEX];
   struct ir3_shader_variant *variant;

   if (vs->ir3_shader->stream_output.num_outputs) {
      variant = builder->variants[MESA_SHADER_VERTEX];
   } else {
      bool created;
      variant = ir3_shader_get_variant(vs->ir3_shader, &key,
                                       true, &created);
      if (!variant)
         return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   builder->binning_vs_offset = builder->shader_total_size;
   builder->shader_total_size +=
      sizeof(uint32_t) * variant->info.sizedwords;
   builder->binning_variant = variant;

   return VK_SUCCESS;
}

static VkResult
tu_pipeline_builder_upload_shaders(struct tu_pipeline_builder *builder,
                                   struct tu_pipeline *pipeline)
{
   struct tu_bo *bo = &pipeline->program.binary_bo;

   VkResult result =
      tu_bo_init_new(builder->device, bo, builder->shader_total_size);
   if (result != VK_SUCCESS)
      return result;

   result = tu_bo_map(builder->device, bo);
   if (result != VK_SUCCESS)
      return result;

   for (uint32_t i = 0; i < MESA_SHADER_STAGES; i++) {
      const struct ir3_shader_variant *variant = builder->variants[i];
      if (!variant)
         continue;

      memcpy(bo->map + builder->shader_offsets[i], variant->bin,
             sizeof(uint32_t) * variant->info.sizedwords);
   }

   if (builder->binning_variant) {
      const struct ir3_shader_variant *variant = builder->binning_variant;
      memcpy(bo->map + builder->binning_vs_offset, variant->bin,
             sizeof(uint32_t) * variant->info.sizedwords);
   }

   return VK_SUCCESS;
}

static void
tu_pipeline_builder_parse_dynamic(struct tu_pipeline_builder *builder,
                                  struct tu_pipeline *pipeline)
{
   const VkPipelineDynamicStateCreateInfo *dynamic_info =
      builder->create_info->pDynamicState;

   if (!dynamic_info)
      return;

   for (uint32_t i = 0; i < dynamic_info->dynamicStateCount; i++) {
      VkDynamicState state = dynamic_info->pDynamicStates[i];
      switch (state) {
      case VK_DYNAMIC_STATE_VIEWPORT ... VK_DYNAMIC_STATE_STENCIL_REFERENCE:
         pipeline->dynamic_state_mask |= BIT(state);
         break;
      case VK_DYNAMIC_STATE_SAMPLE_LOCATIONS_EXT:
         pipeline->dynamic_state_mask |= BIT(TU_DYNAMIC_STATE_SAMPLE_LOCATIONS);
         break;
      default:
         assert(!"unsupported dynamic state");
         break;
      }
   }
}

static void
tu_pipeline_set_linkage(struct tu_program_descriptor_linkage *link,
                        struct tu_shader *shader,
                        struct ir3_shader_variant *v)
{
   link->const_state = *ir3_const_state(v);
   link->constlen = v->constlen;
   link->push_consts = shader->push_consts;
}

static void
tu_pipeline_builder_parse_shader_stages(struct tu_pipeline_builder *builder,
                                        struct tu_pipeline *pipeline)
{
   struct tu_cs prog_cs;
   tu_cs_begin_sub_stream(&pipeline->cs, 512, &prog_cs);
   tu6_emit_program(&prog_cs, builder, &pipeline->program.binary_bo, false, &pipeline->streamout);
   pipeline->program.state_ib = tu_cs_end_sub_stream(&pipeline->cs, &prog_cs);

   tu_cs_begin_sub_stream(&pipeline->cs, 512, &prog_cs);
   tu6_emit_program(&prog_cs, builder, &pipeline->program.binary_bo, true, &pipeline->streamout);
   pipeline->program.binning_state_ib = tu_cs_end_sub_stream(&pipeline->cs, &prog_cs);

   VkShaderStageFlags stages = 0;
   for (unsigned i = 0; i < builder->create_info->stageCount; i++) {
      stages |= builder->create_info->pStages[i].stage;
   }
   pipeline->active_stages = stages;

   uint32_t desc_sets = 0;
   for (unsigned i = 0; i < MESA_SHADER_STAGES; i++) {
      if (!builder->shaders[i])
         continue;

      tu_pipeline_set_linkage(&pipeline->program.link[i],
                              builder->shaders[i],
                              builder->variants[i]);
      desc_sets |= builder->shaders[i]->active_desc_sets;
   }
   pipeline->active_desc_sets = desc_sets;
}

static void
tu_pipeline_builder_parse_vertex_input(struct tu_pipeline_builder *builder,
                                       struct tu_pipeline *pipeline)
{
   const VkPipelineVertexInputStateCreateInfo *vi_info =
      builder->create_info->pVertexInputState;
   const struct ir3_shader_variant *vs = builder->variants[MESA_SHADER_VERTEX];
   const struct ir3_shader_variant *bs = builder->binning_variant;

   struct tu_cs vi_cs;
   tu_cs_begin_sub_stream(&pipeline->cs,
                          MAX_VERTEX_ATTRIBS * 7 + 2, &vi_cs);
   tu6_emit_vertex_input(&vi_cs, vs, vi_info,
                         &pipeline->vi.bindings_used);
   pipeline->vi.state_ib = tu_cs_end_sub_stream(&pipeline->cs, &vi_cs);

   if (bs) {
      tu_cs_begin_sub_stream(&pipeline->cs,
                             MAX_VERTEX_ATTRIBS * 7 + 2, &vi_cs);
      tu6_emit_vertex_input(
         &vi_cs, bs, vi_info, &pipeline->vi.bindings_used);
      pipeline->vi.binning_state_ib =
         tu_cs_end_sub_stream(&pipeline->cs, &vi_cs);
   }
}

static void
tu_pipeline_builder_parse_input_assembly(struct tu_pipeline_builder *builder,
                                         struct tu_pipeline *pipeline)
{
   const VkPipelineInputAssemblyStateCreateInfo *ia_info =
      builder->create_info->pInputAssemblyState;

   pipeline->ia.primtype = tu6_primtype(ia_info->topology);
   pipeline->ia.primitive_restart = ia_info->primitiveRestartEnable;
}

static bool
tu_pipeline_static_state(struct tu_pipeline *pipeline, struct tu_cs *cs,
                         uint32_t id, uint32_t size)
{
   struct ts_cs_memory memory;

   if (pipeline->dynamic_state_mask & BIT(id))
      return false;

   /* TODO: share this logc with tu_cmd_dynamic_state */
   tu_cs_alloc(&pipeline->cs, size, 1, &memory);
   tu_cs_init_external(cs, memory.map, memory.map + size);
   tu_cs_begin(cs);
   tu_cs_reserve_space(cs, size);

   assert(id < ARRAY_SIZE(pipeline->dynamic_state));
   pipeline->dynamic_state[id].iova = memory.iova;
   pipeline->dynamic_state[id].size = size;
   return true;
}

static void
tu_pipeline_builder_parse_viewport(struct tu_pipeline_builder *builder,
                                   struct tu_pipeline *pipeline)
{
   /* The spec says:
    *
    *    pViewportState is a pointer to an instance of the
    *    VkPipelineViewportStateCreateInfo structure, and is ignored if the
    *    pipeline has rasterization disabled."
    *
    * We leave the relevant registers stale in that case.
    */
   if (builder->rasterizer_discard)
      return;

   const VkPipelineViewportStateCreateInfo *vp_info =
      builder->create_info->pViewportState;

   struct tu_cs cs;

   if (tu_pipeline_static_state(pipeline, &cs, VK_DYNAMIC_STATE_VIEWPORT, 18))
      tu6_emit_viewport(&cs, vp_info->pViewports);

   if (tu_pipeline_static_state(pipeline, &cs, VK_DYNAMIC_STATE_SCISSOR, 3))
      tu6_emit_scissor(&cs, vp_info->pScissors);
}

static void
tu_pipeline_builder_parse_rasterization(struct tu_pipeline_builder *builder,
                                        struct tu_pipeline *pipeline)
{
   const VkPipelineRasterizationStateCreateInfo *rast_info =
      builder->create_info->pRasterizationState;

   assert(rast_info->polygonMode == VK_POLYGON_MODE_FILL);

   struct tu_cs cs;
   tu_cs_begin_sub_stream(&pipeline->cs, 7, &cs);

   tu_cs_emit_regs(&cs,
                   A6XX_GRAS_CL_CNTL(
                     .znear_clip_disable = rast_info->depthClampEnable,
                     .zfar_clip_disable = rast_info->depthClampEnable,
                     .unk5 = rast_info->depthClampEnable,
                     .zero_gb_scale_z = 1,
                     .vp_clip_code_ignore = 1));
   /* move to hw ctx init? */
   tu_cs_emit_regs(&cs, A6XX_GRAS_UNKNOWN_8001());
   tu_cs_emit_regs(&cs,
                   A6XX_GRAS_SU_POINT_MINMAX(.min = 1.0f / 16.0f, .max = 4092.0f),
                   A6XX_GRAS_SU_POINT_SIZE(1.0f));

   pipeline->rast.state_ib = tu_cs_end_sub_stream(&pipeline->cs, &cs);

   pipeline->gras_su_cntl =
      tu6_gras_su_cntl(rast_info, builder->samples);

   if (tu_pipeline_static_state(pipeline, &cs, VK_DYNAMIC_STATE_LINE_WIDTH, 2)) {
      pipeline->gras_su_cntl |=
         A6XX_GRAS_SU_CNTL_LINEHALFWIDTH(rast_info->lineWidth / 2.0f);
      tu_cs_emit_regs(&cs, A6XX_GRAS_SU_CNTL(.dword = pipeline->gras_su_cntl));
   }

   if (tu_pipeline_static_state(pipeline, &cs, VK_DYNAMIC_STATE_DEPTH_BIAS, 4)) {
      tu6_emit_depth_bias(&cs, rast_info->depthBiasConstantFactor,
                          rast_info->depthBiasClamp,
                          rast_info->depthBiasSlopeFactor);
   }

}

static void
tu_pipeline_builder_parse_depth_stencil(struct tu_pipeline_builder *builder,
                                        struct tu_pipeline *pipeline)
{
   /* The spec says:
    *
    *    pDepthStencilState is a pointer to an instance of the
    *    VkPipelineDepthStencilStateCreateInfo structure, and is ignored if
    *    the pipeline has rasterization disabled or if the subpass of the
    *    render pass the pipeline is created against does not use a
    *    depth/stencil attachment.
    *
    * Disable both depth and stencil tests if there is no ds attachment,
    * Disable depth test if ds attachment is S8_UINT, since S8_UINT defines
    * only the separate stencil attachment
    */
   static const VkPipelineDepthStencilStateCreateInfo dummy_ds_info;
   const VkPipelineDepthStencilStateCreateInfo *ds_info =
      builder->depth_attachment_format != VK_FORMAT_UNDEFINED
         ? builder->create_info->pDepthStencilState
         : &dummy_ds_info;
   const VkPipelineDepthStencilStateCreateInfo *ds_info_depth =
      builder->depth_attachment_format != VK_FORMAT_S8_UINT
         ? ds_info : &dummy_ds_info;

   struct tu_cs cs;
   tu_cs_begin_sub_stream(&pipeline->cs, 6, &cs);

   /* move to hw ctx init? */
   tu_cs_emit_regs(&cs, A6XX_RB_ALPHA_CONTROL());
   tu6_emit_depth_control(&cs, ds_info_depth,
                          builder->create_info->pRasterizationState);
   tu6_emit_stencil_control(&cs, ds_info);

   pipeline->ds.state_ib = tu_cs_end_sub_stream(&pipeline->cs, &cs);

   if (tu_pipeline_static_state(pipeline, &cs, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, 2)) {
      tu_cs_emit_regs(&cs, A6XX_RB_STENCILMASK(.mask = ds_info->front.compareMask & 0xff,
                                               .bfmask = ds_info->back.compareMask & 0xff));
   }

   if (tu_pipeline_static_state(pipeline, &cs, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, 2)) {
      tu_cs_emit_regs(&cs, A6XX_RB_STENCILWRMASK(.wrmask = ds_info->front.writeMask & 0xff,
                                                 .bfwrmask = ds_info->back.writeMask & 0xff));
   }

   if (tu_pipeline_static_state(pipeline, &cs, VK_DYNAMIC_STATE_STENCIL_REFERENCE, 2)) {
      tu_cs_emit_regs(&cs, A6XX_RB_STENCILREF(.ref = ds_info->front.reference & 0xff,
                                              .bfref = ds_info->back.reference & 0xff));
   }
}

static void
tu_pipeline_builder_parse_multisample_and_color_blend(
   struct tu_pipeline_builder *builder, struct tu_pipeline *pipeline)
{
   /* The spec says:
    *
    *    pMultisampleState is a pointer to an instance of the
    *    VkPipelineMultisampleStateCreateInfo, and is ignored if the pipeline
    *    has rasterization disabled.
    *
    * Also,
    *
    *    pColorBlendState is a pointer to an instance of the
    *    VkPipelineColorBlendStateCreateInfo structure, and is ignored if the
    *    pipeline has rasterization disabled or if the subpass of the render
    *    pass the pipeline is created against does not use any color
    *    attachments.
    *
    * We leave the relevant registers stale when rasterization is disabled.
    */
   if (builder->rasterizer_discard)
      return;

   static const VkPipelineColorBlendStateCreateInfo dummy_blend_info;
   const VkPipelineMultisampleStateCreateInfo *msaa_info =
      builder->create_info->pMultisampleState;
   const VkPipelineColorBlendStateCreateInfo *blend_info =
      builder->use_color_attachments ? builder->create_info->pColorBlendState
                                     : &dummy_blend_info;

   struct tu_cs cs;
   tu_cs_begin_sub_stream(&pipeline->cs, MAX_RTS * 3 + 4, &cs);

   uint32_t blend_enable_mask;
   tu6_emit_rb_mrt_controls(&cs, blend_info,
                            builder->color_attachment_formats,
                            &blend_enable_mask);

   tu6_emit_blend_control(&cs, blend_enable_mask,
                          builder->use_dual_src_blend, msaa_info);

   pipeline->blend.state_ib = tu_cs_end_sub_stream(&pipeline->cs, &cs);

   if (tu_pipeline_static_state(pipeline, &cs, VK_DYNAMIC_STATE_BLEND_CONSTANTS, 5)) {
      tu_cs_emit_pkt4(&cs, REG_A6XX_RB_BLEND_RED_F32, 4);
      tu_cs_emit_array(&cs, (const uint32_t *) blend_info->blendConstants, 4);
   }

   const struct VkPipelineSampleLocationsStateCreateInfoEXT *sample_locations =
      vk_find_struct_const(msaa_info->pNext, PIPELINE_SAMPLE_LOCATIONS_STATE_CREATE_INFO_EXT);
   const VkSampleLocationsInfoEXT *samp_loc = NULL;

   if (sample_locations && sample_locations->sampleLocationsEnable)
      samp_loc = &sample_locations->sampleLocationsInfo;

    if (tu_pipeline_static_state(pipeline, &cs, TU_DYNAMIC_STATE_SAMPLE_LOCATIONS,
                                 samp_loc ? 9 : 6)) {
      tu6_emit_sample_locations(&cs, samp_loc);
    }
}

static void
tu_pipeline_finish(struct tu_pipeline *pipeline,
                   struct tu_device *dev,
                   const VkAllocationCallbacks *alloc)
{
   tu_cs_finish(&pipeline->cs);

   if (pipeline->program.binary_bo.gem_handle)
      tu_bo_finish(dev, &pipeline->program.binary_bo);
}

static VkResult
tu_pipeline_builder_build(struct tu_pipeline_builder *builder,
                          struct tu_pipeline **pipeline)
{
   VkResult result = tu_pipeline_create(builder->device, builder->layout,
                                        false, builder->alloc, pipeline);
   if (result != VK_SUCCESS)
      return result;

   (*pipeline)->layout = builder->layout;

   /* compile and upload shaders */
   result = tu_pipeline_builder_compile_shaders(builder);
   if (result == VK_SUCCESS)
      result = tu_pipeline_builder_upload_shaders(builder, *pipeline);
   if (result != VK_SUCCESS) {
      tu_pipeline_finish(*pipeline, builder->device, builder->alloc);
      vk_free2(&builder->device->alloc, builder->alloc, *pipeline);
      *pipeline = VK_NULL_HANDLE;

      return result;
   }

   tu_pipeline_builder_parse_dynamic(builder, *pipeline);
   tu_pipeline_builder_parse_shader_stages(builder, *pipeline);
   tu_pipeline_builder_parse_vertex_input(builder, *pipeline);
   tu_pipeline_builder_parse_input_assembly(builder, *pipeline);
   tu_pipeline_builder_parse_viewport(builder, *pipeline);
   tu_pipeline_builder_parse_rasterization(builder, *pipeline);
   tu_pipeline_builder_parse_depth_stencil(builder, *pipeline);
   tu_pipeline_builder_parse_multisample_and_color_blend(builder, *pipeline);
   tu6_emit_load_state(*pipeline, false);

   /* we should have reserved enough space upfront such that the CS never
    * grows
    */
   assert((*pipeline)->cs.bo_count == 1);

   return VK_SUCCESS;
}

static void
tu_pipeline_builder_finish(struct tu_pipeline_builder *builder)
{
   for (uint32_t i = 0; i < MESA_SHADER_STAGES; i++) {
      if (!builder->shaders[i])
         continue;
      tu_shader_destroy(builder->device, builder->shaders[i], builder->alloc);
   }
}

static void
tu_pipeline_builder_init_graphics(
   struct tu_pipeline_builder *builder,
   struct tu_device *dev,
   struct tu_pipeline_cache *cache,
   const VkGraphicsPipelineCreateInfo *create_info,
   const VkAllocationCallbacks *alloc)
{
   TU_FROM_HANDLE(tu_pipeline_layout, layout, create_info->layout);

   *builder = (struct tu_pipeline_builder) {
      .device = dev,
      .cache = cache,
      .create_info = create_info,
      .alloc = alloc,
      .layout = layout,
   };

   builder->rasterizer_discard =
      create_info->pRasterizationState->rasterizerDiscardEnable;

   if (builder->rasterizer_discard) {
      builder->samples = VK_SAMPLE_COUNT_1_BIT;
   } else {
      builder->samples = create_info->pMultisampleState->rasterizationSamples;

      const struct tu_render_pass *pass =
         tu_render_pass_from_handle(create_info->renderPass);
      const struct tu_subpass *subpass =
         &pass->subpasses[create_info->subpass];

      const uint32_t a = subpass->depth_stencil_attachment.attachment;
      builder->depth_attachment_format = (a != VK_ATTACHMENT_UNUSED) ?
         pass->attachments[a].format : VK_FORMAT_UNDEFINED;

      assert(subpass->color_count == 0 ||
             !create_info->pColorBlendState ||
             subpass->color_count == create_info->pColorBlendState->attachmentCount);
      builder->color_attachment_count = subpass->color_count;
      for (uint32_t i = 0; i < subpass->color_count; i++) {
         const uint32_t a = subpass->color_attachments[i].attachment;
         if (a == VK_ATTACHMENT_UNUSED)
            continue;

         builder->color_attachment_formats[i] = pass->attachments[a].format;
         builder->use_color_attachments = true;
         builder->render_components |= 0xf << (i * 4);
      }

      if (tu_blend_state_is_dual_src(create_info->pColorBlendState)) {
         builder->color_attachment_count++;
         builder->use_dual_src_blend = true;
         /* dual source blending has an extra fs output in the 2nd slot */
         if (subpass->color_attachments[0].attachment != VK_ATTACHMENT_UNUSED)
            builder->render_components |= 0xf << 4;
      }
   }
}

static VkResult
tu_graphics_pipeline_create(VkDevice device,
                            VkPipelineCache pipelineCache,
                            const VkGraphicsPipelineCreateInfo *pCreateInfo,
                            const VkAllocationCallbacks *pAllocator,
                            VkPipeline *pPipeline)
{
   TU_FROM_HANDLE(tu_device, dev, device);
   TU_FROM_HANDLE(tu_pipeline_cache, cache, pipelineCache);

   struct tu_pipeline_builder builder;
   tu_pipeline_builder_init_graphics(&builder, dev, cache,
                                     pCreateInfo, pAllocator);

   struct tu_pipeline *pipeline = NULL;
   VkResult result = tu_pipeline_builder_build(&builder, &pipeline);
   tu_pipeline_builder_finish(&builder);

   if (result == VK_SUCCESS)
      *pPipeline = tu_pipeline_to_handle(pipeline);
   else
      *pPipeline = VK_NULL_HANDLE;

   return result;
}

VkResult
tu_CreateGraphicsPipelines(VkDevice device,
                           VkPipelineCache pipelineCache,
                           uint32_t count,
                           const VkGraphicsPipelineCreateInfo *pCreateInfos,
                           const VkAllocationCallbacks *pAllocator,
                           VkPipeline *pPipelines)
{
   VkResult final_result = VK_SUCCESS;

   for (uint32_t i = 0; i < count; i++) {
      VkResult result = tu_graphics_pipeline_create(device, pipelineCache,
                                                    &pCreateInfos[i], pAllocator,
                                                    &pPipelines[i]);

      if (result != VK_SUCCESS)
         final_result = result;
   }

   return final_result;
}

static VkResult
tu_compute_upload_shader(VkDevice device,
                         struct tu_pipeline *pipeline,
                         struct ir3_shader_variant *v)
{
   TU_FROM_HANDLE(tu_device, dev, device);
   struct tu_bo *bo = &pipeline->program.binary_bo;

   uint32_t shader_size = sizeof(uint32_t) * v->info.sizedwords;
   VkResult result =
      tu_bo_init_new(dev, bo, shader_size);
   if (result != VK_SUCCESS)
      return result;

   result = tu_bo_map(dev, bo);
   if (result != VK_SUCCESS)
      return result;

   memcpy(bo->map, v->bin, shader_size);

   return VK_SUCCESS;
}


static VkResult
tu_compute_pipeline_create(VkDevice device,
                           VkPipelineCache _cache,
                           const VkComputePipelineCreateInfo *pCreateInfo,
                           const VkAllocationCallbacks *pAllocator,
                           VkPipeline *pPipeline)
{
   TU_FROM_HANDLE(tu_device, dev, device);
   TU_FROM_HANDLE(tu_pipeline_layout, layout, pCreateInfo->layout);
   const VkPipelineShaderStageCreateInfo *stage_info = &pCreateInfo->stage;
   VkResult result;

   struct tu_pipeline *pipeline;

   *pPipeline = VK_NULL_HANDLE;

   result = tu_pipeline_create(dev, layout, true, pAllocator, &pipeline);
   if (result != VK_SUCCESS)
      return result;

   pipeline->layout = layout;

   struct ir3_shader_key key = {};

   struct tu_shader *shader =
      tu_shader_create(dev, MESA_SHADER_COMPUTE, stage_info, layout, pAllocator);
   if (!shader) {
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
      goto fail;
   }

   bool created;
   struct ir3_shader_variant *v =
      ir3_shader_get_variant(shader->ir3_shader, &key, false, &created);
   if (!v)
      goto fail;

   tu_pipeline_set_linkage(&pipeline->program.link[MESA_SHADER_COMPUTE],
                           shader, v);

   result = tu_compute_upload_shader(device, pipeline, v);
   if (result != VK_SUCCESS)
      goto fail;

   for (int i = 0; i < 3; i++)
      pipeline->compute.local_size[i] = v->shader->nir->info.cs.local_size[i];

   struct tu_cs prog_cs;
   tu_cs_begin_sub_stream(&pipeline->cs, 512, &prog_cs);
   tu6_emit_cs_config(&prog_cs, shader, v, pipeline->program.binary_bo.iova);
   pipeline->program.state_ib = tu_cs_end_sub_stream(&pipeline->cs, &prog_cs);

   tu6_emit_load_state(pipeline, true);

   *pPipeline = tu_pipeline_to_handle(pipeline);
   return VK_SUCCESS;

fail:
   if (shader)
      tu_shader_destroy(dev, shader, pAllocator);

   tu_pipeline_finish(pipeline, dev, pAllocator);
   vk_free2(&dev->alloc, pAllocator, pipeline);

   return result;
}

VkResult
tu_CreateComputePipelines(VkDevice device,
                          VkPipelineCache pipelineCache,
                          uint32_t count,
                          const VkComputePipelineCreateInfo *pCreateInfos,
                          const VkAllocationCallbacks *pAllocator,
                          VkPipeline *pPipelines)
{
   VkResult final_result = VK_SUCCESS;

   for (uint32_t i = 0; i < count; i++) {
      VkResult result = tu_compute_pipeline_create(device, pipelineCache,
                                                   &pCreateInfos[i],
                                                   pAllocator, &pPipelines[i]);
      if (result != VK_SUCCESS)
         final_result = result;
   }

   return final_result;
}

void
tu_DestroyPipeline(VkDevice _device,
                   VkPipeline _pipeline,
                   const VkAllocationCallbacks *pAllocator)
{
   TU_FROM_HANDLE(tu_device, dev, _device);
   TU_FROM_HANDLE(tu_pipeline, pipeline, _pipeline);

   if (!_pipeline)
      return;

   tu_pipeline_finish(pipeline, dev, pAllocator);
   vk_free2(&dev->alloc, pAllocator, pipeline);
}
