/*
 * Copyright © 2014 Broadcom
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

/**
 * Command list validator for VC4.
 *
 * The VC4 has no IOMMU between it and system memory.  So, a user with
 * access to execute command lists could escalate privilege by
 * overwriting system memory (drawing to it as a framebuffer) or
 * reading system memory it shouldn't (reading it as a texture, or
 * uniform data, or vertex data).
 *
 * This validates command lists to ensure that all accesses are within
 * the bounds of the GEM objects referenced.  It explicitly whitelists
 * packets, and looks at the offsets in any address fields to make
 * sure they're constrained within the BOs they reference.
 *
 * Note that because of the validation that's happening anyway, this
 * is where GEM relocation processing happens.
 */

#include "vc4_simulator_validate.h"
#include "vc4_packet.h"

#define VALIDATE_ARGS \
	struct exec_info *exec,				\
	void *validated,				\
	void *untrusted

static int
validate_branch_to_sublist(VALIDATE_ARGS)
{
	struct drm_gem_cma_object *target;

	/* XXX: Validate address jumped to */

	target = exec->bo[exec->bo_index[0]];

	*(uint32_t *)(validated + 0) =
		*(uint32_t *)(untrusted + 0) + target->paddr;

	return 0;
}

static int
validate_loadstore_tile_buffer_general(VALIDATE_ARGS)
{
	uint32_t packet_b0 = *(uint8_t *)(untrusted + 0);
	struct drm_gem_cma_object *fbo = exec->bo[exec->bo_index[0]];

	if ((packet_b0 & 0xf) == VC4_LOADSTORE_TILE_BUFFER_NONE)
		return 0;

	/* XXX: Validate address offset */
	*(uint32_t *)(validated + 2) =
		*(uint32_t *)(untrusted + 2) + fbo->paddr;

	return 0;
}

static int
validate_indexed_prim_list(VALIDATE_ARGS)
{
	struct drm_gem_cma_object *ib;
	uint32_t max_index = *(uint32_t *)(untrusted + 9);
	uint32_t index_size = (*(uint8_t *)(untrusted + 0) >> 4) ? 2 : 1;
	uint32_t ib_access_end = (max_index + 1) * index_size;

	/* Check overflow condition */
	if (max_index == ~0) {
		DRM_ERROR("unlimited max index\n");
		return -EINVAL;
	}

	if (ib_access_end < max_index) {
		DRM_ERROR("IB access overflow\n");
		return -EINVAL;
	}

	ib = exec->bo[exec->bo_index[0]];
	if (ib_access_end > ib->base.size) {
		DRM_ERROR("IB access out of bounds (%d/%d)\n",
			  ib_access_end, ib->base.size);
		return -EINVAL;
	}

	*(uint32_t *)(validated + 5) =
		*(uint32_t *)(untrusted + 5) + ib->paddr;

	return 0;
}

static int
validate_gl_shader_state(VALIDATE_ARGS)
{
	uint32_t i = exec->shader_state_count++;

	if (i >= exec->shader_state_size) { /* XXX? */
		DRM_ERROR("More requests for shader states than declared\n");
		return -EINVAL;
	}

	exec->shader_state[i].packet = VC4_PACKET_GL_SHADER_STATE;
	exec->shader_state[i].addr = *(uint32_t *)untrusted;

	*(uint32_t *)validated = exec->shader_state[i].addr +
		exec->shader_paddr;

	return 0;
}

static int
validate_nv_shader_state(VALIDATE_ARGS)
{
	uint32_t i = exec->shader_state_count++;

	if (i >= exec->shader_state_size) {
		DRM_ERROR("More requests for shader states than declared\n");
		return -EINVAL;
	}

	exec->shader_state[i].packet = VC4_PACKET_NV_SHADER_STATE;
	exec->shader_state[i].addr = *(uint32_t *)untrusted;

	if (exec->shader_state[i].addr & 15) {
		DRM_ERROR("NV shader state address 0x%08x misaligned\n",
			  exec->shader_state[i].addr);
		return -EINVAL;
	}

	*(uint32_t *)validated =
		exec->shader_state[i].addr + exec->shader_paddr;

	return 0;
}

static int
validate_tile_binning_config(VALIDATE_ARGS)
{
	struct drm_gem_cma_object *tile_allocation;
	struct drm_gem_cma_object *tile_state_data_array;

	tile_allocation = exec->bo[exec->bo_index[0]];
	tile_state_data_array = exec->bo[exec->bo_index[1]];

	/* XXX: Validate offsets */
	*(uint32_t *)validated =
		*(uint32_t *)untrusted + tile_allocation->paddr;

	*(uint32_t *)(validated + 8) =
		*(uint32_t *)(untrusted + 8) + tile_state_data_array->paddr;

	return 0;
}

static int
validate_tile_rendering_mode_config(VALIDATE_ARGS)
{
	struct drm_gem_cma_object *fbo;

	fbo = exec->bo[exec->bo_index[0]];

	/* XXX: Validate offsets */
	*(uint32_t *)validated =
		*(uint32_t *)untrusted + fbo->paddr;

	return 0;
}

static int
validate_gem_handles(VALIDATE_ARGS)
{
	int i;

	memcpy(exec->bo_index, untrusted, sizeof(exec->bo_index));

	for (i = 0; i < ARRAY_SIZE(exec->bo_index); i++) {
		if (exec->bo_index[i] >= exec->bo_count) {
			DRM_ERROR("Validated BO index %d >= %d\n",
				  exec->bo_index[i], exec->bo_count);
			return -EINVAL;
		}
	}

	return 0;
}

static const struct cmd_info {
	bool bin;
	bool render;
	uint16_t len;
	const char *name;
	int (*func)(struct exec_info *exec, void *validated, void *untrusted);
} cmd_info[] = {
	[0] = { 1, 1, 1, "halt", NULL },
	[1] = { 1, 1, 1, "nop", NULL },
	[4] = { 1, 1, 1, "flush", NULL },
	[5] = { 1, 0, 1, "flush all state", NULL },
	[6] = { 1, 0, 1, "start tile binning", NULL },
	[7] = { 1, 0, 1, "increment semaphore", NULL },
	[8] = { 1, 1, 1, "wait on semaphore", NULL },
	[17] = { 1, 1, 5, "branch to sublist", validate_branch_to_sublist },
	[24] = { 0, 1, 1, "store MS resolved tile color buffer", NULL },
	[25] = { 0, 1, 1, "store MS resolved tile color buffer and EOF", NULL },

	[28] = { 0, 1, 7, "Store Tile Buffer General",
		 validate_loadstore_tile_buffer_general },
	[29] = { 0, 1, 7, "Load Tile Buffer General",
		 validate_loadstore_tile_buffer_general },

	[32] = { 1, 1, 14, "Indexed Primitive List",
		 validate_indexed_prim_list },

	/* XXX: bounds check verts? */
	[33] = { 1, 1, 10, "Vertex Array Primitives", NULL },

	[56] = { 1, 1, 2, "primitive list format", NULL }, /* XXX: bin valid? */

	[64] = { 1, 1, 5, "GL Shader State", validate_gl_shader_state },
	[65] = { 1, 1, 5, "NV Shader State", validate_nv_shader_state },

	[96] = { 1, 1, 4, "configuration bits", NULL },
	[97] = { 1, 1, 5, "flat shade flags", NULL },
	[98] = { 1, 1, 5, "point size", NULL },
	[99] = { 1, 1, 5, "line width", NULL },
	[100] = { 1, 1, 3, "RHT X boundary", NULL },
	[101] = { 1, 1, 5, "Depth Offset", NULL },
	[102] = { 1, 1, 9, "Clip Window", NULL },
	[103] = { 1, 1, 5, "Viewport Offset", NULL },
	[105] = { 1, 1, 9, "Clipper XY Scaling", NULL },
	/* Note: The docs say this was also 105, but it was 106 in the
	 * initial userland code drop.
	 */
	[106] = { 1, 1, 9, "Clipper Z Scale and Offset", NULL },

	[112] = { 1, 0, 16, "tile binning configuration",
		  validate_tile_binning_config },

	/* XXX: Do we need to validate this one?  It's got width/height in it.
	 */
	[113] = { 0, 1, 11, "tile rendering mode configuration",
		  validate_tile_rendering_mode_config},

	[114] = { 0, 1, 14, "Clear Colors", NULL },

	/* XXX: Do we need to validate here?  It's got tile x/y number for
	 * rendering
	 */
	[115] = { 0, 1, 3, "Tile Coordinates", NULL },

	[254] = { 1, 1, 9, "GEM handles", validate_gem_handles },
};

int
vc4_validate_cl(struct drm_device *dev,
		void *validated,
		void *unvalidated,
		uint32_t len,
		bool is_bin,
		struct exec_info *exec)
{
	uint32_t dst_offset = 0;
	uint32_t src_offset = 0;

	while (src_offset < len) {
		void *dst_pkt = validated + dst_offset;
		void *src_pkt = unvalidated + src_offset;
		u8 cmd = *(uint8_t *)src_pkt;
		const struct cmd_info *info;

		if (cmd > ARRAY_SIZE(cmd_info)) {
			DRM_ERROR("0x%08x: packet %d out of bounds\n",
				  src_offset, cmd);
			return -EINVAL;
		}

		info = &cmd_info[cmd];
		if (!info->name) {
			DRM_ERROR("0x%08x: packet %d invalid\n",
				  src_offset, cmd);
			return -EINVAL;
		}

#if 0
		DRM_INFO("0x%08x: packet %d (%s) size %d processing...\n",
			 src_offset, cmd, info->name, info->len);
#endif

		if ((is_bin && !info->bin) ||
		    (!is_bin && !info->render)) {
			DRM_ERROR("0x%08x: packet %d (%s) invalid for %s\n",
				  src_offset, cmd, info->name,
				  is_bin ? "binner" : "render");
			return -EINVAL;
		}

		if (src_offset + info->len > len) {
			DRM_ERROR("0x%08x: packet %d (%s) length 0x%08x "
				  "exceeds bounds (0x%08x)\n",
				  src_offset, cmd, info->name, info->len,
				  src_offset + len);
			return -EINVAL;
		}

		if (cmd != 254)
			memcpy(dst_pkt, src_pkt, info->len);

		if (info->func && info->func(exec,
					     dst_pkt + 1,
					     src_pkt + 1)) {
			DRM_ERROR("0x%08x: packet %d (%s) failed to "
				  "validate\n",
				  src_offset, cmd, info->name);
			return -EINVAL;
		}

		src_offset += info->len;
		/* GEM handle loading doesn't produce HW packets. */
		if (cmd != 254)
			dst_offset += info->len;

		/* When the CL hits halt, it'll stop reading anything else. */
		if (cmd == 0)
			break;
	}

	return 0;
}

static int
validate_shader_rec(struct drm_device *dev,
		    struct exec_info *exec,
		    void *validated,
		    void *unvalidated,
		    uint32_t len,
		    struct vc4_shader_state *state)
{
	uint32_t *src_handles = unvalidated;
	void *src_pkt;
	void *dst_pkt = validated;
	static const int gl_bo_offsets[] = {
		4, 8, /* fs code, ubo */
		16, 20, /* vs code, ubo */
		28, 32, /* cs code, ubo */
	};
	static const int nv_bo_offsets[] = {
		4, 8, /* fs code, ubo */
		12, /* vbo */
	};
	struct drm_gem_cma_object *bo[ARRAY_SIZE(gl_bo_offsets) + 8];
	const int *bo_offsets;
	uint32_t nr_attributes = 0, nr_bo, packet_size;
	int i;

	if (state->packet == VC4_PACKET_NV_SHADER_STATE) {
		bo_offsets = nv_bo_offsets;
		nr_bo = ARRAY_SIZE(nv_bo_offsets);

		packet_size = 16;
	} else {
		bo_offsets = gl_bo_offsets;
		nr_bo = ARRAY_SIZE(gl_bo_offsets);

		nr_attributes = state->addr & 0x7;
		if (nr_attributes == 0)
			nr_attributes = 8;
		packet_size = 36 + nr_attributes * 8;
	}
	if ((nr_bo + nr_attributes) * 4 + packet_size > len) {
		DRM_ERROR("overflowed shader packet read "
			  "(handles %d, packet %d, len %d)\n",
			  (nr_bo + nr_attributes) * 4, packet_size, len);
		return -EINVAL;
	}

	src_pkt = unvalidated + 4 * (nr_bo + nr_attributes);
	memcpy(dst_pkt, src_pkt, packet_size);

	for (i = 0; i < nr_bo + nr_attributes; i++) {
		if (src_handles[i] >= exec->bo_count) {
			DRM_ERROR("shader rec bo index %d > %d\n",
				  src_handles[i], exec->bo_count);
			return -EINVAL;
		}
		bo[i] = exec->bo[src_handles[i]];
	}

	for (i = 0; i < nr_bo; i++) {
		/* XXX: validation */
		uint32_t o = bo_offsets[i];
		*(uint32_t *)(dst_pkt + o) =
			bo[i]->paddr + *(uint32_t *)(src_pkt + o);
	}

	for (i = 0; i < nr_attributes; i++) {
		/* XXX: validation */
		uint32_t o = 36 + i * 8;
		*(uint32_t *)(dst_pkt + o) =
			bo[nr_bo + i]->paddr + *(uint32_t *)(src_pkt + o);
	}

	return 0;
}

int
vc4_validate_shader_recs(struct drm_device *dev,
			 void *validated,
			 void *unvalidated,
			 uint32_t len,
			 struct exec_info *exec)
{
	uint32_t dst_offset = 0;
	uint32_t src_offset = 0;
	uint32_t i;
	int ret = 0;

	for (i = 0; i < exec->shader_state_count; i++) {
		if ((exec->shader_state[i].addr & ~0xf) !=
		    (validated - exec->exec_bo->vaddr -
		     (exec->shader_paddr - exec->exec_bo->paddr))) {
			DRM_ERROR("unexpected shader rec offset: "
				  "0x%08x vs 0x%08x\n",
				  exec->shader_state[i].addr & ~0xf,
				  (int)(validated -
					exec->exec_bo->vaddr -
					(exec->shader_paddr -
					 exec->exec_bo->paddr)));
			return -EINVAL;
		}

		ret = validate_shader_rec(dev, exec,
					  validated + dst_offset,
					  unvalidated + src_offset,
					  len - src_offset,
					  &exec->shader_state[i]);
		if (ret)
			return ret;
		/* XXX: incr dst/src offset */
	}

	return ret;
}