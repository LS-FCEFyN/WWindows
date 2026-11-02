#include <wwindows/adapters/wayland/globals/image_copy_capture.hpp>

#include <wwindows/adapters/wayland/protocols/image-copy-capture-server-protocol.h>
#include <wwindows/adapters/wayland/globals/image_capture_source.hpp>
#include <wwindows/core/surface_manager.hpp>
#include <wwindows/support/log.hpp>

#include <wayland-server-core.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <optional>

namespace ww::image_copy_capture {

namespace {

// User data for the ext_image_copy_capture_manager_v1 global.
class ImageCopyCaptureManagerGlobal {
public:
    std::vector<wl_resource *> managers;
    wl_listener display_destroy{};
};

// User data for each ext_image_copy_capture_session_v1
struct CaptureSessionData {
    ImageCopyCaptureManagerGlobal *global{nullptr};
    wl_resource *source{nullptr};
    uint32_t options{0};
    wl_resource *frame{nullptr}; // Current frame (max 1)
    bool stopped = false;

    // Buffer constraints
    uint32_t buffer_width{0};
    uint32_t buffer_height{0};
    std::vector<uint32_t> shm_formats;
    std::vector<std::pair<uint32_t, std::vector<uint64_t>>> dmabuf_formats; // format, modifiers
    std::optional<std::vector<uint64_t>> dmabuf_device;
    bool constraints_sent = false;
};

// User data for each ext_image_copy_capture_frame_v1
struct CaptureFrameData {
    CaptureSessionData *session{nullptr};
    wl_resource *buffer{nullptr};
    bool capture_sent = false;
    bool ready_sent = false;

    struct DamageRect {
        int32_t x, y, width, height;
    };
    std::vector<DamageRect> damage_rects;
};

// User data for each ext_image_copy_capture_cursor_session_v1
struct CursorCaptureSessionData {
    ImageCopyCaptureManagerGlobal *global{nullptr};
    wl_resource *source{nullptr};
    wl_resource *pointer{nullptr};
    wl_resource *capture_session{nullptr}; // The linked capture session
    bool session_created = false;
};

// Forward declarations
static void send_constraints(CaptureSessionData *session);

// Resource destroy callbacks
static void session_resource_destroyed(wl_resource *resource) {
    auto *data = static_cast<CaptureSessionData *>(wl_resource_get_user_data(resource));
    if (!data)
        return;

    if (data->frame) {
        wl_resource_destroy(data->frame);
        data->frame = nullptr;
    }
    delete data;
}

static void frame_resource_destroyed(wl_resource *resource) {
    auto *data = static_cast<CaptureFrameData *>(wl_resource_get_user_data(resource));
    if (!data)
        return;

    if (data->session) {
        data->session->frame = nullptr;
    }
    delete data;
}

static void cursor_session_resource_destroyed(wl_resource *resource) {
    auto *data = static_cast<CursorCaptureSessionData *>(wl_resource_get_user_data(resource));
    if (!data)
        return;

    if (data->capture_session) {
        wl_resource_destroy(data->capture_session);
    }
    delete data;
}

// ext_image_copy_capture_frame_v1 interface
// Order: destroy, attach_buffer, damage_buffer, capture
static void frame_destroy(wl_client *, wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void frame_attach_buffer(wl_client *, wl_resource *resource, wl_resource *buffer) {
    auto *data = static_cast<CaptureFrameData *>(wl_resource_get_user_data(resource));
    if (!data || data->capture_sent) {
        wl_resource_post_error(resource,
            EXT_IMAGE_COPY_CAPTURE_FRAME_V1_ERROR_ALREADY_CAPTURED,
            "attach_buffer after capture");
        return;
    }
    data->buffer = buffer;
}

static void frame_damage_buffer(wl_client *, wl_resource *resource,
                                int32_t x, int32_t y, int32_t width, int32_t height) {
    auto *data = static_cast<CaptureFrameData *>(wl_resource_get_user_data(resource));
    if (!data || data->capture_sent) {
        wl_resource_post_error(resource,
            EXT_IMAGE_COPY_CAPTURE_FRAME_V1_ERROR_ALREADY_CAPTURED,
            "damage_buffer after capture");
        return;
    }
    if (x < 0 || y < 0 || width <= 0 || height <= 0) {
        wl_resource_post_error(resource,
            EXT_IMAGE_COPY_CAPTURE_FRAME_V1_ERROR_INVALID_BUFFER_DAMAGE,
            "invalid buffer damage");
        return;
    }
    data->damage_rects.push_back({x, y, width, height});
}

static void frame_capture(wl_client *, wl_resource *resource) {
    auto *data = static_cast<CaptureFrameData *>(wl_resource_get_user_data(resource));
    if (!data) return;

    if (data->capture_sent) {
        wl_resource_post_error(resource,
            EXT_IMAGE_COPY_CAPTURE_FRAME_V1_ERROR_ALREADY_CAPTURED,
            "capture already sent");
        return;
    }
    if (!data->buffer) {
        wl_resource_post_error(resource,
            EXT_IMAGE_COPY_CAPTURE_FRAME_V1_ERROR_NO_BUFFER,
            "no buffer attached");
        return;
    }

    data->capture_sent = true;

    // In a real implementation, this would trigger the actual capture
    // For now, we send a minimal response with transform=0 (normal), no damage, and ready
    ext_image_copy_capture_frame_v1_send_transform(resource, WL_OUTPUT_TRANSFORM_NORMAL);
    ext_image_copy_capture_frame_v1_send_ready(resource);
    data->ready_sent = true;

    WW_LOG_TRACE("[image-copy-capture] frame captured");
}

static const struct ext_image_copy_capture_frame_v1_interface frame_impl = {
    .destroy = frame_destroy,
    .attach_buffer = frame_attach_buffer,
    .damage_buffer = frame_damage_buffer,
    .capture = frame_capture,
};

// ext_image_copy_capture_session_v1 interface
// Order: create_frame, destroy
static void session_create_frame(wl_client *client, wl_resource *resource, uint32_t id) {
    auto *session_data = static_cast<CaptureSessionData *>(wl_resource_get_user_data(resource));
    if (!session_data)
        return;

    if (session_data->frame) {
        wl_resource_post_error(resource,
            EXT_IMAGE_COPY_CAPTURE_SESSION_V1_ERROR_DUPLICATE_FRAME,
            "frame already exists");
        return;
    }

    wl_resource *frame = wl_resource_create(client, &ext_image_copy_capture_frame_v1_interface,
                                            wl_resource_get_version(resource), id);
    if (!frame)
        return;

    auto *frame_data = new CaptureFrameData{};
    frame_data->session = session_data;

    wl_resource_set_implementation(frame, &frame_impl, frame_data, frame_resource_destroyed);
    session_data->frame = frame;
}

static void session_destroy(wl_client *, wl_resource *resource) {
    wl_resource_destroy(resource);
}

static const struct ext_image_copy_capture_session_v1_interface session_impl = {
    .create_frame = session_create_frame,
    .destroy = session_destroy,
};

// ext_image_copy_capture_cursor_session_v1 interface
// Order: destroy, get_capture_session
static void cursor_session_destroy(wl_client *, wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void cursor_session_get_capture_session(wl_client *client, wl_resource *resource, uint32_t id) {
    auto *data = static_cast<CursorCaptureSessionData *>(wl_resource_get_user_data(resource));
    if (!data || data->session_created) {
        wl_resource_post_error(resource,
            EXT_IMAGE_COPY_CAPTURE_CURSOR_SESSION_V1_ERROR_DUPLICATE_SESSION,
            "get_capture_session already called");
        return;
    }

    // Create a capture session for the cursor
    wl_resource *session = wl_resource_create(client, &ext_image_copy_capture_session_v1_interface,
                                              wl_resource_get_version(resource), id);
    if (!session)
        return;

    auto *session_data = new CaptureSessionData{};
    session_data->global = data->global;
    session_data->source = data->source;
    session_data->options = 0; // No cursor painting for cursor session

    wl_resource_set_implementation(session, &session_impl, session_data, session_resource_destroyed);
    data->capture_session = session;
    data->session_created = true;

    // Send constraints for cursor session
    send_constraints(session_data);
}

static const struct ext_image_copy_capture_cursor_session_v1_interface cursor_session_impl = {
    .destroy = cursor_session_destroy,
    .get_capture_session = cursor_session_get_capture_session,
};

// ext_image_copy_capture_manager_v1 interface
// Order: create_session, create_pointer_cursor_session, destroy
static void manager_create_session(wl_client *client, wl_resource *resource,
                                   uint32_t id, wl_resource *source, uint32_t options) {
    auto *global = static_cast<ImageCopyCaptureManagerGlobal *>(wl_resource_get_user_data(resource));
    if (!global)
        return;

    // Validate options
    if (options & ~static_cast<uint32_t>(EXT_IMAGE_COPY_CAPTURE_MANAGER_V1_OPTIONS_PAINT_CURSORS)) {
        wl_resource_post_error(resource,
            EXT_IMAGE_COPY_CAPTURE_MANAGER_V1_ERROR_INVALID_OPTION,
            "invalid option flag");
        return;
    }

    wl_resource *session = wl_resource_create(client, &ext_image_copy_capture_session_v1_interface,
                                              wl_resource_get_version(resource), id);
    if (!session)
        return;

    auto *session_data = new CaptureSessionData{};
    session_data->global = global;
    session_data->source = source;
    session_data->options = options;

    wl_resource_set_implementation(session, &session_impl, session_data, session_resource_destroyed);

    // Send buffer constraints
    send_constraints(session_data);

    WW_LOG_TRACE("[image-copy-capture] create_session (source={}, options={})",
                 static_cast<void *>(source), options);
}

static void manager_create_pointer_cursor_session(wl_client *client, wl_resource *resource,
                                                  uint32_t id, wl_resource *source, wl_resource *pointer) {
    auto *global = static_cast<ImageCopyCaptureManagerGlobal *>(wl_resource_get_user_data(resource));
    if (!global)
        return;

    wl_resource *cursor_session = wl_resource_create(client, &ext_image_copy_capture_cursor_session_v1_interface,
                                                     wl_resource_get_version(resource), id);
    if (!cursor_session)
        return;

    auto *data = new CursorCaptureSessionData{};
    data->global = global;
    data->source = source;
    data->pointer = pointer;

    wl_resource_set_implementation(cursor_session, &cursor_session_impl, data, cursor_session_resource_destroyed);

    WW_LOG_TRACE("[image-copy-capture] create_pointer_cursor_session (source={}, pointer={})",
                 static_cast<void *>(source), static_cast<void *>(pointer));
}

static void manager_destroy(wl_client *, wl_resource *resource) {
    wl_resource_destroy(resource);
}

static const struct ext_image_copy_capture_manager_v1_interface manager_impl = {
    .create_session = manager_create_session,
    .create_pointer_cursor_session = manager_create_pointer_cursor_session,
    .destroy = manager_destroy,
};

// Send buffer constraints to the client
static void send_constraints(CaptureSessionData *session) {
    if (session->constraints_sent)
        return;

    // In a real implementation, we would send:
    // - buffer_size (width, height)
    // - shm_format (e.g., WL_SHM_FORMAT_ARGB8888)
    // - dmabuf_format (e.g., DRM_FORMAT_ARGB8888 with modifiers)
    // - dmabuf_device
    // - done

    session->constraints_sent = true;
}

static void bind_image_copy_capture_manager(wl_client *client, void *data,
                                            uint32_t version, uint32_t id) {
    auto *global = static_cast<ImageCopyCaptureManagerGlobal *>(data);
    wl_resource *r = wl_resource_create(
        client, &ext_image_copy_capture_manager_v1_interface,
        static_cast<int>(version), id);
    wl_resource_set_implementation(r, &manager_impl, global, nullptr);
    global->managers.push_back(r);
}

} // anonymous namespace

Result<void *> register_image_copy_capture(wl_display *display, SurfaceManager & /*surface_manager*/) {
    auto *global = new ImageCopyCaptureManagerGlobal{};

    if (!wl_global_create(display, &ext_image_copy_capture_manager_v1_interface, 1,
                          global, bind_image_copy_capture_manager)) {
        delete global;
        return std::unexpected(Error::wayland_init(
            "Failed to create ext_image_copy_capture_manager_v1 global"));
    }

    global->display_destroy.notify = [](wl_listener *listener, void *) {
        ImageCopyCaptureManagerGlobal *g = wl_container_of(listener, g, display_destroy);
        for (wl_resource *r : g->managers) {
            wl_resource_set_user_data(r, nullptr);
        }
        delete g;
    };
    wl_display_add_destroy_listener(display, &global->display_destroy);

    WW_LOG_INFO("[wayland] ext_image_copy_capture_v1 registered");
    return global;
}

} // namespace ww::image_copy_capture