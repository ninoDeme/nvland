#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>
#include <math.h>
#include <wayland-server-protocol.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <libdrm/drm_fourcc.h>

#include <wlr/types/wlr_output_layout.h>
#include <wlr/util/log.h>

#include <cairo.h>

#define FONT_SIZE 38
#define FONT "DejaVu Sans Mono"

struct cairo_buffer {
  struct wlr_buffer base;
  cairo_surface_t *surface;
};

static void cairo_buffer_destroy(struct wlr_buffer *wlr_buffer) {
  struct cairo_buffer *buffer = wl_container_of(wlr_buffer, buffer, base);
  wlr_buffer_finish(wlr_buffer);
  cairo_surface_destroy(buffer->surface);
  free(buffer);
}

static bool cairo_buffer_begin_data_ptr_access(struct wlr_buffer *wlr_buffer,
    uint32_t flags, void **data, uint32_t *format, size_t *stride) {
  struct cairo_buffer *buffer = wl_container_of(wlr_buffer, buffer, base);

  if (flags & WLR_BUFFER_DATA_PTR_ACCESS_WRITE) {
    return false;
  }

  *format = DRM_FORMAT_ARGB8888;
  *data = cairo_image_surface_get_data(buffer->surface);
  *stride = cairo_image_surface_get_stride(buffer->surface);
  return true;
}

static void cairo_buffer_end_data_ptr_access(struct wlr_buffer *wlr_buffer) {
}

static const struct wlr_buffer_impl cairo_buffer_impl = {
  .destroy = cairo_buffer_destroy,
  .begin_data_ptr_access = cairo_buffer_begin_data_ptr_access,
  .end_data_ptr_access = cairo_buffer_end_data_ptr_access
};

static struct cairo_buffer *create_cairo_buffer(int width, int height) {
  struct cairo_buffer *buffer = calloc(1, sizeof(*buffer));
  if (!buffer) {
    return NULL;
  }

  wlr_buffer_init(&buffer->base, &cairo_buffer_impl, width, height);

  buffer->surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  if (cairo_surface_status(buffer->surface) != CAIRO_STATUS_SUCCESS) {
    free(buffer);
    return NULL;
  }

  return buffer;
}

struct nvland_server {
  struct wl_display *wl_display;
  struct wl_event_loop *wl_event_loop;
  struct wlr_allocator *allocator;

  struct wlr_backend *backend;
  struct wlr_session *session;
  struct wlr_renderer *renderer;
  struct wlr_scene *scene;
  struct wlr_scene_output_layout *scene_layout;

  struct wl_listener new_output;

  struct wlr_output_layout *output_layout;
  struct wl_list outputs;
};

struct rgb24 {
  uint8_t r;
  uint8_t g;
  uint8_t b;
};

#pragma pack(1)
struct nvland_cell {
  struct rgb24 color;
  bool blink;
  bool bold;
  bool conceal;
  bool dim;
  bool italic;
  bool nocombine;
  bool overline;
  bool reverse;
  bool standout;
  bool strikethrough;
  bool undercurl;
  bool underdashed;
  bool underdotted;
  bool underdouble;
  bool underline;
  char glyph[4];
};

struct nvland_output_state {
  int width;
  int height;

  int offset_height;
  int cell_width;
  int cell_height;
  int font_size;

  struct nvland_cell *text_buffer;
};

struct nvland_output {
  struct wlr_output *wlr_output;
  struct nvland_server *server;
  struct timespec last_frame;

  struct wl_list link;
  struct wl_listener frame;
  struct wl_listener request_state;
  struct wl_listener destroy;

  struct nvland_output_state state;

  struct cairo_buffer *buffer;
  struct wlr_scene_buffer *scene_buffer;
};

void draw_cell(cairo_t *cr, int x, int y, struct nvland_cell cell, struct nvland_output_state *state) {
  char buff[] = "╳\0";
  cairo_move_to(cr, x * state->cell_width, y * state->cell_height + state->offset_height);
  cairo_show_text(cr, buff);
}

static void output_frame(struct wl_listener *listener, void *data) {
  /* This function is called every time an output is ready to display a frame,
   * generally at the output's refresh rate (e.g. 60Hz). */
  struct nvland_output *output = wl_container_of(listener, output, frame);
  struct wlr_output *wlr_output = output->wlr_output;
  struct wlr_scene *scene = output->server->scene;

  struct wlr_scene_output *scene_output = wlr_scene_get_scene_output(scene, output->wlr_output);

  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);

	cairo_t *cr = cairo_create(output->buffer->surface);
	cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);
  cairo_paint(cr);
	cairo_set_source_rgba(cr, 1, 1, 1, 0.5);
  cairo_select_font_face(cr, FONT, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, output->state.font_size);

  int i = 0;
  for (int h = 0; h < output->state.height; h++) {
    for (int w = 0; w < output->state.width; w++) {
      draw_cell(cr, w, h, output->state.text_buffer[i], &output->state);
      i += 1;
    }
  }

  cairo_set_source_rgb(cr, 1, 0, 0);
  cairo_rectangle(cr, 0, 0, 1280, 720);
  cairo_stroke(cr);

	cairo_destroy(cr);
	/* End drawing */

  wlr_scene_buffer_set_buffer(output->scene_buffer, &output->buffer->base);

  /* Render the scene and commit the output */
  wlr_scene_output_commit(scene_output, NULL);
  wlr_scene_output_send_frame_done(scene_output, &now);

  output->last_frame = now;
}


static void new_output_notify(struct wl_listener *listener, void *data) {
  struct nvland_server *server = wl_container_of(listener, server, new_output);
  struct wlr_output *wlr_output = data;

  wlr_output_init_render(wlr_output, server->allocator, server->renderer);

  /* The output may be disabled, switch it on. */
  struct wlr_output_state state;
  wlr_output_state_init(&state);
  wlr_output_state_set_enabled(&state, true);

  /* Some backends don't have modes. DRM+KMS does, and we need to set a mode
   * before we can use the output. The mode is a tuple of (width, height,
   * refresh rate), and each monitor supports only a specific set of modes. We
   * just pick the monitor's preferred mode, a more sophisticated compositor
   * would let the user configure it. */
  struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
  if (mode != NULL) {
    wlr_output_state_set_mode(&state, mode);
  }

  /* Atomically applies the new output state. */
  wlr_output_commit_state(wlr_output, &state);
  wlr_output_state_finish(&state);

  struct nvland_output *output = calloc(1, sizeof(struct nvland_output));

  output->server = server;
  output->wlr_output = wlr_output;

  output->frame.notify = output_frame;
  wl_signal_add(&wlr_output->events.frame, &output->frame);

  clock_gettime(CLOCK_MONOTONIC, &output->last_frame);
  wl_list_insert(&server->outputs, &output->link);

  struct wlr_output_layout_output *l_output = wlr_output_layout_add_auto(server->output_layout, wlr_output);
  struct wlr_scene_output *scene_output = wlr_scene_output_create(server->scene, wlr_output);
  wlr_scene_output_layout_add_output(server->scene_layout, l_output, scene_output);

  output->buffer = create_cairo_buffer(wlr_output->width, wlr_output->height);
  output->state.font_size = FONT_SIZE;

  {
    cairo_t *cr = cairo_create(output->buffer->surface);
    cairo_select_font_face(cr, FONT, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, output->state.font_size);
    cairo_font_extents_t font_extents;
    cairo_font_extents(cr, &font_extents);
    cairo_text_extents_t text_extents;
    cairo_text_extents(cr, "m", &text_extents);

    output->state.cell_width = text_extents.x_advance;
    output->state.cell_height = font_extents.height;
    output->state.offset_height = font_extents.ascent;
    // printf("%f %f %f\n", font_extents.ascent, font_extents.descent, font_extents.height);

    cairo_destroy(cr);
  }

  output->state.width = (float) wlr_output->width / (float) output->state.cell_width;
  output->state.height = (float) wlr_output->height / (float) output->state.cell_height;
  printf("%i %i %i\n", output->state.cell_width, wlr_output->width, output->state.width);
  output->state.text_buffer = calloc(output->state.width * output->state.height, sizeof(struct nvland_cell));

//  wlr_buffer_drop(&output->buffer->base);

  output->scene_buffer = wlr_scene_buffer_create(&server->scene->tree, NULL);
  wlr_scene_node_set_position(&output->scene_buffer->node, l_output->x, l_output->y);
}

int main (int argc, char **argv) {
  wlr_log_init(WLR_DEBUG, NULL);
  struct nvland_server server = {0};
  server.wl_display = wl_display_create();
  assert(server.wl_display);
  server.wl_event_loop = wl_display_get_event_loop(server.wl_display);
  assert(server.wl_event_loop);

  server.backend = wlr_backend_autocreate(server.wl_event_loop, NULL);
  if (server.backend == NULL) {
    wlr_log(WLR_ERROR, "failed to create wlr_backend");
    return 1;
  }

  /* Autocreates a renderer, either Pixman, GLES2 or Vulkan for us. The user
   * can also specify a renderer using the WLR_RENDERER env var.
   * The renderer is responsible for defining the various pixel formats it
   * supports for shared memory, this configures that for clients. */
  server.renderer = wlr_renderer_autocreate(server.backend);
  if (server.renderer == NULL) {
    wlr_log(WLR_ERROR, "failed to create wlr_renderer");
    return 1;
  }

  wlr_renderer_init_wl_display(server.renderer, server.wl_display);

  /* Autocreates an allocator for us.
   * The allocator is the bridge between the renderer and the backend. It
   * handles the buffer creation, allowing wlroots to render onto the
   * screen */
  server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);
  if (server.allocator == NULL) {
    wlr_log(WLR_ERROR, "failed to create wlr_allocator");
    return 1;
  }

  wlr_compositor_create(server.wl_display, 5, server.renderer);
  wlr_subcompositor_create(server.wl_display);
  // wlr_data_device_manager_create(server.wl_display);

  wl_list_init(&server.outputs);

  server.new_output.notify = new_output_notify;
  wl_signal_add(&server.backend->events.new_output, &server.new_output);

  server.output_layout = wlr_output_layout_create(server.wl_display);

  server.scene = wlr_scene_create();
  server.scene->WLR_PRIVATE.direct_scanout = false; // TODO: possibly remove when updating to newer versions, or at least find a workaround
  server.scene_layout = wlr_scene_attach_output_layout(server.scene, server.output_layout);

  if (!wlr_backend_start(server.backend)) {
    fprintf(stderr, "Failed to start backend\n");
    wlr_backend_destroy(server.backend);
    wl_display_destroy(server.wl_display);
    return 1;
  }

  wl_display_run(server.wl_display);

  wl_display_destroy_clients(server.wl_display);
  wl_display_destroy(server.wl_display);
  return 0;
}
