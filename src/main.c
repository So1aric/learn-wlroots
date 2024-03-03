#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <wayland-util.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/gles2.h>
#include <wlr/render/swapchain.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_matrix.h>
#include <wlr/util/box.h>
#include <xkbcommon/xkbcommon.h>

#define PHYSAC_IMPLEMENTATION
#define PHYSAC_STANDALONE
#include <physac.h>

#define log(...) fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n")

// #define DEBUG true

#if DEBUG
#define _add_listener(name, event, data)                                \
    void _##name##_##event(struct name *, void *);                      \
    void name##_##event(struct wl_listener *wl_listener, void *data) {  \
        log("Listener triggered: " #name "_" #event);                   \
        struct name *name = wl_container_of(wl_listener, name, event);  \
        _##name##_##event(name, data);                                  \
    }                                                                   \
    void _##name##_##event(struct name *name, void *data)
#else
#define _add_listener(name, event, data)                                \
    void _##name##_##event(struct name *, void *);                      \
    void name##_##event(struct wl_listener *wl_listener, void *data) {  \
        struct name *name = wl_container_of(wl_listener, name, event);  \
        _##name##_##event(name, data);                                  \
    }                                                                   \
    void _##name##_##event(struct name *name, void *data)
#endif

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

    struct wlr_cursor *cursor;
    struct wlr_xcursor_manager *cursor_mgr;
    struct wl_listener cursor_motion;
    struct wl_listener cursor_motion_absolute;
    struct wl_listener cursor_button;
    struct wl_listener cursor_axis;
    struct wl_listener cursor_frame;

    struct wlr_seat *seat;
    struct wl_list keyboards;
    struct wl_listener new_input;
    struct wl_listener request_cursor;
    struct wl_listener request_set_selection;

    PhysicsBody floor;
} Server;

typedef struct output {
    struct server *server;
    struct wl_list link;
    struct wlr_output *base;

    struct wl_listener frame;
} Output;

typedef struct toplevel {
    struct server *server;
    struct wl_list link;
    struct wlr_xdg_toplevel *base;

    // Physac
    Vector2 pos, size;
    PhysicsBody body;

    struct wl_listener map;
    struct wl_listener unmap;
    struct wl_listener destroy;
} Toplevel;

typedef struct keyboard {
    struct server *server;
    struct wl_list link;
    struct wlr_keyboard *base;

    struct wl_listener modifiers;
    struct wl_listener key;
    struct wl_listener destroy;
} Keyboard;

#define server_listener(event, data)   _add_listener(server, event, data)
#define output_listener(event, data)   _add_listener(output, event, data)
#define toplevel_listener(event, data) _add_listener(toplevel, event, data)
#define keyboard_listener(event, data) _add_listener(keyboard, event, data)

#define listener_definition(name) \
    void name(struct wl_listener *, void *data)

listener_definition(server_new_output);
listener_definition(server_new_xdg_surface);
listener_definition(server_cursor_motion);
listener_definition(server_cursor_motion_absolute);
listener_definition(server_cursor_button);
listener_definition(server_cursor_axis);
listener_definition(server_cursor_frame);
listener_definition(server_new_input);

listener_definition(output_frame);

listener_definition(toplevel_map);
listener_definition(toplevel_unmap);
listener_definition(toplevel_destroy);

listener_definition(keyboard_modifiers);
listener_definition(keyboard_key);
listener_definition(keyboard_destroy);

void focus_toplevel(Toplevel *toplevel, struct wlr_surface *surface) {
    if (toplevel == NULL) return;

    Server *server = toplevel->server;
    struct wlr_seat *seat = server->seat;

    struct wlr_surface *prev_surface = seat->keyboard_state.focused_surface;
    if (prev_surface == surface) return;
    if (prev_surface) {
        struct wlr_xdg_toplevel *prev_toplevel = wlr_xdg_toplevel_try_from_wlr_surface(prev_surface);
        if (prev_toplevel) {
            wlr_xdg_toplevel_set_activated(prev_toplevel, false);
        }
    }

    wlr_xdg_toplevel_set_activated(toplevel->base, true);
    struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
    if (keyboard) {
        wlr_seat_keyboard_notify_enter(seat, toplevel->base->base->surface, keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
    }
}

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
    output->base = wlr_output;

    output->frame.notify = output_frame;
    wl_signal_add(&wlr_output->events.frame, &output->frame);

    wl_list_insert(&server->outputs, &output->link);

    /* struct wlr_output_layout_output *layout_output =*/ wlr_output_layout_add_auto(server->output_layout, wlr_output);

    // We add a floor and two walls for the new output
    PhysicsBody floor = CreatePhysicsBodyRectangle(
        (Vector2){ (float)wlr_output->width / 2, wlr_output->height },
        wlr_output->width, 1, 1
    );
    floor->enabled = false;

    PhysicsBody left_wall = CreatePhysicsBodyRectangle(
        (Vector2){ 0, (float)wlr_output->height / 2 },
        1, wlr_output->height, 1
    );
    left_wall->enabled = false;

    PhysicsBody right_wall = CreatePhysicsBodyRectangle(
        (Vector2){ wlr_output->width, (float)wlr_output->height / 2 },
        1, wlr_output->height, 1
    );
    right_wall->enabled = false;
}

server_listener(new_xdg_surface, data) {
    struct wlr_xdg_surface *xdg_surface = data;
    struct wlr_xdg_toplevel *xdg_toplevel = xdg_surface->toplevel;

    if (xdg_surface->role == WLR_XDG_SURFACE_ROLE_NONE || xdg_surface->role == WLR_XDG_SURFACE_ROLE_POPUP) return;

    Toplevel *toplevel = malloc(sizeof(*toplevel));
    toplevel->server = server;
    toplevel->base = xdg_toplevel;
    Output *output = wl_container_of(server->outputs.next, output, link);
    toplevel->pos.x = (float)output->base->width / 2;
    toplevel->pos.y = -400;
    toplevel->body = NULL;

    toplevel->map.notify = toplevel_map;
    wl_signal_add(&xdg_surface->surface->events.map, &toplevel->map);

    toplevel->unmap.notify = toplevel_unmap;
    wl_signal_add(&xdg_surface->surface->events.unmap, &toplevel->unmap);

    toplevel->destroy.notify = toplevel_destroy;
    wl_signal_add(&xdg_surface->surface->events.destroy, &toplevel->destroy);
}

void new_keyboard(Server *server, struct wlr_input_device *device) {
    struct wlr_keyboard *wlr_keyboard = wlr_keyboard_from_input_device(device);

    Keyboard *keyboard = malloc(sizeof(*keyboard));
    keyboard->server = server;
    keyboard->base = wlr_keyboard;

    struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);

    wlr_keyboard_set_keymap(wlr_keyboard, keymap);
    wlr_keyboard_set_repeat_info(wlr_keyboard, 25, 600);

    xkb_keymap_unref(keymap);
    xkb_context_unref(context);

    keyboard->modifiers.notify = keyboard_modifiers;
    wl_signal_add(&wlr_keyboard->events.modifiers, &keyboard->modifiers);

    keyboard->key.notify = keyboard_key;
    wl_signal_add(&wlr_keyboard->events.key, &keyboard->key);

    keyboard->destroy.notify = keyboard_destroy;
    wl_signal_add(&device->events.destroy, &keyboard->destroy);

    wlr_seat_set_keyboard(server->seat, wlr_keyboard);
    wl_list_insert(&server->keyboards, &keyboard->link);
}

void new_pointer(Server *server, struct wlr_input_device *device) {
    wlr_cursor_attach_input_device(server->cursor, device);
}

server_listener(new_input, data) {
    struct wlr_input_device *device = data;

    switch (device->type) {
    case WLR_INPUT_DEVICE_KEYBOARD:
        new_keyboard(server, device);
        break;
    case WLR_INPUT_DEVICE_POINTER:
        new_pointer(server, device);
        break;
    default:
        break;
    }

    uint32_t caps = WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD;
    wlr_seat_set_capabilities(server->seat, caps);
}

void process_cursor_motion(Server *server) {
    wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
}

server_listener(cursor_motion, data) {
    struct wlr_pointer_motion_event *event = data;
    wlr_cursor_move(server->cursor, &event->pointer->base, event->delta_x, event->delta_y);
    process_cursor_motion(server);
}

server_listener(cursor_motion_absolute, data) {
    struct wlr_pointer_motion_absolute_event *event = data;
    wlr_cursor_warp_absolute(server->cursor, &event->pointer->base, event->x, event->y);
}

server_listener(cursor_button, data) {
    struct wlr_pointer_button_event *event = data;
    wlr_seat_pointer_notify_button(server->seat, event->time_msec, event->button, event->state);
}

server_listener(cursor_axis, data) {
    struct wlr_pointer_axis_event *event = data;
    wlr_seat_pointer_notify_axis(server->seat, event->time_msec, event->orientation, event->delta, event->delta_discrete, event->source);
}

server_listener(cursor_frame, data) {
    wlr_seat_pointer_notify_frame(server->seat);
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
    Frame *frame = start_frame(output->base);
    struct wlr_renderer *renderer = output->base->renderer;

    wlr_renderer_begin_with_buffer(renderer, frame->buffer);

    wlr_renderer_clear(renderer, (float[]){ 0, 0, 0, 1 });

    Toplevel *toplevel;
    wl_list_for_each(toplevel, &output->server->toplevels, link) {
        struct wlr_surface *surface = toplevel->base->base->surface;
        struct wlr_texture *texture = wlr_surface_get_texture(surface);

        PhysicsBody body = toplevel->body;
        Vector2 position = body->position;
        float rotation = body->orient;
        float half_width = (float)texture->width / 2;
        float half_height = (float)texture->height / 2;

        float proj[9];
        wlr_matrix_identity(proj);
        wlr_matrix_translate(proj, position.x, position.y);
        wlr_matrix_rotate(proj, rotation);

        wlr_render_texture(
            renderer,
            texture,
            proj,
            -half_width, -half_height, 1.0
        );

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        wlr_surface_send_frame_done(surface, &now);
    }
    
    wlr_renderer_end(renderer);

    if (!wlr_render_pass_submit(frame->render_pass)) {
        log("Fail to submit render_pass");
        wlr_buffer_unlock(frame->buffer);
        return;
    }

    wlr_output_state_set_buffer(&frame->state, frame->buffer);
    wlr_buffer_unlock(frame->buffer);

    if (!wlr_output_test_state(output->base, &frame->state)) {
        log("Output state test failed");
        return;
    }

    if (!wlr_output_commit_state(output->base, &frame->state)) {
        log("Fail to commit output state");
        return;
    }

    frame_destroy(frame);
}

toplevel_listener(map, data) {
    wl_list_insert(&toplevel->server->toplevels, &toplevel->link);

    struct wlr_surface *surface = toplevel->base->base->surface;
    struct wlr_texture *texture = wlr_surface_get_texture(surface);
    toplevel->size.x = texture->width;
    toplevel->size.y = texture->height;

    // Here we have enough information to create a physics object.
    if (toplevel->body) return;
    toplevel->body = CreatePhysicsBodyRectangle(toplevel->pos, toplevel->size.x, toplevel->size.y, 1);
    SetPhysicsBodyRotation(toplevel->body, (float)rand() / RAND_MAX);
}

toplevel_listener(unmap, data) {
    wl_list_remove(&toplevel->link);
}

toplevel_listener(destroy, data) {
    wl_list_remove(&toplevel->map.link);
    wl_list_remove(&toplevel->unmap.link);
    wl_list_remove(&toplevel->destroy.link);

    DestroyPhysicsBody(toplevel->body);

    free(toplevel);
}

keyboard_listener(modifiers, data) {
    wlr_seat_set_keyboard(keyboard->server->seat, keyboard->base);
    wlr_seat_keyboard_notify_modifiers(keyboard->server->seat,&keyboard->base->modifiers );
}

bool handle_keybinding(Server *server, xkb_keysym_t sym) {
    switch (sym) {
    case XKB_KEY_Escape:
        wl_display_terminate(server->display);
        break;
    case XKB_KEY_F1: {
        Toplevel *toplevel = wl_container_of(server->toplevels.next, toplevel, link);
        focus_toplevel(toplevel, toplevel->base->base->surface);
        break;
    }
    default:
        return false;
    }
    return true;
}

keyboard_listener(key, data) {
    struct wlr_keyboard_key_event *event = data;

    Server *server = keyboard->server;
    struct wlr_seat *seat = server->seat;

    uint32_t keycode = event->keycode + 8;
    const xkb_keysym_t *syms;
    int nsyms = xkb_state_key_get_syms(keyboard->base->xkb_state, keycode, &syms);

    bool handled = false;
    uint32_t modifiers = wlr_keyboard_get_modifiers(keyboard->base);
    if ((modifiers & WLR_MODIFIER_ALT) && event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        for (int i = 0; i < nsyms; i++) {
            handled = handle_keybinding(server, syms[i]);
        }
    }
    if (handled) return;

    wlr_seat_set_keyboard(seat, keyboard->base);
    wlr_seat_keyboard_notify_key(seat, event->time_msec, event->keycode, event->state);
}

keyboard_listener(destroy, data) {
    
}

int main(int argc, char** argv) {
    Server server = { 0 };

    server.display = wl_display_create();

    server.backend = wlr_backend_autocreate(server.display, NULL);

    server.renderer = wlr_renderer_autocreate(server.backend);
    wlr_renderer_init_wl_display(server.renderer, server.display);

    // Here we assume that the renderer is gles2
    // struct wlr_egl *wlr_egl = wlr_gles2_renderer_get_egl(server.renderer);

    server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);

    wlr_compositor_create(server.display, 6, server.renderer);
    wlr_subcompositor_create(server.display);
    wlr_data_device_manager_create(server.display);

    server.output_layout = wlr_output_layout_create();
    wl_list_init(&server.outputs);
    server.new_output.notify = server_new_output;
    wl_signal_add(&server.backend->events.new_output, &server.new_output);

    wl_list_init(&server.toplevels);
    server.xdg_shell = wlr_xdg_shell_create(server.display, 3);
    server.new_xdg_surface.notify = server_new_xdg_surface;
    wl_signal_add(&server.xdg_shell->events.new_surface, &server.new_xdg_surface);

    server.cursor = wlr_cursor_create();
    wlr_cursor_attach_output_layout(server.cursor, server.output_layout);
    server.cursor_mgr = wlr_xcursor_manager_create(NULL, 24);

    server.cursor_motion.notify = server_cursor_motion;
    wl_signal_add(&server.cursor->events.motion, &server.cursor_motion);

    server.cursor_motion_absolute.notify = server_cursor_motion_absolute;
    wl_signal_add(&server.cursor->events.motion_absolute, &server.cursor_motion_absolute);

    server.cursor_button.notify = server_cursor_button;
    wl_signal_add(&server.cursor->events.button, &server.cursor_button);

    server.cursor_axis.notify = server_cursor_axis;
    wl_signal_add(&server.cursor->events.axis, &server.cursor_axis);

    server.cursor_frame.notify = server_cursor_frame;
    wl_signal_add(&server.cursor->events.frame, &server.cursor_frame);

    wl_list_init(&server.keyboards);
    server.new_input.notify = server_new_input;
    wl_signal_add(&server.backend->events.new_input, &server.new_input);

    server.seat = wlr_seat_create(server.display, "seat0");

    const char *socket = wl_display_add_socket_auto(server.display);
    log("socket: <%s>", socket);

    // Set up Physac
    InitPhysics();
    SetPhysicsGravity(0, 1);

    wlr_backend_start(server.backend);
    // setenv("WAYLAND_DISPLAY", socket, true);

    if (fork() == 0) {
        execl("/bin/sh", "/bin/sh", "-c", "WAYLAND_DISPLAY=wayland-0 foot", NULL);
    }

    wl_display_run(server.display);

    wl_display_destroy_clients(server.display);
    wlr_output_layout_destroy(server.output_layout);
    wl_display_destroy(server.display);
    
    return 0;
}
