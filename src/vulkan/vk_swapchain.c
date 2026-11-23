#include "vk_swapchain.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jpt_port.h"

static void destroy_swapchain_images(jpt_vk_swapchain_t* sc) {
    jpt_vk_context_t* ctx = sc->ctx;
    const jpt_vk_loader_t* vk = &ctx->vk;

    for (uint32_t i = 0; i < sc->image_count; i++) {
        if (sc->framebuffers[i] != VK_NULL_HANDLE) {
            vk->vkDestroyFramebuffer(ctx->device, sc->framebuffers[i], NULL);
            sc->framebuffers[i] = VK_NULL_HANDLE;
            ctx->lifetime.framebuffers--;
        }
        if (sc->views[i] != VK_NULL_HANDLE) {
            vk->vkDestroyImageView(ctx->device, sc->views[i], NULL);
            sc->views[i] = VK_NULL_HANDLE;
            ctx->lifetime.image_views--;
        }
        if (sc->render_finished[i] != VK_NULL_HANDLE) {
            vk->vkDestroySemaphore(ctx->device, sc->render_finished[i], NULL);
            sc->render_finished[i] = VK_NULL_HANDLE;
            ctx->lifetime.semaphores--;
        }
        sc->images[i] = VK_NULL_HANDLE;
    }
    if (!ctx->headless) ctx->lifetime.swapchain_images -= (int32_t)sc->image_count;
    sc->image_count = 0;

    if (sc->headless_image != VK_NULL_HANDLE) {
        jpt_vk_destroy_image(&ctx->alloc, &sc->headless_image, &sc->headless_alloc);
    }
}

static int ensure_present_render_pass(jpt_vk_swapchain_t* sc, VkFormat format) {
    jpt_vk_context_t* ctx = sc->ctx;
    const jpt_vk_loader_t* vk = &ctx->vk;
    VkAttachmentDescription color;
    VkAttachmentReference ref;
    VkSubpassDescription subpass;
    VkSubpassDependency dep;
    VkRenderPassCreateInfo rpci;

    if (sc->present_pass != VK_NULL_HANDLE && sc->format == format) return 1;
    if (sc->present_pass != VK_NULL_HANDLE) {
        vk->vkDestroyRenderPass(ctx->device, sc->present_pass, NULL);
        sc->present_pass = VK_NULL_HANDLE;
        ctx->lifetime.render_passes--;
    }

    memset(&color, 0, sizeof color);
    color.format = format;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = ctx->headless ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    ref.attachment = 0;
    ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    memset(&subpass, 0, sizeof subpass);
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &ref;

    memset(&dep, 0, sizeof dep);
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    memset(&rpci, 0, sizeof rpci);
    rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpci.attachmentCount = 1;
    rpci.pAttachments = &color;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &subpass;
    rpci.dependencyCount = 1;
    rpci.pDependencies = &dep;

    if (vk->vkCreateRenderPass(ctx->device, &rpci, NULL, &sc->present_pass) != VK_SUCCESS) return 0;
    ctx->lifetime.render_passes++;
    return 1;
}

static int create_image_views_and_fbs(jpt_vk_swapchain_t* sc) {
    jpt_vk_context_t* ctx = sc->ctx;
    const jpt_vk_loader_t* vk = &ctx->vk;

    for (uint32_t i = 0; i < sc->image_count; i++) {
        VkImageViewCreateInfo ivci;
        VkFramebufferCreateInfo fbci;
        VkSemaphoreCreateInfo sci;

        memset(&ivci, 0, sizeof ivci);
        ivci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        ivci.image = sc->images[i];
        ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ivci.format = sc->format;
        ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        ivci.subresourceRange.levelCount = 1;
        ivci.subresourceRange.layerCount = 1;
        if (vk->vkCreateImageView(ctx->device, &ivci, NULL, &sc->views[i]) != VK_SUCCESS) return 0;
        ctx->lifetime.image_views++;

        memset(&fbci, 0, sizeof fbci);
        fbci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbci.renderPass = sc->present_pass;
        fbci.attachmentCount = 1;
        fbci.pAttachments = &sc->views[i];
        fbci.width = sc->extent.width;
        fbci.height = sc->extent.height;
        fbci.layers = 1;
        if (vk->vkCreateFramebuffer(ctx->device, &fbci, NULL, &sc->framebuffers[i]) != VK_SUCCESS) return 0;
        ctx->lifetime.framebuffers++;

        memset(&sci, 0, sizeof sci);
        sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if (vk->vkCreateSemaphore(ctx->device, &sci, NULL, &sc->render_finished[i]) != VK_SUCCESS) return 0;
        ctx->lifetime.semaphores++;
    }
    return 1;
}

static int build_headless_target(jpt_vk_swapchain_t* sc, uint32_t width, uint32_t height) {
    jpt_vk_context_t* ctx = sc->ctx;
    VkImageCreateInfo ici;

    if (width == 0 || height == 0) return 0;
    sc->format = VK_FORMAT_R8G8B8A8_UNORM;
    sc->color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    sc->present_mode = VK_PRESENT_MODE_FIFO_KHR;
    sc->extent.width = width;
    sc->extent.height = height;

    if (!ensure_present_render_pass(sc, sc->format)) return 0;

    memset(&ici, 0, sizeof ici);
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = sc->format;
    ici.extent.width = width;
    ici.extent.height = height;
    ici.extent.depth = 1;
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (jpt_vk_create_image(&ctx->alloc, &ici, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, &sc->headless_image,
                            &sc->headless_alloc) != VK_SUCCESS)
        return 0;

    sc->image_count = 1;
    sc->images[0] = sc->headless_image;
    return create_image_views_and_fbs(sc);
}

static int build_window_swapchain(jpt_vk_swapchain_t* sc, uint32_t width, uint32_t height, int vsync) {
    jpt_vk_context_t* ctx = sc->ctx;
    const jpt_vk_loader_t* vk = &ctx->vk;
    VkSurfaceCapabilitiesKHR caps;
    VkSurfaceFormatKHR formats[64];
    uint32_t fmt_count = 64;
    VkPresentModeKHR modes[16];
    uint32_t mode_count = 16;
    VkSurfaceFormatKHR chosen_fmt;
    VkPresentModeKHR chosen_mode;
    VkExtent2D extent;
    VkSwapchainCreateInfoKHR sci;
    VkSwapchainKHR old_sc = sc->swapchain;
    VkSwapchainKHR new_sc = VK_NULL_HANDLE;
    uint32_t qfamilies[2];
    VkResult vr;

    if (width == 0 || height == 0) return 0;
    vr = vk->vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx->physical_device, ctx->surface, &caps);
    if (vr != VK_SUCCESS) return 0;

    if (caps.currentExtent.width != 0xFFFFFFFFu) {
        extent = caps.currentExtent;
    } else {
        extent.width = width;
        extent.height = height;
        if (extent.width < caps.minImageExtent.width) extent.width = caps.minImageExtent.width;
        if (extent.width > caps.maxImageExtent.width) extent.width = caps.maxImageExtent.width;
        if (extent.height < caps.minImageExtent.height) extent.height = caps.minImageExtent.height;
        if (extent.height > caps.maxImageExtent.height) extent.height = caps.maxImageExtent.height;
    }
    if (extent.width == 0 || extent.height == 0) return 0;

    vk->vkGetPhysicalDeviceSurfaceFormatsKHR(ctx->physical_device, ctx->surface, &fmt_count, formats);
    vk->vkGetPhysicalDeviceSurfacePresentModesKHR(ctx->physical_device, ctx->surface, &mode_count, modes);

    chosen_fmt = jpt_vk_choose_surface_format(formats, fmt_count);
    chosen_mode = jpt_vk_choose_present_mode(vsync, modes, mode_count);

    if (!ensure_present_render_pass(sc, chosen_fmt.format)) return 0;

    memset(&sci, 0, sizeof sci);
    sci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    sci.surface = ctx->surface;
    sci.minImageCount = jpt_vk_choose_image_count_for(caps.minImageCount, caps.maxImageCount, chosen_mode, jpt_port_in_gamescope());
    sci.imageFormat = chosen_fmt.format;
    sci.imageColorSpace = chosen_fmt.colorSpace;
    sci.imageExtent = extent;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) sci.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

    if (ctx->graphics_family != ctx->present_family) {
        qfamilies[0] = ctx->graphics_family;
        qfamilies[1] = ctx->present_family;
        sci.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        sci.queueFamilyIndexCount = 2;
        sci.pQueueFamilyIndices = qfamilies;
    } else {
        sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    sci.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
                           ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                           : caps.currentTransform;
    if (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
        sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    else if (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR)
        sci.compositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    else
        sci.compositeAlpha = VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;

    sci.presentMode = chosen_mode;
    sci.clipped = VK_TRUE;
    sci.oldSwapchain = old_sc;

    vr = vk->vkCreateSwapchainKHR(ctx->device, &sci, NULL, &new_sc);
    if (vr != VK_SUCCESS) return 0;

    destroy_swapchain_images(sc);
    if (old_sc != VK_NULL_HANDLE) vk->vkDestroySwapchainKHR(ctx->device, old_sc, NULL);

    sc->swapchain = new_sc;
    sc->format = chosen_fmt.format;
    sc->color_space = chosen_fmt.colorSpace;
    sc->present_mode = chosen_mode;
    sc->extent = extent;

    sc->image_count = JPT_VK_MAX_SWAPCHAIN_IMAGES;
    vr = vk->vkGetSwapchainImagesKHR(ctx->device, sc->swapchain, &sc->image_count, sc->images);
    if (vr != VK_SUCCESS && vr != VK_INCOMPLETE) {
        sc->image_count = 0;
        return 0;
    }
    ctx->lifetime.swapchain_images += (int32_t)sc->image_count;
    return create_image_views_and_fbs(sc);
}

static int create_frame_sync(jpt_vk_swapchain_t* sc) {
    jpt_vk_context_t* ctx = sc->ctx;
    const jpt_vk_loader_t* vk = &ctx->vk;

    for (uint32_t i = 0; i < JPT_VK_FRAMES_IN_FLIGHT; i++) {
        VkCommandPoolCreateInfo cpci;
        VkCommandBufferAllocateInfo cbai;
        VkFenceCreateInfo fci;
        VkSemaphoreCreateInfo sci;

        memset(&cpci, 0, sizeof cpci);
        cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        cpci.queueFamilyIndex = ctx->graphics_family;
        cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        if (vk->vkCreateCommandPool(ctx->device, &cpci, NULL, &sc->frames[i].cmd_pool) != VK_SUCCESS) return 0;
        ctx->lifetime.command_pools++;

        memset(&cbai, 0, sizeof cbai);
        cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbai.commandPool = sc->frames[i].cmd_pool;
        cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 1;
        if (vk->vkAllocateCommandBuffers(ctx->device, &cbai, &sc->frames[i].cmd) != VK_SUCCESS) return 0;

        memset(&fci, 0, sizeof fci);
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (vk->vkCreateFence(ctx->device, &fci, NULL, &sc->frames[i].in_flight) != VK_SUCCESS) return 0;
        ctx->lifetime.fences++;

        memset(&sci, 0, sizeof sci);
        sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if (vk->vkCreateSemaphore(ctx->device, &sci, NULL, &sc->frames[i].image_available) != VK_SUCCESS) return 0;
        ctx->lifetime.semaphores++;
    }
    return 1;
}

int jpt_vk_swapchain_init(jpt_vk_swapchain_t* sc, jpt_vk_context_t* ctx, uint32_t width, uint32_t height, int vsync,
                          int fullscreen) {
    if (!sc || !ctx || !ctx->initialized) return 0;
    memset(sc, 0, sizeof *sc);
    sc->ctx = ctx;
    jpt_vk_sc_init(&sc->health, width, height, vsync, fullscreen);

    if (!create_frame_sync(sc)) {
        jpt_vk_swapchain_shutdown(sc);
        return 0;
    }
    if (!jpt_vk_swapchain_recreate(sc, width, height, vsync, fullscreen)) {
        jpt_vk_swapchain_shutdown(sc);
        return 0;
    }
    return 1;
}

int jpt_vk_swapchain_recreate(jpt_vk_swapchain_t* sc, uint32_t width, uint32_t height, int vsync, int fullscreen) {
    int ok;
    if (!sc || !sc->ctx || !sc->ctx->initialized) return 0;
    if (width == 0 || height == 0) {
        jpt_vk_sc_on_resize(&sc->health, 0, 0);
        return 0;
    }
    sc->ctx->vk.vkDeviceWaitIdle(sc->ctx->device);
    if (sc->ctx->headless) {
        destroy_swapchain_images(sc);
        ok = build_headless_target(sc, width, height);
    } else {
        ok = build_window_swapchain(sc, width, height, vsync);
    }
    if (ok) jpt_vk_sc_on_recreated(&sc->health, sc->extent.width, sc->extent.height, vsync, fullscreen);
    return ok;
}

void jpt_vk_swapchain_shutdown(jpt_vk_swapchain_t* sc) {
    jpt_vk_context_t* ctx;
    const jpt_vk_loader_t* vk;
    if (!sc || !sc->ctx) return;
    ctx = sc->ctx;
    vk = &ctx->vk;

    if (ctx->device != VK_NULL_HANDLE) vk->vkDeviceWaitIdle(ctx->device);
    destroy_swapchain_images(sc);

    if (sc->swapchain != VK_NULL_HANDLE) {
        vk->vkDestroySwapchainKHR(ctx->device, sc->swapchain, NULL);
        sc->swapchain = VK_NULL_HANDLE;
    }
    if (sc->present_pass != VK_NULL_HANDLE) {
        vk->vkDestroyRenderPass(ctx->device, sc->present_pass, NULL);
        sc->present_pass = VK_NULL_HANDLE;
        ctx->lifetime.render_passes--;
    }
    for (uint32_t i = 0; i < JPT_VK_FRAMES_IN_FLIGHT; i++) {
        if (sc->frames[i].image_available != VK_NULL_HANDLE) {
            vk->vkDestroySemaphore(ctx->device, sc->frames[i].image_available, NULL);
            sc->frames[i].image_available = VK_NULL_HANDLE;
            ctx->lifetime.semaphores--;
        }
        if (sc->frames[i].in_flight != VK_NULL_HANDLE) {
            vk->vkDestroyFence(ctx->device, sc->frames[i].in_flight, NULL);
            sc->frames[i].in_flight = VK_NULL_HANDLE;
            ctx->lifetime.fences--;
        }
        if (sc->frames[i].cmd_pool != VK_NULL_HANDLE) {
            vk->vkDestroyCommandPool(ctx->device, sc->frames[i].cmd_pool, NULL);
            sc->frames[i].cmd_pool = VK_NULL_HANDLE;
            ctx->lifetime.command_pools--;
        }
    }
    sc->frame_begun = 0;
}

jpt_vk_sc_action_t jpt_vk_swapchain_begin_frame(jpt_vk_swapchain_t* sc, VkCommandBuffer* out_cmd,
                                                VkFramebuffer* out_fb, VkExtent2D* out_extent) {
    jpt_vk_context_t* ctx = sc->ctx;
    const jpt_vk_loader_t* vk = &ctx->vk;
    jpt_vk_frame_sync_t* f = &sc->frames[sc->frame_index];
    VkCommandBufferBeginInfo bi;
    VkResult vr;

    if (sc->health.device_lost) return JPT_VK_SC_ACTION_DEVICE_LOST;
    if (sc->extent.width == 0 || sc->extent.height == 0 || sc->image_count == 0) return JPT_VK_SC_ACTION_WAIT_EXTENT;

    vr = vk->vkWaitForFences(ctx->device, 1, &f->in_flight, VK_TRUE, UINT64_MAX);
    if (vr == VK_ERROR_DEVICE_LOST) return jpt_vk_sc_on_result(&sc->health, vr);

    if (!ctx->headless) {
        vr = vk->vkAcquireNextImageKHR(ctx->device, sc->swapchain, UINT64_MAX, f->image_available, VK_NULL_HANDLE,
                                       &sc->acquired_image);
        if (vr != VK_SUCCESS && vr != VK_SUBOPTIMAL_KHR) {
            return jpt_vk_sc_on_result(&sc->health, vr);
        }
        if (vr == VK_SUBOPTIMAL_KHR) {
            jpt_vk_sc_action_t act = jpt_vk_sc_on_result(&sc->health, vr);
            if (act != JPT_VK_SC_ACTION_OK) {
                /* Still submit/present or recreate cleanly after this frame. */
                sc->health.dirty = 1;
            }
        }
    } else {
        sc->acquired_image = 0;
    }

    vk->vkResetFences(ctx->device, 1, &f->in_flight);
    vk->vkResetCommandPool(ctx->device, f->cmd_pool, 0);

    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vk->vkBeginCommandBuffer(f->cmd, &bi) != VK_SUCCESS) return JPT_VK_SC_ACTION_DEVICE_LOST;

    sc->frame_begun = 1;
    if (out_cmd) *out_cmd = f->cmd;
    if (out_fb) *out_fb = sc->framebuffers[sc->acquired_image];
    if (out_extent) *out_extent = sc->extent;
    return JPT_VK_SC_ACTION_OK;
}

jpt_vk_sc_action_t jpt_vk_swapchain_end_frame(jpt_vk_swapchain_t* sc) {
    jpt_vk_context_t* ctx = sc->ctx;
    const jpt_vk_loader_t* vk = &ctx->vk;
    jpt_vk_frame_sync_t* f = &sc->frames[sc->frame_index];
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si;
    VkResult vr;

    if (!sc->frame_begun) return JPT_VK_SC_ACTION_OK;
    sc->frame_begun = 0;

    vr = vk->vkEndCommandBuffer(f->cmd);
    if (vr != VK_SUCCESS) return jpt_vk_sc_on_result(&sc->health, vr);

    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &f->cmd;
    if (!ctx->headless) {
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &f->image_available;
        si.pWaitDstStageMask = &wait_stage;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &sc->render_finished[sc->acquired_image];
    }

    vr = vk->vkQueueSubmit(ctx->graphics_queue, 1, &si, f->in_flight);
    if (vr != VK_SUCCESS) return jpt_vk_sc_on_result(&sc->health, vr);

    if (!ctx->headless) {
        VkPresentInfoKHR pi;
        memset(&pi, 0, sizeof pi);
        pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &sc->render_finished[sc->acquired_image];
        pi.swapchainCount = 1;
        pi.pSwapchains = &sc->swapchain;
        pi.pImageIndices = &sc->acquired_image;
        vr = vk->vkQueuePresentKHR(ctx->present_queue, &pi);
        sc->frame_index = (sc->frame_index + 1u) % JPT_VK_FRAMES_IN_FLIGHT;
        jpt_vk_sc_action_t act = jpt_vk_sc_on_result(&sc->health, vr);
        if (act == JPT_VK_SC_ACTION_OK && sc->health.dirty) return JPT_VK_SC_ACTION_RECREATE;
        return act;
    }

    sc->frame_index = (sc->frame_index + 1u) % JPT_VK_FRAMES_IN_FLIGHT;
    return JPT_VK_SC_ACTION_OK;
}

int jpt_vk_swapchain_flush_frame(jpt_vk_swapchain_t* sc) {
    jpt_vk_context_t* ctx;
    const jpt_vk_loader_t* vk;
    jpt_vk_frame_sync_t* f;
    VkCommandBufferBeginInfo bi;
    VkSubmitInfo si;
    VkFenceCreateInfo fci;
    VkFence fence = VK_NULL_HANDLE;
    int ok = 0;

    if (!sc || !sc->ctx || !sc->frame_begun) return 0;
    ctx = sc->ctx;
    vk = &ctx->vk;
    f = &sc->frames[sc->frame_index];

    memset(&fci, 0, sizeof fci);
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (vk->vkEndCommandBuffer(f->cmd) != VK_SUCCESS) return 0;
    if (vk->vkCreateFence(ctx->device, &fci, NULL, &fence) == VK_SUCCESS) {
        memset(&si, 0, sizeof si);
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &f->cmd;
        if (vk->vkQueueSubmit(ctx->graphics_queue, 1, &si, fence) == VK_SUCCESS)
            ok = vk->vkWaitForFences(ctx->device, 1, &fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS;
        vk->vkDestroyFence(ctx->device, fence, NULL);
    }
    vk->vkResetCommandBuffer(f->cmd, 0);
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vk->vkBeginCommandBuffer(f->cmd, &bi) != VK_SUCCESS) return 0;
    return ok;
}

int jpt_vk_swapchain_readback_headless(jpt_vk_swapchain_t* sc, uint8_t* dst_rgba, size_t dst_bytes) {
    jpt_vk_context_t* ctx;
    const jpt_vk_loader_t* vk;
    VkDeviceSize needed;
    VkBuffer staging = VK_NULL_HANDLE;
    jpt_vk_allocation_t alloc;
    VkCommandBuffer cmd;
    VkCommandBufferBeginInfo bi;
    VkBufferImageCopy region;
    VkSubmitInfo si;
    jpt_vk_frame_sync_t* f;

    if (!sc || !sc->ctx || !sc->ctx->headless || !dst_rgba || sc->headless_image == VK_NULL_HANDLE) return 0;
    ctx = sc->ctx;
    vk = &ctx->vk;
    needed = (VkDeviceSize)sc->extent.width * (VkDeviceSize)sc->extent.height * 4u;
    if (dst_bytes < (size_t)needed) return 0;

    f = &sc->frames[sc->frame_index];
    vk->vkWaitForFences(ctx->device, 1, &f->in_flight, VK_TRUE, UINT64_MAX);
    vk->vkDeviceWaitIdle(ctx->device);

    if (jpt_vk_create_buffer(&ctx->alloc, needed, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                             VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 1, &staging, &alloc) != VK_SUCCESS)
        return 0;

    vk->vkResetFences(ctx->device, 1, &f->in_flight);
    vk->vkResetCommandPool(ctx->device, f->cmd_pool, 0);
    cmd = f->cmd;
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk->vkBeginCommandBuffer(cmd, &bi);

    memset(&region, 0, sizeof region);
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent.width = sc->extent.width;
    region.imageExtent.height = sc->extent.height;
    region.imageExtent.depth = 1;
    vk->vkCmdCopyImageToBuffer(cmd, sc->headless_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1, &region);

    vk->vkEndCommandBuffer(cmd);
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vk->vkQueueSubmit(ctx->graphics_queue, 1, &si, f->in_flight);
    vk->vkWaitForFences(ctx->device, 1, &f->in_flight, VK_TRUE, UINT64_MAX);

    if (alloc.mapped) memcpy(dst_rgba, alloc.mapped, (size_t)needed);
    jpt_vk_destroy_buffer(&ctx->alloc, &staging, &alloc);
    return 1;
}
