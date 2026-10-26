/* Compile the actual executor with narrowly injected GL allocation failures.
 * Everything else uses the real context/driver, including pixel verification.
 * No fault hooks are added to the shipped library or its public interface. */
#include "gl_api.h"
static int fail_texture, fail_framebuffer;
static unsigned deleted_textures, deleted_framebuffers;
static GLenum injected_error, allocation_error = 0x0505u;
static void fault_tex_image(GLenum target, GLint level, GLint format,
                            GLsizei w, GLsizei h, GLint border,
                            GLenum pixel_format, GLenum type, const void *pixels)
{
    if (fail_texture) { fail_texture = 0; injected_error = allocation_error; return; }
    glTexImage2D(target, level, format, w, h, border, pixel_format, type, pixels);
}
static GLenum fault_framebuffer_status(GLenum target)
{
    if (fail_framebuffer) { fail_framebuffer = 0; return 0x8CD6u; }
    return glCheckFramebufferStatus(target);
}
static GLenum fault_error(void)
{
    if (injected_error) { GLenum e = injected_error; injected_error = 0; return e; }
    return glGetError();
}
static void count_delete_textures(GLsizei n, const GLuint *ids)
{ deleted_textures += (unsigned)n; glDeleteTextures(n, ids); }
static void count_delete_framebuffers(GLsizei n, const GLuint *ids)
{ deleted_framebuffers += (unsigned)n; glDeleteFramebuffers(n, ids); }
#define glTexImage2D fault_tex_image
#define glCheckFramebufferStatus fault_framebuffer_status
#define glGetError fault_error
#define glDeleteTextures count_delete_textures
#define glDeleteFramebuffers count_delete_framebuffers
#include "../src/gl.c"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); exit(1); } } while (0)
#define OK(x) CHECK((x) == NEOX_OK)

static neox_frame *begin(neox_device *dev, neox_gl_surface *surface)
{
    static const neox_color red = {1,0,0,1};
    neox_target_desc target = {0}; neox_frame *frame;
    target.kind = NEOX_BACKEND_GPU; target.surface = surface; target.clear = &red;
    OK(neox_frame_begin(dev, &target, &frame)); return frame;
}
static void red_pixels(neox_gl_surface *surface)
{
    uint8_t pixels[16*16*4];
    OK(neox_gl_surface_read_pixels(surface, pixels));
    for (size_t i = 0; i < (size_t)surface->w * surface->h; i++)
        CHECK(pixels[i*4] == 255 && !pixels[i*4+1] && !pixels[i*4+2] && pixels[i*4+3] == 255);
}
int main(void)
{
    neox_context *ctx; neox_device *dev; neox_gl_surface *surface;
    OK(neox_context_create(NULL, &ctx));
    neox_gl_device_desc gd = {0}; gd.struct_size = sizeof(gd); gd.magic = NEOX_GL_DESC_MAGIC;
    neox_device_desc dd = {0}; dd.backend = NEOX_BACKEND_GPU;
    dd.platform = &gd; dd.platform_size = sizeof(gd);
    if (neox_device_create(ctx, &dd, &dev) != NEOX_OK) {
        const char *required = getenv("NEOX_REQUIRE_GL");
        CHECK(!required || !*required);
        neox_context_destroy(ctx); return 77;
    }
    OK(neox_gl_surface_create(dev, 8, 8, &surface));
    neox_submission sub; neox_frame *frame = begin(dev, surface);
    CHECK(neox_gl_surface_resize(surface, 16, 16) == NEOX_ERROR_INVALID_ARGUMENT);
    OK(neox_frame_end(frame, &sub));
    CHECK(neox_gl_surface_resize(surface, 16, 16) == NEOX_ERROR_INVALID_ARGUMENT);
    OK(neox_submission_wait(dev, sub)); neox_submission_release(dev, sub); red_pixels(surface);
    GLuint texture = surface->tex, fbo = surface->fbo;
    /* Model the owned CPU snapshot without requiring a native window. */
    surface->presented = nx_alloc(ctx, 8*8*4, 16); CHECK(surface->presented);
    memset(surface->presented, 0x59, 8*8*4); surface->presented_valid = 1;
    uint8_t *snapshot = surface->presented;
    size_t used = ctx->used;
    for (int fault = 0; fault < 2; fault++) {
        unsigned nt = deleted_textures, nf = deleted_framebuffers;
        fail_texture = !fault; fail_framebuffer = fault;
        CHECK(neox_gl_surface_resize(surface, 16, 16) == NEOX_ERROR_BACKEND);
        CHECK(!neox_device_is_lost(dev));
        CHECK(surface->tex == texture && surface->fbo == fbo && surface->w == 8 && surface->h == 8);
        CHECK(surface->presented == snapshot && surface->presented_valid && snapshot[0] == 0x59);
        CHECK(ctx->used == used && deleted_textures == nt+1 && deleted_framebuffers == nf+1);
        red_pixels(surface);
    }
    /* EGL association is independent of color attachment replacement. No native
     * API may consume this opaque sentinel; it must simply survive the resize. */
    surface->win_egl = (void *)(uintptr_t)1;
    OK(neox_gl_surface_resize(surface, 16, 16));
    CHECK(surface->win_egl == (void *)(uintptr_t)1); surface->win_egl = NULL;
    CHECK(surface->w == 16 && surface->h == 16 && surface->tex != texture && surface->fbo != fbo);
    CHECK(!surface->presented && !surface->presented_valid && ctx->used == used - 8*8*4);
    OK(neox_frame_end(begin(dev, surface), &sub)); OK(neox_submission_wait(dev, sub));
    neox_submission_release(dev, sub); red_pixels(surface);
    fail_texture = 1; allocation_error = 0x0507u; /* observed GL_CONTEXT_LOST */
    CHECK(neox_gl_surface_resize(surface, 8, 8) == NEOX_ERROR_DEVICE_LOST);
    CHECK(neox_device_is_lost(dev));
    CHECK(neox_gl_surface_resize(surface, 16, 16) == NEOX_ERROR_DEVICE_LOST);
    neox_gl_surface_destroy(dev, surface); neox_device_destroy(dev);
    CHECK(ctx->used == 0); neox_context_destroy(ctx);
    puts("ALL PASS (GL resize allocation rollback, snapshots, pending work, retry, cleanup)");
    return 0;
}
