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

#ifdef USE_VC4_SIMULATOR

#include <stdio.h>

#include "util/u_memory.h"

#include "vc4_screen.h"
#include "vc4_context.h"
#include "vc4_simulator_validate.h"
#include "simpenrose/simpenrose.h"

static struct drm_gem_cma_object *
vc4_wrap_bo_with_cma(struct drm_device *dev, struct vc4_bo *bo)
{
        struct vc4_context *vc4 = dev->vc4;
        struct vc4_screen *screen = vc4->screen;
        struct drm_gem_cma_object *obj = CALLOC_STRUCT(drm_gem_cma_object);
        uint32_t size = align(bo->size, 4096);

        obj->bo = bo;
        obj->base.size = size;
        obj->vaddr = screen->simulator_mem_base + dev->simulator_mem_next;
        obj->paddr = simpenrose_hw_addr(obj->vaddr);

        dev->simulator_mem_next += size;
        dev->simulator_mem_next = align(dev->simulator_mem_next, 4096);
        assert(dev->simulator_mem_next <= screen->simulator_mem_size);

        return obj;
}

static struct drm_gem_cma_object *
drm_gem_cma_create(struct drm_device *dev, size_t size)
{
        struct vc4_context *vc4 = dev->vc4;
        struct vc4_screen *screen = vc4->screen;

        struct vc4_bo *bo = vc4_bo_alloc(screen, size, "simulator validate");
        return vc4_wrap_bo_with_cma(dev, bo);
}

static int
vc4_simulator_pin_bos(struct drm_device *dev, struct drm_vc4_submit_cl *args,
                      struct exec_info *exec)
{
        struct vc4_context *vc4 = dev->vc4;
        struct vc4_bo **bos = vc4->bo_pointers.base;

        exec->bo_count = args->bo_handle_count;
        exec->bo = calloc(exec->bo_count, sizeof(void *));
        for (int i = 0; i < exec->bo_count; i++) {
                struct vc4_bo *bo = bos[i];
                struct drm_gem_cma_object *obj = vc4_wrap_bo_with_cma(dev, bo);

                memcpy(obj->vaddr, bo->map, bo->size);

                exec->bo[i] = obj;
        }

        return 0;
}

static int
vc4_simulator_unpin_bos(struct drm_vc4_submit_cl *args,
                        struct exec_info *exec)
{
        for (int i = 0; i < exec->bo_count; i++) {
                struct drm_gem_cma_object *obj = exec->bo[i];
                struct vc4_bo *bo = obj->bo;

                memcpy(bo->map, obj->vaddr, bo->size);

                free(obj);
        }

        free(exec->bo);

        return 0;
}

static int
vc4_cl_validate(struct drm_device *dev, struct drm_vc4_submit_cl *args,
		struct exec_info *exec)
{
	void *temp = NULL;
	void *bin, *render, *shader_rec;
	int ret = 0;
	uint32_t bin_offset = 0;
	uint32_t render_offset = bin_offset + args->bin_cl_len;
	uint32_t shader_rec_offset = roundup(render_offset +
					     args->render_cl_len, 16);
	uint32_t exec_size = shader_rec_offset + args->shader_record_len;
	uint32_t temp_size = exec_size + (sizeof(struct vc4_shader_state) *
					  args->shader_record_count);

	if (shader_rec_offset < render_offset ||
	    exec_size < shader_rec_offset ||
	    args->shader_record_count >= (UINT_MAX /
					  sizeof(struct vc4_shader_state)) ||
	    temp_size < exec_size) {
		DRM_ERROR("overflow in exec arguments\n");
		goto fail;
	}

	/* Allocate space where we'll store the copied in user command lists
	 * and shader records.
	 *
	 * We don't just copy directly into the BOs because we need to
	 * read the contents back for validation, and I think the
	 * bo->vaddr is uncached access.
	 */
	temp = kmalloc(temp_size, GFP_KERNEL);
	if (!temp) {
		DRM_ERROR("Failed to allocate storage for copying "
			  "in bin/render CLs.\n");
		ret = -ENOMEM;
		goto fail;
	}
	bin = temp + bin_offset;
	render = temp + render_offset;
	shader_rec = temp + shader_rec_offset;
	exec->shader_state = temp + exec_size;
	exec->shader_state_size = args->shader_record_count;

	ret = copy_from_user(bin, args->bin_cl, args->bin_cl_len);
	if (ret) {
		DRM_ERROR("Failed to copy in bin cl\n");
		goto fail;
	}

	ret = copy_from_user(render, args->render_cl, args->render_cl_len);
	if (ret) {
		DRM_ERROR("Failed to copy in render cl\n");
		goto fail;
	}

	ret = copy_from_user(shader_rec, args->shader_records,
			     args->shader_record_len);
	if (ret) {
		DRM_ERROR("Failed to copy in shader recs\n");
		goto fail;
	}

	exec->exec_bo = drm_gem_cma_create(dev, exec_size);
#if 0
	if (IS_ERR(exec->exec_bo)) {
		DRM_ERROR("Couldn't allocate BO for exec\n");
		ret = PTR_ERR(exec->exec_bo);
		exec->exec_bo = NULL;
		goto fail;
	}
#endif

	exec->ct0ca = exec->exec_bo->paddr + bin_offset;
	exec->ct0ea = exec->ct0ca + args->bin_cl_len;
	exec->ct1ca = exec->exec_bo->paddr + render_offset;
	exec->ct1ea = exec->ct1ca + args->render_cl_len;
	exec->shader_paddr = exec->exec_bo->paddr + shader_rec_offset;

	ret = vc4_validate_cl(dev,
			      exec->exec_bo->vaddr + bin_offset,
			      bin,
			      args->bin_cl_len,
			      true,
			      exec);
	if (ret)
		goto fail;

	ret = vc4_validate_cl(dev,
			      exec->exec_bo->vaddr + render_offset,
			      render,
			      args->render_cl_len,
			      false,
			      exec);
	if (ret)
		goto fail;

	ret = vc4_validate_shader_recs(dev,
				       exec->exec_bo->vaddr + shader_rec_offset,
				       shader_rec,
				       args->shader_record_len,
				       exec);

fail:
	kfree(temp);
	return ret;
}

int
vc4_simulator_flush(struct vc4_context *vc4, struct drm_vc4_submit_cl *args,
                    struct vc4_surface *csurf)
{
        struct vc4_resource *ctex = vc4_resource(csurf->base.texture);
        uint32_t winsys_stride = ctex->bo->simulator_winsys_stride;
        uint32_t sim_stride = ctex->slices[0].stride;
        uint32_t row_len = MIN2(sim_stride, winsys_stride);
        struct exec_info exec;
        struct drm_device local_dev = {
                .vc4 = vc4,
                .simulator_mem_next = 0,
        };
        struct drm_device *dev = &local_dev;
        int ret;

        memset(&exec, 0, sizeof(exec));

        if (ctex->bo->simulator_winsys_map) {
#if 0
                fprintf(stderr, "%dx%d %d %d %d\n",
                        ctex->base.b.width0, ctex->base.b.height0,
                        winsys_stride,
                        sim_stride,
                        ctex->bo->size);
#endif

                for (int y = 0; y < ctex->base.b.height0; y++) {
                        memcpy(ctex->bo->map + y * sim_stride,
                               ctex->bo->simulator_winsys_map + y * winsys_stride,
                               row_len);
                }
        }

        ret = vc4_simulator_pin_bos(dev, args, &exec);
        if (ret)
                return ret;

        ret = vc4_cl_validate(dev, args, &exec);
        if (ret)
                return ret;

        simpenrose_do_binning(exec.ct0ca, exec.ct0ea);
        simpenrose_do_rendering(exec.ct1ca, exec.ct1ea);

        ret = vc4_simulator_unpin_bos(args, &exec);
        if (ret)
                return ret;

        free(exec.exec_bo);

        if (ctex->bo->simulator_winsys_map) {
                for (int y = 0; y < ctex->base.b.height0; y++) {
                        memcpy(ctex->bo->simulator_winsys_map + y * winsys_stride,
                               ctex->bo->map + y * sim_stride,
                               row_len);
                }
        }

        return 0;
}

void
vc4_simulator_init(struct vc4_screen *screen)
{
        simpenrose_init_hardware();
        screen->simulator_mem_base = simpenrose_get_mem_start();
        screen->simulator_mem_size = simpenrose_get_mem_size();
}

#endif /* USE_VC4_SIMULATOR */