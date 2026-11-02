#include <wwindows/adapters/wayland/globals/image_capture_source.hpp>

#include <wwindows/adapters/wayland/protocols/image-capture-source-server-protocol.h>
#include <wwindows/adapters/wayland/globals/foreign_toplevel_list.hpp>
#include <wwindows/core/surface_manager.hpp>
#include <wwindows/support/log.hpp>

#include <wayland-server-core.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ww::image_capture_source {

namespace {

// User data for the ext_image_capture_source_manager_v1 global.
class ImageCaptureSourceManagerGlobal {
public:
    std::vector<wl_resource *> managers;
    wl_listener display_destroy{};
};

// User data for each ext_image_capture_source_v1 (per-source object).
struct CaptureSourceData {
    ImageCaptureSourceManagerGlobal *global{nullptr};
    enum class Type { Output, Toplevel } type;
    wl_resource *output = nullptr;            // for output type
    wl_resource *toplevel_handle = nullptr;   // for toplevel type
    bool destroyed = false;
};

// Resource destroy callback for ext_image_capture_source_v1
static void capture_source_resource_destroyed(wl_resource *resource) {
    auto *data = static_cast<CaptureSourceData *>(wl_resource_get_user_data(resource));
    if (!data)
        return;

    if (data->global) {
        // Could remove from a list if we tracked them
    }
    delete data;
}

// ext_image_capture_source_v1 interface implementation
static void capture_source_destroy(wl_client *, wl_resource *resource) {
    wl_resource_destroy(resource);
}

static const struct ext_image_capture_source_v1_interface capture_source_impl = {
    .destroy = capture_source_destroy,
};

// ext_image_capture_source_manager_v1 interface implementation
// Order must match generated protocol: create_from_output, create_from_toplevel, destroy
static void manager_create_from_output(wl_client *client, wl_resource *resource,
                                       uint32_t id, wl_resource *output) {
    auto *global = static_cast<ImageCaptureSourceManagerGlobal *>(wl_resource_get_user_data(resource));
    if (!global)
        return;

    wl_resource *source = wl_resource_create(client, &ext_image_capture_source_v1_interface,
                                             wl_resource_get_version(resource), id);
    if (!source)
        return;

    auto *data = new CaptureSourceData{};
    data->global = global;
    data->type = CaptureSourceData::Type::Output;
    data->output = output;

    wl_resource_set_implementation(source, &capture_source_impl, data, capture_source_resource_destroyed);

    WW_LOG_TRACE("[image-capture-source] create_from_output (output={})", static_cast<void *>(output));
}

static void manager_create_from_toplevel(wl_client *client, wl_resource *resource,
                                         uint32_t id, wl_resource *toplevel_handle) {
    auto *global = static_cast<ImageCaptureSourceManagerGlobal *>(wl_resource_get_user_data(resource));
    if (!global)
        return;

    wl_resource *source = wl_resource_create(client, &ext_image_capture_source_v1_interface,
                                             wl_resource_get_version(resource), id);
    if (!source)
        return;

    auto *data = new CaptureSourceData{};
    data->global = global;
    data->type = CaptureSourceData::Type::Toplevel;
    data->toplevel_handle = toplevel_handle;

    wl_resource_set_implementation(source, &capture_source_impl, data, capture_source_resource_destroyed);

    WW_LOG_TRACE("[image-capture-source] create_from_toplevel (toplevel_handle={})", static_cast<void *>(toplevel_handle));
}

static void manager_destroy(wl_client *, wl_resource *resource) {
    wl_resource_destroy(resource);
}

static const struct ext_image_capture_source_manager_v1_interface manager_impl = {
    .create_from_output = manager_create_from_output,
    .create_from_toplevel = manager_create_from_toplevel,
    .destroy = manager_destroy,
};

static void bind_image_capture_source_manager(wl_client *client, void *data,
                                              uint32_t version, uint32_t id) {
    auto *global = static_cast<ImageCaptureSourceManagerGlobal *>(data);
    wl_resource *r = wl_resource_create(
        client, &ext_image_capture_source_manager_v1_interface,
        static_cast<int>(version), id);
    wl_resource_set_implementation(r, &manager_impl, global, nullptr);
    global->managers.push_back(r);
}

} // anonymous namespace

Result<void *> register_image_capture_source(wl_display *display, SurfaceManager & /*surface_manager*/) {
    auto *global = new ImageCaptureSourceManagerGlobal{};

    if (!wl_global_create(display, &ext_image_capture_source_manager_v1_interface, 1,
                          global, bind_image_capture_source_manager)) {
        delete global;
        return std::unexpected(Error::wayland_init(
            "Failed to create ext_image_capture_source_manager_v1 global"));
    }

    global->display_destroy.notify = [](wl_listener *listener, void *) {
        ImageCaptureSourceManagerGlobal *g = wl_container_of(listener, g, display_destroy);
        for (wl_resource *r : g->managers) {
            wl_resource_set_user_data(r, nullptr);
        }
        delete g;
    };
    wl_display_add_destroy_listener(display, &global->display_destroy);

    WW_LOG_INFO("[wayland] ext_image_capture_source_v1 registered");
    return global;
}

} // namespace ww::image_capture_source