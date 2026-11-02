#include <wwindows/adapters/wayland/globals/foreign_toplevel_list.hpp>

#include <wwindows/adapters/wayland/protocols/foreign-toplevel-list-server-protocol.h>
#include <wwindows/core/surface_manager.hpp>
#include <wwindows/support/log.hpp>

#include <wayland-server-core.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ww::foreign_toplevel_list {

namespace {

// User data for the ext_foreign_toplevel_list_v1 global (manager).
class ForeignToplevelListGlobal {
public:
    std::vector<wl_resource *> lists;
    wl_listener display_destroy{};
};

// User data for each ext_foreign_toplevel_handle_v1 (per-toplevel object).
struct ToplevelHandleData {
    ForeignToplevelListGlobal *global{nullptr};
    wl_resource *surface{nullptr}; // The wl_surface this handle represents
    std::string title;
    std::string app_id;
    std::string identifier;
    bool closed = false;
    bool sent_initial = false;
};

// User data for each ext_foreign_toplevel_list_v1 (per-client list).
struct ListData {
    ForeignToplevelListGlobal *global{nullptr};
    std::vector<wl_resource *> handles;
    bool stopped = false;
};

// Resource destroy callback for ext_foreign_toplevel_handle_v1
static void toplevel_handle_resource_destroyed(wl_resource *resource) {
    auto *data = static_cast<ToplevelHandleData *>(wl_resource_get_user_data(resource));
    if (!data)
        return;

    if (data->global) {
        // Remove from all lists
        for (wl_resource *list_res : data->global->lists) {
            auto *list_data = static_cast<ListData *>(wl_resource_get_user_data(list_res));
            if (list_data) {
                std::erase(list_data->handles, resource);
            }
        }
    }
    delete data;
}

// ext_foreign_toplevel_handle_v1 interface implementation
static void toplevel_handle_destroy(wl_client *, wl_resource *resource) {
    wl_resource_destroy(resource);
}

static const struct ext_foreign_toplevel_handle_v1_interface toplevel_handle_impl = {
    .destroy = toplevel_handle_destroy,
};

// Send initial properties for a toplevel handle
static void send_toplevel_initial_properties(wl_resource *handle) {
    auto *data = static_cast<ToplevelHandleData *>(wl_resource_get_user_data(handle));
    if (!data || data->sent_initial)
        return;

    if (!data->identifier.empty()) {
        ext_foreign_toplevel_handle_v1_send_identifier(handle, data->identifier.c_str());
    }
    if (!data->title.empty()) {
        ext_foreign_toplevel_handle_v1_send_title(handle, data->title.c_str());
    }
    if (!data->app_id.empty()) {
        ext_foreign_toplevel_handle_v1_send_app_id(handle, data->app_id.c_str());
    }
    ext_foreign_toplevel_handle_v1_send_done(handle);
    data->sent_initial = true;
}

// Resource destroy callback for ext_foreign_toplevel_list_v1
static void list_resource_destroyed(wl_resource *resource) {
    auto *data = static_cast<ListData *>(wl_resource_get_user_data(resource));
    if (!data)
        return;

    if (data->global) {
        std::erase(data->global->lists, resource);
    }
    delete data;
}

// ext_foreign_toplevel_list_v1 interface implementation
// Order must match generated protocol: stop, destroy
static void list_stop(wl_client *, wl_resource *resource) {
    auto *data = static_cast<ListData *>(wl_resource_get_user_data(resource));
    if (!data)
        return;

    data->stopped = true;
    ext_foreign_toplevel_list_v1_send_finished(resource);
}

static void list_destroy(wl_client *, wl_resource *resource) {
    wl_resource_destroy(resource);
}

static const struct ext_foreign_toplevel_list_v1_interface list_impl = {
    .stop = list_stop,
    .destroy = list_destroy,
};

static void bind_foreign_toplevel_list(wl_client *client, void *data,
                                       uint32_t version, uint32_t id) {
    auto *global = static_cast<ForeignToplevelListGlobal *>(data);
    wl_resource *r = wl_resource_create(
        client, &ext_foreign_toplevel_list_v1_interface,
        static_cast<int>(version), id);
    wl_resource_set_implementation(r, &list_impl, data, list_resource_destroyed);

    auto *list_data = new ListData{};
    list_data->global = global;
    global->lists.push_back(r);
}

// Create a toplevel handle for a surface
wl_resource *create_toplevel_handle_internal(ForeignToplevelListGlobal *global, wl_client *client, uint32_t version, wl_resource *surface, const std::string &identifier) {
    wl_resource *handle = wl_resource_create(client, &ext_foreign_toplevel_handle_v1_interface, static_cast<int>(version), 0);
    if (!handle)
        return nullptr;

    auto *data = new ToplevelHandleData{};
    data->global = global;
    data->surface = surface;
    data->identifier = identifier;
    data->title = "";
    data->app_id = "";
    data->closed = false;
    data->sent_initial = false;

    wl_resource_set_implementation(handle, &toplevel_handle_impl, data, toplevel_handle_resource_destroyed);

    // Add to all active lists
    for (wl_resource *list_res : global->lists) {
        auto *list_data = static_cast<ListData *>(wl_resource_get_user_data(list_res));
        if (list_data && !list_data->stopped) {
            list_data->handles.push_back(handle);
            ext_foreign_toplevel_list_v1_send_toplevel(list_res, handle);
            send_toplevel_initial_properties(handle);
        }
    }

    return handle;
}

// Close a toplevel handle (when surface is destroyed/unmapped)
void close_toplevel_handle_internal(ForeignToplevelListGlobal *global, wl_resource *surface) {
    // Find handles for this surface
    for (wl_resource *list_res : global->lists) {
        auto *list_data = static_cast<ListData *>(wl_resource_get_user_data(list_res));
        if (!list_data) continue;

        for (wl_resource *handle : list_data->handles) {
            auto *handle_data = static_cast<ToplevelHandleData *>(wl_resource_get_user_data(handle));
            if (handle_data && handle_data->surface == surface && !handle_data->closed) {
                handle_data->closed = true;
                ext_foreign_toplevel_handle_v1_send_closed(handle);
                // Don't destroy the handle resource - client must do that
            }
        }
    }
}

} // anonymous namespace

Result<void *> register_foreign_toplevel_list(wl_display *display, SurfaceManager & /*surface_manager*/) {
    auto *global = new ForeignToplevelListGlobal{};

    if (!wl_global_create(display, &ext_foreign_toplevel_list_v1_interface, 1,
                          global, bind_foreign_toplevel_list)) {
        delete global;
        return std::unexpected(Error::wayland_init(
            "Failed to create ext_foreign_toplevel_list_v1 global"));
    }

    global->display_destroy.notify = [](wl_listener *listener, void *) {
        ForeignToplevelListGlobal *g = wl_container_of(listener, g, display_destroy);
        for (wl_resource *r : g->lists) {
            auto *list_data = static_cast<ListData *>(wl_resource_get_user_data(r));
            if (list_data) {
                list_data->global = nullptr;
            }
            wl_resource_set_user_data(r, nullptr);
        }
        delete g;
    };
    wl_display_add_destroy_listener(display, &global->display_destroy);

    WW_LOG_INFO("[wayland] ext_foreign_toplevel_list_v1 registered");
    return global;
}

// Exported functions for SurfaceManager to call
extern "C" {
wl_resource *foreign_toplevel_list_create_handle(void *global_ptr, wl_client *client, uint32_t version, wl_resource *surface, const char *identifier) {
    auto *global = static_cast<ForeignToplevelListGlobal *>(global_ptr);
    return create_toplevel_handle_internal(global, client, version, surface, identifier);
}

void foreign_toplevel_list_close_handle(void *global_ptr, wl_resource *surface) {
    auto *global = static_cast<ForeignToplevelListGlobal *>(global_ptr);
    close_toplevel_handle_internal(global, surface);
}

void foreign_toplevel_list_set_title(void *global_ptr, wl_resource *surface, const char *title) {
    auto *global = static_cast<ForeignToplevelListGlobal *>(global_ptr);
    for (wl_resource *list_res : global->lists) {
        auto *list_data = static_cast<ListData *>(wl_resource_get_user_data(list_res));
        if (!list_data) continue;
        for (wl_resource *handle : list_data->handles) {
            auto *handle_data = static_cast<ToplevelHandleData *>(wl_resource_get_user_data(handle));
            if (handle_data && handle_data->surface == surface) {
                handle_data->title = title;
                ext_foreign_toplevel_handle_v1_send_title(handle, title);
                ext_foreign_toplevel_handle_v1_send_done(handle);
            }
        }
    }
}

void foreign_toplevel_list_set_app_id(void *global_ptr, wl_resource *surface, const char *app_id) {
    auto *global = static_cast<ForeignToplevelListGlobal *>(global_ptr);
    for (wl_resource *list_res : global->lists) {
        auto *list_data = static_cast<ListData *>(wl_resource_get_user_data(list_res));
        if (!list_data) continue;
        for (wl_resource *handle : list_data->handles) {
            auto *handle_data = static_cast<ToplevelHandleData *>(wl_resource_get_user_data(handle));
            if (handle_data && handle_data->surface == surface) {
                handle_data->app_id = app_id;
                ext_foreign_toplevel_handle_v1_send_app_id(handle, app_id);
                ext_foreign_toplevel_handle_v1_send_done(handle);
            }
        }
    }
}

} // extern "C"

} // namespace ww::foreign_toplevel_list