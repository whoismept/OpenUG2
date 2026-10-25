/* rhi_vk.c - Vulkan backend of rhi.h (issue #14, P11a slice).
 *
 * Offscreen v0: no swapchain. (P12a adds a window surface when the window can take one.) The "main target" is a single RGBA8
 * VkImage (plus a D16 depth image) sized to the window, which we clear and read
 * back. This slice implements only device/frame/readback/clear/caps; buffers,
 * textures, shaders, pipelines, draws and passes are stubs returning {0} /
 * RHI_ERR_UNSUPPORTED until the later P11b.. slices fill them in.
 *
 * The device/queue/offscreen-image/readback flow mirrors the proven,
 * validation-clean spike in ~/nfsu2/vk-spike/vk_triangle.c (its CHECK macro,
 * dbg_cb, find_mem and image->host-buffer copy); the g_vk singleton mirrors
 * g_rhi in rhi_gl.c and the semantics (caps, drawable size, read_pixels row
 * order/format, rhi_cmd_clear_main) match rhi_gl.c exactly. */
#include "rhi.h"
#include "generated_shaders_vk.h"   /* build-time SPIR-V for the src/shaders/vk shaders */
#include <SDL.h>
#include <SDL_vulkan.h>
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

/* Validation-message counter, like the spike. Non-static so the smoke test can
 * read it (no rhi.h surface for it); WARNING and above count as a failure. */
unsigned rhi_vk_validation_messages;

/* Mid-frame UBO-arena flushes so far (no rhi.h surface; the smoke reads it to
 * confirm the arena-overflow path actually fired). */
unsigned rhi_vk_arena_flushes;
static unsigned g_test_fail_pending_alloc;   /* test hook: make the next n growths of the deferred-free list fail */

#define VK_DEFAULT_W 1280
#define VK_DEFAULT_H 720

/* Buffer handle id = generation << 24 | (slot + 1), id 0 = no buffer, the same
 * scheme and bit layout as the GL backend's buffer table (rhi_gl.c). */
#define VK_SLOT_BITS 24u
#define VK_SLOT_MASK ((1u << VK_SLOT_BITS) - 1u)
#define VK_MAX_TIMERS 64u

typedef struct {
    VkBuffer           buf;         /* VK_NULL_HANDLE when the slot holds no storage */
    VkDeviceMemory     mem;
    void              *mapped;      /* persistent map for STREAM, NULL for STATIC */
    VkDeviceSize       size;        /* current storage size in bytes */
    VkBufferUsageFlags usage;       /* VERTEX or INDEX usage bit (no transfer bits) */
    uint8_t            update;      /* RHI_UPDATE_STATIC / RHI_UPDATE_STREAM */
    uint8_t            live;        /* 1 while the slot is allocated (STREAM size 0 keeps buf NULL) */
    uint8_t            generation;
    /* STREAM buffers updated between draws (hud pattern): see the stream arena. */
    uint8_t            in_use;      /* a recorded, unsubmitted draw reads the current version */
    uint8_t            in_arena;    /* the current version lives in g_vk.stream (copy-on-write) */
    VkDeviceSize       cur_off;     /* offset of that version in the stream arena */
} RhiVkBuffer;

/* Texture handle slot, same scheme as RhiVkBuffer. The whole image (every mip
 * level and array layer) is tracked by one `layout`: uploads/blits/readbacks
 * transition the full subresource range together, so it stays uniform. */
typedef struct {
    VkImage            img;         /* VK_NULL_HANDLE = free slot */
    VkDeviceMemory     mem;
    VkImageView        view;        /* default 2D/CUBE sampling (or depth) view */
    VkImageView        att_view[6]; /* lazy single-level, single-face 2D views for pass attachments */
    VkImageUsageFlags  vku;         /* image usage, kept to recreate the storage (copy resize) */
    VkFormat           vkfmt;
    VkImageAspectFlags aspect;      /* COLOR, or DEPTH for D16/DEPTH (view aspect) */
    VkImageAspectFlags bar_aspect;  /* aspect for barriers/clears: DEPTH|STENCIL for the
                                       combined main-precision RHI_FORMAT_DEPTH format */
    uint64_t           touched;     /* g_vk.submits + 1 when last referenced by the
                                       still-unsubmitted frame (see texture_touch) */
    VkImageLayout      layout;      /* current layout of all subresources */
    RhiFormat          format;
    uint32_t           usage;
    int32_t            width, height;
    uint32_t           layers;      /* 1 for 2D, 6 for CUBE */
    uint32_t           mip_levels;
    RhiSampler         sampler;     /* sampled with this (recorded; used in P11e) */
    uint8_t            live;
    uint8_t            generation;
} RhiVkTexture;

/* One parameter block = PLAIN host memory (the std140 "mirror") the game writes
 * params into via rhi_shader_params. The engine is immediate-mode (write params,
 * draw, write different params, draw, reusing one shader), and the backend defers
 * the whole frame into one command buffer, so each draw must snapshot the params
 * it was issued with -- the mirror is copied into the per-frame UBO arena
 * (g_vk.arena) at draw time (P11-review Fix 1). The mirror's layout equals the
 * RhiParamLayout, which rhi_param_layout_check enforces. */
#define VK_MAX_PARAM_BLOCKS 4u
#define VK_PARAM_BLOCK_MAX (64u * 1024u)   /* largest parameter block accepted (the device's uniform-buffer range may be lower) */
typedef struct {
    void          *mirror;   /* calloc'd, size = layout->size; freed on shader destroy */
    uint32_t       size;
    const RhiParamLayout *layout;
} RhiVkParamBlock;

typedef struct {
    VkShaderModule  vert, frag;     /* VK_NULL_HANDLE = free slot */
    RhiVkParamBlock blocks[VK_MAX_PARAM_BLOCKS];
    uint32_t        block_count;
    uint32_t        attrib_count;   /* RhiShaderDesc.attribute_count: locations 0..n-1 exist */
    uint32_t        cube_mask;      /* bit n: sampler slot n is a samplerCube */
    uint8_t         live;
    uint8_t         generation;
} RhiVkShader;

/* One created VkGraphicsPipeline. Deduped by value like the GL backend: equal
 * RhiPipelineDesc -> same handle (index + 1). The desc is kept for dedup and so
 * a draw can walk the vertex layout to bind its streams; every pipeline shares
 * g_vk.pipe_layout / g_vk.set_layout (push descriptors), so only the VkPipeline
 * object is per-entry. Pipelines are never destroyed individually (the GL
 * backend never retires them either). */
/* Pipeline variants, one per render-target format combination. */
enum { VK_PV_MAIN = 0,        /* main target: colour RGBA8 + depth_fmt (depth AND stencil) */
       VK_PV_PASS_D16,        /* offscreen pass: RGBA8 + D16 depth, no stencil */
       VK_PV_PASS_NODEPTH,    /* offscreen pass: RGBA8 only */
       VK_PV_COUNT };

/* A pipeline is built per (declared-but-unbound vertex streams, polygon mode, target format): index = ((absent * 2 + wire) * VK_PV_COUNT + v). */
#define VK_VARIANTS ((1u << RHI_MAX_VERTEX_STREAMS) * 2u * VK_PV_COUNT)

typedef struct {
    VkPipeline      variant[VK_VARIANTS];   /* MAIN fill with every stream bound is built eagerly, the rest on first use */
    RhiShader       shader;         /* the desc's shader, re-checked at bind/draw */
    RhiPipelineDesc desc;
} RhiVkPipeline;

static struct {
    SDL_Window              *window;
    VkInstance               instance;
    VkDebugUtilsMessengerEXT messenger;
    VkSurfaceKHR             surface;      /* present mode only (P12a: created, not yet presented to) */
    int                      present;      /* 1 = window surface + present-capable queue, 0 = offscreen */
    int                      vsync;        /* RHI_DEVICE_VSYNC: FIFO, else MAILBOX/IMMEDIATE when offered */
    /* Swapchain (present mode). The game renders into the offscreen colour image above; frame_end
     * blits it (y-flipped) into the acquired swapchain image and presents. */
    VkSwapchainKHR           sc;
    VkImage                 *sc_img;       /* sc_n entries, owned by the swapchain */
    VkSemaphore             *sc_render_sem; /* sc_n entries, one per image: present may outlive the fence */
    uint32_t                 sc_n;
    VkExtent2D               sc_ext;
    VkFormat                 sc_fmt;
    int                      sc_dirty;     /* out of date / suboptimal: recreate before the next present */
    VkSemaphore              acquire_sem;
    VkPhysicalDevice         phys;
    VkDevice                 device;
    uint32_t                 queue_family;
    VkQueue                  queue;
    VkCommandPool            pool;
    VkCommandBuffer          cmd;        /* one primary buffer, re-used per frame */
    VkFence                  fence;      /* frame submit completion */

    VkImage        color;                /* offscreen RGBA8 main target */
    VkDeviceMemory color_mem;
    VkImageView    color_view;           /* colour attachment view for dynamic rendering */
    VkImageLayout  color_layout;         /* tracked so barriers are minimal/clean */
    VkImage        depth;                /* D16 depth target */
    VkDeviceMemory depth_mem;
    VkImageView    depth_view;           /* depth attachment view */
    VkImageLayout  depth_layout;         /* tracked across clear/render */
    VkFormat       depth_fmt;            /* D24_UNORM_S8_UINT, else D32_SFLOAT_S8_UINT */
    int            wireframe;            /* rhi_cmd_set_wireframe: triangle pipelines draw as edges */
    int            host_query_reset;     /* hostQueryReset enabled (timers need it) */
    VkQueryPool    timer_pool;           /* 2 timestamps per timer slot */
    struct {                             /* GPU timers: fixed table, handle = generation << 24 | (slot + 1) */
        uint8_t  live, generation;
        uint8_t  state;                  /* 0 never measured, 1 running, 2 ended (result may be pending) */
        uint64_t unsubmitted;            /* g_vk.submits + 1 while its stamps sit in the open recording */
    } timers[VK_MAX_TIMERS];
    int            timer_running;        /* slot of the running timer, -1 none */
    int            target_variant;       /* pipeline variant for the current target (P11f-2 switches it) */
    uint64_t       submits;              /* frame command-buffer submits so far */

    int            rendering;            /* inside a vkCmdBeginRendering scope */

    /* Render target. Main unless a pass is open (rhi_cmd_begin_pass). The first
     * scope of a pass uses the desc's load ops; scopes reopened after a
     * frame_split use LOAD (`opened`). */
    struct {
        int        active;
        RhiTexture color, depth;     /* depth.id == 0: no depth attachment */
        int32_t    face;
        int32_t    w, h;
        int        clear_color, clear_depth;
        float      clear_value[4];
        float      clear_z;
        int        opened;
    } pass;

    VkBuffer       readback;             /* host-visible, full-frame sized */
    VkDeviceMemory readback_mem;

    /* 1x1 white dummy texture: written to every sampler binding a draw does not
     * bind, so no push-descriptor image slot is left undefined. */
    VkImage        dummy_img;
    VkDeviceMemory dummy_mem;
    VkImageView    dummy_view;
    VkSampler      dummy_sampler;
    /* 1x1x6 cube dummy (opaque black, like an incomplete GL cube) for samplerCube
     * bindings a draw leaves unbound. */
    VkImage        dummy_cube_img;
    VkDeviceMemory dummy_cube_mem;
    VkImageView    dummy_cube_view;
    /* Zero-filled vertex buffer bound to a pipeline stream a draw leaves unbound
     * (the GL backend disables the attribute; Vulkan has no such concept, so the
     * shader reads zeros from this instead). */
    VkBuffer       dummy_vbuf;
    VkDeviceMemory dummy_vbuf_mem;
    uint32_t       last_stride[RHI_MAX_VERTEX_STREAMS];   /* test hook: binding stride per stream of the last draw */
    uint32_t       build_absent_offset;                   /* test hook: largest attribute offset on an absent stream in the last pipeline built */
    uint32_t       draw_absent;                           /* streams the current draw leaves without a usable vertex buffer */
    /* One vec4 (0,0,0,1): the value a shader reads for an attribute its pipeline
     * layout does not name (GL's default generic attribute value). Bound on the
     * extra binding index RHI_MAX_VERTEX_STREAMS with stride 0. */
    VkBuffer       dummy_attr_buf;
    VkDeviceMemory dummy_attr_mem;

    /* Shared push-descriptor set layout + pipeline layout (identical for every
     * pipeline: no push constants, one set). */
    VkDescriptorSetLayout         set_layout;
    VkPipelineLayout              pipe_layout;
    PFN_vkCmdPushDescriptorSetKHR push_desc;
    int                           wide_lines;   /* wideLines feature enabled */

    struct {                             /* pipeline table, deduped by value */
        RhiVkPipeline *slots;
        uint32_t       count, capacity;
    } pipelines;

    /* Draw-time bind state, the shape of the GL backend's g_cmd. */
    int        bound_pipeline;           /* index into pipelines, -1 = none */
    RhiBuffer  bound_vertex[RHI_MAX_VERTEX_STREAMS];
    uint32_t   bound_vertex_offset[RHI_MAX_VERTEX_STREAMS];
    RhiBuffer  bound_index;
    /* Like GL, a unit has separate 2D and CUBE bindings. */
    RhiTexture bound_2d[RHI_TEXTURE_SLOTS];
    RhiTexture bound_cube[RHI_TEXTURE_SLOTS];
    int32_t    viewport[4];              /* GL-convention x, y, w, h (origin bottom-left) */

    RhiCaps  caps;
    int32_t  width, height;              /* offscreen target size (pixels) */
    uint64_t frame_index;
    int      in_frame;
    int      recording;                  /* cmd buffer is mid-recording */

    struct {                             /* buffer handle table, shape of GL's g_buffers */
        RhiVkBuffer *slots;
        uint32_t     count, capacity;
        uint32_t    *free_list;
        uint32_t     free_count;
        unsigned     stale_logged;
    } buffers;
    struct {                             /* texture handle table, same shape */
        RhiVkTexture *slots;
        uint32_t      count, capacity;
        uint32_t     *free_list;
        uint32_t      free_count;
        unsigned      stale_logged;
    } textures;
    struct {                             /* sampler list, searched by value (GL's g_samplers) */
        RhiSamplerDesc *descs;
        VkSampler      *handles;
        uint32_t        count, capacity;
    } samplers;
    struct {                             /* shader handle table, same shape as buffers */
        RhiVkShader *slots;
        uint32_t     count, capacity;
        uint32_t    *free_list;
        uint32_t     free_count;
        unsigned     stale_logged;
    } shaders;

    /* Per-frame UBO arena (P11-review Fix 1): one host-visible+coherent buffer,
     * persistently mapped. Each draw aligns `offset` up to min_ubo_align, copies
     * its shader block-0 mirror in and push-binds {buf, offset, size}. `demand`
     * accumulates the frame's total aligned byte demand ignoring mid-frame
     * flushes, so frame_begin can grow the arena if a frame overflowed it. */
    struct {
        VkBuffer       buf;
        VkDeviceMemory mem;
        void          *mapped;
        VkDeviceSize   size;     /* current capacity */
        VkDeviceSize   offset;   /* bump pointer within the current segment */
        VkDeviceSize   demand;   /* total aligned demand this frame (flush-agnostic) */
    } arena;
    VkDeviceSize min_ubo_align;  /* minUniformBufferOffsetAlignment */
    uint32_t     max_ubo_range;  /* maxUniformBufferRange */

    /* Stream arena: versions of STREAM buffers updated while a recorded draw still
     * reads the old contents. Everything is recorded and submitted at frame_end (or
     * a split), but the engine updates in place between draws, so an update to an
     * in-use STREAM buffer copies the buffer here and writes the copy; draws bind
     * the current version. flush() copies the versions back to the buffers' own
     * storage and restarts the arena. demand grows it at frame_begin like `arena`. */
    struct {
        VkBuffer       buf;
        VkDeviceMemory mem;
        unsigned char *mapped;
        VkDeviceSize   size, offset, demand;
    } stream;

    /* Buffers whose storage a recorded-but-unsubmitted draw may still reference;
     * freed at frame_end after the submit+fence wait (P11-review Fix 2). */
    struct {
        struct { VkBuffer buf; VkDeviceMemory mem; VkImage img; VkImageView view; } *items;
        uint32_t count, capacity;
    } pending_free;
} g_vk;

/* -------------------------------------------------------------------------- */
/* Helpers                                                                    */
/* -------------------------------------------------------------------------- */
static VKAPI_ATTR VkBool32 VKAPI_CALL dbg_cb(VkDebugUtilsMessageSeverityFlagBitsEXT sev,
    VkDebugUtilsMessageTypeFlagsEXT type, const VkDebugUtilsMessengerCallbackDataEXT *d, void *u) {
    (void)type; (void)u;
    if (sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        rhi_vk_validation_messages++;
        fprintf(stderr, "[vk validation %s] %s\n",
                sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ? "ERROR" : "WARN", d->pMessage);
    }
    return VK_FALSE;
}

/* Log and translate a VkResult; returns 1 on VK_SUCCESS. */
static int vk_ok(VkResult r, const char *what) {
    if (r == VK_SUCCESS) return 1;
    fprintf(stderr, "rhi_vk: %s failed: %d\n", what, (int)r);
    return 0;
}

/* First memory type in `bits` with every `want` property, UINT32_MAX if none. */
static uint32_t find_mem(uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(g_vk.phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

/* Create an image + device-local memory, binding them. 1 on success. */
static int make_image_ex(VkFormat fmt, uint32_t w, uint32_t h, uint32_t layers, VkImageCreateFlags flags,
                         VkImageUsageFlags usage, VkImage *img, VkDeviceMemory *mem) {
    VkImageCreateInfo ci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, flags, VK_IMAGE_TYPE_2D, fmt,
        { w, h, 1 }, 1, layers, VK_SAMPLE_COUNT_1_BIT,
        VK_IMAGE_TILING_OPTIMAL, usage, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED };
    if (!vk_ok(vkCreateImage(g_vk.device, &ci, NULL, img), "vkCreateImage")) return 0;
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(g_vk.device, *img, &mr);
    uint32_t mt = find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt == UINT32_MAX) { fprintf(stderr, "rhi_vk: no device-local memory type\n"); return 0; }
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, mt };
    if (!vk_ok(vkAllocateMemory(g_vk.device, &ai, NULL, mem), "vkAllocateMemory(image)")) return 0;
    return vk_ok(vkBindImageMemory(g_vk.device, *img, *mem, 0), "vkBindImageMemory");
}

static int make_image(VkFormat fmt, VkImageUsageFlags usage, VkImage *img, VkDeviceMemory *mem) {
    return make_image_ex(fmt, (uint32_t)g_vk.width, (uint32_t)g_vk.height, 1, 0, usage, img, mem);
}

/* The main depth image carries stencil too, and a barrier/clear on an image with
 * both aspects must name both. */
#define VK_DS_ASPECT (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)

/* D24S8 if the device can use it as the main depth-stencil target (attachment,
 * copy source/destination, sampled), else D32S8; UNDEFINED if neither. */
static VkFormat pick_depth_format(void) {
    static const VkFormat cands[2] = { VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT };
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
        VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    /* RHI_VK_DEPTH_FORMAT=d32s8 (debug): skip D24S8 so the fallback path can be exercised
     * on drivers that support both. */
    const char *force = getenv("RHI_VK_DEPTH_FORMAT");
    int first = force && !strcmp(force, "d32s8") ? 1 : 0;
    for (int i = first; i < 2; i++) {
        VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(g_vk.phys, cands[i], &fp);
        if ((fp.optimalTilingFeatures & need) == need) {
            if (force) fprintf(stderr, "rhi_vk: main depth format %s\n", i ? "D32_SFLOAT_S8_UINT" : "D24_UNORM_S8_UINT");
            return cands[i];
        }
    }
    return VK_FORMAT_UNDEFINED;
}

/* Record an image layout transition covering a single colour or depth level. */
static void image_barrier(VkImage img, VkImageAspectFlags aspect, VkImageLayout old, VkImageLayout neu,
                          VkAccessFlags src_access, VkAccessFlags dst_access,
                          VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, src_access, dst_access,
        old, neu, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, img, { aspect, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(g_vk.cmd, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
}

/* One-shot command helpers + a multi-subresource barrier, all defined with the
 * texture code further down but used already by device_create's dummy texture. */
static void drain_pending_frees(void);
static void stream_after_submit(void);
static VkCommandBuffer oneshot_begin(void);
static int oneshot_end(VkCommandBuffer cb);
static void img_barrier(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect,
                        uint32_t base_lvl, uint32_t n_lvl, uint32_t base_layer, uint32_t n_layer,
                        VkImageLayout old, VkImageLayout neu, VkAccessFlags srcA, VkAccessFlags dstA,
                        VkPipelineStageFlags srcS, VkPipelineStageFlags dstS);

/* Fresh recording on the frame command buffer if not already open. */
static int ensure_recording(void) {
    if (g_vk.recording) return 1;
    if (!vk_ok(vkResetCommandBuffer(g_vk.cmd, 0), "vkResetCommandBuffer")) return 0;
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
        VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL };
    if (!vk_ok(vkBeginCommandBuffer(g_vk.cmd, &bi), "vkBeginCommandBuffer")) return 0;
    g_vk.recording = 1;
    return 1;
}

/* End, submit and wait for the frame command buffer. `wait`/`signal` (optional) are the swapchain
 * acquire / render-finished semaphores of a presenting frame_end. */
static int flush_sync(VkSemaphore wait, VkSemaphore signal) {
    if (!g_vk.recording) return 1;
    if (!vk_ok(vkEndCommandBuffer(g_vk.cmd), "vkEndCommandBuffer")) return 0;
    g_vk.recording = 0;
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;   /* the blit into the swapchain image */
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, wait ? 1u : 0u, wait ? &wait : NULL,
                        wait ? &wait_stage : NULL, 1, &g_vk.cmd, signal ? 1u : 0u, signal ? &signal : NULL };
    if (!vk_ok(vkQueueSubmit(g_vk.queue, 1, &si, g_vk.fence), "vkQueueSubmit")) return 0;
    g_vk.submits++;
    if (!vk_ok(vkWaitForFences(g_vk.device, 1, &g_vk.fence, VK_TRUE, UINT64_MAX), "vkWaitForFences")) return 0;
    drain_pending_frees();   /* everything recorded so far has completed */
    stream_after_submit();
    return vk_ok(vkResetFences(g_vk.device, 1, &g_vk.fence), "vkResetFences");
}
static int flush(void) { return flush_sync(VK_NULL_HANDLE, VK_NULL_HANDLE); }

/* Orientation: the engine's shaders are authored for GL clip space (+y up) and
 * rhi_read_pixels (P11a) flips image rows to match GL's bottom-left glReadPixels.
 * Vulkan NDC is +y down, so a plain POSITIVE-height viewport sends a GL "+y"
 * vertex to the bottom of image memory; read_pixels' flip then returns it at the
 * top -- i.e. identical to the GL backend. (A negative-height viewport would
 * double-flip with that readback.) This reflects y, so winding flips and the
 * pipeline uses VK_FRONT_FACE_CLOCKWISE. So draws use a positive-height viewport whose
 * VkViewport.y is the GL y directly (NOT H-(y+h)): the full target is unchanged
 * (gl_y = 0 either way), and a partial GL viewport lands where GL puts it once
 * read_pixels flips rows. The readback is as committed.
 *
 * Open a dynamic-rendering scope on the frame command buffer if not already in
 * one. Draws need it; transfers (clear, readback, copies) must not be inside it.
 * loadOp = LOAD keeps whatever a prior clear or draw left in the attachments, so
 * the colour image transitions from its current layout (TRANSFER_SRC after a
 * clear) to COLOR_ATTACHMENT_OPTIMAL, the depth from its current to
 * DEPTH_ATTACHMENT_OPTIMAL. 1 on success. */
static RhiVkTexture *texture_find(RhiTexture tex);
static VkImageView texture_att_view(RhiVkTexture *t, int face);
static void texture_touch(RhiVkTexture *t);

/* Size of the current render target (main frame or the open pass's colour). */
static int32_t target_width(void)  { return g_vk.pass.active ? g_vk.pass.w : g_vk.width; }
static int32_t target_height(void) { return g_vk.pass.active ? g_vk.pass.h : g_vk.height; }

static int ensure_rendering(void) {
    if (g_vk.rendering) return 1;
    if (!ensure_recording()) return 0;
    VkRenderingAttachmentInfo ca, da, sa;
    const VkRenderingAttachmentInfo *pda = NULL, *psa = NULL;
    if (!g_vk.pass.active) {
        image_barrier(g_vk.color, VK_IMAGE_ASPECT_COLOR_BIT, g_vk.color_layout,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT,
                      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        g_vk.color_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        /* Depth: carry the depth stages/access on BOTH sides so a scope resuming
         * after a transfer (e.g. read_pixels mid-frame) synchronises the previous
         * scope's depth writes to this scope's depth tests, not just a prior clear's
         * transfer write (P11-review Fix 3). */
        image_barrier(g_vk.depth, VK_DS_ASPECT, g_vk.depth_layout,
                      VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                      VK_ACCESS_TRANSFER_WRITE_BIT |
                          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT |
                          VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                      VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT);
        g_vk.depth_layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        ca = (VkRenderingAttachmentInfo){ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, NULL, g_vk.color_view,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_RESOLVE_MODE_NONE, VK_NULL_HANDLE,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE, {{{0}}} };
        da = (VkRenderingAttachmentInfo){ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, NULL, g_vk.depth_view,
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_RESOLVE_MODE_NONE, VK_NULL_HANDLE,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE, {{{0}}} };
        sa = da;   /* same combined view feeds the stencil attachment */
        pda = &da; psa = &sa;
    } else {
        /* Offscreen pass: attachments are single-face views of the pass textures,
         * transitioned from their tracked layouts (the whole image, so the other
         * faces of a cube keep their contents). The FIRST scope of the pass honours
         * the desc's load ops; scopes reopened after a frame_split resume with LOAD. */
        RhiVkTexture *c = texture_find(g_vk.pass.color);
        RhiVkTexture *d = g_vk.pass.depth.id ? texture_find(g_vk.pass.depth) : NULL;
        if (!c || (g_vk.pass.depth.id && !d)) return 0;
        /* Every (re)opened scope references the attachments in the CURRENT recording
         * (the stamp from begin_pass goes stale across a frame_split). */
        texture_touch(c);
        if (d) texture_touch(d);
        VkImageView cv = texture_att_view(c, g_vk.pass.face);
        VkImageView dv = d ? texture_att_view(d, 0) : VK_NULL_HANDLE;
        if (!cv || (d && !dv)) return 0;
        img_barrier(g_vk.cmd, c->img, VK_IMAGE_ASPECT_COLOR_BIT, 0, c->mip_levels, 0, c->layers,
                    c->layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT,
                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                        VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        c->layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        if (d) {
            img_barrier(g_vk.cmd, d->img, d->bar_aspect, 0, 1, 0, 1,
                        d->layout, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                        VK_ACCESS_TRANSFER_WRITE_BIT |
                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT |
                            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT);
            d->layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        }
        int first = !g_vk.pass.opened;
        int clear_c = first && g_vk.pass.clear_color, clear_d = first && g_vk.pass.clear_depth;
        VkClearValue cc, cd;
        memset(&cc, 0, sizeof cc); memset(&cd, 0, sizeof cd);
        memcpy(cc.color.float32, g_vk.pass.clear_value, sizeof cc.color.float32);
        cd.depthStencil.depth = g_vk.pass.clear_z;
        ca = (VkRenderingAttachmentInfo){ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, NULL, cv,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_RESOLVE_MODE_NONE, VK_NULL_HANDLE,
            VK_IMAGE_LAYOUT_UNDEFINED, clear_c ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD,
            VK_ATTACHMENT_STORE_OP_STORE, cc };
        if (d) {
            da = (VkRenderingAttachmentInfo){ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, NULL, dv,
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_RESOLVE_MODE_NONE, VK_NULL_HANDLE,
                VK_IMAGE_LAYOUT_UNDEFINED, clear_d ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD,
                VK_ATTACHMENT_STORE_OP_STORE, cd };
            pda = &da;
        }
        g_vk.pass.opened = 1;
    }
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO, NULL, 0,
        { { 0, 0 }, { (uint32_t)target_width(), (uint32_t)target_height() } }, 1, 0, 1, &ca, pda, psa };
    vkCmdBeginRendering(g_vk.cmd, &ri);
    g_vk.rendering = 1;
    return 1;
}

/* Close any open rendering scope. For the main target the colour image is left in
 * TRANSFER_SRC so a following readback/transfer needs no extra barrier; a pass
 * scope just ends (rhi_cmd_end_pass does the transitions, and a frame_split
 * resumes the same scope later). No-op when not rendering. */
static void end_rendering_if_active(void) {
    if (!g_vk.rendering) return;
    vkCmdEndRendering(g_vk.cmd);
    g_vk.rendering = 0;
    if (g_vk.pass.active) return;
    image_barrier(g_vk.color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    g_vk.color_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    /* Make this scope's depth writes available to whatever resumes depth next (a
     * following clear or the next rendering scope). Layout is unchanged -- only a
     * memory/execution dependency on the depth stages/access (P11-review Fix 3). */
    image_barrier(g_vk.depth, VK_DS_ASPECT, g_vk.depth_layout, g_vk.depth_layout,
                  VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                  VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                  VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                  VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT);
}

/* Create a VkBuffer of `size` bytes with `usage`, back it with memory having
 * every `props` property and bind them. Cleans up on any failure. 1 on success. */
static int make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props,
                       VkBuffer *buf, VkDeviceMemory *mem) {
    *buf = VK_NULL_HANDLE; *mem = VK_NULL_HANDLE;
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size, usage,
        VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
    if (!vk_ok(vkCreateBuffer(g_vk.device, &bci, NULL, buf), "vkCreateBuffer")) return 0;
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(g_vk.device, *buf, &mr);
    uint32_t mt = find_mem(mr.memoryTypeBits, props);
    if (mt == UINT32_MAX) {
        fprintf(stderr, "rhi_vk: no suitable buffer memory type\n");
        vkDestroyBuffer(g_vk.device, *buf, NULL); *buf = VK_NULL_HANDLE; return 0;
    }
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, mt };
    if (!vk_ok(vkAllocateMemory(g_vk.device, &ai, NULL, mem), "vkAllocateMemory(buffer)")) {
        vkDestroyBuffer(g_vk.device, *buf, NULL); *buf = VK_NULL_HANDLE; return 0;
    }
    if (!vk_ok(vkBindBufferMemory(g_vk.device, *buf, *mem, 0), "vkBindBufferMemory")) {
        vkFreeMemory(g_vk.device, *mem, NULL); vkDestroyBuffer(g_vk.device, *buf, NULL);
        *buf = VK_NULL_HANDLE; *mem = VK_NULL_HANDLE; return 0;
    }
    return 1;
}

/* One-shot `vkCmdCopyBuffer` of `size` bytes src[src_off] -> dst[dst_off] on a
 * temporary command buffer, blocking on a fence until it completes. The device
 * queue serialises this with any frame submit, so it is safe mid-frame. 1 ok. */
static int copy_buffer(VkBuffer src, VkDeviceSize src_off, VkBuffer dst, VkDeviceSize dst_off,
                       VkDeviceSize size) {
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, g_vk.pool,
        VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
    if (!vk_ok(vkAllocateCommandBuffers(g_vk.device, &cbai, &cb), "vkAllocateCommandBuffers(copy)")) return 0;
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
        VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL };
    int ok = vk_ok(vkBeginCommandBuffer(cb, &bi), "vkBeginCommandBuffer(copy)");
    if (ok) {
        VkBufferCopy region = { src_off, dst_off, size };
        vkCmdCopyBuffer(cb, src, dst, 1, &region);
        ok = vk_ok(vkEndCommandBuffer(cb), "vkEndCommandBuffer(copy)");
    }
    VkFence fence = VK_NULL_HANDLE;
    if (ok) {
        VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, 0 };
        ok = vk_ok(vkCreateFence(g_vk.device, &fci, NULL, &fence), "vkCreateFence(copy)");
    }
    if (ok) {
        VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL };
        ok = vk_ok(vkQueueSubmit(g_vk.queue, 1, &si, fence), "vkQueueSubmit(copy)") &&
             vk_ok(vkWaitForFences(g_vk.device, 1, &fence, VK_TRUE, UINT64_MAX), "vkWaitForFences(copy)");
    }
    if (fence) vkDestroyFence(g_vk.device, fence, NULL);
    vkFreeCommandBuffers(g_vk.device, g_vk.pool, 1, &cb);
    return ok;
}

/* Upload `size` bytes of `data` into device-local `dst` at `dst_off` through a
 * temporary host-visible staging buffer and a one-shot copy. 1 on success. */
static int stage_to_device(VkBuffer dst, VkDeviceSize dst_off, const void *data, VkDeviceSize size) {
    VkBuffer staging; VkDeviceMemory smem;
    if (!make_buffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     &staging, &smem)) return 0;
    void *map = NULL;
    int ok = vk_ok(vkMapMemory(g_vk.device, smem, 0, VK_WHOLE_SIZE, 0, &map), "vkMapMemory(staging)");
    if (ok) {
        memcpy(map, data, size);
        vkUnmapMemory(g_vk.device, smem);
        ok = copy_buffer(staging, 0, dst, dst_off, size);
    }
    vkDestroyBuffer(g_vk.device, staging, NULL);
    vkFreeMemory(g_vk.device, smem, NULL);
    return ok;
}

/* -------------------------------------------------------------------------- */
/* Per-frame UBO arena + deferred buffer frees (P11-review fixes 1 & 2)       */
/* -------------------------------------------------------------------------- */
#define VK_ARENA_INITIAL (4u * 1024u * 1024u)   /* a few MB; grows if a frame overflows */

/* (Re)allocate the arena buffer at `size` bytes, host-visible+coherent and
 * persistently mapped. Resets the bump pointer. 1 on success. */
static int arena_alloc(VkDeviceSize size) {
    if (!make_buffer(size, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     &g_vk.arena.buf, &g_vk.arena.mem)) return 0;
    if (!vk_ok(vkMapMemory(g_vk.device, g_vk.arena.mem, 0, VK_WHOLE_SIZE, 0, &g_vk.arena.mapped),
               "vkMapMemory(arena)")) {
        vkDestroyBuffer(g_vk.device, g_vk.arena.buf, NULL);
        vkFreeMemory(g_vk.device, g_vk.arena.mem, NULL);
        g_vk.arena.buf = VK_NULL_HANDLE; g_vk.arena.mem = VK_NULL_HANDLE;
        return 0;
    }
    g_vk.arena.size = size;
    g_vk.arena.offset = 0;
    return 1;
}

static void arena_free(void) {
    if (g_vk.arena.mapped) { vkUnmapMemory(g_vk.device, g_vk.arena.mem); g_vk.arena.mapped = NULL; }
    if (g_vk.arena.buf) vkDestroyBuffer(g_vk.device, g_vk.arena.buf, NULL);
    if (g_vk.arena.mem) vkFreeMemory(g_vk.device, g_vk.arena.mem, NULL);
    g_vk.arena.buf = VK_NULL_HANDLE; g_vk.arena.mem = VK_NULL_HANDLE; g_vk.arena.size = 0;
}

/* Mid-frame split: close the render scope, submit + wait the recorded commands
 * (which also drains deferred frees) and start a fresh recording. Image layouts
 * are tracked, and every draw re-binds its pipeline/viewport/resources, and the
 * next ensure_rendering resumes the target with loadOp LOAD, so nothing drawn so
 * far is lost. Used when a CPU-side operation must observe (or precede) work the
 * unsubmitted frame already holds. 1 on success. */
static int frame_split(void) {
    end_rendering_if_active();
    if (!flush()) return 0;
    return g_vk.in_frame ? ensure_recording() : 1;
}

#define VK_STREAM_ARENA_INITIAL (8u * 1024u * 1024u)

static int stream_arena_alloc(VkDeviceSize size) {
    if (!make_buffer(size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     &g_vk.stream.buf, &g_vk.stream.mem)) return 0;
    void *map = NULL;
    if (!vk_ok(vkMapMemory(g_vk.device, g_vk.stream.mem, 0, VK_WHOLE_SIZE, 0, &map), "vkMapMemory(stream arena)")) {
        vkDestroyBuffer(g_vk.device, g_vk.stream.buf, NULL);
        vkFreeMemory(g_vk.device, g_vk.stream.mem, NULL);
        g_vk.stream.buf = VK_NULL_HANDLE; g_vk.stream.mem = VK_NULL_HANDLE;
        return 0;
    }
    g_vk.stream.mapped = (unsigned char *)map;
    g_vk.stream.size = size;
    g_vk.stream.offset = 0;
    return 1;
}

static void stream_arena_free(void) {
    if (g_vk.stream.mapped) { vkUnmapMemory(g_vk.device, g_vk.stream.mem); g_vk.stream.mapped = NULL; }
    if (g_vk.stream.buf) vkDestroyBuffer(g_vk.device, g_vk.stream.buf, NULL);
    if (g_vk.stream.mem) vkFreeMemory(g_vk.device, g_vk.stream.mem, NULL);
    g_vk.stream.buf = VK_NULL_HANDLE; g_vk.stream.mem = VK_NULL_HANDLE; g_vk.stream.size = 0;
}

/* The version of a buffer a draw reads / an update writes: its own storage, or
 * its copy in the stream arena. */
static VkBuffer buf_cur(const RhiVkBuffer *b) { return b->in_arena ? g_vk.stream.buf : b->buf; }
static VkDeviceSize buf_cur_off(const RhiVkBuffer *b) { return b->in_arena ? b->cur_off : 0; }
static unsigned char *buf_cur_ptr(const RhiVkBuffer *b) {
    return b->in_arena ? g_vk.stream.mapped + b->cur_off : (unsigned char *)b->mapped;
}

/* After a submit + fence wait: every recorded draw has completed. Copy the arena
 * versions back to the buffers' own storage and let the arena restart. */
static void stream_after_submit(void) {
    for (uint32_t i = 0; i < g_vk.buffers.count; i++) {
        RhiVkBuffer *b = &g_vk.buffers.slots[i];
        if (!b->live) continue;
        if (b->update == RHI_UPDATE_STREAM && b->in_arena && b->mapped && b->size)
            memcpy(b->mapped, g_vk.stream.mapped + b->cur_off, (size_t)b->size);
        b->in_arena = 0; b->in_use = 0;
    }
    g_vk.stream.offset = 0;
}

/* Snapshot `sz` bytes of `src` into the arena at an aligned offset for one draw
 * and write the offset to *out_off. If the current segment would overflow, do a
 * MID-FRAME FLUSH: close any render scope, submit + fence-wait the current
 * command buffer, begin a fresh recording and restart the arena at 0 -- the next
 * draw's ensure_rendering re-opens the scope with loadOp=LOAD, so earlier draws'
 * output survives. 1 on success, 0 on a submit failure. */
static int arena_place(const void *src, VkDeviceSize sz, VkDeviceSize *out_off) {
    VkDeviceSize align = g_vk.min_ubo_align ? g_vk.min_ubo_align : 1u;
    VkDeviceSize aligned = (g_vk.arena.offset + align - 1u) & ~(align - 1u);
    /* Virtual demand ignores flushes, so frame_begin can grow the arena to fit. */
    VkDeviceSize vdem = (g_vk.arena.demand + align - 1u) & ~(align - 1u);
    g_vk.arena.demand = vdem + sz;
    if (aligned + sz > g_vk.arena.size) {
        if (!frame_split()) return 0;
        g_vk.arena.offset = 0;
        aligned = 0;
        rhi_vk_arena_flushes++;
        if (sz > g_vk.arena.size) {
            /* One block larger than the whole arena. The split submitted and waited, so nothing recorded refers to the
             * arena any more: grow it now, before anything is copied. */
            VkDeviceSize ns = g_vk.arena.size ? g_vk.arena.size : VK_ARENA_INITIAL;
            while (ns < sz) ns *= 2u;
            arena_free();
            if (!arena_alloc(ns)) return 0;
        }
    }
    memcpy((unsigned char *)g_vk.arena.mapped + aligned, src, (size_t)sz);
    g_vk.arena.offset = aligned + sz;
    *out_off = aligned;
    return 1;
}

/* Destroy a deferred-free entry's objects (views first, memory last). */
static void destroy_free_entry(VkBuffer buf, VkDeviceMemory mem, VkImage img, VkImageView view) {
    if (view) vkDestroyImageView(g_vk.device, view, NULL);
    if (buf)  vkDestroyBuffer(g_vk.device, buf, NULL);
    if (img)  vkDestroyImage(g_vk.device, img, NULL);
    if (mem)  vkFreeMemory(g_vk.device, mem, NULL);
}

/* Free these objects now, or defer them until the frame command buffer has been
 * submitted and waited on when a frame is open (a recorded-but-unsubmitted draw
 * may reference them). */
static void defer_or_free(VkBuffer buf, VkDeviceMemory mem, VkImage img, VkImageView view) {
    if (!buf && !mem && !img && !view) return;
    if (!g_vk.in_frame) { destroy_free_entry(buf, mem, img, view); return; }
    if (g_vk.pending_free.count == g_vk.pending_free.capacity || g_test_fail_pending_alloc) {
        uint32_t cap = g_vk.pending_free.capacity ? g_vk.pending_free.capacity * 2u : 16u;
        void *n = g_test_fail_pending_alloc ? (g_test_fail_pending_alloc--, NULL)
                                            : realloc(g_vk.pending_free.items, (size_t)cap * sizeof *g_vk.pending_free.items);
        if (!n) {
            /* Cannot remember the resource: finish the recorded work first, so nothing pending can still use it, then free it now.
             * If the submit itself fails the device is lost; leak the resource rather than free it while it may be in use. */
            if (g_vk.recording && !frame_split()) return;
            destroy_free_entry(buf, mem, img, view);
            return;
        }
        g_vk.pending_free.items = n;
        g_vk.pending_free.capacity = cap;
    }
    g_vk.pending_free.items[g_vk.pending_free.count].buf = buf;
    g_vk.pending_free.items[g_vk.pending_free.count].mem = mem;
    g_vk.pending_free.items[g_vk.pending_free.count].img = img;
    g_vk.pending_free.items[g_vk.pending_free.count].view = view;
    g_vk.pending_free.count++;
}

static void defer_or_free_buffer(VkBuffer buf, VkDeviceMemory mem) {
    defer_or_free(buf, mem, VK_NULL_HANDLE, VK_NULL_HANDLE);
}

/* Destroy every deferred entry. Call only after a submit+fence wait. */
static void drain_pending_frees(void) {
    for (uint32_t i = 0; i < g_vk.pending_free.count; i++)
        destroy_free_entry(g_vk.pending_free.items[i].buf, g_vk.pending_free.items[i].mem,
                           g_vk.pending_free.items[i].img, g_vk.pending_free.items[i].view);
    g_vk.pending_free.count = 0;
}

/* Test-only (not in rhi.h): recreate the UBO arena at `bytes` so the smoke can
 * force a mid-frame flush with just a couple of draws. Safe only at the start of
 * a frame, before any draw has referenced the arena this frame. */
void rhi_vk_test_force_arena(size_t bytes) {
    arena_free();
    arena_alloc((VkDeviceSize)bytes);
    g_vk.arena.demand = 0;
}

/* Test-only: record two transfer clears of the colour image with NO barrier between
 * them (a write-after-write hazard). Synchronization validation must flag it; the
 * smoke uses this to prove the layer is really active before trusting a clean run. */
int rhi_vk_test_sync_hazard(void) {
    if (!g_vk.in_frame || g_vk.pass.active || !ensure_recording()) return 0;
    end_rendering_if_active();
    image_barrier(g_vk.color, VK_IMAGE_ASPECT_COLOR_BIT, g_vk.color_layout,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    g_vk.color_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    VkClearColorValue a = {{ 1, 0, 0, 1 }}, b = {{ 0, 0, 1, 1 }};
    VkImageSubresourceRange r = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdClearColorImage(g_vk.cmd, g_vk.color, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &a, 1, &r);
    vkCmdClearColorImage(g_vk.cmd, g_vk.color, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &b, 1, &r);   /* no barrier */
    return 1;
}

/* Test-only: recreate the stream arena at `bytes` (same rules as the UBO hook). */
/* Test-only: size of the per-frame parameter arena. */
size_t rhi_vk_test_arena_size(void) { return (size_t)g_vk.arena.size; }

/* Test-only: the next n growths of the deferred-free list fail, as if realloc returned NULL. */
void rhi_vk_test_fail_pending_alloc(unsigned n) { g_test_fail_pending_alloc = n; }

/* Test-only: the vertex-binding stride the last draw's pipeline used for `stream` (0xFFFFFFFF if the stream was not used). */
uint32_t rhi_vk_test_last_absent_offset(void) { return g_vk.build_absent_offset; }
uint32_t rhi_vk_test_last_stride(uint32_t stream) { return stream < RHI_MAX_VERTEX_STREAMS ? g_vk.last_stride[stream] : 0xFFFFFFFFu; }

void rhi_vk_test_force_stream_arena(size_t bytes) {
    stream_arena_free();
    stream_arena_alloc((VkDeviceSize)bytes);
    g_vk.stream.demand = 0;
}

/* -------------------------------------------------------------------------- */
/* Window setup                                                               */
/* -------------------------------------------------------------------------- */
RhiResult rhi_prepare_window(RhiBackend backend) {
    if (backend != RHI_BACKEND_DEFAULT && backend != RHI_BACKEND_VULKAN) return RHI_ERR_UNSUPPORTED;
    return RHI_OK;   /* Vulkan needs no SDL_GL_* context attributes */
}

uint32_t rhi_window_flags(RhiBackend backend) {
    return (backend == RHI_BACKEND_DEFAULT || backend == RHI_BACKEND_VULKAN)
               ? (uint32_t)SDL_WINDOW_VULKAN : 0u;
}

/* -------------------------------------------------------------------------- */
/* Capabilities                                                               */
/* -------------------------------------------------------------------------- */
/* 1 if `fmt` can be sampled with linear filtering from optimal-tiled storage. */
static uint8_t bc_supported(VkFormat fmt) {
    VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(g_vk.phys, fmt, &fp);
    const VkFormatFeatureFlags need =
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    return (fp.optimalTilingFeatures & need) == need;
}

static void query_caps(RhiCaps *c) {
    VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(g_vk.phys, &props);
    VkPhysicalDeviceFeatures feats; vkGetPhysicalDeviceFeatures(g_vk.phys, &feats);
    const VkPhysicalDeviceLimits *lim = &props.limits;
    memset(c, 0, sizeof *c);
    c->backend = RHI_BACKEND_VULKAN;
    /* One 2D image dimension bounds both the target and a texture. */
    c->max_viewport[0] = c->max_viewport[1] = (int32_t)lim->maxImageDimension2D;
    c->max_texture_size = (int32_t)lim->maxImageDimension2D;
    c->tex_bc1 = bc_supported(VK_FORMAT_BC1_RGBA_UNORM_BLOCK);   /* DXT1 */
    c->tex_bc2 = bc_supported(VK_FORMAT_BC2_UNORM_BLOCK);        /* DXT3 */
    c->tex_bc3 = bc_supported(VK_FORMAT_BC3_UNORM_BLOCK);        /* DXT5 */
    c->wireframe = (uint8_t)feats.fillModeNonSolid;
    c->max_anisotropy = feats.samplerAnisotropy ? lim->maxSamplerAnisotropy : 1.0f;
    /* Timer queries need timestamps on the graphics queue family. */
    uint32_t qc = 0; vkGetPhysicalDeviceQueueFamilyProperties(g_vk.phys, &qc, NULL);
    VkQueueFamilyProperties *qp = calloc(qc ? qc : 1, sizeof *qp);
    vkGetPhysicalDeviceQueueFamilyProperties(g_vk.phys, &qc, qp);
    c->gpu_timer = (uint8_t)(lim->timestampComputeAndGraphics &&
                             g_vk.queue_family < qc && qp[g_vk.queue_family].timestampValidBits > 0);
    free(qp);
}

const RhiCaps *rhi_caps(void) {
    return &g_vk.caps;
}

/* -------------------------------------------------------------------------- */
/* Device lifetime                                                            */
/* -------------------------------------------------------------------------- */
static int has_ext(const VkExtensionProperties *ep, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; i++) if (!strcmp(ep[i].extensionName, name)) return 1;
    return 0;
}

static int device_has_ext(VkPhysicalDevice pd, const char *name) {
    uint32_t ec = 0; vkEnumerateDeviceExtensionProperties(pd, NULL, &ec, NULL);
    VkExtensionProperties *ep = calloc(ec ? ec : 1, sizeof *ep);
    vkEnumerateDeviceExtensionProperties(pd, NULL, &ec, ep);
    int r = has_ext(ep, ec, name);
    free(ep);
    return r;
}

/* ---- swapchain ---------------------------------------------------------- */
static void swapchain_destroy(void) {
    if (!g_vk.device) return;
    vkDeviceWaitIdle(g_vk.device);
    for (uint32_t i = 0; g_vk.sc_render_sem && i < g_vk.sc_n; i++)
        if (g_vk.sc_render_sem[i]) vkDestroySemaphore(g_vk.device, g_vk.sc_render_sem[i], NULL);
    free(g_vk.sc_render_sem); g_vk.sc_render_sem = NULL;
    free(g_vk.sc_img);        g_vk.sc_img = NULL;
    if (g_vk.sc) vkDestroySwapchainKHR(g_vk.device, g_vk.sc, NULL);
    g_vk.sc = VK_NULL_HANDLE;
    g_vk.sc_n = 0;
}

/* (Re)creates the swapchain for the surface's current size. Returns 0 when the device cannot present
 * at all (no usable format/usage); returns 1 with sc == NULL when the surface is currently 0x0. */
static int swapchain_create(void) {
    VkSurfaceCapabilitiesKHR caps;
    if (!vk_ok(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_vk.phys, g_vk.surface, &caps), "surface capabilities")) return 0;
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        fprintf(stderr, "rhi_vk: swapchain images cannot be a transfer destination\n"); return 0;
    }
    uint32_t fc = 0; vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk.phys, g_vk.surface, &fc, NULL);
    VkSurfaceFormatKHR *fm = calloc(fc ? fc : 1, sizeof *fm);
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk.phys, g_vk.surface, &fc, fm);
    static const VkFormat pref[2] = { VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM };   /* UNORM = GL's default framebuffer */
    int pick = -1;
    for (int p = 0; p < 2 && pick < 0; p++)
        for (uint32_t i = 0; i < fc; i++)
            if (fm[i].format == pref[p] && fm[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { pick = (int)i; break; }
    for (uint32_t i = 0; i < fc && pick < 0; i++) {   /* any format the blit can write */
        VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(g_vk.phys, fm[i].format, &fp);
        if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) pick = (int)i;
    }
    VkSurfaceFormatKHR chosen = pick >= 0 ? fm[pick] : (VkSurfaceFormatKHR){0};
    free(fm);
    if (pick < 0) { fprintf(stderr, "rhi_vk: no swapchain format the blit can write\n"); return 0; }
    {   VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(g_vk.phys, chosen.format, &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
            fprintf(stderr, "rhi_vk: swapchain format %d is not a blit destination\n", (int)chosen.format); return 0; } }

    VkExtent2D ext = caps.currentExtent;
    if (ext.width == UINT32_MAX) {   /* surface follows the swapchain: use the target size */
        uint32_t tw = (uint32_t)g_vk.width, th = (uint32_t)g_vk.height;
        ext.width  = tw < caps.minImageExtent.width  ? caps.minImageExtent.width  :
                     tw > caps.maxImageExtent.width  ? caps.maxImageExtent.width  : tw;
        ext.height = th < caps.minImageExtent.height ? caps.minImageExtent.height :
                     th > caps.maxImageExtent.height ? caps.maxImageExtent.height : th;
    }
    swapchain_destroy();
    g_vk.sc_dirty = 0;
    if (ext.width == 0 || ext.height == 0) return 1;   /* minimised: present nothing until it has a size */

    uint32_t pmc = 0; vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.phys, g_vk.surface, &pmc, NULL);
    VkPresentModeKHR *pm = calloc(pmc ? pmc : 1, sizeof *pm);
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.phys, g_vk.surface, &pmc, pm);
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;   /* always available; what vsync means */
    if (!g_vk.vsync) {
        int have_mailbox = 0, have_immediate = 0;
        for (uint32_t i = 0; i < pmc; i++) {
            if (pm[i] == VK_PRESENT_MODE_MAILBOX_KHR) have_mailbox = 1;
            if (pm[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) have_immediate = 1;
        }
        mode = have_mailbox ? VK_PRESENT_MODE_MAILBOX_KHR : have_immediate ? VK_PRESENT_MODE_IMMEDIATE_KHR : mode;
    }
    free(pm);

    uint32_t want = caps.minImageCount + 1;
    if (caps.maxImageCount && want > caps.maxImageCount) want = caps.maxImageCount;
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & alpha))
        for (uint32_t b = 1; b <= VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR; b <<= 1)
            if (caps.supportedCompositeAlpha & b) { alpha = (VkCompositeAlphaFlagBitsKHR)b; break; }
    VkSwapchainCreateInfoKHR sci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, NULL, 0, g_vk.surface, want,
        chosen.format, chosen.colorSpace, ext, 1, VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL,
        caps.currentTransform, alpha, mode, VK_TRUE, VK_NULL_HANDLE };
    if (!vk_ok(vkCreateSwapchainKHR(g_vk.device, &sci, NULL, &g_vk.sc), "vkCreateSwapchainKHR")) { g_vk.sc = VK_NULL_HANDLE; return 0; }
    /* The driver may create more images than asked for. A swapchain that is not fully set up is torn down,
     * never left half built for present_frame to use. */
    uint32_t n = 0;
    if (!vk_ok(vkGetSwapchainImagesKHR(g_vk.device, g_vk.sc, &n, NULL), "vkGetSwapchainImagesKHR") || n == 0) {
        swapchain_destroy(); return 0;
    }
    g_vk.sc_img = calloc(n, sizeof *g_vk.sc_img);
    g_vk.sc_render_sem = calloc(n, sizeof *g_vk.sc_render_sem);
    g_vk.sc_n = n;
    if (!g_vk.sc_img || !g_vk.sc_render_sem ||
        !vk_ok(vkGetSwapchainImagesKHR(g_vk.device, g_vk.sc, &n, g_vk.sc_img), "vkGetSwapchainImagesKHR")) {
        swapchain_destroy(); return 0;
    }
    VkSemaphoreCreateInfo semi = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    for (uint32_t i = 0; i < n; i++)
        if (!vk_ok(vkCreateSemaphore(g_vk.device, &semi, NULL, &g_vk.sc_render_sem[i]), "vkCreateSemaphore")) {
            swapchain_destroy(); return 0;
        }
    g_vk.sc_ext = ext;
    g_vk.sc_fmt = chosen.format;
    return 1;
}

/* Offscreen main target: colour + depth images, their views and the readback buffer, sized
 * g_vk.width x g_vk.height. Created at device creation and again whenever the window resizes. */
static int main_target_create(void) {
    /* ---- offscreen main target: RGBA8 colour + depth-stencil (GL asks for 24+8) ----
     * The colour image needs TRANSFER_DST for vkCmdClearColorImage and
     * TRANSFER_SRC for the readback copy, besides COLOR_ATTACHMENT for draws
     * (P11e). The depth image needs TRANSFER_DST for vkCmdClearDepthStencilImage
     * and TRANSFER_SRC so P11f can copy it into a RHI_FORMAT_DEPTH texture. */
    if (!make_image(VK_FORMAT_R8G8B8A8_UNORM,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    &g_vk.color, &g_vk.color_mem)) return 0;
    g_vk.color_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    g_vk.depth_fmt = pick_depth_format();
    if (g_vk.depth_fmt == VK_FORMAT_UNDEFINED) {
        fprintf(stderr, "rhi_vk: no usable depth-stencil format (D24S8/D32S8)\n"); return 0;
    }
    if (!make_image(g_vk.depth_fmt,
                    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    &g_vk.depth, &g_vk.depth_mem)) return 0;
    g_vk.depth_layout = VK_IMAGE_LAYOUT_UNDEFINED;

    /* Attachment views for dynamic rendering (P11a made the images, not views). */
    {
        VkImageViewCreateInfo cv = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, g_vk.color,
            VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM,
            { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
              VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY },
            { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
        if (!vk_ok(vkCreateImageView(g_vk.device, &cv, NULL, &g_vk.color_view), "vkCreateImageView(color)")) return 0;
        VkImageViewCreateInfo dv = cv;
        dv.image = g_vk.depth; dv.format = g_vk.depth_fmt;
        dv.subresourceRange.aspectMask = VK_DS_ASPECT;
        if (!vk_ok(vkCreateImageView(g_vk.device, &dv, NULL, &g_vk.depth_view), "vkCreateImageView(depth)")) return 0;
    }

    /* ---- host-visible readback buffer, full-frame sized (reused each read) ---- */
    VkDeviceSize bsize = (VkDeviceSize)g_vk.width * g_vk.height * 4;
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, bsize,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
    if (!vk_ok(vkCreateBuffer(g_vk.device, &bci, NULL, &g_vk.readback), "vkCreateBuffer")) return 0;
    VkMemoryRequirements bmr; vkGetBufferMemoryRequirements(g_vk.device, g_vk.readback, &bmr);
    uint32_t bmt = find_mem(bmr.memoryTypeBits,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (bmt == UINT32_MAX) { fprintf(stderr, "rhi_vk: no host-visible memory type\n"); return 0; }
    VkMemoryAllocateInfo bmai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, bmr.size, bmt };
    if (!vk_ok(vkAllocateMemory(g_vk.device, &bmai, NULL, &g_vk.readback_mem), "vkAllocateMemory(buffer)")) return 0;
    if (!vk_ok(vkBindBufferMemory(g_vk.device, g_vk.readback, g_vk.readback_mem, 0), "vkBindBufferMemory")) return 0;
    return 1;
}

/* Callers guarantee the GPU is idle with the target (frame_begin, after the previous frame's wait). */
static void main_target_destroy(void) {
    if (g_vk.color_view)   vkDestroyImageView(g_vk.device, g_vk.color_view, NULL);
    if (g_vk.depth_view)   vkDestroyImageView(g_vk.device, g_vk.depth_view, NULL);
    if (g_vk.readback)     vkDestroyBuffer(g_vk.device, g_vk.readback, NULL);
    if (g_vk.readback_mem) vkFreeMemory(g_vk.device, g_vk.readback_mem, NULL);
    if (g_vk.depth)        vkDestroyImage(g_vk.device, g_vk.depth, NULL);
    if (g_vk.depth_mem)    vkFreeMemory(g_vk.device, g_vk.depth_mem, NULL);
    if (g_vk.color)        vkDestroyImage(g_vk.device, g_vk.color, NULL);
    if (g_vk.color_mem)    vkFreeMemory(g_vk.device, g_vk.color_mem, NULL);
    g_vk.color_view = g_vk.depth_view = VK_NULL_HANDLE;
    g_vk.readback = VK_NULL_HANDLE; g_vk.readback_mem = VK_NULL_HANDLE;
    g_vk.depth = VK_NULL_HANDLE; g_vk.depth_mem = VK_NULL_HANDLE;
    g_vk.color = VK_NULL_HANDLE; g_vk.color_mem = VK_NULL_HANDLE;
}

/* Present mode: follow the window. The main target takes the new size, the swapchain is rebuilt
 * before the next present, and the default viewport covers the new target. */
static int main_target_resize(int32_t w, int32_t h) {
    vkDeviceWaitIdle(g_vk.device);
    main_target_destroy();
    g_vk.width = w; g_vk.height = h;
    if (!main_target_create()) return 0;
    g_vk.viewport[0] = 0; g_vk.viewport[1] = 0;
    g_vk.viewport[2] = w; g_vk.viewport[3] = h;
    g_vk.sc_dirty = 1;
    return 1;
}

RhiResult rhi_device_create(const RhiDeviceDesc *desc) {
    if (!desc || !desc->window || g_vk.device) return RHI_ERR_INVALID;
    if (desc->backend != RHI_BACKEND_DEFAULT && desc->backend != RHI_BACKEND_VULKAN)
        return RHI_ERR_UNSUPPORTED;

    g_vk.window = (SDL_Window *)desc->window;
    g_vk.vsync = (desc->flags & RHI_DEVICE_VSYNC) != 0;
    int w = 0, h = 0;
    SDL_GetWindowSize(g_vk.window, &w, &h);
    g_vk.width = w > 0 ? w : VK_DEFAULT_W;
    g_vk.height = h > 0 ? h : VK_DEFAULT_H;

    /* ---- instance: validation layer + debug messenger when present ---- */
    const char *layers[1]; uint32_t n_layers = 0;
    const char *exts[16]; uint32_t n_exts = 0;
    int have_debug = 0, have_features = 0;
    VkInstanceCreateFlags ici_flags = 0;
    /* Present mode needs a window created with SDL_WINDOW_VULKAN (the offscreen video driver
     * and the headless shim refuse it) and the instance extensions SDL asks for. */
    int want_present = (SDL_GetWindowFlags(g_vk.window) & SDL_WINDOW_VULKAN) != 0;
    if (want_present) {
        unsigned sdl_n = 0;
        if (!SDL_Vulkan_GetInstanceExtensions(g_vk.window, &sdl_n, NULL) || sdl_n == 0 || sdl_n > 8) {
            fprintf(stderr, "rhi_vk: no SDL Vulkan surface extensions (%s); offscreen\n", SDL_GetError());
            want_present = 0;
        } else {
            SDL_Vulkan_GetInstanceExtensions(g_vk.window, &sdl_n, &exts[n_exts]);
            n_exts += sdl_n;
        }
    }
    {   /* MoltenVK (and other portability drivers) are only enumerated with this opt-in. */
        uint32_t ec = 0; vkEnumerateInstanceExtensionProperties(NULL, &ec, NULL);
        VkExtensionProperties *ep = calloc(ec ? ec : 1, sizeof *ep);
        vkEnumerateInstanceExtensionProperties(NULL, &ec, ep);
        if (has_ext(ep, ec, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
            exts[n_exts++] = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
            ici_flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        }
        free(ep);
    }
    {
        uint32_t lc = 0; vkEnumerateInstanceLayerProperties(&lc, NULL);
        VkLayerProperties *lp = calloc(lc ? lc : 1, sizeof *lp);
        vkEnumerateInstanceLayerProperties(&lc, lp);
        for (uint32_t i = 0; i < lc; i++)
            if (!strcmp(lp[i].layerName, "VK_LAYER_KHRONOS_validation")) layers[n_layers++] = "VK_LAYER_KHRONOS_validation";
        free(lp);
        if (n_layers) {   /* debug_utils rides with the layer */
            uint32_t ec = 0; vkEnumerateInstanceExtensionProperties(NULL, &ec, NULL);
            VkExtensionProperties *ep = calloc(ec ? ec : 1, sizeof *ep);
            vkEnumerateInstanceExtensionProperties(NULL, &ec, ep);
            for (uint32_t i = 0; i < ec; i++)
                if (!strcmp(ep[i].extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) have_debug = 1;
            free(ep);
            if (have_debug) exts[n_exts++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
            /* Synchronization validation is opt-in (RHI_VK_SYNC_VALIDATION=1, a debug aid:
             * the layer's env/settings route did not enable it under lavapipe). The layer
             * offers VK_EXT_validation_features, through which the app asks for it. */
            const char *sv = getenv("RHI_VK_SYNC_VALIDATION");
            if (sv && *sv && *sv != '0') {
                uint32_t lec = 0; vkEnumerateInstanceExtensionProperties("VK_LAYER_KHRONOS_validation", &lec, NULL);
                VkExtensionProperties *lep = calloc(lec ? lec : 1, sizeof *lep);
                vkEnumerateInstanceExtensionProperties("VK_LAYER_KHRONOS_validation", &lec, lep);
                for (uint32_t i = 0; i < lec; i++)
                    if (!strcmp(lep[i].extensionName, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME)) have_features = 1;
                free(lep);
                if (have_features) { exts[n_exts++] = VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME;
                                     fprintf(stderr, "rhi_vk: synchronization validation requested\n"); }
                else fprintf(stderr, "rhi_vk: VK_EXT_validation_features unavailable; no sync validation\n");
            }
        } else {
            fprintf(stderr, "rhi_vk: VK_LAYER_KHRONOS_validation not found (set VK_ADD_LAYER_PATH)\n");
        }
    }
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "OpenUG2", 1, "OpenUG2", 1,
                              VK_API_VERSION_1_3 };   /* core dynamic rendering */
    static const VkValidationFeatureEnableEXT sync_on[] = { VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT };
    VkValidationFeaturesEXT vfe = { VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT, NULL, 1, sync_on, 0, NULL };
    VkDebugUtilsMessengerCreateInfoEXT dci = { VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT, NULL, 0,
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT, dbg_cb, NULL };
    /* The instance's pNext chain: messenger for instance-creation messages, then the
     * validation features; vkCreateDebugUtilsMessengerEXT below gets dci alone. */
    VkDebugUtilsMessengerCreateInfoEXT dci_ici = dci;
    dci_ici.pNext = have_features ? &vfe : NULL;
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                 have_debug ? (const void *)&dci_ici : have_features ? (const void *)&vfe : NULL, ici_flags, &app,
                                 n_layers, layers, n_exts, exts };
    if (!vk_ok(vkCreateInstance(&ici, NULL, &g_vk.instance), "vkCreateInstance")) goto fail;
    if (have_debug) {
        PFN_vkCreateDebugUtilsMessengerEXT cdm =
            (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(g_vk.instance, "vkCreateDebugUtilsMessengerEXT");
        if (cdm && !vk_ok(cdm(g_vk.instance, &dci, NULL, &g_vk.messenger), "vkCreateDebugUtilsMessengerEXT")) goto fail;
    }

    if (want_present) {
        if (SDL_Vulkan_CreateSurface(g_vk.window, g_vk.instance, &g_vk.surface)) {
            g_vk.present = 1;
            fprintf(stderr, "rhi_vk: window surface created (present mode)\n");
        }
        else {
            fprintf(stderr, "rhi_vk: SDL_Vulkan_CreateSurface: %s; offscreen\n", SDL_GetError());
            g_vk.surface = VK_NULL_HANDLE;
        }
    }

    /* ---- physical device + graphics queue family (that can present, in present mode) ---- */
    uint32_t pdc = 0;
    if (!vk_ok(vkEnumeratePhysicalDevices(g_vk.instance, &pdc, NULL), "vkEnumeratePhysicalDevices") || !pdc) {
        fprintf(stderr, "rhi_vk: no Vulkan devices (check VK_DRIVER_FILES)\n"); goto fail;
    }
    VkPhysicalDevice *pds = calloc(pdc, sizeof *pds);
    vkEnumeratePhysicalDevices(g_vk.instance, &pdc, pds);
    for (;;) {
    for (uint32_t i = 0; i < pdc && !g_vk.phys; i++) {
        if (g_vk.present && !device_has_ext(pds[i], VK_KHR_SWAPCHAIN_EXTENSION_NAME)) continue;
        uint32_t qc = 0; vkGetPhysicalDeviceQueueFamilyProperties(pds[i], &qc, NULL);
        VkQueueFamilyProperties *qp = calloc(qc ? qc : 1, sizeof *qp);
        vkGetPhysicalDeviceQueueFamilyProperties(pds[i], &qc, qp);
        for (uint32_t q = 0; q < qc; q++)
            if (qp[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                VkBool32 can_present = VK_TRUE;
                if (g_vk.present) {
                    can_present = VK_FALSE;
                    vkGetPhysicalDeviceSurfaceSupportKHR(pds[i], q, g_vk.surface, &can_present);
                }
                if (can_present) { g_vk.phys = pds[i]; g_vk.queue_family = q; break; }
            }
        free(qp);
    }
    if (g_vk.phys || !g_vk.present) break;
    /* No device can present to this window: keep the offscreen target, as before the surface existed. */
    fprintf(stderr, "rhi_vk: no device with a graphics+present queue and VK_KHR_swapchain; offscreen\n");
    vkDestroySurfaceKHR(g_vk.instance, g_vk.surface, NULL);
    g_vk.surface = VK_NULL_HANDLE;
    g_vk.present = 0;
    }
    free(pds);
    if (!g_vk.phys) { fprintf(stderr, "rhi_vk: no device with a graphics queue\n"); goto fail; }

    /* ---- logical device: enable only supported features we advertise ---- */
    VkPhysicalDeviceFeatures sup; vkGetPhysicalDeviceFeatures(g_vk.phys, &sup);
    VkPhysicalDeviceFeatures enable = {0};
    enable.textureCompressionBC = sup.textureCompressionBC;
    enable.samplerAnisotropy    = sup.samplerAnisotropy;
    enable.fillModeNonSolid     = sup.fillModeNonSolid;
    enable.wideLines            = sup.wideLines;   /* line_width > 1 for LINES pipelines */
    g_vk.wide_lines             = sup.wideLines;
    /* Dynamic rendering (core in 1.3): draws use vkCmdBeginRendering with the
     * offscreen views instead of a VkRenderPass/VkFramebuffer. Chained via the
     * feature struct alongside pEnabledFeatures (no VkPhysicalDeviceFeatures2). */
    /* Host query reset (core 1.2): timers reset their queries from the CPU. */
    VkPhysicalDeviceVulkan12Features sup12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    VkPhysicalDeviceFeatures2 sup2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &sup12 };
    vkGetPhysicalDeviceFeatures2(g_vk.phys, &sup2);
    VkPhysicalDeviceVulkan12Features en12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    en12.hostQueryReset = sup12.hostQueryReset;
    g_vk.host_query_reset = sup12.hostQueryReset ? 1 : 0;
    VkPhysicalDeviceDynamicRenderingFeatures dyn = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES, &en12, VK_TRUE };
    /* Push descriptors (set 0 written per draw, no descriptor pool). Not core
     * before 1.4, so enable the extension and fetch its entry point by name. */
    const char *dev_exts[3]; uint32_t n_dev_exts = 0;
    dev_exts[n_dev_exts++] = VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME;
    if (g_vk.present) dev_exts[n_dev_exts++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    {   /* A portability device (MoltenVK) must enable VK_KHR_portability_subset when it offers it. */
        uint32_t ec = 0; vkEnumerateDeviceExtensionProperties(g_vk.phys, NULL, &ec, NULL);
        VkExtensionProperties *ep = calloc(ec ? ec : 1, sizeof *ep);
        vkEnumerateDeviceExtensionProperties(g_vk.phys, NULL, &ec, ep);
        if (has_ext(ep, ec, "VK_KHR_portability_subset")) dev_exts[n_dev_exts++] = "VK_KHR_portability_subset";
        free(ep);
    }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, g_vk.queue_family, 1, &prio };
    VkDeviceCreateInfo dc = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &dyn, 0, 1, &qci, 0, NULL,
                              n_dev_exts, dev_exts, &enable };
    if (!vk_ok(vkCreateDevice(g_vk.phys, &dc, NULL, &g_vk.device), "vkCreateDevice")) goto fail;
    vkGetDeviceQueue(g_vk.device, g_vk.queue_family, 0, &g_vk.queue);
    g_vk.push_desc = (PFN_vkCmdPushDescriptorSetKHR)
        vkGetDeviceProcAddr(g_vk.device, "vkCmdPushDescriptorSetKHR");
    if (!g_vk.push_desc) { fprintf(stderr, "rhi_vk: vkCmdPushDescriptorSetKHR unavailable\n"); goto fail; }

    /* ---- command pool + one primary buffer + a frame fence ---- */
    VkCommandPoolCreateInfo cpci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL,
        VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, g_vk.queue_family };
    if (!vk_ok(vkCreateCommandPool(g_vk.device, &cpci, NULL, &g_vk.pool), "vkCreateCommandPool")) goto fail;
    VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, g_vk.pool,
        VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
    if (!vk_ok(vkAllocateCommandBuffers(g_vk.device, &cbai, &g_vk.cmd), "vkAllocateCommandBuffers")) goto fail;
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, 0 };
    if (!vk_ok(vkCreateFence(g_vk.device, &fci, NULL, &g_vk.fence), "vkCreateFence")) goto fail;
    if (g_vk.present) {   /* a swapchain that cannot be built leaves the backend offscreen, as before P12 */
        VkSemaphoreCreateInfo semi = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        if (!vk_ok(vkCreateSemaphore(g_vk.device, &semi, NULL, &g_vk.acquire_sem), "vkCreateSemaphore") ||
            !swapchain_create()) {
            fprintf(stderr, "rhi_vk: swapchain unavailable; offscreen\n");
            swapchain_destroy();
            g_vk.present = 0;
        }
    }

    if (!main_target_create()) goto fail;

    /* ---- 1x1 white dummy texture (for unbound sampler slots) ---- */
    if (!make_image(VK_FORMAT_R8G8B8A8_UNORM,
                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    &g_vk.dummy_img, &g_vk.dummy_mem)) goto fail;
    {   /* stage one opaque-white texel and settle to SHADER_READ_ONLY */
        VkBuffer staging; VkDeviceMemory smem;
        const unsigned char white[4] = { 255, 255, 255, 255 };
        if (!make_buffer(4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                         &staging, &smem)) goto fail;
        void *map = NULL;
        int ok = vk_ok(vkMapMemory(g_vk.device, smem, 0, VK_WHOLE_SIZE, 0, &map), "vkMapMemory(dummy)");
        if (ok) { memcpy(map, white, 4); vkUnmapMemory(g_vk.device, smem); }
        VkCommandBuffer cb = ok ? oneshot_begin() : VK_NULL_HANDLE;
        if (cb) {
            img_barrier(cb, g_vk.dummy_img, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1,
                        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        0, VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy reg = { 0, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { 1, 1, 1 } };
            vkCmdCopyBufferToImage(cb, staging, g_vk.dummy_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &reg);
            img_barrier(cb, g_vk.dummy_img, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
            ok = oneshot_end(cb);
        } else ok = 0;
        vkDestroyBuffer(g_vk.device, staging, NULL);
        vkFreeMemory(g_vk.device, smem, NULL);
        if (!ok) goto fail;
    }
    {
        VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, g_vk.dummy_img,
            VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM,
            { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
              VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY },
            { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
        if (!vk_ok(vkCreateImageView(g_vk.device, &vi, NULL, &g_vk.dummy_view), "vkCreateImageView(dummy)")) goto fail;
        VkSamplerCreateInfo si = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, NULL, 0,
            VK_FILTER_NEAREST, VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST,
            VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_REPEAT,
            0.0f, VK_FALSE, 1.0f, VK_FALSE, VK_COMPARE_OP_ALWAYS, 0.0f, 0.0f,
            VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK, VK_FALSE };
        if (!vk_ok(vkCreateSampler(g_vk.device, &si, NULL, &g_vk.dummy_sampler), "vkCreateSampler(dummy)")) goto fail;
    }

    /* ---- 1x1x6 cube dummy (opaque black) for samplerCube slots a draw leaves unbound ---- */
    if (!make_image_ex(VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 6, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
                       VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                       &g_vk.dummy_cube_img, &g_vk.dummy_cube_mem)) goto fail;
    {
        VkCommandBuffer cb = oneshot_begin();
        if (!cb) goto fail;
        img_barrier(cb, g_vk.dummy_cube_img, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6,
                    VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    0, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkClearColorValue black = {{ 0.0f, 0.0f, 0.0f, 1.0f }};
        VkImageSubresourceRange rng = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6 };
        vkCmdClearColorImage(cb, g_vk.dummy_cube_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &rng);
        img_barrier(cb, g_vk.dummy_cube_img, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        if (!oneshot_end(cb)) goto fail;
        VkImageViewCreateInfo cvi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, g_vk.dummy_cube_img,
            VK_IMAGE_VIEW_TYPE_CUBE, VK_FORMAT_R8G8B8A8_UNORM,
            { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
              VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY },
            { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6 } };
        if (!vk_ok(vkCreateImageView(g_vk.device, &cvi, NULL, &g_vk.dummy_cube_view), "vkCreateImageView(cube dummy)")) goto fail;
    }

    /* ---- zero-filled dummy vertex buffer for streams a draw leaves unbound ---- */
    if (!make_buffer(256, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &g_vk.dummy_vbuf, &g_vk.dummy_vbuf_mem)) goto fail;
    {
        VkCommandBuffer cb = oneshot_begin();
        if (!cb) goto fail;
        vkCmdFillBuffer(cb, g_vk.dummy_vbuf, 0, 256, 0);
        if (!oneshot_end(cb)) goto fail;
    }
    if (!make_buffer(16, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &g_vk.dummy_attr_buf, &g_vk.dummy_attr_mem)) goto fail;
    {
        static const float def[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        VkCommandBuffer cb = oneshot_begin();
        if (!cb) goto fail;
        vkCmdUpdateBuffer(cb, g_vk.dummy_attr_buf, 0, sizeof def, def);
        if (!oneshot_end(cb)) goto fail;
    }

    /* ---- shared push-descriptor set + pipeline layout ----
     * set 0: binding 0 = uniform buffer (vertex+fragment), bindings 1..SLOTS =
     * combined image samplers (fragment). The Vulkan shaders use binding 0 for
     * the FeParams UBO and binding (slot+1) for texture slot `slot`. */
    {
        VkDescriptorSetLayoutBinding binds[1 + RHI_TEXTURE_SLOTS];
        binds[0] = (VkDescriptorSetLayoutBinding){ 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
        for (uint32_t i = 0; i < RHI_TEXTURE_SLOTS; i++)
            binds[1 + i] = (VkDescriptorSetLayoutBinding){ 1 + i,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
        VkDescriptorSetLayoutCreateInfo dli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL,
            VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR, 1 + RHI_TEXTURE_SLOTS, binds };
        if (!vk_ok(vkCreateDescriptorSetLayout(g_vk.device, &dli, NULL, &g_vk.set_layout),
                   "vkCreateDescriptorSetLayout")) goto fail;
        VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0,
            1, &g_vk.set_layout, 0, NULL };
        if (!vk_ok(vkCreatePipelineLayout(g_vk.device, &pli, NULL, &g_vk.pipe_layout),
                   "vkCreatePipelineLayout")) goto fail;
    }

    /* ---- per-frame UBO arena (P11-review Fix 1) ---- */
    {
        VkPhysicalDeviceProperties pdp; vkGetPhysicalDeviceProperties(g_vk.phys, &pdp);
        g_vk.min_ubo_align = pdp.limits.minUniformBufferOffsetAlignment;
        g_vk.max_ubo_range = pdp.limits.maxUniformBufferRange;
    }
    if (!arena_alloc(VK_ARENA_INITIAL)) goto fail;
    if (!stream_arena_alloc(VK_STREAM_ARENA_INITIAL)) goto fail;

    /* Default viewport = whole target in GL convention (origin bottom-left). */
    g_vk.viewport[0] = 0; g_vk.viewport[1] = 0;
    g_vk.viewport[2] = g_vk.width; g_vk.viewport[3] = g_vk.height;
    g_vk.bound_pipeline = -1;

    g_vk.frame_index = 0;
    query_caps(&g_vk.caps);
    g_vk.timer_running = -1;
    if (g_vk.caps.gpu_timer) {   /* one pool, two timestamps per timer */
        VkQueryPoolCreateInfo qpi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, NULL, 0,
            VK_QUERY_TYPE_TIMESTAMP, 2u * VK_MAX_TIMERS, 0 };
        if (!g_vk.host_query_reset ||
            !vk_ok(vkCreateQueryPool(g_vk.device, &qpi, NULL, &g_vk.timer_pool), "vkCreateQueryPool"))
            g_vk.caps.gpu_timer = 0;
    }
    return RHI_OK;

fail:
    rhi_device_destroy();
    return RHI_ERR_UNSUPPORTED;
}

void rhi_device_destroy(void) {
    if (g_vk.device) vkDeviceWaitIdle(g_vk.device);
    /* Drain deferred buffer frees and release the per-frame UBO arena (Fixes 1 & 2). */
    drain_pending_frees();
    free(g_vk.pending_free.items);
    arena_free();
    stream_arena_free();
    /* Free every live buffer, then the handle table's own heap arrays. */
    for (uint32_t i = 0; i < g_vk.buffers.count; i++) {
        RhiVkBuffer *b = &g_vk.buffers.slots[i];
        if (b->mapped) vkUnmapMemory(g_vk.device, b->mem);
        if (b->buf) vkDestroyBuffer(g_vk.device, b->buf, NULL);
        if (b->mem) vkFreeMemory(g_vk.device, b->mem, NULL);
    }
    free(g_vk.buffers.slots);
    free(g_vk.buffers.free_list);
    /* Free every live texture (view/image/memory), then its table arrays. */
    for (uint32_t i = 0; i < g_vk.textures.count; i++) {
        RhiVkTexture *t = &g_vk.textures.slots[i];
        for (int f = 0; f < 6; f++)
            if (t->att_view[f]) vkDestroyImageView(g_vk.device, t->att_view[f], NULL);
        if (t->view) vkDestroyImageView(g_vk.device, t->view, NULL);
        if (t->img)  vkDestroyImage(g_vk.device, t->img, NULL);
        if (t->mem)  vkFreeMemory(g_vk.device, t->mem, NULL);
    }
    free(g_vk.textures.slots);
    free(g_vk.textures.free_list);
    /* Destroy every VkSampler, then the sampler list arrays. */
    for (uint32_t i = 0; i < g_vk.samplers.count; i++)
        if (g_vk.samplers.handles[i]) vkDestroySampler(g_vk.device, g_vk.samplers.handles[i], NULL);
    free(g_vk.samplers.descs);
    free(g_vk.samplers.handles);
    /* Free every live shader (modules + param mirrors), then its table arrays. */
    for (uint32_t i = 0; i < g_vk.shaders.count; i++) {
        RhiVkShader *s = &g_vk.shaders.slots[i];
        for (uint32_t b = 0; b < s->block_count; b++) free(s->blocks[b].mirror);
        if (s->vert) vkDestroyShaderModule(g_vk.device, s->vert, NULL);
        if (s->frag) vkDestroyShaderModule(g_vk.device, s->frag, NULL);
    }
    free(g_vk.shaders.slots);
    free(g_vk.shaders.free_list);
    /* Every created VkPipeline, then its table, then the shared layouts. */
    for (uint32_t i = 0; i < g_vk.pipelines.count; i++)
        for (unsigned v = 0; v < VK_VARIANTS; v++)
            if (g_vk.pipelines.slots[i].variant[v])
                vkDestroyPipeline(g_vk.device, g_vk.pipelines.slots[i].variant[v], NULL);
    free(g_vk.pipelines.slots);
    if (g_vk.pipe_layout) vkDestroyPipelineLayout(g_vk.device, g_vk.pipe_layout, NULL);
    if (g_vk.set_layout)  vkDestroyDescriptorSetLayout(g_vk.device, g_vk.set_layout, NULL);
    if (g_vk.dummy_sampler) vkDestroySampler(g_vk.device, g_vk.dummy_sampler, NULL);
    if (g_vk.dummy_view)    vkDestroyImageView(g_vk.device, g_vk.dummy_view, NULL);
    if (g_vk.dummy_img)     vkDestroyImage(g_vk.device, g_vk.dummy_img, NULL);
    if (g_vk.dummy_mem)     vkFreeMemory(g_vk.device, g_vk.dummy_mem, NULL);
    if (g_vk.dummy_cube_view) vkDestroyImageView(g_vk.device, g_vk.dummy_cube_view, NULL);
    if (g_vk.dummy_cube_img)  vkDestroyImage(g_vk.device, g_vk.dummy_cube_img, NULL);
    if (g_vk.dummy_cube_mem)  vkFreeMemory(g_vk.device, g_vk.dummy_cube_mem, NULL);
    if (g_vk.dummy_attr_buf) vkDestroyBuffer(g_vk.device, g_vk.dummy_attr_buf, NULL);
    if (g_vk.dummy_attr_mem) vkFreeMemory(g_vk.device, g_vk.dummy_attr_mem, NULL);
    if (g_vk.dummy_vbuf)     vkDestroyBuffer(g_vk.device, g_vk.dummy_vbuf, NULL);
    if (g_vk.dummy_vbuf_mem) vkFreeMemory(g_vk.device, g_vk.dummy_vbuf_mem, NULL);
    main_target_destroy();
    if (g_vk.timer_pool)   vkDestroyQueryPool(g_vk.device, g_vk.timer_pool, NULL);
    swapchain_destroy();
    if (g_vk.acquire_sem)  vkDestroySemaphore(g_vk.device, g_vk.acquire_sem, NULL);
    if (g_vk.fence)        vkDestroyFence(g_vk.device, g_vk.fence, NULL);
    if (g_vk.pool)         vkDestroyCommandPool(g_vk.device, g_vk.pool, NULL);
    if (g_vk.device)       vkDestroyDevice(g_vk.device, NULL);
    if (g_vk.messenger) {
        PFN_vkDestroyDebugUtilsMessengerEXT ddm =
            (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(g_vk.instance, "vkDestroyDebugUtilsMessengerEXT");
        if (ddm) ddm(g_vk.instance, g_vk.messenger, NULL);
    }
    if (g_vk.surface)  vkDestroySurfaceKHR(g_vk.instance, g_vk.surface, NULL);
    if (g_vk.instance) vkDestroyInstance(g_vk.instance, NULL);
    memset(&g_vk, 0, sizeof g_vk);
}

/* Present mode: the window's current drawable size (what the next frame_begin will adopt). Offscreen:
 * the fixed target size. */
void rhi_drawable_size(int32_t *width, int32_t *height) {
    int w = g_vk.width, h = g_vk.height;
    if (g_vk.present) {
        int dw = 0, dh = 0;
        SDL_Vulkan_GetDrawableSize(g_vk.window, &dw, &dh);
        w = dw; h = dh;
    }
    if (width)  *width  = w;
    if (height) *height = h;
}

/* -------------------------------------------------------------------------- */
/* Frame lifecycle                                                            */
/* -------------------------------------------------------------------------- */
RhiResult rhi_frame_begin(RhiFrameInfo *out) {
    if (!g_vk.device) return RHI_ERR_INVALID;
    if (g_vk.present) {
        /* A minimised or zero-sized window has nothing to present. Otherwise follow its size
         * (the previous frame was submitted and waited for, so the target is idle). */
        int dw = 0, dh = 0;
        SDL_Vulkan_GetDrawableSize(g_vk.window, &dw, &dh);
        if ((SDL_GetWindowFlags(g_vk.window) & SDL_WINDOW_MINIMIZED) || dw <= 0 || dh <= 0)
            return RHI_ERR_SKIP_FRAME;
        if (dw != g_vk.width || dh != g_vk.height) {
            if (!main_target_resize(dw, dh)) return RHI_ERR_DEVICE_LOST;
        }
    }
    /* Grow the arena if last frame's demand overflowed it (safe here: the prior
     * frame was already submitted + waited at frame_end), then restart the bump
     * allocator for this frame (P11-review Fix 1). */
    if (g_vk.arena.demand > g_vk.arena.size) {
        VkDeviceSize ns = g_vk.arena.size ? g_vk.arena.size : VK_ARENA_INITIAL;
        while (ns < g_vk.arena.demand) ns *= 2u;
        arena_free();
        if (!arena_alloc(ns)) return RHI_ERR_DEVICE_LOST;
    }
    g_vk.arena.offset = 0;
    g_vk.arena.demand = 0;
    if (g_vk.stream.demand > g_vk.stream.size) {   /* same for the stream arena */
        VkDeviceSize ns = g_vk.stream.size ? g_vk.stream.size : VK_STREAM_ARENA_INITIAL;
        while (ns < g_vk.stream.demand) ns *= 2u;
        stream_arena_free();
        if (!stream_arena_alloc(ns)) return RHI_ERR_DEVICE_LOST;
    }
    g_vk.stream.offset = 0;
    g_vk.stream.demand = 0;
    if (!ensure_recording()) return RHI_ERR_DEVICE_LOST;
    g_vk.in_frame = 1;
    if (out) {
        out->width = g_vk.width;
        out->height = g_vk.height;
        out->frame_index = g_vk.frame_index;
    }
    return RHI_OK;
}

/* Present mode: blit the offscreen colour image into the next swapchain image (flipping y: the image
 * is bottom-up, the swapchain top-down), submit, wait, present. Returns 1 when the frame's command
 * buffer has been submitted and waited for (whether or not an image was presented), 0 when it
 * has not been submitted (offscreen mode, nothing to present to) so the caller flushes it, -1 when
 * the submit failed. */
static int present_frame(void) {
    if (!g_vk.present) return 0;
    /* A hidden window (--shot capture) has nothing to show: skip the acquire/blit/present round trip. */
    if (SDL_GetWindowFlags(g_vk.window) & SDL_WINDOW_HIDDEN) return 0;
    if (g_vk.sc_dirty || !g_vk.sc) swapchain_create();
    if (!g_vk.sc) return 0;
    uint32_t idx = 0;
    VkResult r = vkAcquireNextImageKHR(g_vk.device, g_vk.sc, UINT64_MAX, g_vk.acquire_sem, VK_NULL_HANDLE, &idx);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {   /* resized between frames: rebuild once and retry */
        swapchain_create();
        if (!g_vk.sc) return 0;
        r = vkAcquireNextImageKHR(g_vk.device, g_vk.sc, UINT64_MAX, g_vk.acquire_sem, VK_NULL_HANDLE, &idx);
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { g_vk.sc_dirty = 1; return 0; }   /* nothing acquired: no wait on the semaphore */
    if (!ensure_recording()) return -1;
    if (g_vk.color_layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
        image_barrier(g_vk.color, VK_IMAGE_ASPECT_COLOR_BIT, g_vk.color_layout,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_MEMORY_WRITE_BIT,
                      VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        g_vk.color_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    }
    VkImage dst = g_vk.sc_img[idx];
    image_barrier(dst, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkImageBlit blit = {
        { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { { 0, 0, 0 }, { (int32_t)g_vk.width, (int32_t)g_vk.height, 1 } },
        { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { { 0, (int32_t)g_vk.sc_ext.height, 0 }, { (int32_t)g_vk.sc_ext.width, 0, 1 } } };
    int same = (uint32_t)g_vk.width == g_vk.sc_ext.width && (uint32_t)g_vk.height == g_vk.sc_ext.height;
    vkCmdBlitImage(g_vk.cmd, g_vk.color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &blit, same ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
    image_barrier(dst, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                  VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    if (!flush_sync(g_vk.acquire_sem, g_vk.sc_render_sem[idx])) return -1;
    VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, NULL, 1, &g_vk.sc_render_sem[idx], 1, &g_vk.sc, &idx, NULL };
    VkResult pr = vkQueuePresentKHR(g_vk.queue, &pi);
    if (pr == VK_SUBOPTIMAL_KHR || pr == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) g_vk.sc_dirty = 1;
    else if (pr != VK_SUCCESS) { vk_ok(pr, "vkQueuePresentKHR"); g_vk.sc_dirty = 1; }
    return 1;
}

RhiResult rhi_frame_end(void) {
    if (!g_vk.device || !g_vk.in_frame) return RHI_ERR_INVALID;
    if (g_vk.pass.active) rhi_cmd_end_pass();         /* a pass does not outlive its frame */
    if (g_vk.rendering) end_rendering_if_active();   /* never submit an open render scope */
    int presented = present_frame();
    if (presented < 0 || (presented == 0 && !flush())) return RHI_ERR_DEVICE_LOST;   /* else: submit + fence wait, offscreen */
    /* flush() drained the deferred frees: every recorded draw has completed (Fix 2) */
    g_vk.in_frame = 0;
    g_vk.frame_index++;
    return RHI_OK;
}

/* -------------------------------------------------------------------------- */
/* Clear                                                                      */
/* -------------------------------------------------------------------------- */
/* Clears the whole main target with transfer clears; contents are discarded
 * (oldLayout UNDEFINED), matching rhi_cmd_clear_main's "clears the whole target"
 * contract. The colour image is left in TRANSFER_SRC_OPTIMAL ready for readback. */
RhiResult rhi_cmd_clear_main(const float color[4], float depth) {
    if (!color || !g_vk.in_frame || g_vk.pass.active) return RHI_ERR_INVALID;
    if (!ensure_recording()) return RHI_ERR_DEVICE_LOST;
    end_rendering_if_active();   /* a clear is a transfer: not inside a render scope */
    /* rhi.h: the viewport stays until rhi_cmd_clear_main, which (like GL) resets it
     * to the whole target. */
    g_vk.viewport[0] = 0; g_vk.viewport[1] = 0;
    g_vk.viewport[2] = g_vk.width; g_vk.viewport[3] = g_vk.height;
    VkImageSubresourceRange crange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkImageSubresourceRange drange = { VK_DS_ASPECT, 0, 1, 0, 1 };

    /* Wait on any prior transfer to these images this frame (an earlier clear,
       a read_pixels or a copy_main_to_texture): source stage TRANSFER with
       TRANSFER_WRITE|READ, not TOP_OF_PIPE, so a second clear is hazard-free.
       oldLayout stays UNDEFINED since the clear discards the old contents. */
    image_barrier(g_vk.color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkClearColorValue cc; memcpy(cc.float32, color, sizeof cc.float32);
    vkCmdClearColorImage(g_vk.cmd, g_vk.color, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cc, 1, &crange);
    /* Settle to TRANSFER_SRC so a later read_pixels copy needs no extra barrier. */
    image_barrier(g_vk.color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    g_vk.color_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

    /* Wait on a prior clear's transfer AND any prior rendering scope's depth
     * writes (early/late fragment tests) before this clear overwrites depth
     * (P11-review Fix 3); oldLayout UNDEFINED since the clear discards contents. */
    image_barrier(g_vk.depth, VK_DS_ASPECT, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT |
                      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                  VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT |
                      VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkClearDepthStencilValue cd = { depth, 0 };   /* stencil clears to 0 like GL's clear_main */
    vkCmdClearDepthStencilImage(g_vk.cmd, g_vk.depth, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cd, 1, &drange);
    g_vk.depth_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;   /* left for ensure_rendering */
    return RHI_OK;
}

/* -------------------------------------------------------------------------- */
/* Readback                                                                   */
/* -------------------------------------------------------------------------- */
/* Copies a w x h rectangle of the offscreen colour image into the host buffer
 * and converts it into `dst`, rows top-down, matching rhi_gl.c's glReadPixels
 * output. (x, y) is the rectangle's top-left corner, as in rhi.h. Draws use a
 * positive-height viewport with VkViewport.y = gl_y (see the orientation note at
 * ensure_rendering), so image memory row r is GL window row r (bottom-up): the
 * copy starts at row frame_h - y - h and the rows are flipped into dst to give
 * top-down order. Blocks until the copy has finished (submit + fence wait). */
RhiResult rhi_read_pixels(int32_t x, int32_t y, int32_t w, int32_t h,
                          RhiReadFormat format, void *dst, size_t stride) {
    size_t bpp = format == RHI_READ_RGB8 ? 3 : format == RHI_READ_RGBA8 ? 4 : 0;
    if (!g_vk.in_frame || g_vk.pass.active || !dst || !bpp || w <= 0 || h <= 0 || x < 0 || y < 0 ||
        w > g_vk.width - x || h > g_vk.height - y) return RHI_ERR_INVALID;
    size_t limit = (size_t)PTRDIFF_MAX;
    if ((size_t)w > limit / 4 / (size_t)h) return RHI_ERR_INVALID;
    size_t row_bytes = (size_t)w * bpp;
    if (!stride) stride = row_bytes;
    if (stride < row_bytes || (size_t)(h - 1) > (limit - row_bytes) / stride) return RHI_ERR_INVALID;
    if (!ensure_recording()) return RHI_ERR_DEVICE_LOST;
    end_rendering_if_active();   /* readback is a transfer: close any draw scope first */

    /* The colour image is in TRANSFER_SRC after a clear; guard in case it is not. */
    if (g_vk.color_layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
        image_barrier(g_vk.color, VK_IMAGE_ASPECT_COLOR_BIT, g_vk.color_layout,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, VK_ACCESS_TRANSFER_READ_BIT,
                      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        g_vk.color_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    }
    /* rhi.h: x, y is the rectangle's TOP-left corner. Image memory row r is GL window
       row r (bottom-up, see the orientation note at ensure_rendering), so the
       rectangle starts at row frame_h - y - h -- the same conversion rhi_gl.c hands
       glReadPixels -- and the per-row flip below yields top-down output. */
    VkBufferImageCopy reg = { 0, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        { x, g_vk.height - y - h, 0 }, { (uint32_t)w, (uint32_t)h, 1 } };
    vkCmdCopyImageToBuffer(g_vk.cmd, g_vk.color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_vk.readback, 1, &reg);
    VkBufferMemoryBarrier bmb = { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_ACCESS_HOST_READ_BIT, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, g_vk.readback, 0, VK_WHOLE_SIZE };
    vkCmdPipelineBarrier(g_vk.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL,
                         1, &bmb, 0, NULL);
    if (!flush()) return RHI_ERR_DEVICE_LOST;

    unsigned char *map = NULL;
    if (!vk_ok(vkMapMemory(g_vk.device, g_vk.readback_mem, 0, VK_WHOLE_SIZE, 0, (void **)&map), "vkMapMemory"))
        return RHI_ERR_DEVICE_LOST;
    /* RGBA8 storage, R at byte 0 (like GL's GL_RGBA read); flip rows to top-down. */
    for (int32_t row = 0; row < h; row++) {
        const unsigned char *src = map + (size_t)(h - 1 - row) * w * 4;
        unsigned char *o = (unsigned char *)dst + (size_t)row * stride;
        if (bpp == 4) { memcpy(o, src, (size_t)w * 4); continue; }
        for (int32_t i = 0; i < w; i++) { o[i*3] = src[i*4]; o[i*3+1] = src[i*4+1]; o[i*3+2] = src[i*4+2]; }
    }
    vkUnmapMemory(g_vk.device, g_vk.readback_mem);
    return RHI_OK;
}

/* ========================================================================== */
/* Stubs for the rest of the rhi.h surface (filled in by P11b..P11g).         */
/* Handle-returning: {0}; RhiResult: RHI_ERR_UNSUPPORTED; void: no-op.        */
/* ========================================================================== */

/* --- Buffers (P11b) ---
 * The handle table mirrors the GL backend's (slot + generation, free list);
 * STATIC buffers are device-local and filled through a staging copy, STREAM
 * buffers are host-visible + coherent and persistently mapped. */

/* Resolve a handle to its live slot, logging a bounded number of stale hits
 * exactly like rhi_gl.c's buffer_lookup. */
static RhiVkBuffer *buffer_lookup(RhiBuffer buf, const char *what) {
    uint32_t slot = buf.id & VK_SLOT_MASK;
    if (!buf.id) return NULL;
    if (slot && slot <= g_vk.buffers.count) {
        RhiVkBuffer *b = &g_vk.buffers.slots[slot - 1];
        if (b->live && b->generation == (uint8_t)(buf.id >> VK_SLOT_BITS)) return b;
    }
    if (g_vk.buffers.stale_logged < 8) {
        g_vk.buffers.stale_logged++;
        fprintf(stderr, "rhi: %s: stale or invalid buffer handle 0x%08x\n", what, (unsigned)buf.id);
    }
    return NULL;
}

static int buffer_size_ok(size_t size) {
    return size <= (size_t)PTRDIFF_MAX;
}

static uint32_t buffer_slot_alloc(void) {
    if (g_vk.buffers.free_count) return g_vk.buffers.free_list[--g_vk.buffers.free_count];
    if (g_vk.buffers.count == g_vk.buffers.capacity) {
        uint32_t cap = g_vk.buffers.capacity ? g_vk.buffers.capacity * 2u : 256u;
        if (cap > VK_SLOT_MASK) return 0;
        RhiVkBuffer *slots = (RhiVkBuffer *)realloc(g_vk.buffers.slots, cap * sizeof *slots);
        if (!slots) return 0;
        g_vk.buffers.slots = slots;
        uint32_t *list = (uint32_t *)realloc(g_vk.buffers.free_list, cap * sizeof *list);
        if (!list) return 0;
        g_vk.buffers.free_list = list;
        g_vk.buffers.capacity = cap;
    }
    memset(&g_vk.buffers.slots[g_vk.buffers.count], 0, sizeof g_vk.buffers.slots[0]);
    return ++g_vk.buffers.count;
}

/* Give `b` fresh storage of `size` bytes (any previous storage must already be
 * released) and optionally fill it from `data`. size 0 leaves the slot empty. */
static RhiResult buffer_alloc_storage(RhiVkBuffer *b, size_t size, const void *data) {
    b->buf = VK_NULL_HANDLE; b->mem = VK_NULL_HANDLE; b->mapped = NULL; b->size = 0;
    if (!size) return RHI_OK;
    if (b->update == RHI_UPDATE_STREAM) {
        if (!make_buffer(size, b->usage,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                         &b->buf, &b->mem))
            return RHI_ERR_DEVICE_LOST;
        if (!vk_ok(vkMapMemory(g_vk.device, b->mem, 0, VK_WHOLE_SIZE, 0, &b->mapped), "vkMapMemory(stream)")) {
            vkDestroyBuffer(g_vk.device, b->buf, NULL); vkFreeMemory(g_vk.device, b->mem, NULL);
            b->buf = VK_NULL_HANDLE; b->mem = VK_NULL_HANDLE; b->mapped = NULL;
            return RHI_ERR_DEVICE_LOST;
        }
        if (data) memcpy(b->mapped, data, size);
    } else {
        if (!make_buffer(size, b->usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &b->buf, &b->mem))
            return RHI_ERR_DEVICE_LOST;
        if (data && !stage_to_device(b->buf, 0, data, size)) {
            vkDestroyBuffer(g_vk.device, b->buf, NULL); vkFreeMemory(g_vk.device, b->mem, NULL);
            b->buf = VK_NULL_HANDLE; b->mem = VK_NULL_HANDLE;
            return RHI_ERR_DEVICE_LOST;
        }
    }
    b->size = size;
    return RHI_OK;
}

RhiBuffer rhi_buffer_create(const RhiBufferDesc *desc) {
    RhiBuffer out = {0};
    if (!desc || (desc->usage != RHI_BUFFER_VERTEX && desc->usage != RHI_BUFFER_INDEX) ||
        (desc->update != RHI_UPDATE_STATIC && desc->update != RHI_UPDATE_STREAM) ||
        !buffer_size_ok(desc->size)) {
        fprintf(stderr, "rhi: buffer_create: invalid description\n");
        return out;
    }
    if (!g_vk.device) { fprintf(stderr, "rhi: buffer_create: no device\n"); return out; }
    uint32_t slot = buffer_slot_alloc();
    if (!slot) { fprintf(stderr, "rhi: buffer_create: out of handles\n"); return out; }
    RhiVkBuffer *b = &g_vk.buffers.slots[slot - 1];
    b->update = (uint8_t)desc->update;
    b->usage  = desc->usage == RHI_BUFFER_INDEX ? VK_BUFFER_USAGE_INDEX_BUFFER_BIT
                                                : VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    b->buf = VK_NULL_HANDLE; b->mem = VK_NULL_HANDLE; b->mapped = NULL; b->size = 0;
    b->in_arena = 0; b->in_use = 0;
    b->live = 1;
    out.id = ((uint32_t)b->generation << VK_SLOT_BITS) | slot;
    /* A STREAM buffer with no size/data gets its storage from the first replace. */
    if (desc->update == RHI_UPDATE_STATIC || desc->size || desc->data) {
        if (buffer_alloc_storage(b, desc->size, desc->data) != RHI_OK) {
            fprintf(stderr, "rhi: buffer_create: %zu bytes could not be allocated\n", desc->size);
            rhi_buffer_destroy(out);
            return (RhiBuffer){0};
        }
    }
    return out;
}

/* Give an in-use STREAM buffer a fresh version to write. 1 on success. */
static int stream_cow(RhiVkBuffer *b) {
    VkDeviceSize off = (g_vk.stream.offset + 15u) & ~(VkDeviceSize)15u;
    VkDeviceSize vdem = (g_vk.stream.demand + 15u) & ~(VkDeviceSize)15u;
    g_vk.stream.demand = vdem + b->size;   /* frame_begin grows the arena to fit */
    if (off + b->size > g_vk.stream.size) {
        /* flush() copies every arena version back and clears in_use: the buffer's
         * own storage is current and nothing recorded reads it any more. */
        return frame_split();
    }
    memcpy(g_vk.stream.mapped + off, buf_cur_ptr(b), (size_t)b->size);
    b->in_arena = 1; b->cur_off = off; b->in_use = 0;
    g_vk.stream.offset = off + b->size;
    return 1;
}

RhiResult rhi_buffer_update(RhiBuffer buf, size_t offset, size_t size, const void *data) {
    RhiVkBuffer *b = buffer_lookup(buf, "buffer_update");
    if (!b || !data || offset > b->size || size > b->size - offset) return RHI_ERR_INVALID;
    if (!size) return RHI_OK;
    if (b->update == RHI_UPDATE_STREAM) {
        /* A recorded draw still reads the current version: write a copy instead,
         * or (arena full) submit first so the write lands in settled storage. */
        if (b->in_use && !stream_cow(b)) return RHI_ERR_DEVICE_LOST;
        memcpy(buf_cur_ptr(b) + offset, data, size);
        return RHI_OK;
    }
    /* STATIC storage is written immediately (a submitted staging copy), so a draw already recorded this frame would see the new
     * contents when it executes: submit those draws first. */
    if (b->in_use && !frame_split()) return RHI_ERR_DEVICE_LOST;
    return stage_to_device(b->buf, offset, data, size) ? RHI_OK : RHI_ERR_DEVICE_LOST;
}

RhiResult rhi_buffer_replace(RhiBuffer buf, size_t size, const void *data) {
    RhiVkBuffer *b = buffer_lookup(buf, "buffer_replace");
    if (!b || !buffer_size_ok(size)) return RHI_ERR_INVALID;
    /* Fresh storage (orphan/realloc). A recorded-but-unsubmitted draw this frame
     * may still reference the old VkBuffer, so defer its free to frame_end (after
     * submit + fence wait) while in a frame; free immediately otherwise (Fix 2). */
    if (b->mapped) { vkUnmapMemory(g_vk.device, b->mem); b->mapped = NULL; }
    defer_or_free_buffer(b->buf, b->mem);
    b->buf = VK_NULL_HANDLE; b->mem = VK_NULL_HANDLE;
    b->in_arena = 0; b->in_use = 0;   /* fresh storage; earlier draws keep their arena copy */
    return buffer_alloc_storage(b, size, data);
}

void rhi_buffer_destroy(RhiBuffer buf) {
    RhiVkBuffer *b = buffer_lookup(buf, "buffer_destroy");
    if (!b) return;
    if (b->mapped) { vkUnmapMemory(g_vk.device, b->mem); b->mapped = NULL; }
    /* A recorded-but-unsubmitted draw may reference this buffer: defer its free to
     * frame_end while in a frame, free immediately otherwise (P11-review Fix 2). */
    defer_or_free_buffer(b->buf, b->mem);
    b->buf = VK_NULL_HANDLE; b->mem = VK_NULL_HANDLE; b->size = 0; b->live = 0;
    b->in_arena = 0; b->in_use = 0;
    /* Retire the slot before its generation wraps and revives stale handles. */
    if (b->generation == UINT8_MAX) return;
    b->generation++;
    g_vk.buffers.free_list[g_vk.buffers.free_count++] = buf.id & VK_SLOT_MASK;
}

/* Test-only (not in rhi.h): read `size` bytes at `offset` of `buf` into `dst`.
 * STREAM reads the persistent mapping; STATIC copies device-local -> a host
 * staging buffer with vkCmdCopyBuffer. The smoke declares it extern to verify
 * buffer contents, since rhi.h exposes no buffer readback. 1 ok, 0 on failure. */
int rhi_vk_buffer_readback(RhiBuffer buf, size_t offset, size_t size, void *dst) {
    RhiVkBuffer *b = buffer_lookup(buf, "buffer_readback");
    if (!b || !dst || offset > b->size || size > b->size - offset) return 0;
    if (!size) return 1;
    if (b->update == RHI_UPDATE_STREAM) {
        memcpy(dst, buf_cur_ptr(b) + offset, size);
        return 1;
    }
    VkBuffer staging; VkDeviceMemory smem;
    if (!make_buffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     &staging, &smem)) return 0;
    int ok = copy_buffer(b->buf, offset, staging, 0, size);
    if (ok) {
        void *map = NULL;
        ok = vk_ok(vkMapMemory(g_vk.device, smem, 0, VK_WHOLE_SIZE, 0, &map), "vkMapMemory(readback)");
        if (ok) { memcpy(dst, map, size); vkUnmapMemory(g_vk.device, smem); }
    }
    vkDestroyBuffer(g_vk.device, staging, NULL);
    vkFreeMemory(g_vk.device, smem, NULL);
    return ok;
}

/* --- Textures (P11c) ---
 * The handle table mirrors the buffer table. A texture is a VkImage with
 * device-local memory and a default sampling view. Uploads go through a
 * host-visible staging buffer and a one-shot vkCmdCopyBufferToImage; mip
 * generation blits the chain down. Every transfer runs on its own fenced
 * command buffer (like copy_buffer/stage_to_device), so texture work is safe
 * whether or not a frame is open. RGB8 is stored as RGBA8 (A expanded to 255
 * on upload); BC1/2/3 store the compressed blocks verbatim. */

static const RhiSamplerDesc k_default_sampler = {
    RHI_FILTER_NEAREST, RHI_FILTER_LINEAR, RHI_MIP_LINEAR, RHI_WRAP_REPEAT, RHI_WRAP_REPEAT, 1.0f
};

static int format_compressed(RhiFormat f) {
    return f == RHI_FORMAT_BC1 || f == RHI_FORMAT_BC2 || f == RHI_FORMAT_BC3;
}

/* Bytes per 4x4 block for the BC formats (0 if not block-compressed). */
static uint32_t format_block_bytes(RhiFormat f) {
    return f == RHI_FORMAT_BC1 ? 8u : (f == RHI_FORMAT_BC2 || f == RHI_FORMAT_BC3) ? 16u : 0u;
}

/* Where a texture rests after an upload / mip generation. SHADER_READ_ONLY only
 * when it is sampled (that layout requires SAMPLED/INPUT_ATTACHMENT usage);
 * otherwise GENERAL, which every usage accepts and the next use re-barriers. */
static VkImageLayout rest_layout(uint32_t usage) {
    return (usage & RHI_TEXTURE_SAMPLED) ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                         : VK_IMAGE_LAYOUT_GENERAL;
}

/* full mip chain length for a w x h image: floor(log2(max(w,h))) + 1 */
static uint32_t mip_count(int32_t w, int32_t h) {
    uint32_t m = (uint32_t)(w > h ? w : h), n = 1;
    while (m > 1) { m >>= 1u; n++; }
    return n;
}

/* Record a layout transition on `cb` over an explicit subresource range. */
static void img_barrier(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect,
                        uint32_t base_lvl, uint32_t n_lvl, uint32_t base_layer, uint32_t n_layer,
                        VkImageLayout old, VkImageLayout neu, VkAccessFlags srcA, VkAccessFlags dstA,
                        VkPipelineStageFlags srcS, VkPipelineStageFlags dstS) {
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, srcA, dstA, old, neu,
        VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, img,
        { aspect, base_lvl, n_lvl, base_layer, n_layer } };
    vkCmdPipelineBarrier(cb, srcS, dstS, 0, 0, NULL, 0, NULL, 1, &b);
}

/* Allocate + begin a one-shot primary command buffer; VK_NULL_HANDLE on error. */
static VkCommandBuffer oneshot_begin(void) {
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, g_vk.pool,
        VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
    if (!vk_ok(vkAllocateCommandBuffers(g_vk.device, &cbai, &cb), "vkAllocateCommandBuffers(oneshot)"))
        return VK_NULL_HANDLE;
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
        VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL };
    if (!vk_ok(vkBeginCommandBuffer(cb, &bi), "vkBeginCommandBuffer(oneshot)")) {
        vkFreeCommandBuffers(g_vk.device, g_vk.pool, 1, &cb);
        return VK_NULL_HANDLE;
    }
    return cb;
}

/* End, submit and block on `cb`, then free it. 1 on success. */
static int oneshot_end(VkCommandBuffer cb) {
    int ok = vk_ok(vkEndCommandBuffer(cb), "vkEndCommandBuffer(oneshot)");
    VkFence fence = VK_NULL_HANDLE;
    if (ok) {
        VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, 0 };
        ok = vk_ok(vkCreateFence(g_vk.device, &fci, NULL, &fence), "vkCreateFence(oneshot)");
    }
    if (ok) {
        VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL };
        ok = vk_ok(vkQueueSubmit(g_vk.queue, 1, &si, fence), "vkQueueSubmit(oneshot)") &&
             vk_ok(vkWaitForFences(g_vk.device, 1, &fence, VK_TRUE, UINT64_MAX), "vkWaitForFences(oneshot)");
    }
    if (fence) vkDestroyFence(g_vk.device, fence, NULL);
    vkFreeCommandBuffers(g_vk.device, g_vk.pool, 1, &cb);
    return ok;
}

/* Mark `t` as referenced by the frame being recorded (not yet submitted). */
static void texture_touch(RhiVkTexture *t) {
    t->touched = g_vk.submits + 1u;
}

/* The immediate-mode contract (GL) says a CPU-side change to a texture happens
 * AFTER the draws already issued. Draws are only recorded here, so if the
 * unsubmitted frame references `t`, submit it first (a mid-frame split). */
static int texture_sync_for_write(const RhiVkTexture *t) {
    if (g_vk.in_frame && g_vk.recording && t->touched == g_vk.submits + 1u) return frame_split();
    return 1;
}

/* One-shot zero clear of every level/layer, then SHADER_READ_ONLY, so a render
 * target (or depth copy destination) is defined before its first render. */
static int texture_init_clear(RhiVkTexture *t) {
    VkCommandBuffer cb = oneshot_begin();
    if (!cb) return 0;
    img_barrier(cb, t->img, t->bar_aspect, 0, t->mip_levels, 0, t->layers,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkImageSubresourceRange rng = { t->bar_aspect, 0, t->mip_levels, 0, t->layers };
    if (t->aspect == VK_IMAGE_ASPECT_COLOR_BIT) {
        VkClearColorValue zero = {{ 0.0f, 0.0f, 0.0f, 0.0f }};
        vkCmdClearColorImage(cb, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &rng);
    } else {
        VkClearDepthStencilValue zero = { 0.0f, 0 };
        vkCmdClearDepthStencilImage(cb, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &rng);
    }
    img_barrier(cb, t->img, t->bar_aspect, 0, t->mip_levels, 0, t->layers,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    if (!oneshot_end(cb)) return 0;
    t->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return 1;
}

static VkFormat texture_vkformat(RhiFormat f) {
    switch (f) {
    case RHI_FORMAT_RGB8:
    case RHI_FORMAT_RGBA8: return VK_FORMAT_R8G8B8A8_UNORM;   /* RGB8 stored as RGBA8 */
    case RHI_FORMAT_BC1:   return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case RHI_FORMAT_BC2:   return VK_FORMAT_BC2_UNORM_BLOCK;
    case RHI_FORMAT_BC3:   return VK_FORMAT_BC3_UNORM_BLOCK;
    case RHI_FORMAT_D16:   return VK_FORMAT_D16_UNORM;
    case RHI_FORMAT_DEPTH: return g_vk.depth_fmt;             /* the main target's precision */
    default:               return VK_FORMAT_UNDEFINED;
    }
}

static RhiVkTexture *texture_find(RhiTexture tex) {
    uint32_t slot = tex.id & VK_SLOT_MASK;
    if (!tex.id) return NULL;
    if (slot && slot <= g_vk.textures.count) {
        RhiVkTexture *t = &g_vk.textures.slots[slot - 1];
        if (t->live && t->generation == (uint8_t)(tex.id >> VK_SLOT_BITS)) return t;
    }
    return NULL;
}

static RhiVkTexture *texture_lookup(RhiTexture tex, const char *what) {
    RhiVkTexture *t = texture_find(tex);
    if (!t && tex.id && g_vk.textures.stale_logged < 8) {
        g_vk.textures.stale_logged++;
        fprintf(stderr, "rhi: %s: stale or invalid texture handle 0x%08x\n", what, (unsigned)tex.id);
    }
    return t;
}

static uint32_t texture_slot_alloc(void) {
    if (g_vk.textures.free_count) return g_vk.textures.free_list[--g_vk.textures.free_count];
    if (g_vk.textures.count == g_vk.textures.capacity) {
        uint32_t cap = g_vk.textures.capacity ? g_vk.textures.capacity * 2u : 256u;
        if (cap > VK_SLOT_MASK) return 0;
        RhiVkTexture *slots = (RhiVkTexture *)realloc(g_vk.textures.slots, cap * sizeof *slots);
        if (!slots) return 0;
        g_vk.textures.slots = slots;
        uint32_t *list = (uint32_t *)realloc(g_vk.textures.free_list, cap * sizeof *list);
        if (!list) return 0;
        g_vk.textures.free_list = list;
        g_vk.textures.capacity = cap;
    }
    memset(&g_vk.textures.slots[g_vk.textures.count], 0, sizeof g_vk.textures.slots[0]);
    return ++g_vk.textures.count;
}

/* Create the image + memory + default view for `t` (format/usage/layers already
 * set) at w x h with `levels` mips. All three out values or none (partial objects
 * are cleaned up). */
static int texture_make_storage(const RhiVkTexture *t, int32_t w, int32_t h, uint32_t levels,
                                VkImage *img, VkDeviceMemory *mem, VkImageView *view) {
    *img = VK_NULL_HANDLE; *mem = VK_NULL_HANDLE; *view = VK_NULL_HANDLE;
    int cube = t->layers == 6;
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL,
        cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0u,
        VK_IMAGE_TYPE_2D, t->vkfmt, { (uint32_t)w, (uint32_t)h, 1 }, levels, t->layers,
        VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, t->vku, VK_SHARING_MODE_EXCLUSIVE, 0, NULL,
        VK_IMAGE_LAYOUT_UNDEFINED };
    if (!vk_ok(vkCreateImage(g_vk.device, &ici, NULL, img), "vkCreateImage(texture)")) goto fail;
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(g_vk.device, *img, &mr);
    uint32_t mt = find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt == UINT32_MAX) { fprintf(stderr, "rhi_vk: no device-local memory type\n"); goto fail; }
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, mt };
    if (!vk_ok(vkAllocateMemory(g_vk.device, &ai, NULL, mem), "vkAllocateMemory(texture)")) goto fail;
    if (!vk_ok(vkBindImageMemory(g_vk.device, *img, *mem, 0), "vkBindImageMemory(texture)")) goto fail;
    /* A view needs a view-compatible usage (sampled / attachment). A transfer-
     * only COPY_DST texture has none, so it gets no view (nor is it ever sampled). */
    if (t->usage & (RHI_TEXTURE_SAMPLED | RHI_TEXTURE_COLOR_TARGET | RHI_TEXTURE_DEPTH_TARGET)) {
        VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, *img,
            cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D, t->vkfmt,
            { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
              VK_COMPONENT_SWIZZLE_IDENTITY }, { t->aspect, 0, levels, 0, t->layers } };
        if (!vk_ok(vkCreateImageView(g_vk.device, &vci, NULL, view), "vkCreateImageView(texture)")) goto fail;
    }
    return 1;
fail:
    if (*view) vkDestroyImageView(g_vk.device, *view, NULL);
    if (*img)  vkDestroyImage(g_vk.device, *img, NULL);
    if (*mem)  vkFreeMemory(g_vk.device, *mem, NULL);
    *img = VK_NULL_HANDLE; *mem = VK_NULL_HANDLE; *view = VK_NULL_HANDLE;
    return 0;
}

/* Single-level, single-layer 2D view of `face` for use as a pass attachment
 * (the default view covers every mip / the cube). Made lazily. */
static VkImageView texture_att_view(RhiVkTexture *t, int face) {
    if (face < 0 || face >= 6 || (uint32_t)face >= t->layers) return VK_NULL_HANDLE;
    if (!t->att_view[face]) {
        VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, t->img,
            VK_IMAGE_VIEW_TYPE_2D, t->vkfmt,
            { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
              VK_COMPONENT_SWIZZLE_IDENTITY }, { t->aspect, 0, 1, (uint32_t)face, 1 } };
        if (!vk_ok(vkCreateImageView(g_vk.device, &vci, NULL, &t->att_view[face]), "vkCreateImageView(attachment)"))
            t->att_view[face] = VK_NULL_HANDLE;
    }
    return t->att_view[face];
}

/* Hand the texture's image, memory and views to the deferred-free list (the
 * unsubmitted frame may still reference them). Clears the slot's fields. */
static void texture_release_storage(RhiVkTexture *t) {
    for (int i = 0; i < 6; i++) {
        if (t->att_view[i]) defer_or_free(VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, t->att_view[i]);
        t->att_view[i] = VK_NULL_HANDLE;
    }
    defer_or_free(VK_NULL_HANDLE, t->mem, t->img, t->view);
    t->view = VK_NULL_HANDLE; t->img = VK_NULL_HANDLE; t->mem = VK_NULL_HANDLE;
}

RhiTexture rhi_texture_create(const RhiTextureDesc *desc) {
    RhiTexture out = {0};
    int depth16 = desc && desc->format == RHI_FORMAT_D16;
    /* Validation mirrors the GL backend exactly (see rhi_texture_create there). */
    if (!desc || (desc->type != RHI_TEXTURE_2D && desc->type != RHI_TEXTURE_CUBE) ||
        desc->format < RHI_FORMAT_RGB8 || desc->format > RHI_FORMAT_DEPTH ||
        desc->width <= 0 || desc->height <= 0 || !desc->usage ||
        (desc->usage & ~(uint32_t)(RHI_TEXTURE_SAMPLED | RHI_TEXTURE_COLOR_TARGET |
                                   RHI_TEXTURE_DEPTH_TARGET | RHI_TEXTURE_COPY_DST)) ||
        (desc->format == RHI_FORMAT_DEPTH &&
         (desc->type != RHI_TEXTURE_2D || desc->usage != (RHI_TEXTURE_SAMPLED | RHI_TEXTURE_COPY_DST))) ||
        (depth16 && (desc->type != RHI_TEXTURE_2D || desc->usage != RHI_TEXTURE_DEPTH_TARGET)) ||
        (!depth16 && (desc->usage & RHI_TEXTURE_DEPTH_TARGET)) ||
        (desc->type == RHI_TEXTURE_CUBE && desc->width != desc->height)) {
        fprintf(stderr, "rhi: texture_create: invalid description\n");
        return out;
    }
    if (!g_vk.device) { fprintf(stderr, "rhi: texture_create: no device\n"); return out; }

    int is_depth = depth16 || desc->format == RHI_FORMAT_DEPTH;
    VkFormat vkfmt = texture_vkformat(desc->format);
    uint32_t layers = desc->type == RHI_TEXTURE_CUBE ? 6u : 1u;
    /* Depth images are single-level; colour images get a full mip chain so they
     * can receive uploads up to level floor(log2(max(w,h))) (matches GL). */
    uint32_t levels = is_depth ? 1u : mip_count(desc->width, desc->height);

    VkImageUsageFlags vku = 0;
    if (desc->usage & RHI_TEXTURE_SAMPLED)       vku |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (desc->usage & RHI_TEXTURE_COLOR_TARGET)  vku |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (desc->usage & RHI_TEXTURE_DEPTH_TARGET)  vku |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (desc->usage & RHI_TEXTURE_COPY_DST)      vku |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    /* Colour textures are uploadable and blittable (mips) and readable by the
     * test path, so they always carry both transfer bits. */
    if (!is_depth) vku |= VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    /* Main-precision depth copies are readable by the test path too. */
    if (desc->format == RHI_FORMAT_DEPTH) vku |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

    uint32_t slot = texture_slot_alloc();
    if (!slot) { fprintf(stderr, "rhi: texture_create: out of handles\n"); return out; }
    RhiVkTexture *t = &g_vk.textures.slots[slot - 1];
    t->img = VK_NULL_HANDLE; t->mem = VK_NULL_HANDLE; t->view = VK_NULL_HANDLE;
    memset(t->att_view, 0, sizeof t->att_view);
    t->touched = 0;
    t->vku = vku;
    t->vkfmt = vkfmt;
    t->aspect = is_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    t->bar_aspect = desc->format == RHI_FORMAT_DEPTH ? VK_DS_ASPECT : t->aspect;   /* D24S8 has stencil too */
    t->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    t->format = desc->format;
    t->usage = desc->usage;
    t->width = desc->width;
    t->height = desc->height;
    t->layers = layers;
    t->mip_levels = levels;
    t->live = 1;
    out.id = ((uint32_t)t->generation << VK_SLOT_BITS) | slot;

    if (!texture_make_storage(t, desc->width, desc->height, levels, &t->img, &t->mem, &t->view)) goto fail;

    /* A fresh texture starts with the documented default sampler. */
    t->sampler = rhi_sampler_create(&k_default_sampler);
    if (!t->sampler.id) goto fail;

    /* Render targets and depth-copy destinations start defined (zero) in
     * SHADER_READ_ONLY, so sampling one before it is first rendered/copied is valid. */
    if (((desc->usage & RHI_TEXTURE_SAMPLED) && (desc->usage & RHI_TEXTURE_COLOR_TARGET)) ||
        desc->format == RHI_FORMAT_DEPTH) {
        if (!texture_init_clear(t)) goto fail;
    }
    return out;

fail:
    rhi_texture_destroy(out);
    return (RhiTexture){0};
}

RhiResult rhi_texture_upload(RhiTexture tex, int32_t face, int32_t level, const void *data, size_t size) {
    RhiVkTexture *t = texture_lookup(tex, "texture_upload");
    int faces = t && t->layers == 6 ? 6 : 1;
    if (!t || t->format == RHI_FORMAT_D16 || t->format == RHI_FORMAT_DEPTH ||
        face < 0 || face >= faces || level < 0 || (uint32_t)level >= t->mip_levels)
        return RHI_ERR_INVALID;
    int compressed = format_compressed(t->format);
    if (compressed && (!data || !size || size > 0x7fffffff)) return RHI_ERR_INVALID;
    /* Uncompressed with no data: the level is already allocated; undefined is fine. */
    if (!compressed && !data) return RHI_OK;

    int32_t w = t->width >> level, h = t->height >> level;
    if (w < 1) w = 1;
    if (h < 1) h = 1;

    /* Compressed: the copy reads the mip's full block footprint, so `size` must
       cover it, else vkCmdCopyBufferToImage reads past the staging buffer. */
    if (compressed) {
        uint32_t blocks = (uint32_t)((w + 3) / 4) * (uint32_t)((h + 3) / 4);
        if (size < (size_t)blocks * format_block_bytes(t->format)) return RHI_ERR_INVALID;
    }

    /* Draws already issued this frame must see the OLD contents (GL semantics). */
    if (!texture_sync_for_write(t)) return RHI_ERR_DEVICE_LOST;

    /* Build the staging contents: BC blocks verbatim, RGBA8 verbatim, RGB8
     * expanded to RGBA8 with A = 255. */
    VkDeviceSize staging_size = compressed ? (VkDeviceSize)size : (VkDeviceSize)w * h * 4;
    VkBuffer staging; VkDeviceMemory smem;
    if (!make_buffer(staging_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     &staging, &smem)) return RHI_ERR_DEVICE_LOST;
    void *map = NULL;
    int ok = vk_ok(vkMapMemory(g_vk.device, smem, 0, VK_WHOLE_SIZE, 0, &map), "vkMapMemory(tex staging)");
    if (ok) {
        if (compressed) {
            memcpy(map, data, size);
        } else if (t->format == RHI_FORMAT_RGB8) {
            const unsigned char *s = (const unsigned char *)data;
            unsigned char *d = (unsigned char *)map;
            for (int32_t i = 0; i < w * h; i++) {
                d[i*4] = s[i*3]; d[i*4+1] = s[i*3+1]; d[i*4+2] = s[i*3+2]; d[i*4+3] = 255;
            }
        } else {
            memcpy(map, data, (size_t)w * h * 4);
        }
        vkUnmapMemory(g_vk.device, smem);
    }
    VkCommandBuffer cb = ok ? oneshot_begin() : VK_NULL_HANDLE;
    if (cb) {
        /* Whole image current -> TRANSFER_DST (preserves other levels unless it
         * was UNDEFINED), copy one subresource, then whole image -> SHADER_READ. */
        img_barrier(cb, t->img, t->aspect, 0, t->mip_levels, 0, t->layers,
                    t->layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy reg = { 0, 0, 0, { t->aspect, (uint32_t)level, (uint32_t)face, 1 },
            { 0, 0, 0 }, { (uint32_t)w, (uint32_t)h, 1 } };
        vkCmdCopyBufferToImage(cb, staging, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &reg);
        VkImageLayout rl = rest_layout(t->usage);
        int sampled = rl == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        img_barrier(cb, t->img, t->aspect, 0, t->mip_levels, 0, t->layers,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, rl,
                    VK_ACCESS_TRANSFER_WRITE_BIT, sampled ? VK_ACCESS_SHADER_READ_BIT : 0,
                    VK_PIPELINE_STAGE_TRANSFER_BIT,
                    sampled ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        ok = oneshot_end(cb);
        if (ok) t->layout = rl;
    } else {
        ok = 0;
    }
    vkDestroyBuffer(g_vk.device, staging, NULL);
    vkFreeMemory(g_vk.device, smem, NULL);
    return ok ? RHI_OK : RHI_ERR_DEVICE_LOST;
}

RhiResult rhi_texture_generate_mips(RhiTexture tex) {
    RhiVkTexture *t = texture_lookup(tex, "texture_generate_mips");
    if (!t || t->layers != 1 || (t->format != RHI_FORMAT_RGB8 && t->format != RHI_FORMAT_RGBA8))
        return RHI_ERR_INVALID;
    if (t->mip_levels < 2) return RHI_OK;   /* nothing to generate */
    if (!texture_sync_for_write(t)) return RHI_ERR_DEVICE_LOST;
    VkCommandBuffer cb = oneshot_begin();
    if (!cb) return RHI_ERR_DEVICE_LOST;
    /* Level 0 -> TRANSFER_SRC, then blit each level from the one above with a
     * LINEAR filter and the standard per-level barriers. */
    img_barrier(cb, t->img, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1,
                t->layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, VK_ACCESS_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    int32_t sw = t->width, sh = t->height;
    for (uint32_t i = 1; i < t->mip_levels; i++) {
        int32_t dw = sw > 1 ? sw >> 1 : 1, dh = sh > 1 ? sh >> 1 : 1;
        img_barrier(cb, t->img, VK_IMAGE_ASPECT_COLOR_BIT, i, 1, 0, 1,
                    VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    0, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkImageBlit blit = {
            { VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 0, 1 }, { { 0, 0, 0 }, { sw, sh, 1 } },
            { VK_IMAGE_ASPECT_COLOR_BIT, i,     0, 1 }, { { 0, 0, 0 }, { dw, dh, 1 } } };
        vkCmdBlitImage(cb, t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
        /* This level becomes the source for the next. */
        img_barrier(cb, t->img, VK_IMAGE_ASPECT_COLOR_BIT, i, 1, 0, 1,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        sw = dw; sh = dh;
    }
    /* Leave every level in the usage-appropriate rest layout. */
    VkImageLayout rl = rest_layout(t->usage);
    int sampled = rl == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    img_barrier(cb, t->img, VK_IMAGE_ASPECT_COLOR_BIT, 0, t->mip_levels, 0, 1,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rl,
                VK_ACCESS_TRANSFER_READ_BIT, sampled ? VK_ACCESS_SHADER_READ_BIT : 0,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                sampled ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    if (!oneshot_end(cb)) return RHI_ERR_DEVICE_LOST;
    t->layout = rl;
    return RHI_OK;
}

void rhi_texture_destroy(RhiTexture tex) {
    RhiVkTexture *t = texture_lookup(tex, "texture_destroy");
    if (!t) return;
    /* Destroying an attachment of the open pass ends the pass first. */
    if (g_vk.pass.active && (tex.id == g_vk.pass.color.id || tex.id == g_vk.pass.depth.id)) rhi_cmd_end_pass();
    /* The unsubmitted frame may still sample it: defer the free past the next submit. */
    texture_release_storage(t);
    t->live = 0;
    /* Retire the slot before its generation wraps and revives stale handles. */
    if (t->generation == UINT8_MAX) return;
    t->generation++;
    g_vk.textures.free_list[g_vk.textures.free_count++] = tex.id & VK_SLOT_MASK;
}

int rhi_texture_is_live(RhiTexture tex) {
    return texture_find(tex) != NULL;
}

uint32_t rhi_texture_native_id(RhiTexture tex) {
    /* Stable, nonzero per live texture (the slot index); 0 for {0}/destroyed.
     * An identity for equality, not an ordering, per the rhi.h contract. */
    return texture_find(tex) ? (tex.id & VK_SLOT_MASK) : 0u;
}

/* Test-only (not in rhi.h): read one uncompressed-colour mip level/face back
 * into `dst` as tightly packed RGBA8 (R at byte 0), via an image->host-buffer
 * copy like rhi_read_pixels. An RHI_FORMAT_DEPTH texture reads back as w*h
 * float32 depth values (0..1) instead. The smoke declares it extern to verify uploaded
 * pixels, since rhi.h exposes no texture readback. 1 ok, 0 on failure. */
int rhi_vk_texture_readback(RhiTexture tex, int32_t face, int32_t level, void *dst) {
    RhiVkTexture *t = texture_lookup(tex, "texture_readback");
    int faces = t && t->layers == 6 ? 6 : 1;
    int depth = t && t->format == RHI_FORMAT_DEPTH;   /* readable depth: the main-precision copy */
    if (!t || !dst || format_compressed(t->format) || (t->aspect != VK_IMAGE_ASPECT_COLOR_BIT && !depth) ||
        face < 0 || face >= faces || level < 0 || (uint32_t)level >= t->mip_levels) return 0;
    int32_t w = t->width >> level, h = t->height >> level;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    VkDeviceSize size = (VkDeviceSize)w * h * 4;
    if (!texture_sync_for_write(t)) return 0;   /* see what the frame drew into it so far */
    VkBuffer staging; VkDeviceMemory smem;
    if (!make_buffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     &staging, &smem)) return 0;
    VkCommandBuffer cb = oneshot_begin();
    int ok = cb != VK_NULL_HANDLE;
    if (ok) {
        img_barrier(cb, t->img, t->bar_aspect, (uint32_t)level, 1, (uint32_t)face, 1,
                    t->layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, VK_ACCESS_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy reg = { 0, 0, 0, { t->aspect, (uint32_t)level, (uint32_t)face, 1 },
            { 0, 0, 0 }, { (uint32_t)w, (uint32_t)h, 1 } };
        vkCmdCopyImageToBuffer(cb, t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1, &reg);
        /* Restore the subresource to the tracked whole-image layout. */
        img_barrier(cb, t->img, t->bar_aspect, (uint32_t)level, 1, (uint32_t)face, 1,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->layout, VK_ACCESS_TRANSFER_READ_BIT, 0,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        ok = oneshot_end(cb);
    }
    if (ok) {
        void *map = NULL;
        ok = vk_ok(vkMapMemory(g_vk.device, smem, 0, VK_WHOLE_SIZE, 0, &map), "vkMapMemory(tex readback)");
        if (ok) {
            if (depth) {   /* D24: low 24 bits of each 32-bit word; D32: float */
                const uint32_t *src = (const uint32_t *)map; float *o = (float *)dst;
                for (int32_t i = 0; i < w * h; i++) {
                    if (g_vk.depth_fmt == VK_FORMAT_D24_UNORM_S8_UINT) o[i] = (float)(src[i] & 0xFFFFFFu) / 16777215.0f;
                    else memcpy(&o[i], &src[i], sizeof o[i]);
                }
            } else memcpy(dst, map, (size_t)size);
            vkUnmapMemory(g_vk.device, smem);
        }
    }
    vkDestroyBuffer(g_vk.device, staging, NULL);
    vkFreeMemory(g_vk.device, smem, NULL);
    return ok;
}

/* --- Samplers (P11c) ---
 * A short list searched by value (GL's g_samplers): equal descriptions return
 * the same handle id (index + 1). Each distinct description gets one VkSampler. */
static int sampler_desc_valid(const RhiSamplerDesc *d) {
    return d && (d->min_filter == RHI_FILTER_NEAREST || d->min_filter == RHI_FILTER_LINEAR) &&
           (d->mag_filter == RHI_FILTER_NEAREST || d->mag_filter == RHI_FILTER_LINEAR) &&
           d->mip_filter >= RHI_MIP_NONE && d->mip_filter <= RHI_MIP_LINEAR &&
           (d->wrap_u == RHI_WRAP_REPEAT || d->wrap_u == RHI_WRAP_CLAMP) &&
           (d->wrap_v == RHI_WRAP_REPEAT || d->wrap_v == RHI_WRAP_CLAMP) &&
           d->max_anisotropy >= 1.0f;
}

static VkSamplerAddressMode vk_wrap(RhiWrap w) {
    return w == RHI_WRAP_CLAMP ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;
}

RhiSampler rhi_sampler_create(const RhiSamplerDesc *desc) {
    RhiSampler out = {0};
    if (!sampler_desc_valid(desc)) { fprintf(stderr, "rhi: sampler_create: invalid description\n"); return out; }
    for (uint32_t i = 0; i < g_vk.samplers.count; i++) {
        const RhiSamplerDesc *d = &g_vk.samplers.descs[i];
        if (d->min_filter == desc->min_filter && d->mag_filter == desc->mag_filter &&
            d->mip_filter == desc->mip_filter && d->wrap_u == desc->wrap_u &&
            d->wrap_v == desc->wrap_v && d->max_anisotropy == desc->max_anisotropy) {
            out.id = i + 1; return out;
        }
    }
    if (g_vk.samplers.count == g_vk.samplers.capacity) {
        uint32_t cap = g_vk.samplers.capacity ? g_vk.samplers.capacity * 2u : 16u;
        RhiSamplerDesc *descs = (RhiSamplerDesc *)realloc(g_vk.samplers.descs, cap * sizeof *descs);
        if (!descs) { fprintf(stderr, "rhi: sampler_create: out of memory\n"); return out; }
        g_vk.samplers.descs = descs;
        VkSampler *handles = (VkSampler *)realloc(g_vk.samplers.handles, cap * sizeof *handles);
        if (!handles) { fprintf(stderr, "rhi: sampler_create: out of memory\n"); return out; }
        g_vk.samplers.handles = handles;
        g_vk.samplers.capacity = cap;
    }
    /* Clamp anisotropy to the device limit, off at 1x or with no support. */
    VkBool32 aniso_en = VK_FALSE;
    float aniso = 1.0f;
    if (g_vk.caps.max_anisotropy > 1.0f && desc->max_anisotropy > 1.0f) {
        aniso_en = VK_TRUE;
        aniso = desc->max_anisotropy < g_vk.caps.max_anisotropy ? desc->max_anisotropy
                                                                : g_vk.caps.max_anisotropy;
    }
    VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, NULL, 0,
        desc->mag_filter == RHI_FILTER_LINEAR ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        desc->min_filter == RHI_FILTER_LINEAR ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        desc->mip_filter == RHI_MIP_LINEAR ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST,
        vk_wrap(desc->wrap_u), vk_wrap(desc->wrap_v), VK_SAMPLER_ADDRESS_MODE_REPEAT,
        0.0f, aniso_en, aniso, VK_FALSE, VK_COMPARE_OP_ALWAYS,
        0.0f, desc->mip_filter == RHI_MIP_NONE ? 0.0f : VK_LOD_CLAMP_NONE,
        VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK, VK_FALSE };
    VkSampler handle = VK_NULL_HANDLE;
    if (!vk_ok(vkCreateSampler(g_vk.device, &sci, NULL, &handle), "vkCreateSampler")) return out;
    g_vk.samplers.descs[g_vk.samplers.count] = *desc;
    g_vk.samplers.handles[g_vk.samplers.count] = handle;
    g_vk.samplers.count++;
    out.id = g_vk.samplers.count;
    return out;
}

const RhiSamplerDesc *rhi_sampler_desc(RhiSampler sampler) {
    if (!sampler.id || sampler.id > g_vk.samplers.count) return NULL;
    return &g_vk.samplers.descs[sampler.id - 1];
}

RhiResult rhi_texture_set_sampler(RhiTexture tex, RhiSampler sampler) {
    RhiVkTexture *t = texture_lookup(tex, "texture_set_sampler");
    const RhiSamplerDesc *d = rhi_sampler_desc(sampler);
    if (!t || !d || t->format == RHI_FORMAT_D16) return RHI_ERR_INVALID;   /* D16 is not sampled */
    t->sampler = sampler;
    return RHI_OK;
}

RhiSampler rhi_texture_sampler(RhiTexture tex) {
    RhiVkTexture *t = texture_lookup(tex, "texture_sampler");
    return t ? t->sampler : (RhiSampler){0};
}

/* --- Shaders / param blocks (P11d) --- */
/* std140 layout validation, identical to the GL backend (backend-neutral). */
static uint32_t param_size(RhiParamType t) {
    switch (t) {
    case RHI_PARAM_VEC2: return 8u;
    case RHI_PARAM_VEC3: return 12u;
    case RHI_PARAM_VEC4: return 16u;
    case RHI_PARAM_MAT4: return 64u;
    default:             return 4u;
    }
}
static uint32_t param_align(RhiParamType t) {
    return t == RHI_PARAM_VEC2 ? 8u : (t == RHI_PARAM_VEC3 || t == RHI_PARAM_VEC4 ||
                                       t == RHI_PARAM_MAT4) ? 16u : 4u;
}
RhiResult rhi_param_layout_check(const RhiParamLayout *l) {
    const char *name = l && l->name ? l->name : "params";
    if (!l || !l->fields || !l->field_count) {
        fprintf(stderr, "rhi: %s: empty parameter layout\n", name);
        return RHI_ERR_INVALID;
    }
    for (uint32_t i = 0; i < l->field_count; i++) {
        const RhiParamField *f = &l->fields[i];
        if (!f->name || f->type < RHI_PARAM_FLOAT || f->type > RHI_PARAM_INT || !f->count) {
            fprintf(stderr, "rhi: %s: field %u is malformed\n", name, (unsigned)i);
            return RHI_ERR_INVALID;
        }
        if (f->count > 1 && f->type != RHI_PARAM_VEC4 && f->type != RHI_PARAM_MAT4) {
            fprintf(stderr, "rhi: %s: %s: std140 arrays must be vec4 or mat4\n", name, f->name);
            return RHI_ERR_INVALID;
        }
        uint64_t end = (uint64_t)f->offset + (uint64_t)param_size(f->type) * f->count;
        if (f->offset % param_align(f->type) || end > l->size || end < f->offset) {
            fprintf(stderr, "rhi: %s: %s: offset %u breaks std140 alignment or the block size\n",
                    name, f->name, (unsigned)f->offset);
            return RHI_ERR_INVALID;
        }
        for (uint32_t j = 0; j < i; j++) {
            const RhiParamField *g = &l->fields[j];
            uint64_t gend = (uint64_t)g->offset + (uint64_t)param_size(g->type) * g->count;
            if (f->offset < gend && g->offset < end) {
                fprintf(stderr, "rhi: %s: %s overlaps %s\n", name, f->name, g->name);
                return RHI_ERR_INVALID;
            }
        }
    }
    return RHI_OK;
}

/* Build-time SPIR-V for a shader label + stage. The Vulkan backend can't compile
 * the GLSL in RhiShaderDesc at runtime, so it ships a Vulkan-GLSL variant per
 * label (src/shaders/vk/), compiled to SPIR-V by the build; desc->label selects.
 * An unknown label is unsupported (the engine's shader set is fixed). */
static const uint32_t *vk_shader_spirv(const char *label, int frag, size_t *bytes) {
    if (label && !strcmp(label, "frontend")) {
        if (frag) { *bytes = sizeof vk_frontend_frag; return vk_frontend_frag; }
        *bytes = sizeof vk_frontend_vert; return vk_frontend_vert;
    }
    if (label && !strcmp(label, "ug2")) {
        if (frag) { *bytes = sizeof vk_ug2_frag; return vk_ug2_frag; }
        *bytes = sizeof vk_ug2_vert; return vk_ug2_vert;
    }
    if (label && !strcmp(label, "headlight shadow")) {
        if (frag) { *bytes = sizeof vk_headlight_shadow_frag; return vk_headlight_shadow_frag; }
        *bytes = sizeof vk_headlight_shadow_vert; return vk_headlight_shadow_vert;
    }
    if (label && !strcmp(label, "world_debug")) {
        if (frag) { *bytes = sizeof vk_world_debug_frag; return vk_world_debug_frag; }
        *bytes = sizeof vk_world_debug_vert; return vk_world_debug_vert;
    }
    if (label && !strcmp(label, "hud")) {
        if (frag) { *bytes = sizeof vk_hud_frag; return vk_hud_frag; }
        *bytes = sizeof vk_hud_vert; return vk_hud_vert;
    }
    return NULL;
}

/* Per-label samplerCube slots (bit n = texture slot n, binding n + 1). In ug2
 * slot 5 is uEnvCube (binding 6); every other slot is a sampler2D. */
static uint32_t vk_shader_cube_mask(const char *label) {
    if (label && !strcmp(label, "ug2")) return 1u << 5;
    return 0u;
}

static RhiVkShader *shader_find(RhiShader sh) {
    uint32_t slot = sh.id & VK_SLOT_MASK;
    if (!sh.id) return NULL;
    if (slot && slot <= g_vk.shaders.count) {
        RhiVkShader *s = &g_vk.shaders.slots[slot - 1];
        if (s->live && s->generation == (uint8_t)(sh.id >> VK_SLOT_BITS)) return s;
    }
    return NULL;
}
static RhiVkShader *shader_lookup(RhiShader sh, const char *what) {
    uint32_t slot = sh.id & VK_SLOT_MASK;
    if (!sh.id) return NULL;
    if (slot && slot <= g_vk.shaders.count) {
        RhiVkShader *s = &g_vk.shaders.slots[slot - 1];
        if (s->live && s->generation == (uint8_t)(sh.id >> VK_SLOT_BITS)) return s;
    }
    if (g_vk.shaders.stale_logged < 8) {
        g_vk.shaders.stale_logged++;
        fprintf(stderr, "rhi: %s: stale or invalid shader handle 0x%08x\n", what, (unsigned)sh.id);
    }
    return NULL;
}
static uint32_t shader_slot_alloc(void) {
    if (g_vk.shaders.free_count) return g_vk.shaders.free_list[--g_vk.shaders.free_count];
    if (g_vk.shaders.count == g_vk.shaders.capacity) {
        uint32_t cap = g_vk.shaders.capacity ? g_vk.shaders.capacity * 2u : 64u;
        if (cap > VK_SLOT_MASK) return 0;
        RhiVkShader *slots = (RhiVkShader *)realloc(g_vk.shaders.slots, cap * sizeof *slots);
        if (!slots) return 0;
        g_vk.shaders.slots = slots;
        uint32_t *list = (uint32_t *)realloc(g_vk.shaders.free_list, cap * sizeof *list);
        if (!list) return 0;
        g_vk.shaders.free_list = list;
        g_vk.shaders.capacity = cap;
    }
    memset(&g_vk.shaders.slots[g_vk.shaders.count], 0, sizeof g_vk.shaders.slots[0]);
    return ++g_vk.shaders.count;
}

static VkShaderModule make_module(const uint32_t *spv, size_t bytes) {
    VkShaderModule m = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, bytes, spv };
    if (!vk_ok(vkCreateShaderModule(g_vk.device, &ci, NULL, &m), "vkCreateShaderModule")) return VK_NULL_HANDLE;
    return m;
}

/* Tear down a shader slot's resources and retire it (bump generation, return the
 * slot to the free list). Used by the create-failure path and rhi_shader_destroy.
 * free(NULL) is a no-op, so a slot whose mirror calloc failed is freed safely. */
static void shader_slot_free(uint32_t slot) {
    RhiVkShader *s = &g_vk.shaders.slots[slot - 1];
    for (uint32_t i = 0; i < s->block_count; i++) free(s->blocks[i].mirror);
    if (s->vert) vkDestroyShaderModule(g_vk.device, s->vert, NULL);
    if (s->frag) vkDestroyShaderModule(g_vk.device, s->frag, NULL);
    uint8_t gen = s->generation;
    memset(s, 0, sizeof *s);
    if (gen != UINT8_MAX) {   /* retire the slot unless its generation would wrap */
        s->generation = gen + 1;
        g_vk.shaders.free_list[g_vk.shaders.free_count++] = slot;
    } else {
        s->generation = gen;
    }
}

RhiShader rhi_shader_create(const RhiShaderDesc *desc) {
    RhiShader out = {0};
    if (!desc || desc->param_block_count > VK_MAX_PARAM_BLOCKS ||
        (desc->param_block_count && !desc->param_blocks)) {
        fprintf(stderr, "rhi: shader_create: invalid description\n"); return out;
    }
    for (uint32_t i = 0; i < desc->param_block_count; i++) {
        if (rhi_param_layout_check(desc->param_blocks[i]) != RHI_OK) return out;
        /* A block is one uniform-buffer descriptor and is copied whole into the per-frame arena at every draw. */
        uint32_t cap = g_vk.max_ubo_range && g_vk.max_ubo_range < VK_PARAM_BLOCK_MAX ? g_vk.max_ubo_range : VK_PARAM_BLOCK_MAX;
        if (desc->param_blocks[i]->size > cap) {
            fprintf(stderr, "rhi_vk: shader_create: parameter block '%s' is %u bytes; at most %u are supported\n",
                    desc->param_blocks[i]->name ? desc->param_blocks[i]->name : "?", (unsigned)desc->param_blocks[i]->size, cap);
            return out;
        }
    }
    size_t vb = 0, fb = 0;
    const uint32_t *vspv = vk_shader_spirv(desc->label, 0, &vb);
    const uint32_t *fspv = vk_shader_spirv(desc->label, 1, &fb);
    if (!vspv || !fspv) {
        fprintf(stderr, "rhi_vk: no Vulkan SPIR-V for shader '%s'\n", desc->label ? desc->label : "?");
        return out;
    }
    if (!g_vk.device) { fprintf(stderr, "rhi: shader_create: no device\n"); return out; }

    uint32_t slot = shader_slot_alloc();
    if (!slot) { fprintf(stderr, "rhi: shader_create: out of handles\n"); return out; }
    RhiVkShader *s = &g_vk.shaders.slots[slot - 1];
    s->vert = make_module(vspv, vb);
    s->frag = make_module(fspv, fb);
    if (!s->vert || !s->frag) goto fail;
    s->block_count = desc->param_block_count;
    s->cube_mask = vk_shader_cube_mask(desc->label);
    s->attrib_count = desc->attribute_count < RHI_MAX_VERTEX_ATTRIBS ? desc->attribute_count
                                                                      : RHI_MAX_VERTEX_ATTRIBS;
    for (uint32_t i = 0; i < s->block_count; i++) {
        const RhiParamLayout *l = desc->param_blocks[i];
        RhiVkParamBlock *pb = &s->blocks[i];
        pb->layout = l;
        pb->size = (uint32_t)l->size;
        pb->mirror = calloc(1, l->size);   /* plain host memory, zeroed like the GL mirror */
        if (!pb->mirror) goto fail;
    }
    s->live = 1;
    out.id = ((uint32_t)s->generation << VK_SLOT_BITS) | slot;
    return out;
fail:
    shader_slot_free(slot);
    return (RhiShader){0};
}

void *rhi_shader_params(RhiShader shader, uint32_t block) {
    RhiVkShader *s = shader_lookup(shader, "shader_params");
    if (!s || block >= s->block_count) return NULL;
    return s->blocks[block].mirror;   /* plain host mirror; snapshotted into the arena per draw */
}

void rhi_shader_destroy(RhiShader shader) {
    /* shader_lookup validates the slot (nonzero, in range), the generation (so a
     * stale handle to a reused slot is ignored) and liveness. */
    if (!shader.id) return;
    if (!shader_lookup(shader, "shader_destroy")) return;
    shader_slot_free(shader.id & VK_SLOT_MASK);
}

uint32_t rhi_shader_native_program(RhiShader shader) {
    RhiVkShader *s = shader_lookup(shader, "shader_native_program");
    return s ? (shader.id & VK_SLOT_MASK) : 0u;   /* stable nonzero id for a live shader */
}

/* --- Pipelines (TODO P11e). desc_init is pure: copy the GL backend's defaults. --- */
void rhi_pipeline_desc_init(RhiPipelineDesc *d) {
    memset(d, 0, sizeof *d);
    d->primitive = RHI_PRIMITIVE_TRIANGLES;
    d->cull = RHI_CULL_NONE;
    d->depth_test = 0;
    d->depth_write = 1;
    d->depth_compare = RHI_COMPARE_LESS;
    d->blend = 0;
    d->src_rgb = d->src_alpha = RHI_BLEND_ONE;
    d->dst_rgb = d->dst_alpha = RHI_BLEND_ZERO;
    d->color_write = 0xF;
    d->line_width = 1.0f;
    d->stencil_test = 0;
    d->stencil_compare = RHI_COMPARE_ALWAYS;
    d->stencil_ref = 0;
    d->stencil_read_mask = d->stencil_write_mask = 0xFF;
    d->stencil_fail = d->stencil_depth_fail = d->stencil_pass = RHI_STENCIL_KEEP;
}
static int vertex_layout_equal(const RhiVertexLayout *a, const RhiVertexLayout *b) {
    if (a->attrib_count != b->attrib_count) return 0;
    for (uint32_t i = 0; i < RHI_MAX_VERTEX_STREAMS; i++)
        if (a->stride[i] != b->stride[i]) return 0;
    for (uint32_t i = 0; i < a->attrib_count; i++) {
        const RhiVertexAttrib *x = &a->attribs[i], *y = &b->attribs[i];
        if (x->location != y->location || x->stream != y->stream || x->offset != y->offset ||
            x->format != y->format) return 0;
    }
    return 1;
}

/* Value equality for dedup, matching the GL backend's pipeline_desc_equal:
 * blend/stencil/depth flags compared as booleans, line_width <=0 folded to 1. */
static int pipeline_desc_equal(const RhiPipelineDesc *a, const RhiPipelineDesc *b) {
    return a->shader.id == b->shader.id && vertex_layout_equal(&a->vertex, &b->vertex) &&
           a->primitive == b->primitive && a->cull == b->cull &&
           !a->depth_test == !b->depth_test && !a->depth_write == !b->depth_write &&
           a->depth_compare == b->depth_compare && !a->blend == !b->blend &&
           a->src_rgb == b->src_rgb && a->dst_rgb == b->dst_rgb &&
           a->src_alpha == b->src_alpha && a->dst_alpha == b->dst_alpha &&
           (a->color_write & 0xF) == (b->color_write & 0xF) &&
           (a->line_width > 0 ? a->line_width : 1.0f) == (b->line_width > 0 ? b->line_width : 1.0f) &&
           !a->stencil_test == !b->stencil_test && a->stencil_compare == b->stencil_compare &&
           a->stencil_ref == b->stencil_ref && a->stencil_read_mask == b->stencil_read_mask &&
           a->stencil_write_mask == b->stencil_write_mask && a->stencil_fail == b->stencil_fail &&
           a->stencil_depth_fail == b->stencil_depth_fail && a->stencil_pass == b->stencil_pass;
}

static uint32_t vertex_format_bytes(RhiVertexFormat format) {
    static const uint32_t bytes[] = { 0, 8, 12, 16, 4, 4 };
    return format >= RHI_VERTEX_FLOAT2 && format <= RHI_VERTEX_FLOAT1 ? bytes[format] : 0;
}
static VkFormat vk_vertex_format(RhiVertexFormat f) {
    switch (f) {
    case RHI_VERTEX_FLOAT1:   return VK_FORMAT_R32_SFLOAT;
    case RHI_VERTEX_FLOAT2:   return VK_FORMAT_R32G32_SFLOAT;
    case RHI_VERTEX_FLOAT3:   return VK_FORMAT_R32G32B32_SFLOAT;
    case RHI_VERTEX_FLOAT4:   return VK_FORMAT_R32G32B32A32_SFLOAT;
    case RHI_VERTEX_UNORM8X4: return VK_FORMAT_R8G8B8A8_UNORM;
    default:                  return VK_FORMAT_UNDEFINED;
    }
}
static VkCompareOp vk_compare(RhiCompare c) {
    static const VkCompareOp t[] = { VK_COMPARE_OP_NEVER, VK_COMPARE_OP_LESS, VK_COMPARE_OP_EQUAL,
        VK_COMPARE_OP_LESS_OR_EQUAL, VK_COMPARE_OP_GREATER, VK_COMPARE_OP_NOT_EQUAL,
        VK_COMPARE_OP_GREATER_OR_EQUAL, VK_COMPARE_OP_ALWAYS };
    return t[c];
}
static VkStencilOp vk_stencil_op(RhiStencilOp op) {
    static const VkStencilOp t[] = { VK_STENCIL_OP_KEEP, VK_STENCIL_OP_ZERO, VK_STENCIL_OP_REPLACE,
        VK_STENCIL_OP_INCREMENT_AND_CLAMP, VK_STENCIL_OP_DECREMENT_AND_CLAMP, VK_STENCIL_OP_INVERT };
    return t[op];
}
static VkBlendFactor vk_blend(RhiBlendFactor f) {
    static const VkBlendFactor t[] = { VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE,
        VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_DST_ALPHA,
        VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA, VK_BLEND_FACTOR_SRC_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
        VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR };
    return t[f];
}

/* Build one VkPipeline of `desc` for a render-target variant (VK_PV_*). */
/* The binding stride build_pipeline gives `stream`: the declared stride, else the packed size of its attributes. */
static uint32_t stream_stride(const RhiPipelineDesc *desc, uint32_t stream) {
    const RhiVkShader *sh = shader_find(desc->shader);
    uint32_t shader_attribs = sh ? sh->attrib_count : 0, stride = 0;
    for (uint32_t i = 0; i < desc->vertex.attrib_count; i++) {
        const RhiVertexAttrib *a = &desc->vertex.attribs[i];
        if (a->location >= shader_attribs || a->stream != stream) continue;
        uint32_t end = a->offset + vertex_format_bytes(a->format);
        if (desc->vertex.stride[stream]) stride = desc->vertex.stride[stream];
        else if (end > stride) stride = end;
    }
    return stride;
}

/* Build one VkPipeline of `desc` for a render-target variant (VK_PV_*). Streams in `absent` (declared by the layout but with no usable
 * vertex buffer at draw time) get a stride-0 binding: they read the first element of the small zero dummy buffer for every vertex, whereas
 * the declared stride would walk off its end. */
static VkPipeline build_pipeline(const RhiPipelineDesc *desc, int variant, int wire, uint32_t absent) {
    /* ---- vertex input: one binding per stream an attribute references ---- */
    VkVertexInputBindingDescription vbind[RHI_MAX_VERTEX_STREAMS + 1];
    uint32_t stride[RHI_MAX_VERTEX_STREAMS] = {0};
    uint8_t  used_stream[RHI_MAX_VERTEX_STREAMS] = {0};
    VkVertexInputAttributeDescription vattr[2 * RHI_MAX_VERTEX_ATTRIBS];
    /* The layout may name attributes the shader does not declare (GL ignores them,
     * Vulkan validation warns): only attributes below the shader's attribute_count
     * are passed on. */
    const RhiVkShader *shv = shader_find(desc->shader);
    uint32_t shader_attribs = shv ? shv->attrib_count : 0, n_attr = 0;
    g_vk.build_absent_offset = 0;
    for (uint32_t i = 0; i < desc->vertex.attrib_count; i++) {
        const RhiVertexAttrib *a = &desc->vertex.attribs[i];
        if (a->location >= shader_attribs) continue;
        uint32_t s = a->stream, bytes = vertex_format_bytes(a->format);
        used_stream[s] = 1;
        uint32_t end = a->offset + bytes;   /* packed stride if stride[] is 0 */
        if (desc->vertex.stride[s]) stride[s] = desc->vertex.stride[s];
        else if (end > stride[s])   stride[s] = end;
        vattr[n_attr].location = a->location;
        vattr[n_attr].binding  = s;
        vattr[n_attr].format   = vk_vertex_format(a->format);
        /* An absent stream reads the 256-byte dummy through a stride-0 binding: its attributes are fetched at offset 0, whatever the
         * layout declares, so no declared offset can reach past the dummy. */
        vattr[n_attr].offset   = (absent >> s & 1u) ? 0u : a->offset;
        if (absent >> s & 1u && vattr[n_attr].offset > g_vk.build_absent_offset) g_vk.build_absent_offset = vattr[n_attr].offset;
        n_attr++;
    }
    for (uint32_t s = 0; s < RHI_MAX_VERTEX_STREAMS; s++)
        if (absent >> s & 1u) stride[s] = 0;
    uint32_t n_bind = 0;
    for (uint32_t s = 0; s < RHI_MAX_VERTEX_STREAMS; s++)
        if (used_stream[s]) {
            vbind[n_bind].binding   = s;
            vbind[n_bind].stride    = stride[s];
            vbind[n_bind].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
            n_bind++;
        }
    /* Locations the shader declares (0..attribute_count-1) but the layout does not
     * name read the GL default (0,0,0,1) from a stride-0 dummy binding. */
    uint32_t named = 0;
    for (uint32_t i = 0; i < n_attr; i++) named |= 1u << vattr[i].location;
    {
        const RhiVkShader *sh0 = shader_find(desc->shader);
        uint32_t want = sh0 ? sh0->attrib_count : 0, extra = 0;
        for (uint32_t loc = 0; loc < want; loc++) {
            if (named & (1u << loc)) continue;
            vattr[n_attr++] = (VkVertexInputAttributeDescription){ loc, RHI_MAX_VERTEX_STREAMS,
                VK_FORMAT_R32G32B32A32_SFLOAT, 0 };
            extra = 1;
        }
        if (extra) vbind[n_bind++] = (VkVertexInputBindingDescription){ RHI_MAX_VERTEX_STREAMS, 0,
            VK_VERTEX_INPUT_RATE_VERTEX };
    }
    VkPipelineVertexInputStateCreateInfo vis = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        NULL, 0, n_bind, vbind, n_attr, vattr };

    VkPipelineInputAssemblyStateCreateInfo ias = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        NULL, 0, desc->primitive == RHI_PRIMITIVE_LINES ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST
                                                        : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_FALSE };

    VkPipelineViewportStateCreateInfo vps = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        NULL, 0, 1, NULL, 1, NULL };   /* dynamic viewport + scissor */

    /* lineWidth must be 1.0 unless the wideLines feature is on (validation checks
     * this for any topology); enabled in device_create when the device supports it. */
    float lw = desc->line_width > 0 ? desc->line_width : 1.0f;
    if (!g_vk.wide_lines) lw = 1.0f;
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        NULL, 0, VK_FALSE, VK_FALSE, wire ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL,
        desc->cull == RHI_CULL_BACK ? VK_CULL_MODE_BACK_BIT
            : desc->cull == RHI_CULL_FRONT ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_NONE,
        /* GL front faces are CCW in NDC (y up). Vulkan evaluates winding in
         * framebuffer space (y down), and the positive-height viewport still maps
         * NDC +y to the image bottom, i.e. it reflects y: a GL-CCW triangle is CW
         * here, so GL's front face is VK_FRONT_FACE_CLOCKWISE (P11f-3). */
        VK_FRONT_FACE_CLOCKWISE, VK_FALSE, 0.0f, 0.0f, 0.0f, lw };

    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        NULL, 0, VK_SAMPLE_COUNT_1_BIT, VK_FALSE, 1.0f, NULL, VK_FALSE, VK_FALSE };

    VkStencilOpState sop = { vk_stencil_op(desc->stencil_fail), vk_stencil_op(desc->stencil_pass),
        vk_stencil_op(desc->stencil_depth_fail), vk_compare(desc->stencil_compare),
        desc->stencil_read_mask, desc->stencil_write_mask, desc->stencil_ref };
    VkPipelineDepthStencilStateCreateInfo dss = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        NULL, 0, desc->depth_test ? VK_TRUE : VK_FALSE, desc->depth_write ? VK_TRUE : VK_FALSE,
        vk_compare(desc->depth_compare), VK_FALSE, desc->stencil_test ? VK_TRUE : VK_FALSE,
        sop, sop, 0.0f, 1.0f };

    VkColorComponentFlags mask =
        ((desc->color_write & 1) ? VK_COLOR_COMPONENT_R_BIT : 0) |
        ((desc->color_write & 2) ? VK_COLOR_COMPONENT_G_BIT : 0) |
        ((desc->color_write & 4) ? VK_COLOR_COMPONENT_B_BIT : 0) |
        ((desc->color_write & 8) ? VK_COLOR_COMPONENT_A_BIT : 0);
    VkPipelineColorBlendAttachmentState cba = { desc->blend ? VK_TRUE : VK_FALSE,
        vk_blend(desc->src_rgb), vk_blend(desc->dst_rgb), VK_BLEND_OP_ADD,
        vk_blend(desc->src_alpha), vk_blend(desc->dst_alpha), VK_BLEND_OP_ADD, mask };
    VkPipelineColorBlendStateCreateInfo cbs = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        NULL, 0, VK_FALSE, VK_LOGIC_OP_COPY, 1, &cba, { 0, 0, 0, 0 } };

    VkDynamicState dyn_states[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dyns = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        NULL, 0, 2, dyn_states };

    RhiVkShader *sh = shader_find(desc->shader);
    if (!sh) return VK_NULL_HANDLE;
    VkPipelineShaderStageCreateInfo stages[2] = {
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT,
          sh->vert, "main", NULL },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT,
          sh->frag, "main", NULL } };

    /* Attachment formats of the target this variant draws into. MAIN has the
     * combined depth-stencil image (depth AND stencil format); the offscreen
     * pass variants have D16 depth without stencil, or no depth at all. */
    VkFormat color_fmt = VK_FORMAT_R8G8B8A8_UNORM;
    VkFormat depth_f = variant == VK_PV_MAIN ? g_vk.depth_fmt
                     : variant == VK_PV_PASS_D16 ? VK_FORMAT_D16_UNORM : VK_FORMAT_UNDEFINED;
    VkFormat stencil_f = variant == VK_PV_MAIN ? g_vk.depth_fmt : VK_FORMAT_UNDEFINED;
    VkPipelineRenderingCreateInfo prc = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, NULL,
        0, 1, &color_fmt, depth_f, stencil_f };
    VkGraphicsPipelineCreateInfo gpci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &prc, 0,
        2, stages, &vis, &ias, NULL, &vps, &rs, &ms, &dss, &cbs, &dyns, g_vk.pipe_layout,
        VK_NULL_HANDLE, 0, VK_NULL_HANDLE, -1 };

    VkPipeline pipe = VK_NULL_HANDLE;
    if (!vk_ok(vkCreateGraphicsPipelines(g_vk.device, VK_NULL_HANDLE, 1, &gpci, NULL, &pipe),
               "vkCreateGraphicsPipelines")) {
        fprintf(stderr, "rhi: pipeline_create: GPU pipeline creation failed\n");
        return VK_NULL_HANDLE;
    }
    return pipe;
}

RhiPipeline rhi_pipeline_create(const RhiPipelineDesc *desc) {
    RhiPipeline out = {0};
    /* Same validation and messages as the GL backend. */
    if (!desc || !shader_find(desc->shader) ||
        desc->vertex.attrib_count > RHI_MAX_VERTEX_ATTRIBS ||
        desc->primitive > RHI_PRIMITIVE_LINES || desc->cull > RHI_CULL_FRONT ||
        desc->depth_compare > RHI_COMPARE_ALWAYS || desc->stencil_compare > RHI_COMPARE_ALWAYS ||
        desc->stencil_fail > RHI_STENCIL_INVERT || desc->stencil_depth_fail > RHI_STENCIL_INVERT ||
        desc->stencil_pass > RHI_STENCIL_INVERT ||
        desc->src_rgb > RHI_BLEND_ONE_MINUS_DST_COLOR || desc->dst_rgb > RHI_BLEND_ONE_MINUS_DST_COLOR ||
        desc->src_alpha > RHI_BLEND_ONE_MINUS_DST_COLOR || desc->dst_alpha > RHI_BLEND_ONE_MINUS_DST_COLOR) {
        fprintf(stderr, "rhi: pipeline_create: invalid description\n");
        return out;
    }
    uint32_t locations = 0;
    for (uint32_t i = 0; i < desc->vertex.attrib_count; i++) {
        const RhiVertexAttrib *a = &desc->vertex.attribs[i];
        uint32_t bytes = vertex_format_bytes(a->format);
        if (a->stream >= RHI_MAX_VERTEX_STREAMS || !bytes ||
            a->location >= RHI_MAX_VERTEX_ATTRIBS || (locations & (1u << a->location)) ||
            desc->vertex.stride[a->stream] > INT_MAX ||
            (uint64_t)a->offset + bytes > (desc->vertex.stride[a->stream] ? desc->vertex.stride[a->stream] : bytes)) {
            fprintf(stderr, "rhi: pipeline_create: invalid vertex attribute %u\n", (unsigned)i);
            return out;
        }
        locations |= 1u << a->location;
    }
    for (uint32_t i = 0; i < g_vk.pipelines.count; i++)
        if (pipeline_desc_equal(&g_vk.pipelines.slots[i].desc, desc)) { out.id = i + 1; return out; }

    VkPipeline pipe = build_pipeline(desc, VK_PV_MAIN, 0, 0);   /* eager: create failures return {0} */
    if (!pipe) return out;
    if (g_vk.pipelines.count == g_vk.pipelines.capacity) {
        uint32_t cap = g_vk.pipelines.capacity ? g_vk.pipelines.capacity * 2u : 64u;
        RhiVkPipeline *next = (RhiVkPipeline *)realloc(g_vk.pipelines.slots, (size_t)cap * sizeof *next);
        if (!next) {
            fprintf(stderr, "rhi: pipeline_create: out of memory\n");
            vkDestroyPipeline(g_vk.device, pipe, NULL);
            return out;
        }
        g_vk.pipelines.slots = next;
        g_vk.pipelines.capacity = cap;
    }
    RhiVkPipeline *e = &g_vk.pipelines.slots[g_vk.pipelines.count];
    memset(e->variant, 0, sizeof e->variant);
    e->variant[VK_PV_MAIN] = pipe;
    e->shader = desc->shader;
    e->desc = *desc;
    out.id = ++g_vk.pipelines.count;
    return out;
}

/* --- Draws --- */
void rhi_cmd_bind_pipeline(RhiPipeline pipeline) {
    if (!pipeline.id || pipeline.id > g_vk.pipelines.count) {
        fprintf(stderr, "rhi: bind_pipeline: invalid handle %u\n", (unsigned)pipeline.id);
        g_vk.bound_pipeline = -1;
        return;
    }
    RhiVkPipeline *e = &g_vk.pipelines.slots[pipeline.id - 1];
    if (!shader_find(e->shader)) {
        fprintf(stderr, "rhi: bind_pipeline: destroyed shader\n");
        g_vk.bound_pipeline = -1;
        return;
    }
    g_vk.bound_pipeline = (int)pipeline.id - 1;
}

void rhi_cmd_bind_vertex_buffer(uint32_t stream, RhiBuffer buffer, uint32_t offset) {
    if (stream >= RHI_MAX_VERTEX_STREAMS) return;
    g_vk.bound_vertex[stream] = buffer;
    g_vk.bound_vertex_offset[stream] = offset;
}

void rhi_cmd_bind_index_buffer(RhiBuffer buffer) { g_vk.bound_index = buffer; }

void rhi_cmd_bind_texture(uint32_t slot, RhiTexture texture) {
    if (slot >= RHI_TEXTURE_SLOTS) {
        fprintf(stderr, "rhi: bind_texture: slot %u out of range\n", (unsigned)slot);
        return;
    }
    /* Like GL: the texture goes to the unit's binding for its own type; {0} (or an
     * invalid / non-sampleable handle) clears the 2D binding only. */
    RhiVkTexture *t = texture_lookup(texture, "bind_texture");
    if (t && !(t->usage & RHI_TEXTURE_SAMPLED)) t = NULL;   /* not sampleable */
    if (t && t->layers == 6) g_vk.bound_cube[slot] = texture;
    else g_vk.bound_2d[slot] = t ? texture : (RhiTexture){0};
}

/* Dynamic viewport (positive height, VkViewport.y = GL y) + full-target scissor,
 * then bind the pipeline. Returns the pipeline entry, NULL to skip on any problem. */
/* The streams `e`'s layout declares that this draw has no usable vertex buffer for (see build_pipeline). */
static uint32_t absent_streams(const RhiVkPipeline *e) {
    uint32_t used = 0, absent = 0;
    for (uint32_t i = 0; i < e->desc.vertex.attrib_count; i++) used |= 1u << e->desc.vertex.attribs[i].stream;
    for (uint32_t s = 0; s < RHI_MAX_VERTEX_STREAMS; s++) {
        if (!(used >> s & 1u)) continue;
        RhiVkBuffer *b = g_vk.bound_vertex[s].id ? buffer_lookup(g_vk.bound_vertex[s], "draw") : NULL;
        if (!(b && b->buf && (b->usage & VK_BUFFER_USAGE_VERTEX_BUFFER_BIT))) absent |= 1u << s;
    }
    return absent;
}

static RhiVkPipeline *draw_begin(const char *what) {
    if (g_vk.bound_pipeline < 0) {
        fprintf(stderr, "rhi: %s: no pipeline bound\n", what);
        return NULL;
    }
    RhiVkPipeline *e = &g_vk.pipelines.slots[g_vk.bound_pipeline];
    if (!shader_find(e->shader)) {
        fprintf(stderr, "rhi: %s: destroyed shader\n", what);
        g_vk.bound_pipeline = -1;
        return NULL;
    }
    if (!g_vk.in_frame) { fprintf(stderr, "rhi: %s: no frame\n", what); return NULL; }
    if (!ensure_rendering()) return NULL;

    int32_t vx = g_vk.viewport[0], vy = g_vk.viewport[1], vw = g_vk.viewport[2], vh = g_vk.viewport[3];
    /* GL (x,y,w,h) is bottom-left origin. The committed P11a read_pixels mimics
     * GL's glReadPixels: it flips image rows so output row 0 (top) == image-memory
     * bottom. For that flip to reproduce GL, a GL +y vertex must land at the
     * BOTTOM of image memory -- which a standard (positive-height) Vulkan viewport
     * already does (clip +y -> framebuffer bottom). So VkViewport.y is the GL y
     * DIRECTLY, not H-(y+h): the full target is identical (gl_y 0 -> 0 either way),
     * and a partial GL viewport lands where GL puts it once read_pixels flips rows
     * (P11-review Fix 4). The viewport RECT is { gl_x, gl_y, gl_w, gl_h }. */
    VkViewport vp = { (float)vx, (float)vy, (float)vw, (float)vh, 0.0f, 1.0f };
    vkCmdSetViewport(g_vk.cmd, 0, 1, &vp);
    /* Scissor never clips beyond the viewport (rhi.h models no scissor state). */
    VkRect2D sc = { { 0, 0 }, { (uint32_t)target_width(), (uint32_t)target_height() } };
    vkCmdSetScissor(g_vk.cmd, 0, 1, &sc);
    /* The pipeline variant for the current target's attachment formats; the
     * non-main ones are built on first use. */
    int wire = g_vk.wireframe && e->desc.primitive == RHI_PRIMITIVE_TRIANGLES;   /* line topology has no polygon mode */
    uint32_t absent = absent_streams(e);
    g_vk.draw_absent = absent;   /* draw_bind_resources binds the dummy buffer for exactly these streams */
    unsigned v = (unsigned)g_vk.target_variant, vi = (absent * 2u + (unsigned)wire) * VK_PV_COUNT + v;
    if (!e->variant[vi]) {
        e->variant[vi] = build_pipeline(&e->desc, (int)v, wire, absent);
        if (!e->variant[vi]) { fprintf(stderr, "rhi: %s: pipeline variant %u failed\n", what, vi); return NULL; }
    }
    vkCmdBindPipeline(g_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, e->variant[vi]);
    return e;
}

/* Bind the pipeline's vertex streams and push set 0 (UBO + textures/dummies).
 * `ubo_off` is this draw's snapshot offset in the per-frame arena (block 0). */
static void draw_bind_resources(RhiVkPipeline *e, VkDeviceSize ubo_off) {
    const RhiVertexLayout *vl = &e->desc.vertex;
    uint8_t used_stream[RHI_MAX_VERTEX_STREAMS] = {0};
    VkBuffer vbufs[RHI_MAX_VERTEX_STREAMS];
    VkDeviceSize voffs[RHI_MAX_VERTEX_STREAMS];
    for (uint32_t i = 0; i < vl->attrib_count; i++) used_stream[vl->attribs[i].stream] = 1;
    for (uint32_t s = 0; s < RHI_MAX_VERTEX_STREAMS; s++) g_vk.last_stride[s] = 0xFFFFFFFFu;
    for (uint32_t s = 0; s < RHI_MAX_VERTEX_STREAMS; s++) {
        if (!used_stream[s]) continue;
        const int absent = g_vk.draw_absent >> s & 1u;
        g_vk.last_stride[s] = absent ? 0 : stream_stride(&e->desc, s);
        RhiVkBuffer *b = absent ? NULL : buffer_lookup(g_vk.bound_vertex[s], "draw");
        if (b && b->buf && (b->usage & VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)) {
            b->in_use = 1;   /* a recorded draw now reads it */
            vbufs[s] = buf_cur(b); voffs[s] = g_vk.bound_vertex_offset[s] + buf_cur_off(b);
        } else {
            /* GL disables the attribute (it then reads the current generic value; the
             * engine supplies its own constants there), so this stays zeros, unlike
             * the (0,0,0,1) default of attributes absent from the layout. */
            vbufs[s] = g_vk.dummy_vbuf; voffs[s] = 0;
        }
        vkCmdBindVertexBuffers(g_vk.cmd, s, 1, &vbufs[s], &voffs[s]);
    }

    {   /* default-valued binding for layout-absent attributes (harmless if unused) */
        VkDeviceSize zero = 0;
        vkCmdBindVertexBuffers(g_vk.cmd, RHI_MAX_VERTEX_STREAMS, 1, &g_vk.dummy_attr_buf, &zero);
    }
    RhiVkShader *sh = shader_find(e->shader);
    VkWriteDescriptorSet writes[1 + RHI_TEXTURE_SLOTS];
    VkDescriptorBufferInfo bi;
    VkDescriptorImageInfo ii[RHI_TEXTURE_SLOTS];
    uint32_t n = 0;
    if (sh && sh->block_count > 0) {
        /* Block 0 = the snapshot this draw placed in the per-frame arena. The
         * engine's shaders use a single param block; TODO: bind blocks 1.. here
         * (from their own arena snapshots) if a shader ever declares more. */
        bi = (VkDescriptorBufferInfo){ g_vk.arena.buf, ubo_off, sh->blocks[0].size };
        writes[n] = (VkWriteDescriptorSet){ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL,
            VK_NULL_HANDLE, 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, NULL, &bi, NULL };
        n++;
    }
    for (uint32_t slot = 0; slot < RHI_TEXTURE_SLOTS; slot++) {
        /* samplerCube slots take the cube binding, the rest the 2D one. A bound
         * texture that is not in a sampleable state (never rendered/uploaded, or
         * mid-pass) reads the dummy instead of an undefined/feedback layout. */
        int cube = sh && ((sh->cube_mask >> slot) & 1u);
        RhiTexture h = cube ? g_vk.bound_cube[slot] : g_vk.bound_2d[slot];
        RhiVkTexture *t = h.id ? texture_find(h) : NULL;
        if (t && (!(t->usage & RHI_TEXTURE_SAMPLED) || !t->view ||
                  t->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL ||
                  (t->layers == 6) != (cube != 0)))
            t = NULL;
        VkImageView view = t ? t->view : cube ? g_vk.dummy_cube_view : g_vk.dummy_view;
        VkSampler smp = g_vk.dummy_sampler;
        if (t) {
            texture_touch(t);
            if (t->sampler.id && t->sampler.id <= g_vk.samplers.count)
                smp = g_vk.samplers.handles[t->sampler.id - 1];
        }
        ii[slot] = (VkDescriptorImageInfo){ smp, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        writes[n] = (VkWriteDescriptorSet){ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL,
            VK_NULL_HANDLE, 1 + slot, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            &ii[slot], NULL, NULL };
        n++;
    }
    g_vk.push_desc(g_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_vk.pipe_layout, 0, n, writes);
}

/* Validate a vertex stream's range the way the GL backend does (per attribute). */
static int draw_vertex_ranges_ok(const RhiVkPipeline *e, uint32_t vertex_count, uint32_t first_vertex) {
    const RhiVertexLayout *vl = &e->desc.vertex;
    for (uint32_t i = 0; i < vl->attrib_count; i++) {
        const RhiVertexAttrib *a = &vl->attribs[i];
        if (!g_vk.bound_vertex[a->stream].id) continue;   /* disabled -> dummy buffer */
        RhiVkBuffer *b = buffer_lookup(g_vk.bound_vertex[a->stream], "draw");
        uint32_t bytes = vertex_format_bytes(a->format);
        uint32_t stride = vl->stride[a->stream] ? vl->stride[a->stream] : bytes;
        uint64_t last = vertex_count ? (uint64_t)first_vertex + vertex_count - 1u : 0;
        uint64_t end = (uint64_t)g_vk.bound_vertex_offset[a->stream] + a->offset + last * stride + bytes;
        if (!b || !(b->usage & VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) || end > PTRDIFF_MAX || end > b->size) {
            fprintf(stderr, "rhi: draw: invalid vertex buffer or range\n");
            return 0;
        }
    }
    return 1;
}

void rhi_cmd_draw(uint32_t vertex_count, uint32_t first_vertex) {
    if (!vertex_count) return;
    if (vertex_count > INT_MAX || first_vertex > INT_MAX) {
        fprintf(stderr, "rhi: draw: invalid vertex count or first vertex\n");
        return;
    }
    if (g_vk.bound_pipeline < 0) { fprintf(stderr, "rhi: draw: no pipeline bound\n"); return; }
    if (!draw_vertex_ranges_ok(&g_vk.pipelines.slots[g_vk.bound_pipeline], vertex_count, first_vertex)) return;
    /* Snapshot this draw's params into the arena BEFORE opening the render scope
     * (a mid-frame flush must close/submit any prior scope first) (Fix 1). */
    RhiVkShader *sh = shader_find(g_vk.pipelines.slots[g_vk.bound_pipeline].shader);
    VkDeviceSize ubo_off = 0;
    if (sh && sh->block_count > 0 && !arena_place(sh->blocks[0].mirror, sh->blocks[0].size, &ubo_off)) {
        fprintf(stderr, "rhi: draw: param arena flush failed\n"); return;
    }
    RhiVkPipeline *e = draw_begin("draw");
    if (!e) return;
    draw_bind_resources(e, ubo_off);
    vkCmdDraw(g_vk.cmd, vertex_count, 1, first_vertex, 0);
}

void rhi_cmd_draw_indexed(uint32_t index_count, uint32_t first_index) {
    if (!index_count) return;
    RhiVkBuffer *index = g_vk.bound_index.id ? buffer_lookup(g_vk.bound_index, "draw_indexed") : NULL;
    if (!index || !index->buf || !(index->usage & VK_BUFFER_USAGE_INDEX_BUFFER_BIT) ||
        index_count > INT_MAX || (uint64_t)first_index * 2u > PTRDIFF_MAX ||
        first_index > index->size / 2u || index_count > index->size / 2u - first_index) {
        fprintf(stderr, "rhi: draw_indexed: invalid index buffer or range\n");
        return;
    }
    if (g_vk.bound_pipeline < 0) { fprintf(stderr, "rhi: draw_indexed: no pipeline bound\n"); return; }
    /* The max index value is not scanned (like the GL backend); vertex ranges are
     * validated against the index count's worst case only loosely, so skip here. */
    /* Snapshot this draw's params into the arena before opening the scope (Fix 1). */
    RhiVkShader *sh = shader_find(g_vk.pipelines.slots[g_vk.bound_pipeline].shader);
    VkDeviceSize ubo_off = 0;
    if (sh && sh->block_count > 0 && !arena_place(sh->blocks[0].mirror, sh->blocks[0].size, &ubo_off)) {
        fprintf(stderr, "rhi: draw_indexed: param arena flush failed\n"); return;
    }
    RhiVkPipeline *e = draw_begin("draw_indexed");
    if (!e) return;
    draw_bind_resources(e, ubo_off);
    index->in_use = 1;   /* a recorded draw now reads it */
    vkCmdBindIndexBuffer(g_vk.cmd, buf_cur(index), buf_cur_off(index), VK_INDEX_TYPE_UINT16);
    vkCmdDrawIndexed(g_vk.cmd, index_count, 1, first_index, 0, 0);
}

/* --- Passes / copies --- */
RhiResult rhi_cmd_begin_pass(const RhiPassDesc *desc) {
    if (!desc || g_vk.pass.active || !g_vk.in_frame) return RHI_ERR_INVALID;
    /* Same validation as the GL backend (rhi_gl.c), with D16 as the only depth. */
    RhiVkTexture *c = texture_lookup(desc->color.texture, "begin_pass");
    RhiVkTexture *d = texture_lookup(desc->depth.texture, "begin_pass");
    if (!c || c->format != RHI_FORMAT_RGBA8 || !(c->usage & RHI_TEXTURE_COLOR_TARGET) ||
        (desc->depth.texture.id && (!d || d->format != RHI_FORMAT_D16 || !(d->usage & RHI_TEXTURE_DEPTH_TARGET))))
        return RHI_ERR_INVALID;
    int faces = c->layers == 6 ? 6 : 1;
    if (desc->color.face < 0 || desc->color.face >= faces ||
        (d && desc->depth.face != 0) ||
        (desc->color_load != RHI_LOAD_KEEP && desc->color_load != RHI_LOAD_CLEAR) ||
        (desc->depth_load != RHI_LOAD_KEEP && desc->depth_load != RHI_LOAD_CLEAR)) return RHI_ERR_INVALID;
    /* An attachment must cover the render area. */
    if (d && (d->width < c->width || d->height < c->height)) return RHI_ERR_UNSUPPORTED;
    if (!ensure_recording()) return RHI_ERR_DEVICE_LOST;
    end_rendering_if_active();   /* close the main scope (it re-opens from the tracked layouts later) */

    g_vk.pass.active = 1;
    g_vk.pass.color = desc->color.texture;
    g_vk.pass.depth = d ? desc->depth.texture : (RhiTexture){0};
    g_vk.pass.face = desc->color.face;
    g_vk.pass.w = c->width; g_vk.pass.h = c->height;
    g_vk.pass.clear_color = desc->color_load == RHI_LOAD_CLEAR;
    g_vk.pass.clear_depth = d && desc->depth_load == RHI_LOAD_CLEAR;
    memcpy(g_vk.pass.clear_value, desc->clear_color, sizeof g_vk.pass.clear_value);
    g_vk.pass.clear_z = desc->clear_depth;
    g_vk.pass.opened = 0;
    g_vk.target_variant = d ? VK_PV_PASS_D16 : VK_PV_PASS_NODEPTH;
    g_vk.viewport[0] = 0; g_vk.viewport[1] = 0; g_vk.viewport[2] = c->width; g_vk.viewport[3] = c->height;
    /* Open the scope now so a pass without draws still performs its clears. */
    if (!ensure_rendering()) {
        g_vk.pass.active = 0;
        g_vk.target_variant = VK_PV_MAIN;
        return RHI_ERR_DEVICE_LOST;
    }
    return RHI_OK;
}

void rhi_cmd_end_pass(void) {
    if (!g_vk.pass.active) return;
    RhiVkTexture *c = texture_find(g_vk.pass.color);
    RhiVkTexture *d = g_vk.pass.depth.id ? texture_find(g_vk.pass.depth) : NULL;
    if (ensure_recording()) {
        end_rendering_if_active();   /* pass scope: just ends it */
        if (c) {
            int sampled = (c->usage & RHI_TEXTURE_SAMPLED) != 0;
            VkImageLayout rl = rest_layout(c->usage);
            img_barrier(g_vk.cmd, c->img, VK_IMAGE_ASPECT_COLOR_BIT, 0, c->mip_levels, 0, c->layers,
                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, rl,
                        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                        VK_ACCESS_TRANSFER_READ_BIT | (sampled ? VK_ACCESS_SHADER_READ_BIT : 0),
                        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT | (sampled ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : 0));
            c->layout = rl;
        }
        if (d) {   /* same layout: make the depth writes available to a following pass */
            img_barrier(g_vk.cmd, d->img, d->bar_aspect, 0, 1, 0, 1, d->layout, d->layout,
                        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT);
        }
    }
    memset(&g_vk.pass, 0, sizeof g_vk.pass);
    g_vk.target_variant = VK_PV_MAIN;   /* the viewport is left as is (GL parity) */
}

/* Copy a rectangle of the main colour (or depth) image into level 0 of a COPY_DST
 * texture. Memory row y of the main image is GL window row y (see the orientation
 * note at ensure_rendering), and the copy keeps row order, so texture row r holds
 * GL row y + r exactly as glCopyTexImage2D does: no flip. */
RhiResult rhi_cmd_copy_main_to_texture(RhiTexture texture, int32_t x, int32_t y, int32_t w, int32_t h) {
    RhiVkTexture *t = texture_lookup(texture, "copy_main_to_texture");
    if (!t || t->layers != 1 || w <= 0 || h <= 0 || x < 0 || y < 0 || g_vk.pass.active ||
        !(t->usage & RHI_TEXTURE_COPY_DST) ||
        (t->format != RHI_FORMAT_RGBA8 && t->format != RHI_FORMAT_DEPTH) ||
        w > INT_MAX - x || h > INT_MAX - y || !g_vk.in_frame ||
        w > g_vk.width - x || h > g_vk.height - y) return RHI_ERR_INVALID;
    int depth = t->format == RHI_FORMAT_DEPTH;
    if (!ensure_recording()) return RHI_ERR_DEVICE_LOST;
    /* "The texture takes the rectangle's size": recreate the storage in place
     * under the same handle. The old image may still be referenced by recorded
     * draws, so it is freed deferred. Colour keeps the full mip chain like
     * rhi_texture_create (so generate_mips still works after a resize); depth is
     * single-level. Only level 0 is written by the copy. */
    if (t->width != w || t->height != h) {
        VkImage img; VkDeviceMemory mem; VkImageView view;
        uint32_t levels = depth ? 1u : mip_count(w, h);
        if (!texture_make_storage(t, w, h, levels, &img, &mem, &view)) return RHI_ERR_UNSUPPORTED;
        texture_release_storage(t);
        t->img = img; t->mem = mem; t->view = view;
        t->layout = VK_IMAGE_LAYOUT_UNDEFINED;
        t->width = w; t->height = h; t->mip_levels = levels;
    }
    end_rendering_if_active();   /* leaves the main colour in TRANSFER_SRC */
    texture_touch(t);

    VkImageAspectFlags src_aspect = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    if (depth) {
        image_barrier(g_vk.depth, VK_DS_ASPECT, g_vk.depth_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_ACCESS_TRANSFER_WRITE_BIT |
                          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                      VK_ACCESS_TRANSFER_READ_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT |
                          VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT);
        g_vk.depth_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    } else if (g_vk.color_layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
        image_barrier(g_vk.color, VK_IMAGE_ASPECT_COLOR_BIT, g_vk.color_layout,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, VK_ACCESS_TRANSFER_READ_BIT,
                      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        g_vk.color_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    }
    /* Whole destination image -> TRANSFER_DST. Earlier draws that sampled it must
     * finish first (GL: they saw the old contents); its contents are replaced. */
    img_barrier(g_vk.cmd, t->img, t->bar_aspect, 0, t->mip_levels, 0, 1,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkImageCopy reg = { { src_aspect, 0, 0, 1 }, { x, y, 0 }, { src_aspect, 0, 0, 1 }, { 0, 0, 0 },
                        { (uint32_t)w, (uint32_t)h, 1 } };
    vkCmdCopyImage(g_vk.cmd, depth ? g_vk.depth : g_vk.color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &reg);
    VkImageLayout rl = rest_layout(t->usage);
    int sampled = rl == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    img_barrier(g_vk.cmd, t->img, t->bar_aspect, 0, t->mip_levels, 0, 1,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, rl,
                VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_TRANSFER_READ_BIT | (sampled ? VK_ACCESS_SHADER_READ_BIT : 0),
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT | (sampled ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : 0));
    t->layout = rl;
    return RHI_OK;
}

/* --- Viewport / state snapshot / wireframe --- */
/* Returns the stored GL-convention viewport (x, y, w, h, origin bottom-left). */
void rhi_viewport(int32_t viewport[4]) {
    viewport[0] = g_vk.viewport[0]; viewport[1] = g_vk.viewport[1];
    viewport[2] = g_vk.viewport[2]; viewport[3] = g_vk.viewport[3];
}
/* What the backend snapshots: the bound pipeline, the viewport and the 2D
 * texture on each sampler slot (cube bindings are not included, like GL). */
#define VK_SNAPSHOT_MAGIC 0x56535431u   /* "VST1" */
typedef struct {
    uint32_t   valid;                    /* VK_SNAPSHOT_MAGIC once saved */
    int32_t    pipeline;                 /* index into pipelines, -1 = none */
    int32_t    viewport[4];
    RhiTexture tex2d[RHI_TEXTURE_SLOTS];
} RhiVkSnapshot;
typedef char rhi_vk_snapshot_fits[sizeof(RhiVkSnapshot) <= sizeof(RhiStateSnapshot) ? 1 : -1];
typedef char rhi_vk_snapshot_slots[RHI_TEXTURE_SLOTS <= RHI_STATE_TEXTURE_SLOTS ? 1 : -1];

void rhi_cmd_save_state(RhiStateSnapshot *out) {
    RhiVkSnapshot v;
    if (!out) return;
    memset(&v, 0, sizeof v);
    v.valid = VK_SNAPSHOT_MAGIC;
    v.pipeline = g_vk.bound_pipeline;
    memcpy(v.viewport, g_vk.viewport, sizeof v.viewport);
    memcpy(v.tex2d, g_vk.bound_2d, sizeof v.tex2d);
    memset(out, 0, sizeof *out);
    memcpy(out, &v, sizeof v);
}

void rhi_state_set_texture(RhiStateSnapshot *state, uint32_t slot, RhiTexture texture) {
    RhiVkSnapshot v;
    const RhiVkTexture *t = texture_lookup(texture, "state_set_texture");
    if (!state || slot >= RHI_STATE_TEXTURE_SLOTS || (t && t->layers == 6)) return;   /* 2D only */
    if (slot >= RHI_TEXTURE_SLOTS) return;   /* the backend has no such sampler slot */
    memcpy(&v, state, sizeof v);
    if (v.valid != VK_SNAPSHOT_MAGIC) return;
    v.tex2d[slot] = t ? texture : (RhiTexture){0};
    memcpy(state, &v, sizeof v);
}

/* Assigns the saved pipeline, viewport and 2D bindings; cube bindings stay. The
 * recorded commands carry no ambient state (every draw sets its own), so this is
 * pure bookkeeping apart from ending an open pass. */
void rhi_cmd_restore_state(const RhiStateSnapshot *state) {
    RhiVkSnapshot v;
    if (!state) return;
    memcpy(&v, state, sizeof v);
    if (v.valid != VK_SNAPSHOT_MAGIC) return;   /* never saved */
    rhi_cmd_end_pass();   /* GL rebinds the main framebuffer here; a pass ends */
    g_vk.bound_pipeline = v.pipeline;
    memcpy(g_vk.viewport, v.viewport, sizeof g_vk.viewport);
    memcpy(g_vk.bound_2d, v.tex2d, sizeof g_vk.bound_2d);
}
/* Stores the GL-convention rect (origin bottom-left); each draw uses it as a
 * VkViewport with y = gl_y (positive height). See the orientation note at
 * ensure_rendering: that plus the row-flip in rhi_read_pixels reproduces the GL
 * backend's output exactly. Nothing is recorded until the next draw. */
RhiResult rhi_cmd_set_viewport(int32_t x, int32_t y, int32_t width, int32_t height) {
    if (width < 0 || height < 0) return RHI_ERR_INVALID;
    g_vk.viewport[0] = x; g_vk.viewport[1] = y;
    g_vk.viewport[2] = width; g_vk.viewport[3] = height;
    return RHI_OK;
}
/* Triangle pipelines switch to their polygonMode=LINE variant (built on first use);
 * pipelines with line topology are unaffected, like GL's glPolygonMode. */
RhiResult rhi_cmd_set_wireframe(int on) {
    if (!g_vk.caps.wireframe) return RHI_ERR_UNSUPPORTED;
    g_vk.wireframe = on ? 1 : 0;
    return RHI_OK;
}

/* --- GPU timers --- */
/* Slot of a live timer handle, or -1. */
static int timer_slot(RhiTimer t) {
    uint32_t slot = t.id & VK_SLOT_MASK;
    if (!t.id || !slot || slot > VK_MAX_TIMERS) return -1;
    if (!g_vk.timers[slot - 1].live || g_vk.timers[slot - 1].generation != (uint8_t)(t.id >> VK_SLOT_BITS)) return -1;
    return (int)slot - 1;
}

RhiTimer rhi_timer_create(void) {
    RhiTimer t = {0};
    if (!g_vk.caps.gpu_timer) return t;
    for (uint32_t i = 0; i < VK_MAX_TIMERS; i++) {
        if (g_vk.timers[i].live) continue;
        if (g_vk.timers[i].generation == UINT8_MAX && g_vk.timers[i].state == 3) continue;   /* retired */
        /* `unsubmitted` is kept: the recycled query pair may still be pending in the open recording */
        g_vk.timers[i].live = 1; g_vk.timers[i].state = 0;
        t.id = ((uint32_t)g_vk.timers[i].generation << VK_SLOT_BITS) | (i + 1u);
        return t;
    }
    fprintf(stderr, "rhi: timer_create: out of timers\n");
    return t;
}

void rhi_timer_destroy(RhiTimer timer) {
    int s = timer_slot(timer);
    if (s < 0) return;
    if (g_vk.timer_running == s) g_vk.timer_running = -1;
    g_vk.timers[s].live = 0;
    /* A running timer already wrote its begin stamp; nothing else is pending on it. */
    if (g_vk.timers[s].generation == UINT8_MAX) g_vk.timers[s].state = 3;   /* retire before wrap */
    else g_vk.timers[s].generation++;
}

/* The measurement is the GPU time between the two stamps of one timer; begin
 * resets them from the host, so a slot reused while its stamps sit in the
 * unsubmitted recording first splits the frame (the fence-waited submit makes the
 * old pair final). */
RhiResult rhi_cmd_timer_begin(RhiTimer timer) {
    if (!g_vk.caps.gpu_timer) return RHI_ERR_UNSUPPORTED;
    int s = timer_slot(timer);
    if (s < 0 || g_vk.timer_running >= 0 || !g_vk.in_frame) return RHI_ERR_INVALID;
    if (g_vk.recording && g_vk.timers[s].unsubmitted == g_vk.submits + 1u && !frame_split())
        return RHI_ERR_DEVICE_LOST;
    if (!ensure_recording()) return RHI_ERR_DEVICE_LOST;
    vkResetQueryPool(g_vk.device, g_vk.timer_pool, 2u * (uint32_t)s, 2);
    vkCmdWriteTimestamp(g_vk.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g_vk.timer_pool, 2u * (uint32_t)s);
    g_vk.timers[s].state = 1;
    g_vk.timers[s].unsubmitted = g_vk.submits + 1u;
    g_vk.timer_running = s;
    return RHI_OK;
}

RhiResult rhi_cmd_timer_end(void) {
    if (!g_vk.caps.gpu_timer) return RHI_ERR_UNSUPPORTED;
    int s = g_vk.timer_running;
    if (s < 0 || !g_vk.in_frame) return RHI_ERR_INVALID;
    if (!ensure_recording()) return RHI_ERR_DEVICE_LOST;
    vkCmdWriteTimestamp(g_vk.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_vk.timer_pool, 2u * (uint32_t)s + 1u);
    g_vk.timers[s].state = 2;
    g_vk.timers[s].unsubmitted = g_vk.submits + 1u;
    g_vk.timer_running = -1;
    return RHI_OK;
}

int rhi_timer_read(RhiTimer timer, uint64_t *ns) {
    int s = timer_slot(timer);
    if (s < 0 || !ns || g_vk.timers[s].state != 2) return 0;
    /* Stamps still in the unsubmitted recording have not executed. */
    if (g_vk.recording && g_vk.timers[s].unsubmitted == g_vk.submits + 1u) return 0;
    uint64_t r[4];   /* {begin, avail, end, avail} */
    VkResult res = vkGetQueryPoolResults(g_vk.device, g_vk.timer_pool, 2u * (uint32_t)s, 2, sizeof r, r,
                                         2 * sizeof r[0], VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    if (res != VK_SUCCESS && res != VK_NOT_READY) return 0;
    if (!r[1] || !r[3]) return 0;
    uint64_t mask = 0;
    {   /* timestampValidBits of the queue family */
        uint32_t qc = 0; vkGetPhysicalDeviceQueueFamilyProperties(g_vk.phys, &qc, NULL);
        VkQueueFamilyProperties *qp = calloc(qc ? qc : 1, sizeof *qp);
        vkGetPhysicalDeviceQueueFamilyProperties(g_vk.phys, &qc, qp);
        uint32_t bits = g_vk.queue_family < qc ? qp[g_vk.queue_family].timestampValidBits : 0;
        free(qp);
        mask = bits >= 64 ? UINT64_MAX : ((1ull << bits) - 1u);
    }
    VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(g_vk.phys, &props);
    uint64_t ticks = (r[2] - r[0]) & mask;
    *ns = (uint64_t)((double)ticks * (double)props.limits.timestampPeriod);
    return 1;
}
