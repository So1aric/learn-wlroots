#include <stdlib.h>
#include <stdio.h>
#include <wayland-server-core.h>
#include <wayland-util.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/swapchain.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_xdg_shell.h>

#define _add_listener(name, event, data)                                \
    void _##name##_##event(struct name *, void *);               \
    void name##_##event(struct wl_listener *wl_listener, void *data) {  \
        struct name *name = wl_container_of(wl_listener, name, event); \
        _##name##_##event(name, data);                           \
    }                                                                   \
    void _##name##_##event(struct name *name, void *data)

typedef struct server {
    struct wl_display *display;
    struct wlr_backend *backend;

    struct wlr_renderer *renderer;
    struct wlr_allocator *allocator;

    struct wlr_output_layout *output_layout;
    struct wl_list outputs;
    struct wl_listener new_output;

    struct wlr_xdg_shell *xdg_shell;
    struct wl_list toplevels;
    struct wl_listener new_xdg_surface;
} Server;

typedef struct output {
    struct server *server;
    struct wl_list link;
    struct wlr_output *wlr_output;

    struct wl_listener frame;
} Output;

typedef struct toplevel {
    struct server *server;
    struct wl_list link;
    struct wlr_xdg_toplevel *xdg_toplevel;

    struct wl_listener map;
    struct wl_listener unmap;
    struct wl_listener destroy;
} Toplevel;

#define server_listener(event, data) _add_listener(server, event, data)
#define output_listener(event, data) _add_listener(output, event, data)
#define toplevel_listener(event, data) _add_listener(toplevel, event, data)

#define listener_definition(name) \
    void name(struct wl_listener *, void *data)

listener_definition(server_new_output);
listener_definition(server_new_xdg_surface);

listener_definition(output_frame);

listener_definition(toplevel_map);
listener_definition(toplevel_unmap);
listener_definition(toplevel_destroy);

server_listener(new_output, data) {
    struct wlr_output *wlr_output = data;

    wlr_output_init_render(wlr_output, server->allocator, server->renderer);

    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);

    struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
    wlr_output_state_set_mode(&state, mode);

    wlr_output_commit_state(wlr_output, &state);
    wlr_output_state_finish(&state);

    Output *output = malloc(sizeof(*output));
    output->server = server;
    output->wlr_output = wlr_output;

    output->frame.notify = output_frame;
    wl_signal_add(&wlr_output->events.frame, &output->frame);

    wl_list_insert(&server->outputs, &output->link);

    /* struct wlr_output_layout_output *layout_output =*/ wlr_output_layout_add_auto(server->output_layout, wlr_output);
}

server_listener(new_xdg_surface, data) {
    struct wlr_xdg_surface *xdg_surface = data;
    struct wlr_xdg_toplevel *xdg_toplevel = xdg_surface->toplevel;

    if (xdg_surface->role == WLR_XDG_SURFACE_ROLE_NONE || xdg_surface->role == WLR_XDG_SURFACE_ROLE_POPUP) return;

    Toplevel *toplevel = malloc(sizeof(*toplevel));
    toplevel->server = server;
    toplevel->xdg_toplevel = xdg_toplevel;

    toplevel->map.notify = toplevel_map;
    wl_signal_add(&xdg_surface->surface->events.map, &toplevel->map);

    toplevel->unmap.notify = toplevel_unmap;
    wl_signal_add(&xdg_surface->surface->events.unmap, &toplevel->unmap);

    toplevel->destroy.notify = toplevel_destroy;
    wl_signal_add(&xdg_surface->surface->events.destroy, &toplevel->destroy);
}

typedef struct frame {
    struct wlr_output_state state;
    struct wlr_buffer *buffer;
    int buffer_age;
    struct wlr_render_pass *render_pass;
} Frame;

Frame *frame_create() {
    Frame *frame = malloc(sizeof(*frame));
    wlr_output_state_init(&frame->state);
    return frame;
}

void frame_destroy(Frame *frame) {
    wlr_output_state_finish(&frame->state);
    free(frame);
}

void acquire_swapchain_buffer(struct wlr_output *output, Frame *frame) {
    // int width, height;
    // wlr_output_transformed_resolution(output, &width, &height);

    frame->buffer = wlr_swapchain_acquire(output->swapchain, &frame->buffer_age);
}

Frame *start_frame(struct wlr_output *output) {
    Frame *frame = frame_create();

    acquire_swapchain_buffer(output, frame);

    frame->render_pass = wlr_renderer_begin_buffer_pass(output->renderer, frame->buffer, NULL);

    return frame;
}

output_listener(frame, data) {
    Frame *frame = start_frame(output->wlr_output);

    wlr_renderer_begin_with_buffer(output->wlr_output->renderer, frame->buffer);
    wlr_renderer_end(output->wlr_output->renderer);
}

toplevel_listener(map, data) {
    wl_list_insert(&toplevel->server->toplevels, &toplevel->link);
}

toplevel_listener(unmap, data) {
    wl_list_remove(&toplevel->link);
}

toplevel_listener(destroy, data) {
    wl_list_remove(&toplevel->map.link);
    wl_list_remove(&toplevel->unmap.link);
    wl_list_remove(&toplevel->destroy.link);

    free(toplevel);
}

int main() {
    Server server = { 0 };

    server.display = wl_display_create();

    server.backend = wlr_backend_autocreate(server.display, NULL);

    server.renderer = wlr_renderer_autocreate(server.backend);
    wlr_renderer_init_wl_display(server.renderer, server.display);

    server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);

    wlr_compositor_create(server.display, 5, server.renderer);

    server.output_layout = wlr_output_layout_create();
    wl_list_init(&server.outputs);
    server.new_output.notify = server_new_output;
    wl_signal_add(&server.backend->events.new_output, &server.new_output);

    wl_list_init(&server.toplevels);
    server.xdg_shell = wlr_xdg_shell_create(server.display, 3);
    server.new_xdg_surface.notify = server_new_xdg_surface;
    wl_signal_add(&server.xdg_shell->events.new_surface, &server.new_xdg_surface);

    const char *socket = wl_display_add_socket_auto(server.display);
    fprintf(stderr, "socket: <%s>\n", socket);

    wlr_backend_start(server.backend);
    // setenv("WAYLAND_DISPLAY", socket, true);

    wl_display_run(server.display);

    wl_display_destroy_clients(server.display);
    wlr_output_layout_destroy(server.output_layout);
    wl_display_destroy(server.display);
    
    return 0;
}
