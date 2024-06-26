/*
 * Copyright 2024 Valve Corporation
 * Copyright 2024 Alyssa Rosenzweig
 * Copyright 2022-2023 Collabora Ltd. and Red Hat Inc.
 * SPDX-License-Identifier: MIT
 */
#include "hk_shader.h"

#include "agx_helpers.h"
#include "nir.h"
#include "nir_builder.h"

#include "agx_bo.h"
#include "hk_cmd_buffer.h"
#include "hk_descriptor_set_layout.h"
#include "hk_device.h"
#include "hk_physical_device.h"
#include "hk_sampler.h"
#include "hk_shader.h"

#include "nir_builder_opcodes.h"
#include "nir_builtin_builder.h"
#include "nir_intrinsics.h"
#include "nir_intrinsics_indices.h"
#include "shader_enums.h"
#include "vk_nir_convert_ycbcr.h"
#include "vk_pipeline.h"
#include "vk_pipeline_layout.h"
#include "vk_shader_module.h"
#include "vk_ycbcr_conversion.h"

#include "asahi/compiler/agx_compile.h"
#include "asahi/lib/agx_linker.h"
#include "asahi/lib/agx_nir_passes.h"
#include "asahi/lib/agx_tilebuffer.h"
#include "asahi/lib/agx_uvs.h"
#include "compiler/spirv/nir_spirv.h"

#include "util/hash_table.h"
#include "util/macros.h"
#include "util/mesa-sha1.h"
#include "util/simple_mtx.h"
#include "util/u_debug.h"
#include "vulkan/vulkan_core.h"

struct hk_fs_key {
   bool zs_self_dep;

   /** True if sample shading is forced on via an API knob such as
    * VkPipelineMultisampleStateCreateInfo::minSampleShading
    */
   bool force_sample_shading;

   uint8_t pad[2];
};
static_assert(sizeof(struct hk_fs_key) == 4, "packed");

static void
shared_var_info(const struct glsl_type *type, unsigned *size, unsigned *align)
{
   assert(glsl_type_is_vector_or_scalar(type));

   uint32_t comp_size =
      glsl_type_is_boolean(type) ? 4 : glsl_get_bit_size(type) / 8;
   unsigned length = glsl_get_vector_elements(type);
   *size = comp_size * length, *align = comp_size;
}

uint64_t
hk_physical_device_compiler_flags(const struct hk_physical_device *pdev)
{
   return 0;
}

const nir_shader_compiler_options *
hk_get_nir_options(struct vk_physical_device *vk_pdev, gl_shader_stage stage,
                   UNUSED const struct vk_pipeline_robustness_state *rs)
{
   return &agx_nir_options;
}

static struct spirv_to_nir_options
hk_get_spirv_options(struct vk_physical_device *vk_pdev,
                     UNUSED gl_shader_stage stage,
                     const struct vk_pipeline_robustness_state *rs)
{
   return (struct spirv_to_nir_options){
      .caps =
         {
            .demote_to_helper_invocation = true,
            .descriptor_array_dynamic_indexing = true,
            .descriptor_array_non_uniform_indexing = true,
            .descriptor_indexing = true,
            .device_group = true,
            .draw_parameters = true,
            .float_controls = true,
            .float64 = true,
            .fragment_barycentric = true,
            .geometry_streams = true,
            .image_atomic_int64 = true,
            .image_ms_array = true,
            .image_read_without_format = true,
            .image_write_without_format = true,
            .int8 = true,
            .int16 = true,
            .int64 = true,
            .int64_atomics = true,
            .float16 = true,
            .min_lod = true,
            .multiview = true,
            .physical_storage_buffer_address = true,
            .runtime_descriptor_array = true,
            .shader_clock = true,
            .shader_sm_builtins_nv = true,
            .shader_viewport_index_layer = true,
            .sparse_residency = true,
            .stencil_export = true,
            .storage_8bit = true,
            .storage_16bit = true,
            .storage_image_ms = true,
            .subgroup_arithmetic = true,
            .subgroup_ballot = true,
            .subgroup_basic = true,
            .subgroup_quad = true,
            .subgroup_rotate = true,
            .subgroup_shuffle = true,
            .subgroup_uniform_control_flow = true,
            .subgroup_vote = true,
            .tessellation = true,
            .transform_feedback = true,
            .variable_pointers = true,
            .vk_memory_model_device_scope = true,
            .vk_memory_model = true,
            .workgroup_memory_explicit_layout = true,
         },
      .ssbo_addr_format = hk_buffer_addr_format(rs->storage_buffers),
      .phys_ssbo_addr_format = nir_address_format_64bit_global,
      .ubo_addr_format = hk_buffer_addr_format(rs->uniform_buffers),
      .shared_addr_format = nir_address_format_32bit_offset,
      .min_ssbo_alignment = HK_MIN_SSBO_ALIGNMENT,
      .min_ubo_alignment = HK_MIN_UBO_ALIGNMENT,
   };
}

static void
hk_preprocess_nir(struct vk_physical_device *vk_pdev, nir_shader *nir)
{
   /* Must lower before io to temps */
   if (nir->info.stage == MESA_SHADER_FRAGMENT) {
      NIR_PASS(_, nir, nir_lower_terminate_to_demote);
      NIR_PASS(_, nir, nir_lower_returns);
   }

   /* Unroll loops before lowering indirects via nir_lower_io_to_temporaries */
   UNUSED bool progress = false;
   NIR_PASS(_, nir, nir_lower_global_vars_to_local);

   do {
      progress = false;
      NIR_PASS(progress, nir, nir_lower_vars_to_ssa);
      NIR_PASS(progress, nir, nir_copy_prop);
      NIR_PASS(progress, nir, nir_opt_dce);
      NIR_PASS(progress, nir, nir_opt_constant_folding);
      NIR_PASS(progress, nir, nir_opt_loop);
      NIR_PASS(progress, nir, nir_opt_loop_unroll);
   } while (progress);

   if (nir->info.stage == MESA_SHADER_FRAGMENT) {
      struct nir_lower_sysvals_to_varyings_options sysvals_opts = {
         .point_coord = true,
      };

      nir_lower_sysvals_to_varyings(nir, &sysvals_opts);
   }

   NIR_PASS(_, nir, nir_lower_system_values);

   /* Gather info before preprocess_nir but after some general lowering, so
    * inputs_read and system_values_read are accurately set.
    */
   nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));

   NIR_PASS_V(nir, nir_lower_io_to_temporaries, nir_shader_get_entrypoint(nir),
              true, false);

   NIR_PASS(_, nir, nir_lower_global_vars_to_local);

   NIR_PASS(_, nir, nir_split_var_copies);
   NIR_PASS(_, nir, nir_split_struct_vars, nir_var_function_temp);

   /* Optimize but allow copies because we haven't lowered them yet */
   agx_preprocess_nir(nir, NULL);

   NIR_PASS(_, nir, nir_lower_load_const_to_scalar);
   NIR_PASS(_, nir, nir_lower_var_copies);
   NIR_PASS(_, nir, nir_lower_compute_system_values, NULL);
}

static void
hk_populate_fs_key(struct hk_fs_key *key,
                   const struct vk_graphics_pipeline_state *state)
{
   memset(key, 0, sizeof(*key));

   if (state == NULL)
      return;

   if (state->pipeline_flags &
       VK_PIPELINE_CREATE_2_DEPTH_STENCIL_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT)
      key->zs_self_dep = true;

   /* We force per-sample interpolation whenever sampleShadingEnable is set
    * regardless of minSampleShading or rasterizationSamples.
    *
    * When sampleShadingEnable is set, few guarantees are made about the
    * location of interpolation of the inputs.  The only real guarantees are
    * that the inputs are interpolated within the pixel and that you get at
    * least `rasterizationSamples * minSampleShading` unique positions.
    * Importantly, it does not require that when `rasterizationSamples *
    * minSampleShading <= 1.0` that those positions are at the fragment
    * center.  Therefore, it's valid to just always do per-sample all the time.
    *
    * The one caveat here is that we have to be careful about gl_SampleMaskIn.
    * When `hk_fs_key::force_sample_shading = true` we also turn any reads of
    * gl_SampleMaskIn into `1 << gl_SampleID` because the hardware sample mask
    * is actually per-fragment, not per-pass.  We handle this by smashing
    * minSampleShading to 1.0 whenever gl_SampleMaskIn is read.
    */
   const struct vk_multisample_state *ms = state->ms;
   if (ms != NULL && ms->sample_shading_enable)
      key->force_sample_shading = true;
}

static void
hk_hash_graphics_state(struct vk_physical_device *device,
                       const struct vk_graphics_pipeline_state *state,
                       VkShaderStageFlags stages, blake3_hash blake3_out)
{
   struct mesa_blake3 blake3_ctx;
   _mesa_blake3_init(&blake3_ctx);
   if (stages & VK_SHADER_STAGE_FRAGMENT_BIT) {
      struct hk_fs_key key;
      hk_populate_fs_key(&key, state);
      _mesa_blake3_update(&blake3_ctx, &key, sizeof(key));

      const bool is_multiview = state->rp->view_mask != 0;
      _mesa_blake3_update(&blake3_ctx, &is_multiview, sizeof(is_multiview));
   }
   _mesa_blake3_final(&blake3_ctx, blake3_out);
}

static bool
lower_load_global_constant_offset_instr(nir_builder *b,
                                        nir_intrinsic_instr *intrin,
                                        UNUSED void *_data)
{
   if (intrin->intrinsic != nir_intrinsic_load_global_constant_offset &&
       intrin->intrinsic != nir_intrinsic_load_global_constant_bounded)
      return false;

   b->cursor = nir_before_instr(&intrin->instr);

   nir_def *base_addr = intrin->src[0].ssa;
   nir_def *offset = intrin->src[1].ssa;

   nir_def *zero = NULL;
   if (intrin->intrinsic == nir_intrinsic_load_global_constant_bounded) {
      nir_def *bound = intrin->src[2].ssa;

      unsigned bit_size = intrin->def.bit_size;
      assert(bit_size >= 8 && bit_size % 8 == 0);
      unsigned byte_size = bit_size / 8;

      zero = nir_imm_zero(b, intrin->num_components, bit_size);

      unsigned load_size = byte_size * intrin->num_components;

      nir_def *sat_offset =
         nir_umin(b, offset, nir_imm_int(b, UINT32_MAX - (load_size - 1)));
      nir_def *in_bounds =
         nir_ilt(b, nir_iadd_imm(b, sat_offset, load_size - 1), bound);

      nir_push_if(b, in_bounds);
   }

   nir_def *val = nir_build_load_global_constant(
      b, intrin->def.num_components, intrin->def.bit_size,
      nir_iadd(b, base_addr, nir_u2u64(b, offset)),
      .align_mul = nir_intrinsic_align_mul(intrin),
      .align_offset = nir_intrinsic_align_offset(intrin),
      .access = nir_intrinsic_access(intrin));

   if (intrin->intrinsic == nir_intrinsic_load_global_constant_bounded) {
      nir_pop_if(b, NULL);
      val = nir_if_phi(b, val, zero);
   }

   nir_def_rewrite_uses(&intrin->def, val);

   return true;
}

struct lower_ycbcr_state {
   uint32_t set_layout_count;
   struct vk_descriptor_set_layout *const *set_layouts;
};

static const struct vk_ycbcr_conversion_state *
lookup_ycbcr_conversion(const void *_state, uint32_t set, uint32_t binding,
                        uint32_t array_index)
{
   const struct lower_ycbcr_state *state = _state;
   assert(set < state->set_layout_count);
   assert(state->set_layouts[set] != NULL);
   const struct hk_descriptor_set_layout *set_layout =
      vk_to_hk_descriptor_set_layout(state->set_layouts[set]);
   assert(binding < set_layout->binding_count);

   const struct hk_descriptor_set_binding_layout *bind_layout =
      &set_layout->binding[binding];

   if (bind_layout->immutable_samplers == NULL)
      return NULL;

   array_index = MIN2(array_index, bind_layout->array_size - 1);

   const struct hk_sampler *sampler =
      bind_layout->immutable_samplers[array_index];

   return sampler && sampler->vk.ycbcr_conversion
             ? &sampler->vk.ycbcr_conversion->state
             : NULL;
}

static inline bool
nir_has_image_var(nir_shader *nir)
{
   nir_foreach_image_variable(_, nir)
      return true;

   return false;
}

static int
glsl_type_size(const struct glsl_type *type, bool bindless)
{
   return glsl_count_attribute_slots(type, false);
}

/*
 * This is the world's worst multiview implementation. We simply duplicate each
 * draw on the CPU side, changing a uniform in between, and then plumb the view
 * index into the layer ID here. Whatever, it works.
 *
 * The "proper" implementation on AGX would use vertex amplification, but a
 * MacBook is not a VR headset.
 */
static void
hk_lower_multiview(nir_shader *nir)
{
   /* If there's an existing layer ID write, ignore it. This avoids validation
    * splat with vk_meta.
    */
   nir_variable *existing = nir_find_variable_with_location(
      nir, nir_var_shader_out, VARYING_SLOT_LAYER);

   if (existing) {
      existing->data.mode = nir_var_shader_temp;
      existing->data.location = 0;
      nir_fixup_deref_modes(nir);
   }

   /* Now write the view index as the layer */
   nir_builder b =
      nir_builder_at(nir_after_impl(nir_shader_get_entrypoint(nir)));

   nir_variable *layer =
      nir_variable_create(nir, nir_var_shader_out, glsl_uint_type(), NULL);

   layer->data.location = VARYING_SLOT_LAYER;

   nir_store_var(&b, layer, nir_load_view_index(&b), nir_component_mask(1));
   b.shader->info.outputs_written |= VARYING_BIT_LAYER;
}

/*
 * KHR_maintenance5 requires that points rasterize with a default point size of
 * 1.0, while our hardware requires an explicit point size write for this.
 * Since topology may be dynamic, we insert an unconditional write if necessary.
 */
static bool
hk_nir_insert_psiz_write(nir_shader *nir)
{
   nir_function_impl *impl = nir_shader_get_entrypoint(nir);

   if (nir->info.outputs_written & VARYING_BIT_PSIZ) {
      nir_metadata_preserve(impl, nir_metadata_all);
      return false;
   }

   nir_builder b = nir_builder_at(nir_after_impl(impl));

   nir_store_output(&b, nir_imm_float(&b, 1.0), nir_imm_int(&b, 0),
                    .write_mask = nir_component_mask(1),
                    .io_semantics.location = VARYING_SLOT_PSIZ,
                    .io_semantics.num_slots = 1);

   nir->info.outputs_written |= VARYING_BIT_PSIZ;
   nir_metadata_preserve(b.impl,
                         nir_metadata_dominance | nir_metadata_block_index);
   return true;
}

static nir_def *
query_custom_border(nir_builder *b, nir_tex_instr *tex)
{
   nir_instr *instr = nir_get_texture_size_without_cursor(b, tex)->parent_instr;
   nir_tex_instr *query = nir_instr_as_tex(instr);

   query->op = nir_texop_custom_border_color_agx;
   query->dest_type = tex->dest_type;
   query->is_sparse = false;
   nir_def_init(instr, &query->def, 4, tex->def.bit_size);
   return &query->def;
}

static nir_def *
has_custom_border(nir_builder *b, nir_tex_instr *tex)
{
   nir_instr *instr = nir_get_texture_size(b, tex)->parent_instr;
   nir_tex_instr *query = nir_instr_as_tex(instr);

   query->op = nir_texop_has_custom_border_color_agx;
   query->dest_type = nir_type_bool1;
   nir_def_init(instr, &query->def, 1, 1);
   return &query->def;
}

static bool
lower(nir_builder *b, nir_instr *instr, UNUSED void *_data)
{
   if (instr->type != nir_instr_type_tex)
      return false;

   nir_tex_instr *tex = nir_instr_as_tex(instr);
   if (!nir_tex_instr_need_sampler(tex) || nir_tex_instr_is_query(tex))
      return false;

   /* XXX: this is a really weird edge case, is this even well-defined? */
   if (tex->is_shadow)
      return false;

   nir_def *has_custom = has_custom_border(b, tex);
   b->cursor = nir_after_instr(&tex->instr);

   nir_instr *orig = nir_instr_clone(b->shader, &tex->instr);
   nir_builder_instr_insert(b, orig);
   nir_def *clamp_to_1 = &nir_instr_as_tex(orig)->def;

   nir_push_if(b, has_custom);
   nir_def *replaced = NULL;
   {
      /* Sample again, this time with clamp-to-0 instead of clamp-to-1 */
      nir_instr *clone_instr = nir_instr_clone(b->shader, &tex->instr);
      nir_builder_instr_insert(b, clone_instr);

      nir_tex_instr *tex_0 = nir_instr_as_tex(clone_instr);
      nir_def *clamp_to_0 = &tex_0->def;

      tex_0->backend_flags |= AGX_TEXTURE_FLAG_CLAMP_TO_0;

      /* Grab the border colour */
      nir_def *border = query_custom_border(b, tex_0);

      if (tex->op == nir_texop_tg4) {
         border = nir_replicate(b, nir_channel(b, border, tex->component), 4);
      }

      /* Combine together with the border */
      if (nir_alu_type_get_base_type(tex->dest_type) == nir_type_float &&
          tex->op != nir_texop_tg4) {

         /* For floats, lerp together:
          *
          * For border texels:  (1 * border) + (0 * border      ) = border
          * For regular texels: (x * border) + (x * (1 - border)) = x.
          *
          * Linear filtering is linear (duh), so lerping is compatible.
          */
         replaced = nir_flrp(b, clamp_to_0, clamp_to_1, border);
      } else {
         /* For integers, just select componentwise since there is no linear
          * filtering. Gathers also use this path since they are unfiltered in
          * each component.
          */
         replaced = nir_bcsel(b, nir_ieq(b, clamp_to_0, clamp_to_1), clamp_to_0,
                              border);
      }
   }
   nir_pop_if(b, NULL);

   /* Put it together with a phi */
   nir_def *phi = nir_if_phi(b, replaced, clamp_to_1);
   nir_def_rewrite_uses(&tex->def, phi);
   nir_instr_remove(&tex->instr);
   return true;
}

static bool
agx_nir_lower_custom_border(nir_shader *nir)
{
   return nir_shader_instructions_pass(nir, lower, nir_metadata_none, NULL);
}

/*
 * In Vulkan, the VIEWPORT should read 0 in the fragment shader if it is not
 * written by the vertex shader, but in our implementation, the varying would
 * otherwise be undefined. This small pass predicates VIEWPORT reads based on
 * whether the hardware vertex shader writes the VIEWPORT (nonzero UVS index).
 */
static bool
lower_viewport_fs(nir_builder *b, nir_intrinsic_instr *intr, UNUSED void *data)
{
   if (intr->intrinsic != nir_intrinsic_load_input)
      return false;

   nir_io_semantics sem = nir_intrinsic_io_semantics(intr);
   if (sem.location != VARYING_SLOT_VIEWPORT)
      return false;

   b->cursor = nir_after_instr(&intr->instr);
   nir_def *orig = &intr->def;

   nir_def *uvs = nir_load_uvs_index_agx(b, .io_semantics = sem);
   nir_def *def = nir_bcsel(b, nir_ine_imm(b, uvs, 0), orig, nir_imm_int(b, 0));

   nir_def_rewrite_uses_after(orig, def, def->parent_instr);
   return true;
}
void
hk_lower_nir(struct hk_device *dev, nir_shader *nir,
             const struct vk_pipeline_robustness_state *rs, bool is_multiview,
             uint32_t set_layout_count,
             struct vk_descriptor_set_layout *const *set_layouts)
{
   if (nir->info.stage == MESA_SHADER_FRAGMENT) {
      NIR_PASS(_, nir, nir_lower_input_attachments,
               &(nir_input_attachment_options){
                  .use_fragcoord_sysval = true,
                  .use_layer_id_sysval = true,
                  .use_view_id_for_layer = is_multiview,
               });
   }

   /* XXX: should be last geometry stage, how do I get to that? */
   if (nir->info.stage == MESA_SHADER_VERTEX) {
      if (is_multiview)
         hk_lower_multiview(nir);
   }

   if (nir->info.stage == MESA_SHADER_TESS_EVAL) {
      NIR_PASS(_, nir, nir_lower_patch_vertices,
               nir->info.tess.tcs_vertices_out, NULL);
   }

   const struct lower_ycbcr_state ycbcr_state = {
      .set_layout_count = set_layout_count,
      .set_layouts = set_layouts,
   };
   NIR_PASS(_, nir, nir_vk_lower_ycbcr_tex, lookup_ycbcr_conversion,
            &ycbcr_state);

   nir_lower_compute_system_values_options csv_options = {
      .has_base_workgroup_id = true,
   };
   NIR_PASS(_, nir, nir_lower_compute_system_values, &csv_options);

   /* Lower push constants before lower_descriptors */
   NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_push_const,
            nir_address_format_32bit_offset);

   // NIR_PASS(_, nir, nir_opt_large_constants, NULL, 32);

   /* Images accessed through the texture or PBE hardware are robust, so we
    * don't set lower_image. (There are some sticky details around txf but
    * they're handled by agx_nir_lower_texture). However, buffer images and
    * image atomics are software so require robustness lowering.
    */
   nir_lower_robust_access_options robustness = {
      .lower_buffer_image = true,
      .lower_image_atomic = true,
   };

   NIR_PASS(_, nir, nir_lower_robust_access, &robustness);

   /* We must do early lowering before hk_nir_lower_descriptors, since this will
    * create lod_bias_agx instructions.
    */
   NIR_PASS(_, nir, agx_nir_lower_texture_early, true /* support_lod_bias */);
   NIR_PASS(_, nir, agx_nir_lower_custom_border);

   NIR_PASS(_, nir, hk_nir_lower_descriptors, rs, set_layout_count,
            set_layouts);
   NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_global,
            nir_address_format_64bit_global);
   NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_ssbo,
            hk_buffer_addr_format(rs->storage_buffers));
   NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_ubo,
            hk_buffer_addr_format(rs->uniform_buffers));
   NIR_PASS(_, nir, nir_shader_intrinsics_pass,
            lower_load_global_constant_offset_instr, nir_metadata_none, NULL);

   if (!nir->info.shared_memory_explicit_layout) {
      /* There may be garbage in shared_size, but it's the job of
       * nir_lower_vars_to_explicit_types to allocate it. We have to reset to
       * avoid overallocation.
       */
      nir->info.shared_size = 0;

      NIR_PASS(_, nir, nir_lower_vars_to_explicit_types, nir_var_mem_shared,
               shared_var_info);
   }
   NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_shared,
            nir_address_format_32bit_offset);

   if (nir->info.zero_initialize_shared_memory && nir->info.shared_size > 0) {
      /* Align everything up to 16B so we can write whole vec4s. */
      nir->info.shared_size = align(nir->info.shared_size, 16);
      NIR_PASS(_, nir, nir_zero_initialize_shared_memory, nir->info.shared_size,
               16);

      /* We need to call lower_compute_system_values again because
       * nir_zero_initialize_shared_memory generates load_invocation_id which
       * has to be lowered to load_invocation_index.
       */
      NIR_PASS(_, nir, nir_lower_compute_system_values, NULL);
   }

   /* TODO: we can do indirect VS output */
   nir_variable_mode lower_indirect_modes = 0;
   if (nir->info.stage == MESA_SHADER_FRAGMENT)
      lower_indirect_modes |= nir_var_shader_out;
   else if (nir->info.stage == MESA_SHADER_VERTEX)
      lower_indirect_modes |= nir_var_shader_in | nir_var_shader_out;

   NIR_PASS(_, nir, nir_lower_indirect_derefs, lower_indirect_modes,
            UINT32_MAX);

   NIR_PASS(_, nir, nir_lower_io, nir_var_shader_in | nir_var_shader_out,
            glsl_type_size, nir_lower_io_lower_64bit_to_32);

   /* XXX: should be last ggeometry stage, how do I get to that? */
   if (nir->info.stage == MESA_SHADER_VERTEX) {
      /* Point size must be clamped, excessively large points don't render
       * properly on G13.
       *
       * Must be synced with pointSizeRange.
       */
      NIR_PASS(_, nir, nir_lower_point_size, 1.0f, 511.95f);
   } else if (nir->info.stage == MESA_SHADER_FRAGMENT) {
      NIR_PASS(_, nir, nir_shader_intrinsics_pass, lower_viewport_fs,
               nir_metadata_dominance | nir_metadata_block_index, NULL);
   }

   NIR_PASS(_, nir, agx_nir_lower_texture);
   NIR_PASS(_, nir, agx_nir_lower_multisampled_image_store);

   agx_preprocess_nir(nir, dev->dev.libagx);
   NIR_PASS(_, nir, nir_opt_conditional_discard);
   NIR_PASS(_, nir, nir_opt_if,
            nir_opt_if_optimize_phi_true_false | nir_opt_if_avoid_64bit_phis);
}

static void
hk_upload_shader(struct hk_device *dev, struct hk_shader *shader)
{
   if (shader->b.info.has_preamble) {
      unsigned offs = shader->b.info.preamble_offset;
      assert(offs < shader->b.binary_size);

      size_t size = shader->b.binary_size - offs;
      assert(size > 0);

      shader->bo = agx_bo_create(&dev->dev, size, AGX_BO_EXEC | AGX_BO_LOW_VA,
                                 "Preamble");
      memcpy(shader->bo->ptr.cpu, shader->b.binary + offs, size);
      shader->preamble_addr = shader->bo->ptr.gpu;
   }

   if (shader->info.stage == MESA_SHADER_COMPUTE) {
      /* If we only have a single variant, link now. */
      shader->only_linked = hk_fast_link(dev, false, shader, NULL, NULL, 0);
   }

   if (shader->info.stage == MESA_SHADER_FRAGMENT) {
      agx_pack(&shader->frag_face, FRAGMENT_FACE_2, cfg) {
         cfg.conservative_depth =
            agx_translate_depth_layout(shader->b.info.depth_layout);
      }
   }

   agx_pack(&shader->counts, COUNTS, cfg) {
      cfg.uniform_register_count = shader->b.info.push_count;
      cfg.preshader_register_count = shader->b.info.nr_preamble_gprs;
      cfg.sampler_state_register_count = agx_translate_sampler_state_count(
         shader->b.info.uses_txf ? 1 : 0, false);
   }
}

static VkResult
hk_compile_nir(struct hk_device *dev, nir_shader *nir,
               VkShaderCreateFlagsEXT shader_flags,
               const struct vk_pipeline_robustness_state *rs,
               const struct hk_fs_key *fs_key, struct hk_shader *shader)
{
   unsigned vs_uniform_base = 0;

   /* For now, only shader objects are supported */
   if (nir->info.stage == MESA_SHADER_VERTEX) {
      assert(!(nir->info.inputs_read & BITFIELD64_MASK(VERT_ATTRIB_GENERIC0)) &&
             "Fixed-function attributes not used in Vulkan");

      shader->info.vs.attribs_read =
         nir->info.inputs_read >> VERT_ATTRIB_GENERIC0;

      NIR_PASS(_, nir, nir_recompute_io_bases, nir_var_shader_in);
      NIR_PASS(_, nir, nir_io_add_const_offset_to_base,
               nir_var_shader_in | nir_var_shader_out);

      NIR_PASS(_, nir, agx_nir_lower_vs_input_to_prolog,
               shader->info.vs.attrib_components_read);

      /* TODO: Optimize out for monolithic? */
      NIR_PASS(_, nir, hk_nir_insert_psiz_write);

      NIR_PASS(_, nir, agx_nir_lower_uvs, &shader->info.uvs);

      shader->info.vs.cull_distance_array_size =
         nir->info.cull_distance_array_size;

      vs_uniform_base =
         6 * DIV_ROUND_UP(
                BITSET_LAST_BIT(shader->info.vs.attrib_components_read), 4);
   } else if (nir->info.stage == MESA_SHADER_FRAGMENT) {
      shader->info.fs.interp = agx_gather_interp_info(nir);
      shader->info.fs.writes_memory = nir->info.writes_memory;

      /* Discards must be lowering before lowering MSAA to handle discards */
      NIR_PASS(_, nir, agx_nir_lower_discard_zs_emit);
      NIR_PASS(_, nir, agx_nir_lower_fs_output_to_epilog,
               &shader->info.fs.epilog_key);
      NIR_PASS(_, nir, agx_nir_lower_sample_mask);

      if (nir->info.fs.uses_sample_shading) {
         /* Ensure the sample ID is preserved in register */
         nir_builder b =
            nir_builder_at(nir_after_impl(nir_shader_get_entrypoint(nir)));
         nir_export_agx(&b, nir_load_exported_agx(&b, 1, 16, .base = 1),
                        .base = 1);

         NIR_PASS(_, nir, agx_nir_lower_to_per_sample);
      }

      NIR_PASS(_, nir, agx_nir_lower_fs_active_samples_to_register);
      NIR_PASS(_, nir, agx_nir_lower_interpolation);
   }

   /* XXX: rename */
   NIR_PASS(_, nir, hk_lower_uvs_index, vs_uniform_base);

#if 0
   /* TODO */
   nir_variable_mode robust2_modes = 0;
   if (rs->uniform_buffers == VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_ROBUST_BUFFER_ACCESS_2_EXT)
      robust2_modes |= nir_var_mem_ubo;
   if (rs->storage_buffers == VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_ROBUST_BUFFER_ACCESS_2_EXT)
      robust2_modes |= nir_var_mem_ssbo;
#endif

   struct agx_shader_key backend_key = {
      .needs_g13x_coherency = (dev->dev.params.gpu_generation == 13 &&
                               dev->dev.params.num_clusters_total > 1) ||
                              dev->dev.params.num_dies > 1,
      .reserved_preamble = 128 /* TODO */,
      .libagx = dev->dev.libagx,
      .no_stop = nir->info.stage == MESA_SHADER_FRAGMENT,
      .has_scratch = true,
   };

   /* For now, sample shading is always dynamic. Indicate that. */
   if (nir->info.stage == MESA_SHADER_FRAGMENT &&
       nir->info.fs.uses_sample_shading)
      backend_key.fs.inside_sample_loop = true;

   agx_compile_shader_nir(nir, &backend_key, NULL, &shader->b);

   shader->code_ptr = shader->b.binary;
   shader->code_size = shader->b.binary_size;

   shader->info.stage = nir->info.stage;

   if (nir->info.stage == MESA_SHADER_COMPUTE) {
      for (unsigned i = 0; i < 3; ++i)
         shader->info.cs.local_size[i] = nir->info.workgroup_size[i];
   }

   if (nir->constant_data_size > 0) {
      uint32_t data_size = align(nir->constant_data_size, HK_MIN_UBO_ALIGNMENT);

      void *data = malloc(data_size);
      if (data == NULL)
         return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);

      memcpy(data, nir->constant_data, nir->constant_data_size);

      assert(nir->constant_data_size <= data_size);
      memset(data + nir->constant_data_size, 0,
             data_size - nir->constant_data_size);

      shader->data_ptr = data;
      shader->data_size = data_size;
   }

   hk_upload_shader(dev, shader);

   return VK_SUCCESS;
}

static const struct vk_shader_ops hk_shader_ops;

static void
hk_destroy_linked_shader(struct hk_linked_shader *linked)
{
   agx_bo_unreference(linked->b.bo);
   ralloc_free(linked);
}

static void
hk_destroy_linked_shader_ht(struct hash_entry *he)
{
   hk_destroy_linked_shader(he->data);
}

DERIVE_HASH_TABLE(hk_fast_link_key_vs);
DERIVE_HASH_TABLE(hk_fast_link_key_fs);

void
hk_shader_destroy(struct vk_device *vk_dev, struct vk_shader *vk_shader,
                  const VkAllocationCallbacks *pAllocator)
{
   struct hk_device *dev = container_of(vk_dev, struct hk_device, vk);
   struct hk_shader *shader = container_of(vk_shader, struct hk_shader, vk);

   free((void *)shader->code_ptr);
   free((void *)shader->data_ptr);
   agx_bo_unreference(shader->bo);

   simple_mtx_destroy(&shader->linked.lock);
   _mesa_hash_table_destroy(shader->linked.ht, hk_destroy_linked_shader_ht);

   if (shader->only_linked)
      hk_destroy_linked_shader(shader->only_linked);

   vk_shader_free(&dev->vk, pAllocator, &shader->vk);
}

static VkResult
hk_init_link_ht(struct hk_shader *shader, gl_shader_stage stage)
{
   bool multiple_variants =
      stage == MESA_SHADER_VERTEX || stage == MESA_SHADER_FRAGMENT;

   if (!multiple_variants)
      return VK_SUCCESS;

   if (stage == MESA_SHADER_VERTEX)
      shader->linked.ht = hk_fast_link_key_vs_table_create(NULL);
   else
      shader->linked.ht = hk_fast_link_key_fs_table_create(NULL);

   return (shader->linked.ht == NULL) ? VK_ERROR_OUT_OF_HOST_MEMORY
                                      : VK_SUCCESS;
}

VkResult
hk_compile_shader(struct hk_device *dev, struct vk_shader_compile_info *info,
                  const struct vk_graphics_pipeline_state *state,
                  const VkAllocationCallbacks *pAllocator,
                  struct hk_shader **shader_out)
{
   struct hk_shader *shader;
   VkResult result;

   /* We consume the NIR, regardless of success or failure */
   nir_shader *nir = info->nir;

   shader = vk_shader_zalloc(&dev->vk, &hk_shader_ops, info->stage, pAllocator,
                             sizeof(*shader));
   if (shader == NULL) {
      ralloc_free(nir);
      return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   result = hk_init_link_ht(shader, nir->info.stage);
   if (result != VK_SUCCESS) {
      ralloc_free(nir);
      vk_shader_free(&dev->vk, pAllocator, &shader->vk);
      return vk_error(dev, result);
   }

   simple_mtx_init(&shader->linked.lock, mtx_plain);

   /* TODO: Multiview with ESO */
   const bool is_multiview = state && state->rp->view_mask != 0;

   hk_lower_nir(dev, nir, info->robustness, is_multiview,
                info->set_layout_count, info->set_layouts);

   struct hk_fs_key fs_key_tmp, *fs_key = NULL;
   if (nir->info.stage == MESA_SHADER_FRAGMENT) {
      hk_populate_fs_key(&fs_key_tmp, state);
      fs_key = &fs_key_tmp;

      nir->info.fs.uses_sample_shading |= fs_key->force_sample_shading;

      /* Force late-Z for Z/S self-deps. TODO: There's probably a less silly way
       * to do this.
       */
      if (fs_key->zs_self_dep) {
         nir_builder b =
            nir_builder_at(nir_before_impl(nir_shader_get_entrypoint(nir)));
         nir_discard_if(&b, nir_imm_false(&b));
         nir->info.fs.uses_discard = true;
      }

      NIR_PASS(_, nir, agx_nir_lower_sample_intrinsics);
   }

   result =
      hk_compile_nir(dev, nir, info->flags, info->robustness, fs_key, shader);
   ralloc_free(nir);
   if (result != VK_SUCCESS) {
      hk_shader_destroy(&dev->vk, &shader->vk, pAllocator);
      return result;
   }

   *shader_out = shader;

   return VK_SUCCESS;
}

static VkResult
hk_compile_shaders(struct vk_device *vk_dev, uint32_t shader_count,
                   struct vk_shader_compile_info *infos,
                   const struct vk_graphics_pipeline_state *state,
                   const VkAllocationCallbacks *pAllocator,
                   struct vk_shader **shaders_out)
{
   struct hk_device *dev = container_of(vk_dev, struct hk_device, vk);

   for (uint32_t i = 0; i < shader_count; i++) {
      VkResult result = hk_compile_shader(dev, &infos[i], state, pAllocator,
                                          (struct hk_shader **)&shaders_out[i]);
      if (result != VK_SUCCESS) {
         /* Clean up all the shaders before this point */
         for (uint32_t j = 0; j < i; j++)
            hk_shader_destroy(&dev->vk, shaders_out[j], pAllocator);

         /* Clean up all the NIR after this point */
         for (uint32_t j = i + 1; j < shader_count; j++)
            ralloc_free(infos[j].nir);

         /* Memset the output array */
         memset(shaders_out, 0, shader_count * sizeof(*shaders_out));

         return result;
      }
   }

   return VK_SUCCESS;
}

static VkResult
hk_deserialize_shader(struct vk_device *vk_dev, struct blob_reader *blob,
                      uint32_t binary_version,
                      const VkAllocationCallbacks *pAllocator,
                      struct vk_shader **shader_out)
{
   struct hk_device *dev = container_of(vk_dev, struct hk_device, vk);
   struct hk_shader *shader;

   struct hk_shader_info info;
   blob_copy_bytes(blob, &info, sizeof(info));

   struct agx_shader_info b_info;
   blob_copy_bytes(blob, &b_info, sizeof(b_info));

   const uint32_t code_size = blob_read_uint32(blob);
   const uint32_t data_size = blob_read_uint32(blob);
   if (blob->overrun) {
      return vk_error(dev, VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT);
   }

   shader = vk_shader_zalloc(&dev->vk, &hk_shader_ops, info.stage, pAllocator,
                             sizeof(*shader));
   if (shader == NULL)
      return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);

   VkResult result = hk_init_link_ht(shader, info.stage);
   if (result != VK_SUCCESS) {
      vk_shader_free(&dev->vk, pAllocator, &shader->vk);
      return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   simple_mtx_init(&shader->linked.lock, mtx_plain);

   shader->b.info = b_info;
   shader->info = info;
   shader->code_size = code_size;
   shader->data_size = data_size;
   shader->b.binary_size = code_size;

   shader->code_ptr = malloc(code_size);
   if (shader->code_ptr == NULL) {
      hk_shader_destroy(&dev->vk, &shader->vk, pAllocator);
      return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   shader->data_ptr = malloc(data_size);
   if (shader->data_ptr == NULL) {
      hk_shader_destroy(&dev->vk, &shader->vk, pAllocator);
      return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   blob_copy_bytes(blob, (void *)shader->code_ptr, shader->code_size);
   blob_copy_bytes(blob, (void *)shader->data_ptr, shader->data_size);
   if (blob->overrun) {
      hk_shader_destroy(&dev->vk, &shader->vk, pAllocator);
      return vk_error(dev, VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT);
   }

   shader->b.binary = (void *)shader->code_ptr;
   hk_upload_shader(dev, shader);

   *shader_out = &shader->vk;

   return VK_SUCCESS;
}

static bool
hk_shader_serialize(struct vk_device *vk_dev, const struct vk_shader *vk_shader,
                    struct blob *blob)
{
   struct hk_shader *shader = container_of(vk_shader, struct hk_shader, vk);

   blob_write_bytes(blob, &shader->info, sizeof(shader->info));
   blob_write_bytes(blob, &shader->b.info, sizeof(shader->b.info));

   blob_write_uint32(blob, shader->code_size);
   blob_write_uint32(blob, shader->data_size);
   blob_write_bytes(blob, shader->code_ptr, shader->code_size);
   blob_write_bytes(blob, shader->data_ptr, shader->data_size);

   return !blob->out_of_memory;
}

#define WRITE_STR(field, ...)                                                  \
   ({                                                                          \
      memset(field, 0, sizeof(field));                                         \
      UNUSED int i = snprintf(field, sizeof(field), __VA_ARGS__);              \
      assert(i > 0 && i < sizeof(field));                                      \
   })

static VkResult
hk_shader_get_executable_properties(
   UNUSED struct vk_device *device, const struct vk_shader *vk_shader,
   uint32_t *executable_count, VkPipelineExecutablePropertiesKHR *properties)
{
   struct hk_shader *shader = container_of(vk_shader, struct hk_shader, vk);
   VK_OUTARRAY_MAKE_TYPED(VkPipelineExecutablePropertiesKHR, out, properties,
                          executable_count);

   vk_outarray_append_typed(VkPipelineExecutablePropertiesKHR, &out, props)
   {
      props->stages = mesa_to_vk_shader_stage(shader->b.info.stage);
      props->subgroupSize = 32;
      WRITE_STR(props->name, "%s",
                _mesa_shader_stage_to_string(shader->b.info.stage));
      WRITE_STR(props->description, "%s shader",
                _mesa_shader_stage_to_string(shader->b.info.stage));
   }

   return vk_outarray_status(&out);
}

static VkResult
hk_shader_get_executable_statistics(
   UNUSED struct vk_device *device, const struct vk_shader *vk_shader,
   uint32_t executable_index, uint32_t *statistic_count,
   VkPipelineExecutableStatisticKHR *statistics)
{
   struct hk_shader *shader = container_of(vk_shader, struct hk_shader, vk);
   VK_OUTARRAY_MAKE_TYPED(VkPipelineExecutableStatisticKHR, out, statistics,
                          statistic_count);

   assert(executable_index == 0);

   vk_outarray_append_typed(VkPipelineExecutableStatisticKHR, &out, stat)
   {
      WRITE_STR(stat->name, "Code Size");
      WRITE_STR(stat->description,
                "Size of the compiled shader binary, in bytes");
      stat->format = VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR;
      stat->value.u64 = shader->code_size;
   }

   vk_outarray_append_typed(VkPipelineExecutableStatisticKHR, &out, stat)
   {
      WRITE_STR(stat->name, "Number of GPRs");
      WRITE_STR(stat->description, "Number of GPRs used by this pipeline");
      stat->format = VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR;
      stat->value.u64 = shader->b.info.nr_gprs;
   }

   return vk_outarray_status(&out);
}

static bool
write_ir_text(VkPipelineExecutableInternalRepresentationKHR *ir,
              const char *data)
{
   ir->isText = VK_TRUE;

   size_t data_len = strlen(data) + 1;

   if (ir->pData == NULL) {
      ir->dataSize = data_len;
      return true;
   }

   strncpy(ir->pData, data, ir->dataSize);
   if (ir->dataSize < data_len)
      return false;

   ir->dataSize = data_len;
   return true;
}

static VkResult
hk_shader_get_executable_internal_representations(
   UNUSED struct vk_device *device, const struct vk_shader *vk_shader,
   uint32_t executable_index, uint32_t *internal_representation_count,
   VkPipelineExecutableInternalRepresentationKHR *internal_representations)
{
   UNUSED struct hk_shader *shader =
      container_of(vk_shader, struct hk_shader, vk);
   VK_OUTARRAY_MAKE_TYPED(VkPipelineExecutableInternalRepresentationKHR, out,
                          internal_representations,
                          internal_representation_count);
   bool incomplete_text = false;

   assert(executable_index == 0);

   /* TODO */
#if 0
   vk_outarray_append_typed(VkPipelineExecutableInternalRepresentationKHR, &out, ir) {
      WRITE_STR(ir->name, "AGX assembly");
      WRITE_STR(ir->description, "AGX assembly");
      if (!write_ir_text(ir, TODO))
         incomplete_text = true;
   }
#endif

   return incomplete_text ? VK_INCOMPLETE : vk_outarray_status(&out);
}

static const struct vk_shader_ops hk_shader_ops = {
   .destroy = hk_shader_destroy,
   .serialize = hk_shader_serialize,
   .get_executable_properties = hk_shader_get_executable_properties,
   .get_executable_statistics = hk_shader_get_executable_statistics,
   .get_executable_internal_representations =
      hk_shader_get_executable_internal_representations,
};

const struct vk_device_shader_ops hk_device_shader_ops = {
   .get_nir_options = hk_get_nir_options,
   .get_spirv_options = hk_get_spirv_options,
   .preprocess_nir = hk_preprocess_nir,
   .hash_graphics_state = hk_hash_graphics_state,
   .compile = hk_compile_shaders,
   .deserialize = hk_deserialize_shader,
   .cmd_set_dynamic_graphics_state = vk_cmd_set_dynamic_graphics_state,
   .cmd_bind_shaders = hk_cmd_bind_shaders,
};

struct hk_linked_shader *
hk_fast_link(struct hk_device *dev, bool fragment, struct hk_shader *main,
             struct agx_shader_part *prolog, struct agx_shader_part *epilog,
             unsigned nr_samples_shaded)
{
   struct hk_linked_shader *s = rzalloc(NULL, struct hk_linked_shader);
   agx_fast_link(&s->b, &dev->dev, fragment, &main->b, prolog, epilog,
                 nr_samples_shaded);

   if (fragment) {
      agx_pack(&s->fs_counts, FRAGMENT_SHADER_WORD_0, cfg) {
         cfg.cf_binding_count = s->b.cf.nr_bindings;
         cfg.uniform_register_count = main->b.info.push_count;
         cfg.preshader_register_count = main->b.info.nr_preamble_gprs;
         cfg.sampler_state_register_count =
            agx_translate_sampler_state_count(s->b.uses_txf ? 1 : 0, false);
      }
   }

   /* Now that we've linked, bake the USC words to bind this program */
   struct agx_usc_builder b = agx_usc_builder(s->usc.data, sizeof(s->usc.data));

   if (main && main->b.info.immediate_size_16) {
      unreachable("todo");
#if 0
      /* XXX: do ahead of time */
      uint64_t ptr = agx_pool_upload_aligned(
         &cmd->pool, s->b.info.immediates, s->b.info.immediate_size_16 * 2, 64);

      for (unsigned range = 0; range < constant_push_ranges; ++range) {
         unsigned offset = 64 * range;
         assert(offset < s->b.info.immediate_size_16);

         agx_usc_uniform(&b, s->b.info.immediate_base_uniform + offset,
                         MIN2(64, s->b.info.immediate_size_16 - offset),
                         ptr + (offset * 2));
      }
#endif
   }

   agx_usc_push_packed(&b, UNIFORM, dev->rodata.image_heap);

   if (s->b.uses_txf)
      agx_usc_push_packed(&b, SAMPLER, dev->rodata.txf_sampler);

   if (main && (main->b.info.stage == PIPE_SHADER_COMPUTE ||
                main->b.info.stage == PIPE_SHADER_TESS_CTRL)) {
      unsigned size = main->b.info.local_size;

      agx_usc_pack(&b, SHARED, cfg) {
         cfg.layout = AGX_SHARED_LAYOUT_VERTEX_COMPUTE;
         cfg.bytes_per_threadgroup = size > 0 ? size : 65536;
         cfg.uses_shared_memory = size > 0;
      }
   } else if (!fragment) {
      agx_usc_shared_none(&b);
   }

   agx_usc_push_packed(&b, SHADER, s->b.shader);
   agx_usc_push_packed(&b, REGISTERS, s->b.regs);

   if (fragment)
      agx_usc_push_packed(&b, FRAGMENT_PROPERTIES, s->b.fragment_props);

   if (main && main->b.info.has_preamble) {
      agx_usc_pack(&b, PRESHADER, cfg) {
         cfg.code = main->preamble_addr;
      }
   } else {
      agx_usc_pack(&b, NO_PRESHADER, cfg)
         ;
   }

   s->usc.size = b.head - s->usc.data;
   return s;
}