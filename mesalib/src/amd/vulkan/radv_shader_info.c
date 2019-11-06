/*
 * Copyright © 2017 Red Hat
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
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */
#include "radv_private.h"
#include "radv_shader.h"
#include "nir/nir.h"
#include "nir/nir_deref.h"
#include "nir/nir_xfb_info.h"

static void mark_sampler_desc(const nir_variable *var,
			      struct radv_shader_info *info)
{
	info->desc_set_used_mask |= (1 << var->data.descriptor_set);
}

static void mark_ls_output(struct radv_shader_info *info,
			   uint32_t param, int num_slots)
{
	uint64_t mask = (1ull << num_slots) - 1ull;
	info->vs.ls_outputs_written |= (mask << param);
}

static void mark_tess_output(struct radv_shader_info *info,
			     bool is_patch, uint32_t param, int num_slots)
{
	uint64_t mask = (1ull << num_slots) - 1ull;
	if (is_patch)
		info->tcs.patch_outputs_written |= (mask << param);
	else
		info->tcs.outputs_written |= (mask << param);
}

static void
get_deref_offset(nir_deref_instr *instr,
                 unsigned *const_out)
{
        nir_variable *var = nir_deref_instr_get_variable(instr);
        nir_deref_path path;
        unsigned idx_lvl = 1;

	if (var->data.compact) {
		assert(instr->deref_type == nir_deref_type_array);
		*const_out = nir_src_as_uint(instr->arr.index);
		return;
	}

	nir_deref_path_init(&path, instr, NULL);

	uint32_t const_offset = 0;

	for (; path.path[idx_lvl]; ++idx_lvl) {
		const struct glsl_type *parent_type = path.path[idx_lvl - 1]->type;
		if (path.path[idx_lvl]->deref_type == nir_deref_type_struct) {
			unsigned index = path.path[idx_lvl]->strct.index;

			for (unsigned i = 0; i < index; i++) {
				const struct glsl_type *ft = glsl_get_struct_field(parent_type, i);
				const_offset += glsl_count_attribute_slots(ft, false);
			}
		} else if(path.path[idx_lvl]->deref_type == nir_deref_type_array) {
			unsigned size = glsl_count_attribute_slots(path.path[idx_lvl]->type, false);
			if (nir_src_is_const(path.path[idx_lvl]->arr.index))
				const_offset += nir_src_as_uint(path.path[idx_lvl]->arr.index) * size;
		} else
			unreachable("Uhandled deref type in get_deref_instr_offset");
	}

	*const_out = const_offset;

	nir_deref_path_finish(&path);
}

static void
gather_intrinsic_load_deref_info(const nir_shader *nir,
			       const nir_intrinsic_instr *instr,
			       struct radv_shader_info *info)
{
	switch (nir->info.stage) {
	case MESA_SHADER_VERTEX: {
		nir_variable *var = nir_deref_instr_get_variable(nir_instr_as_deref(instr->src[0].ssa->parent_instr));

		if (var && var->data.mode == nir_var_shader_in) {
			unsigned idx = var->data.location;
			uint8_t mask = nir_ssa_def_components_read(&instr->dest.ssa);

			info->vs.input_usage_mask[idx] |=
				mask << var->data.location_frac;
		}
		break;
	}
	default:
		break;
	}
}

static uint32_t
widen_writemask(uint32_t wrmask)
{
	uint32_t new_wrmask = 0;
	for(unsigned i = 0; i < 4; i++)
		new_wrmask |= (wrmask & (1 << i) ? 0x3 : 0x0) << (i * 2);
	return new_wrmask;
}

static void
set_output_usage_mask(const nir_shader *nir, const nir_intrinsic_instr *instr,
		      uint8_t *output_usage_mask)
{
	nir_deref_instr *deref_instr =
		nir_instr_as_deref(instr->src[0].ssa->parent_instr);
	nir_variable *var = nir_deref_instr_get_variable(deref_instr);
	unsigned attrib_count = glsl_count_attribute_slots(deref_instr->type, false);
	unsigned idx = var->data.location;
	unsigned comp = var->data.location_frac;
	unsigned const_offset = 0;

	get_deref_offset(deref_instr, &const_offset);

	if (var->data.compact) {
		assert(!glsl_type_is_64bit(deref_instr->type));
		const_offset += comp;
		output_usage_mask[idx + const_offset / 4] |= 1 << (const_offset % 4);
		return;
	}

	uint32_t wrmask = nir_intrinsic_write_mask(instr);
	if (glsl_type_is_64bit(deref_instr->type))
		wrmask = widen_writemask(wrmask);

	for (unsigned i = 0; i < attrib_count; i++)
		output_usage_mask[idx + i + const_offset] |=
			((wrmask >> (i * 4)) & 0xf) << comp;
}

static void
gather_intrinsic_store_deref_info(const nir_shader *nir,
				const nir_intrinsic_instr *instr,
				struct radv_shader_info *info)
{
	nir_variable *var = nir_deref_instr_get_variable(nir_instr_as_deref(instr->src[0].ssa->parent_instr));

	if (var && var->data.mode == nir_var_shader_out) {
		unsigned idx = var->data.location;

		switch (nir->info.stage) {
		case MESA_SHADER_VERTEX:
			set_output_usage_mask(nir, instr,
					      info->vs.output_usage_mask);
			break;
		case MESA_SHADER_GEOMETRY:
			set_output_usage_mask(nir, instr,
					      info->gs.output_usage_mask);
			break;
		case MESA_SHADER_TESS_EVAL:
			set_output_usage_mask(nir, instr,
					      info->tes.output_usage_mask);
			break;
		case MESA_SHADER_TESS_CTRL: {
			unsigned param = shader_io_get_unique_index(idx);
			const struct glsl_type *type = var->type;

			if (!var->data.patch)
				type = glsl_get_array_element(var->type);

			unsigned slots =
				var->data.compact ? DIV_ROUND_UP(var->data.location_frac + glsl_get_length(type), 4)
						  : glsl_count_attribute_slots(type, false);

			mark_tess_output(info, var->data.patch, param, slots);
			break;
		}
		default:
			break;
		}
	}
}

static void
gather_push_constant_info(const nir_shader *nir,
			  const nir_intrinsic_instr *instr,
			  struct radv_shader_info *info)
{
	int base = nir_intrinsic_base(instr);

	if (!nir_src_is_const(instr->src[0])) {
		info->has_indirect_push_constants = true;
	} else {
		uint32_t min = base + nir_src_as_uint(instr->src[0]);
		uint32_t max = min + instr->num_components * 4;

		info->max_push_constant_used =
			MAX2(max, info->max_push_constant_used);
		info->min_push_constant_used =
			MIN2(min, info->min_push_constant_used);
	}

	if (instr->dest.ssa.bit_size != 32)
		info->has_only_32bit_push_constants = false;

	info->loads_push_constants = true;
}

static void
gather_intrinsic_info(const nir_shader *nir, const nir_intrinsic_instr *instr,
		      struct radv_shader_info *info)
{
	switch (instr->intrinsic) {
	case nir_intrinsic_load_barycentric_at_sample:
		info->ps.needs_sample_positions = true;
		break;
	case nir_intrinsic_load_draw_id:
		info->vs.needs_draw_id = true;
		break;
	case nir_intrinsic_load_instance_id:
		info->vs.needs_instance_id = true;
		break;
	case nir_intrinsic_load_num_work_groups:
		info->cs.uses_grid_size = true;
		break;
	case nir_intrinsic_load_local_invocation_id:
	case nir_intrinsic_load_work_group_id: {
		unsigned mask = nir_ssa_def_components_read(&instr->dest.ssa);
		while (mask) {
			unsigned i = u_bit_scan(&mask);

			if (instr->intrinsic == nir_intrinsic_load_work_group_id)
				info->cs.uses_block_id[i] = true;
			else
				info->cs.uses_thread_id[i] = true;
		}
		break;
	}
	case nir_intrinsic_load_local_invocation_index:
	case nir_intrinsic_load_subgroup_id:
	case nir_intrinsic_load_num_subgroups:
		info->cs.uses_local_invocation_idx = true;
		break;
	case nir_intrinsic_load_sample_id:
		info->ps.force_persample = true;
		break;
	case nir_intrinsic_load_sample_pos:
		info->ps.force_persample = true;
		break;
	case nir_intrinsic_load_view_index:
		info->needs_multiview_view_index = true;
		if (nir->info.stage == MESA_SHADER_FRAGMENT)
			info->ps.layer_input = true;
		break;
	case nir_intrinsic_load_layer_id:
		if (nir->info.stage == MESA_SHADER_FRAGMENT)
			info->ps.layer_input = true;
		break;
	case nir_intrinsic_load_invocation_id:
		info->uses_invocation_id = true;
		break;
	case nir_intrinsic_load_primitive_id:
		info->uses_prim_id = true;
		break;
	case nir_intrinsic_load_push_constant:
		gather_push_constant_info(nir, instr, info);
		break;
	case nir_intrinsic_vulkan_resource_index:
		info->desc_set_used_mask |= (1 << nir_intrinsic_desc_set(instr));
		break;
	case nir_intrinsic_image_deref_load:
	case nir_intrinsic_image_deref_store:
	case nir_intrinsic_image_deref_atomic_add:
	case nir_intrinsic_image_deref_atomic_imin:
	case nir_intrinsic_image_deref_atomic_umin:
	case nir_intrinsic_image_deref_atomic_imax:
	case nir_intrinsic_image_deref_atomic_umax:
	case nir_intrinsic_image_deref_atomic_and:
	case nir_intrinsic_image_deref_atomic_or:
	case nir_intrinsic_image_deref_atomic_xor:
	case nir_intrinsic_image_deref_atomic_exchange:
	case nir_intrinsic_image_deref_atomic_comp_swap:
	case nir_intrinsic_image_deref_size: {
		nir_variable *var = nir_deref_instr_get_variable(nir_instr_as_deref(instr->src[0].ssa->parent_instr));
		mark_sampler_desc(var, info);

		if (instr->intrinsic == nir_intrinsic_image_deref_store ||
		    instr->intrinsic == nir_intrinsic_image_deref_atomic_add ||
		    instr->intrinsic == nir_intrinsic_image_deref_atomic_imin ||
		    instr->intrinsic == nir_intrinsic_image_deref_atomic_umin ||
		    instr->intrinsic == nir_intrinsic_image_deref_atomic_imax ||
		    instr->intrinsic == nir_intrinsic_image_deref_atomic_umax ||
		    instr->intrinsic == nir_intrinsic_image_deref_atomic_and ||
		    instr->intrinsic == nir_intrinsic_image_deref_atomic_or ||
		    instr->intrinsic == nir_intrinsic_image_deref_atomic_xor ||
		    instr->intrinsic == nir_intrinsic_image_deref_atomic_exchange ||
		    instr->intrinsic == nir_intrinsic_image_deref_atomic_comp_swap) {
			if (nir->info.stage == MESA_SHADER_FRAGMENT)
				info->ps.writes_memory = true;
			else if (nir->info.stage == MESA_SHADER_GEOMETRY)
				info->gs.writes_memory = true;
		}
		break;
	}
	case nir_intrinsic_store_ssbo:
	case nir_intrinsic_ssbo_atomic_add:
	case nir_intrinsic_ssbo_atomic_imin:
	case nir_intrinsic_ssbo_atomic_umin:
	case nir_intrinsic_ssbo_atomic_imax:
	case nir_intrinsic_ssbo_atomic_umax:
	case nir_intrinsic_ssbo_atomic_and:
	case nir_intrinsic_ssbo_atomic_or:
	case nir_intrinsic_ssbo_atomic_xor:
	case nir_intrinsic_ssbo_atomic_exchange:
	case nir_intrinsic_ssbo_atomic_comp_swap:
		if (nir->info.stage == MESA_SHADER_FRAGMENT)
			info->ps.writes_memory = true;
		else if (nir->info.stage == MESA_SHADER_GEOMETRY)
			info->gs.writes_memory = true;
		break;
	case nir_intrinsic_load_deref:
		gather_intrinsic_load_deref_info(nir, instr, info);
		break;
	case nir_intrinsic_store_deref:
		gather_intrinsic_store_deref_info(nir, instr, info);
		break;
	default:
		break;
	}
}

static void
gather_tex_info(const nir_shader *nir, const nir_tex_instr *instr,
		struct radv_shader_info *info)
{
	for (unsigned i = 0; i < instr->num_srcs; i++) {
		switch (instr->src[i].src_type) {
		case nir_tex_src_texture_deref:
			mark_sampler_desc(nir_deref_instr_get_variable(nir_src_as_deref(instr->src[i].src)), info);
			break;
		case nir_tex_src_sampler_deref:
			mark_sampler_desc(nir_deref_instr_get_variable(nir_src_as_deref(instr->src[i].src)), info);
			break;
		default:
			break;
		}
	}
}

static void
gather_info_block(const nir_shader *nir, const nir_block *block,
		  struct radv_shader_info *info)
{
	nir_foreach_instr(instr, block) {
		switch (instr->type) {
		case nir_instr_type_intrinsic:
			gather_intrinsic_info(nir, nir_instr_as_intrinsic(instr), info);
			break;
		case nir_instr_type_tex:
			gather_tex_info(nir, nir_instr_as_tex(instr), info);
			break;
		default:
			break;
		}
	}
}

static void
gather_info_input_decl_vs(const nir_shader *nir, const nir_variable *var,
			  struct radv_shader_info *info,
			  const struct radv_shader_variant_key *key)
{
	unsigned attrib_count = glsl_count_attribute_slots(var->type, true);
	int idx = var->data.location;

	if (idx >= VERT_ATTRIB_GENERIC0 && idx <= VERT_ATTRIB_GENERIC15)
		info->vs.has_vertex_buffers = true;

	for (unsigned i = 0; i < attrib_count; ++i) {
		unsigned attrib_index = var->data.location + i - VERT_ATTRIB_GENERIC0;

		if (key->vs.instance_rate_inputs & (1u << attrib_index))
			info->vs.needs_instance_id = true;
	}
}

static void
mark_16bit_ps_input(struct radv_shader_info *info, const struct glsl_type *type,
		    int location)
{
	if (glsl_type_is_scalar(type) || glsl_type_is_vector(type) || glsl_type_is_matrix(type)) {
		unsigned attrib_count = glsl_count_attribute_slots(type, false);
		if (glsl_type_is_16bit(type)) {
			info->ps.float16_shaded_mask |= ((1ull << attrib_count) - 1) << location;
		}
	} else if (glsl_type_is_array(type)) {
		unsigned stride = glsl_count_attribute_slots(glsl_get_array_element(type), false);
		for (unsigned i = 0; i < glsl_get_length(type); ++i) {
			mark_16bit_ps_input(info, glsl_get_array_element(type), location + i * stride);
		}
	} else {
		assert(glsl_type_is_struct_or_ifc(type));
		for (unsigned i = 0; i < glsl_get_length(type); i++) {
			mark_16bit_ps_input(info, glsl_get_struct_field(type, i), location);
			location += glsl_count_attribute_slots(glsl_get_struct_field(type, i), false);
		}
	}
}
static void
gather_info_input_decl_ps(const nir_shader *nir, const nir_variable *var,
			  struct radv_shader_info *info)
{
	unsigned attrib_count = glsl_count_attribute_slots(var->type, false);
	const struct glsl_type *type = glsl_without_array(var->type);
	int idx = var->data.location;

	switch (idx) {
	case VARYING_SLOT_PNTC:
		info->ps.has_pcoord = true;
		break;
	case VARYING_SLOT_PRIMITIVE_ID:
		info->ps.prim_id_input = true;
		break;
	case VARYING_SLOT_LAYER:
		info->ps.layer_input = true;
		break;
	case VARYING_SLOT_CLIP_DIST0:
	case VARYING_SLOT_CLIP_DIST1:
		info->ps.num_input_clips_culls += attrib_count;
		break;
	default:
		break;
	}

	if (glsl_get_base_type(type) == GLSL_TYPE_FLOAT) {
		if (var->data.sample)
			info->ps.force_persample = true;
	}

	if (var->data.compact) {
		unsigned component_count = var->data.location_frac +
		                           glsl_get_length(var->type);
		attrib_count = (component_count + 3) / 4;
	} else {
		mark_16bit_ps_input(info, var->type, var->data.driver_location);
	}

	uint64_t mask = ((1ull << attrib_count) - 1);

	if (var->data.interpolation == INTERP_MODE_FLAT)
		info->ps.flat_shaded_mask |= mask << var->data.driver_location;

	if (var->data.location >= VARYING_SLOT_VAR0)
		info->ps.input_mask |= mask << (var->data.location - VARYING_SLOT_VAR0);
}

static void
gather_info_input_decl(const nir_shader *nir, const nir_variable *var,
		       struct radv_shader_info *info,
		       const struct radv_shader_variant_key *key)
{
	switch (nir->info.stage) {
	case MESA_SHADER_VERTEX:
		gather_info_input_decl_vs(nir, var, info, key);
		break;
	case MESA_SHADER_FRAGMENT:
		gather_info_input_decl_ps(nir, var, info);
		break;
	default:
		break;
	}
}

static void
gather_info_output_decl_ls(const nir_shader *nir, const nir_variable *var,
			   struct radv_shader_info *info)
{
	int idx = var->data.location;
	unsigned param = shader_io_get_unique_index(idx);
	int num_slots = glsl_count_attribute_slots(var->type, false);
	if (var->data.compact)
		num_slots = DIV_ROUND_UP(var->data.location_frac + glsl_get_length(var->type), 4);
	mark_ls_output(info, param, num_slots);
}

static void
gather_info_output_decl_ps(const nir_shader *nir, const nir_variable *var,
			   struct radv_shader_info *info)
{
	int idx = var->data.location;

	switch (idx) {
	case FRAG_RESULT_DEPTH:
		info->ps.writes_z = true;
		break;
	case FRAG_RESULT_STENCIL:
		info->ps.writes_stencil = true;
		break;
	case FRAG_RESULT_SAMPLE_MASK:
		info->ps.writes_sample_mask = true;
		break;
	default:
		break;
	}
}

static void
gather_info_output_decl_gs(const nir_shader *nir, const nir_variable *var,
			   struct radv_shader_info *info)
{
	unsigned num_components = glsl_get_component_slots(var->type);
	unsigned stream = var->data.stream;
	unsigned idx = var->data.location;

	assert(stream < 4);

	info->gs.max_stream = MAX2(info->gs.max_stream, stream);
	info->gs.num_stream_output_components[stream] += num_components;
	info->gs.output_streams[idx] = stream;
}

static void
gather_info_output_decl(const nir_shader *nir, const nir_variable *var,
			struct radv_shader_info *info,
			const struct radv_shader_variant_key *key)
{
	struct radv_vs_output_info *vs_info = NULL;

	switch (nir->info.stage) {
	case MESA_SHADER_FRAGMENT:
		gather_info_output_decl_ps(nir, var, info);
		break;
	case MESA_SHADER_VERTEX:
		if (!key->vs_common_out.as_ls &&
		    !key->vs_common_out.as_es)
			vs_info = &info->vs.outinfo;

		if (key->vs_common_out.as_ls)
			gather_info_output_decl_ls(nir, var, info);
		else if (key->vs_common_out.as_ngg)
			gather_info_output_decl_gs(nir, var, info);
		break;
	case MESA_SHADER_GEOMETRY:
		vs_info = &info->vs.outinfo;
		gather_info_output_decl_gs(nir, var, info);
		break;
	case MESA_SHADER_TESS_EVAL:
		if (!key->vs_common_out.as_es)
			vs_info = &info->tes.outinfo;
		break;
	default:
		break;
	}

	if (vs_info) {
		switch (var->data.location) {
		case VARYING_SLOT_CLIP_DIST0:
			vs_info->clip_dist_mask =
				(1 << nir->info.clip_distance_array_size) - 1;
			vs_info->cull_dist_mask =
				(1 << nir->info.cull_distance_array_size) - 1;
			vs_info->cull_dist_mask <<= nir->info.clip_distance_array_size;
			break;
		case VARYING_SLOT_PSIZ:
			vs_info->writes_pointsize = true;
			break;
		case VARYING_SLOT_VIEWPORT:
			vs_info->writes_viewport_index = true;
			break;
		case VARYING_SLOT_LAYER:
			vs_info->writes_layer = true;
			break;
		default:
			break;
		}
	}
}

static void
gather_xfb_info(const nir_shader *nir, struct radv_shader_info *info)
{
	nir_xfb_info *xfb = nir_gather_xfb_info(nir, NULL);
	struct radv_streamout_info *so = &info->so;

	if (!xfb)
		return;

	assert(xfb->output_count < MAX_SO_OUTPUTS);
	so->num_outputs = xfb->output_count;

	for (unsigned i = 0; i < xfb->output_count; i++) {
		struct radv_stream_output *output = &so->outputs[i];

		output->buffer = xfb->outputs[i].buffer;
		output->stream = xfb->buffer_to_stream[xfb->outputs[i].buffer];
		output->offset = xfb->outputs[i].offset;
		output->location = xfb->outputs[i].location;
		output->component_mask = xfb->outputs[i].component_mask;

		so->enabled_stream_buffers_mask |=
			(1 << output->buffer) << (output->stream * 4);

	}

	for (unsigned i = 0; i < NIR_MAX_XFB_BUFFERS; i++) {
		so->strides[i] = xfb->buffers[i].stride / 4;
	}

	ralloc_free(xfb);
}

void
radv_nir_shader_info_init(struct radv_shader_info *info)
{
	/* Assume that shaders only have 32-bit push constants by default. */
	info->min_push_constant_used = UINT8_MAX;
	info->has_only_32bit_push_constants = true;
}

void
radv_nir_shader_info_pass(const struct nir_shader *nir,
			  const struct radv_pipeline_layout *layout,
			  const struct radv_shader_variant_key *key,
			  struct radv_shader_info *info)
{
	struct nir_function *func =
		(struct nir_function *)exec_list_get_head_const(&nir->functions);

	if (layout && layout->dynamic_offset_count &&
	    (layout->dynamic_shader_stages & mesa_to_vk_shader_stage(nir->info.stage))) {
		info->loads_push_constants = true;
		info->loads_dynamic_offsets = true;
	}

	nir_foreach_variable(variable, &nir->inputs)
		gather_info_input_decl(nir, variable, info, key);

	nir_foreach_block(block, func->impl) {
		gather_info_block(nir, block, info);
	}

	nir_foreach_variable(variable, &nir->outputs)
		gather_info_output_decl(nir, variable, info, key);

	if (nir->info.stage == MESA_SHADER_VERTEX ||
	    nir->info.stage == MESA_SHADER_TESS_EVAL ||
	    nir->info.stage == MESA_SHADER_GEOMETRY)
		gather_xfb_info(nir, info);

	/* Make sure to export the LayerID if the fragment shader needs it. */
	if (key->vs_common_out.export_layer_id) {
		switch (nir->info.stage) {
		case MESA_SHADER_VERTEX:
			info->vs.output_usage_mask[VARYING_SLOT_LAYER] |= 0x1;
			break;
		case MESA_SHADER_TESS_EVAL:
			info->tes.output_usage_mask[VARYING_SLOT_LAYER] |= 0x1;
			break;
		case MESA_SHADER_GEOMETRY:
			info->gs.output_usage_mask[VARYING_SLOT_LAYER] |= 0x1;
			break;
		default:
			break;
		}
	}

	/* Make sure to export the LayerID if the subpass has multiviews. */
	if (key->has_multiview_view_index) {
		switch (nir->info.stage) {
		case MESA_SHADER_VERTEX:
			info->vs.outinfo.writes_layer = true;
			break;
		case MESA_SHADER_TESS_EVAL:
			info->tes.outinfo.writes_layer = true;
			break;
		case MESA_SHADER_GEOMETRY:
			info->vs.outinfo.writes_layer = true;
			break;
		default:
			break;
		}
	}

	/* Make sure to export the PrimitiveID if the fragment shader needs it. */
	if (key->vs_common_out.export_prim_id) {
		switch (nir->info.stage) {
		case MESA_SHADER_VERTEX:
			info->vs.outinfo.export_prim_id = true;
			break;
		case MESA_SHADER_TESS_EVAL:
			info->tes.outinfo.export_prim_id = true;
			break;
		case MESA_SHADER_GEOMETRY:
			info->vs.outinfo.export_prim_id = true;
			break;
		default:
			break;
		}
	}

	if (nir->info.stage == MESA_SHADER_FRAGMENT)
		info->ps.num_interp = nir->num_inputs;

	switch (nir->info.stage) {
        case MESA_SHADER_COMPUTE:
                for (int i = 0; i < 3; ++i)
                        info->cs.block_size[i] = nir->info.cs.local_size[i];
                break;
        case MESA_SHADER_FRAGMENT:
		info->ps.can_discard = nir->info.fs.uses_discard;
                info->ps.early_fragment_test = nir->info.fs.early_fragment_tests;
                info->ps.post_depth_coverage = nir->info.fs.post_depth_coverage;
                break;
        case MESA_SHADER_GEOMETRY:
                info->gs.vertices_in = nir->info.gs.vertices_in;
                info->gs.vertices_out = nir->info.gs.vertices_out;
                info->gs.output_prim = nir->info.gs.output_primitive;
                info->gs.invocations = nir->info.gs.invocations;
                break;
        case MESA_SHADER_TESS_EVAL:
                info->tes.primitive_mode = nir->info.tess.primitive_mode;
                info->tes.spacing = nir->info.tess.spacing;
                info->tes.ccw = nir->info.tess.ccw;
                info->tes.point_mode = nir->info.tess.point_mode;
                info->tes.as_es = key->vs_common_out.as_es;
                info->tes.export_prim_id = key->vs_common_out.export_prim_id;
                info->is_ngg = key->vs_common_out.as_ngg;
                break;
        case MESA_SHADER_TESS_CTRL:
                info->tcs.tcs_vertices_out = nir->info.tess.tcs_vertices_out;
                break;
        case MESA_SHADER_VERTEX:
                info->vs.as_es = key->vs_common_out.as_es;
                info->vs.as_ls = key->vs_common_out.as_ls;
                info->vs.export_prim_id = key->vs_common_out.export_prim_id;
                info->is_ngg = key->vs_common_out.as_ngg;
                break;
        default:
                break;
        }

	if (nir->info.stage == MESA_SHADER_GEOMETRY) {
		unsigned add_clip = nir->info.clip_distance_array_size +
				    nir->info.cull_distance_array_size > 4;
		info->gs.gsvs_vertex_size =
			(util_bitcount64(nir->info.outputs_written) + add_clip) * 16;
		info->gs.max_gsvs_emit_size =
			info->gs.gsvs_vertex_size * nir->info.gs.vertices_out;
	}

	/* Compute the ESGS item size for VS or TES as ES. */
	if ((nir->info.stage == MESA_SHADER_VERTEX ||
	     nir->info.stage == MESA_SHADER_TESS_EVAL) &&
	    key->vs_common_out.as_es) {
		struct radv_es_output_info *es_info =
			nir->info.stage == MESA_SHADER_VERTEX ? &info->vs.es_info : &info->tes.es_info;
		uint32_t max_output_written = 0;

		uint64_t output_mask = nir->info.outputs_written;
		while (output_mask) {
			const int i = u_bit_scan64(&output_mask);
			unsigned param_index = shader_io_get_unique_index(i);

			max_output_written = MAX2(param_index, max_output_written);
		}

		es_info->esgs_itemsize = (max_output_written + 1) * 16;
	}

	info->float_controls_mode = nir->info.float_controls_execution_mode;
}
