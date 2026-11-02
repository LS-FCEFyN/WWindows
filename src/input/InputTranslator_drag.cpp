#include "wwindows/input/detail/input_translator_impl.hpp"
#include <wwindows/support/log.hpp>

namespace ww {

void InputTranslator::begin_drag(DragCallbacks cbs) {
  auto &im = *impl_;
  im.drag_cbs = std::move(cbs);
  im.drag_active = true;
  im.drag_target = nullptr;

  // X11 won't refire EnterNotify for us — the pointer hasn't crossed a
  // window boundary since the drag began — so synthesize the first target
  // from wherever it already sits.
  //
  // Tracking the target is unconditional: it drives on_motion and on_leave
  // dispatch too. Setting it only when an on_enter callback happened to be
  // supplied left a caller that asked for motion but not enter with a null
  // target, and therefore with no motion or leave events at all.
  if (im.ptr_focus) {
    im.drag_target = im.ptr_focus;
    if (im.drag_cbs.on_enter)
      im.drag_cbs.on_enter(im.ptr_focus, im.last_local_x, im.last_local_y);
  }
  WW_LOG_DEBUG("[input] drag started (initial target={})",
              static_cast<void *>(im.drag_target));
}

void InputTranslator::end_drag() {
  auto &im = *impl_;
  if (!im.drag_active)
    return;
  WW_LOG_DEBUG("[input] drag ended");
  im.drag_active = false;
  im.drag_target = nullptr;
  im.drag_cbs = {};
}

void InputTranslator::set_keyboard_focus_changed_callback(
    KeyboardFocusChangedCallback cb) {
  impl_->kbd_focus_changed_cb = std::move(cb);
}

} // namespace ww
