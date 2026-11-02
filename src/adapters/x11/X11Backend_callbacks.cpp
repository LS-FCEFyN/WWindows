#include "wwindows/adapters/x11/detail/x11_backend_impl.hpp"

namespace ww {

void X11Backend::set_resize_callback(ResizeCallback cb) {
  impl_->on_resize = std::move(cb);
}

void X11Backend::set_wm_state_callback(WmStateCallback cb) {
  impl_->on_wm_state = std::move(cb);
}

void X11Backend::set_input_callbacks(InputCallbacks cbs) {
  impl_->input.input_cbs = std::move(cbs);
}

void X11Backend::set_events_processed_callback(std::function<void()> cb) {
  impl_->events_processed_cb = std::move(cb);
}

void X11Backend::set_close_callback(std::function<void(uint32_t)> cb) {
  impl_->close_cb = std::move(cb);
}

} // namespace ww
