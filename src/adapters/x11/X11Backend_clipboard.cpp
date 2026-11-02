// wl_data_device selection (clipboard) <-> X11 CLIPBOARD interop.
//
// WWindows plays both roles ICCCM defines:
//   - Owner: a Wayland client called wl_data_device.set_selection. We take
//     CLIPBOARD ownership on a hidden utility window (selection_window) and
//     answer SelectionRequest from native X11 apps by pulling data out of
//     the owning wl_data_source through a pipe (ClipboardSendCallback).
//   - Requestor: a native X11 app owns CLIPBOARD and a Wayland client wants
//     to paste. We ConvertSelection from selection_window and hand the
//     result back through ClipboardDataCallback.
//
// MIME types are represented by same-named X atoms when bridging arbitrary
// payloads. Legacy text atoms (UTF8_STRING and STRING) are mapped to the
// preferred offered text MIME for compatibility. INCR is used for payloads
// larger than a single X11 property; MULTIPLE supports batched target requests.

#include "wwindows/adapters/x11/detail/x11_backend_impl.hpp"
#include "wwindows/core/event_loop.hpp"
#include <wwindows/support/log.hpp>
#include <wwindows/adapters/x11/policy/clipboard_mime.hpp>

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstdlib>
#include <string>
#include <utility>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <xcb/xcbext.h> // xcb_poll_for_reply
#include <xcb/xfixes.h>

namespace ww {

namespace {

uint64_t incr_key(xcb_window_t requestor, xcb_atom_t property) {
  return (static_cast<uint64_t>(requestor) << 32) | property;
}

void send_selection_notify(xcb_connection_t *conn, xcb_window_t requestor,
                           xcb_timestamp_t time, xcb_atom_t selection,
                           xcb_atom_t target, xcb_atom_t property) {
  xcb_selection_notify_event_t note{};
  note.response_type = XCB_SELECTION_NOTIFY;
  note.time = time;
  note.requestor = requestor;
  note.selection = selection;
  note.target = target;
  note.property = property;
  xcb_send_event(conn, 0, requestor, XCB_EVENT_MASK_NO_EVENT,
                 reinterpret_cast<const char *>(&note));
}

xcb_atom_t intern_atom(xcb_connection_t *conn, const std::string &name) {
  xcb_intern_atom_cookie_t c = xcb_intern_atom(
      conn, 0, static_cast<uint16_t>(name.size()), name.c_str());
  xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(conn, c, nullptr);
  const xcb_atom_t atom = r ? r->atom : static_cast<xcb_atom_t>(XCB_ATOM_NONE);
  ::free(r);
  return atom;
}

} // namespace

void X11Backend::Impl::intern_clipboard_atoms(Impl &im) {
  im.clipboard.atom_clipboard = intern_atom(im.conn, "CLIPBOARD");
  im.clipboard.atom_targets = intern_atom(im.conn, "TARGETS");
  im.clipboard.atom_multiple = intern_atom(im.conn, "MULTIPLE");
  im.clipboard.atom_incr = intern_atom(im.conn, "INCR");
  im.clipboard.atom_atom_pair = intern_atom(im.conn, "ATOM_PAIR");
  im.clipboard.atom_utf8_string = intern_atom(im.conn, "UTF8_STRING");
  im.clipboard.atom_text_plain_utf8 = intern_atom(im.conn, "text/plain;charset=utf-8");
  im.clipboard.atom_ww_selection = intern_atom(im.conn, "WWINDOWS_SELECTION");
}

// -----------------------------------------------------------------------
// Owner side: we hold CLIPBOARD, answer SelectionRequest from X11 apps.
// -----------------------------------------------------------------------

void X11Backend::set_clipboard_owner(std::vector<std::string> mime_types) {
  auto &im = *impl_;
  im.clipboard.owned_mime_types = std::move(mime_types);
  im.clipboard.outgoing_atom_to_mime.clear();

  if (im.clipboard.owned_mime_types.empty()) {
    xcb_set_selection_owner(im.conn, XCB_NONE, im.clipboard.atom_clipboard,
                            XCB_CURRENT_TIME);
    xcb_flush(im.conn);
    WW_LOG_DEBUG("[x11-clipboard] released CLIPBOARD ownership");
    return;
  }

  for (const auto &mime : im.clipboard.owned_mime_types)
    im.clipboard.outgoing_atom_to_mime[intern_atom(im.conn, mime)] = mime;

  // Which of the offered types is "the" text payload is a pure decision,
  // so it lives in x11::policy and is unit-tested there rather than being
  // re-derived by hand in the middle of this atom bookkeeping.
  const std::string text_mime{x11::policy::preferred_text_mime(im.clipboard.owned_mime_types)};
  // Legacy aliases (UTF8_STRING/STRING/text-plain-as-atom) so X11
  // applications that only ever ask for those specific atoms — most of
  // them — still get an answer, pointing at whichever offered mime type is
  // the actual text payload.
  if (!text_mime.empty()) {
    im.clipboard.outgoing_atom_to_mime[im.clipboard.atom_utf8_string] = text_mime;
    im.clipboard.outgoing_atom_to_mime[static_cast<xcb_atom_t>(XCB_ATOM_STRING)] =
        text_mime;
    im.clipboard.outgoing_atom_to_mime[im.clipboard.atom_text_plain_utf8] = text_mime;
  }

  xcb_set_selection_owner(im.conn, im.clipboard.selection_window, im.clipboard.atom_clipboard,
                          XCB_CURRENT_TIME);
  xcb_flush(im.conn);
  WW_LOG_DEBUG("[x11-clipboard] took CLIPBOARD ownership ({} mime type(s))",
              im.clipboard.owned_mime_types.size());
}

void X11Backend::set_clipboard_send_callback(ClipboardSendCallback cb) {
  impl_->clipboard.clipboard_send_cb = std::move(cb);
}

void X11Backend::Impl::finish_clipboard_send(Impl &im, int pipe_rfd,
                                             bool success) {
  auto it = im.clipboard.pending_sends.find(pipe_rfd);
  if (it == im.clipboard.pending_sends.end())
    return;
  PendingClipboardSend pending = std::move(it->second);
  im.clipboard.pending_sends.erase(it);
  if (im.loop)
    im.loop->remove(pipe_rfd);
  ::close(pipe_rfd);

  xcb_atom_t reply_property = static_cast<xcb_atom_t>(XCB_ATOM_NONE);
  if (success) {
    reply_property = pending.property;
    if (pending.data.size() > kClipboardIncrChunkBytes) {
      // ICCCM INCR handshake: publish the total byte count, then wait for
      // the requestor to delete the property before sending each chunk.
      const uint32_t total = static_cast<uint32_t>(pending.data.size());
      // STRUCTURE_NOTIFY lets us see the requestor die mid-transfer
      // (dispatch_destroy_notify) instead of holding the payload forever.
      const uint32_t event_mask =
          XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_STRUCTURE_NOTIFY;
      xcb_change_window_attributes(im.conn, pending.requestor,
                                   XCB_CW_EVENT_MASK, &event_mask);
      xcb_change_property(im.conn, XCB_PROP_MODE_REPLACE, pending.requestor,
                          pending.property, im.clipboard.atom_incr, 32, 1, &total);
      im.clipboard.pending_incr_sends[incr_key(pending.requestor, pending.property)] =
          PendingClipboardIncrSend{pending.requestor, pending.property,
                                   pending.target, std::move(pending.data), 0};
      WW_LOG_DEBUG("[x11-clipboard] starting INCR send ({} bytes, requestor={}, property={})",
                   total, pending.requestor, pending.property);
    } else {
      xcb_change_property(im.conn, XCB_PROP_MODE_REPLACE, pending.requestor,
                          pending.property, pending.target, 8,
                          static_cast<uint32_t>(pending.data.size()),
                          pending.data.data());
    }
  }

  if (pending.multiple_id) {
    auto group = im.clipboard.pending_multiples.find(pending.multiple_id);
    if (group != im.clipboard.pending_multiples.end()) {
      if (!success) {
        const std::size_t property_index = pending.multiple_pair_index * 2u + 1u;
        if (property_index < group->second.pairs.size())
          group->second.pairs[property_index] = XCB_ATOM_NONE;
      }
      if (group->second.pending > 0)
        --group->second.pending;
      if (group->second.pending == 0)
        finish_clipboard_multiple(im, pending.multiple_id);
    }
  } else {
    send_selection_notify(im.conn, pending.requestor, XCB_CURRENT_TIME,
                          im.clipboard.atom_clipboard, pending.target, reply_property);
  }
  xcb_flush(im.conn);
  WW_LOG_DEBUG(
      "[x11-clipboard] answered SelectionRequest ({} byte(s), success={})",
      pending.data.size(), success);
}

bool X11Backend::Impl::start_clipboard_send(Impl &im,
                                             xcb_window_t requestor,
                                             xcb_atom_t property,
                                             xcb_atom_t target,
                                             uint64_t multiple_id,
                                             std::size_t pair_index) {
  const auto mime_it = im.clipboard.outgoing_atom_to_mime.find(target);
  if (mime_it == im.clipboard.outgoing_atom_to_mime.end() || !im.clipboard.clipboard_send_cb ||
      !im.loop)
    return false;

  int pipefd[2];
  if (::pipe(pipefd) != 0) {
    WW_LOG_WARN("[x11-clipboard] pipe() failed: {}", std::strerror(errno));
    return false;
  }
  const int flags = ::fcntl(pipefd[0], F_GETFL, 0);
  if (flags >= 0)
    (void)::fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);

  im.clipboard.pending_sends[pipefd[0]] = PendingClipboardSend{
      requestor, property, target, {}, multiple_id, pair_index};
  if (multiple_id) {
    auto group = im.clipboard.pending_multiples.find(multiple_id);
    if (group != im.clipboard.pending_multiples.end())
      ++group->second.pending;
  }
  im.clipboard.clipboard_send_cb(mime_it->second, pipefd[1]); // callee writes + closes

  Impl *self = &im;
  const int rfd = pipefd[0];
  {
    const auto added = im.loop->add(rfd, IoIn | IoHup, [self, rfd](uint32_t /*events*/) {
      uint8_t buf[4096];
      for (;;) {
        const ssize_t n = ::read(rfd, buf, sizeof(buf));
        if (n > 0) {
          auto pit = self->clipboard.pending_sends.find(rfd);
          if (pit != self->clipboard.pending_sends.end()) {
            if (pit->second.data.size() + static_cast<size_t>(n) >
                kClipboardMaxBytes) {
              WW_LOG_WARN("[x11-clipboard] selection exceeds {} bytes — "
                          "aborting transfer", kClipboardMaxBytes);
              Impl::finish_clipboard_send(*self, rfd, false);
              return;
            }
            pit->second.data.insert(pit->second.data.end(), buf, buf + n);
          }
          continue;
        }
        if (n == 0) {
          Impl::finish_clipboard_send(*self, rfd, true);
          return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK)
          return;
        Impl::finish_clipboard_send(*self, rfd, false);
        return;
      }
    });
    if (!added) {
      // Without a watcher the pipe would never be drained: reply "refused"
      // and release the fd and the pending entry instead of leaking them.
      WW_LOG_WARN("[x11-clipboard] could not watch clipboard pipe; refusing "
                  "the request");
      Impl::finish_clipboard_send(*self, rfd, false);
    }
  }
  return true;
}

void X11Backend::Impl::finish_clipboard_multiple(Impl &im,
                                                 uint64_t multiple_id) {
  auto it = im.clipboard.pending_multiples.find(multiple_id);
  if (it == im.clipboard.pending_multiples.end())
    return;
  PendingClipboardMultiple group = std::move(it->second);
  im.clipboard.pending_multiples.erase(it);
  xcb_change_property(im.conn, XCB_PROP_MODE_REPLACE, group.requestor,
                      group.property, im.clipboard.atom_atom_pair, 32,
                      static_cast<uint32_t>(group.pairs.size()),
                      group.pairs.data());
  send_selection_notify(im.conn, group.requestor, group.time, im.clipboard.atom_clipboard,
                        im.clipboard.atom_multiple, group.property);
  xcb_flush(im.conn);
}

void X11Backend::Impl::poll_pending_clipboard_multiple_reads(Impl &im) {
  for (std::size_t i = 0; i < im.clipboard.pending_multiple_reads.size();) {
    auto &read = im.clipboard.pending_multiple_reads[i];
    void *reply_ptr = nullptr;
    xcb_generic_error_t *error_ptr = nullptr;
    if (!xcb_poll_for_reply(im.conn, read.sequence, &reply_ptr, &error_ptr)) {
      ++i;
      continue;
    }
    PendingClipboardMultipleRead completed = read;
    im.clipboard.pending_multiple_reads[i] = im.clipboard.pending_multiple_reads.back();
    im.clipboard.pending_multiple_reads.pop_back();
    auto *reply = static_cast<xcb_get_property_reply_t *>(reply_ptr);
    std::vector<xcb_atom_t> pairs;
    bool valid = reply && !error_ptr && reply->format == 32 &&
                 reply->type == im.clipboard.atom_atom_pair;
    if (valid) {
      const int bytes = xcb_get_property_value_length(reply);
      const auto *values = static_cast<const xcb_atom_t *>(
          xcb_get_property_value(reply));
      valid = bytes >= 0 && (bytes % 8) == 0 && bytes <= 4096;
      if (valid && bytes > 0)
        pairs.assign(values, values + static_cast<std::size_t>(bytes / 4));
    }
    ::free(reply);
    ::free(error_ptr);
    if (!valid || pairs.empty()) {
      send_selection_notify(im.conn, completed.requestor, completed.time,
                            im.clipboard.atom_clipboard, im.clipboard.atom_multiple, XCB_ATOM_NONE);
      continue;
    }

    uint64_t id = im.clipboard.next_multiple_id++;
    if (id == 0)
      id = im.clipboard.next_multiple_id++;
    im.clipboard.pending_multiples.emplace(
        id, PendingClipboardMultiple{completed.requestor, completed.property,
                                     completed.time, std::move(pairs), 0});
    auto group_it = im.clipboard.pending_multiples.find(id);
    auto &group = group_it->second;
    for (std::size_t pair_index = 0; pair_index + 1 < group.pairs.size();
         pair_index += 2) {
      const xcb_atom_t target = group.pairs[pair_index];
      const xcb_atom_t property = group.pairs[pair_index + 1];
      if (property == XCB_ATOM_NONE || property == group.property ||
          target == im.clipboard.atom_multiple || target == XCB_ATOM_NONE) {
        group.pairs[pair_index + 1] = XCB_ATOM_NONE;
        continue;
      }
      if (target == im.clipboard.atom_targets) {
        std::vector<xcb_atom_t> atoms{im.clipboard.atom_targets, im.clipboard.atom_multiple};
        atoms.reserve(im.clipboard.outgoing_atom_to_mime.size() + 2);
        for (const auto &[atom, mime] : im.clipboard.outgoing_atom_to_mime) {
          (void)mime;
          atoms.push_back(atom);
        }
        xcb_change_property(im.conn, XCB_PROP_MODE_REPLACE, completed.requestor,
                            property, XCB_ATOM_ATOM, 32,
                            static_cast<uint32_t>(atoms.size()), atoms.data());
        continue;
      }
      if (!start_clipboard_send(im, completed.requestor, property, target, id,
                                pair_index / 2u))
        group.pairs[pair_index + 1] = XCB_ATOM_NONE;
    }
    if (group.pairs.size() % 2 != 0)
      group.pairs.pop_back();
    if (group.pending == 0)
      finish_clipboard_multiple(im, id);
  }
}

void X11Backend::Impl::dispatch_selection_request(Impl &im,
                                                   xcb_generic_event_t *ev) {
  auto *e = reinterpret_cast<xcb_selection_request_event_t *>(ev);
  if (e->selection != im.clipboard.atom_clipboard || e->owner != im.clipboard.selection_window)
    return;

  const xcb_window_t requestor = e->requestor;
  const xcb_atom_t target = e->target;
  const xcb_atom_t property =
      (e->property != static_cast<xcb_atom_t>(XCB_ATOM_NONE)) ? e->property
                                                               : e->target;
  const xcb_timestamp_t time = e->time;

  auto refuse = [&]() {
    xcb_selection_notify_event_t note{};
    note.response_type = XCB_SELECTION_NOTIFY;
    note.time = time;
    note.requestor = requestor;
    note.selection = im.clipboard.atom_clipboard;
    note.target = target;
    note.property = static_cast<xcb_atom_t>(XCB_ATOM_NONE);
    xcb_send_event(im.conn, 0, requestor, XCB_EVENT_MASK_NO_EVENT,
                   reinterpret_cast<const char *>(&note));
    xcb_flush(im.conn);
  };

  if (target == im.clipboard.atom_multiple) {
    if (e->property == XCB_ATOM_NONE) {
      refuse();
      return;
    }
    const auto cookie = xcb_get_property(im.conn, 0, requestor, property,
                                         im.clipboard.atom_atom_pair, 0, 1024);
    im.clipboard.pending_multiple_reads.push_back(
        PendingClipboardMultipleRead{cookie.sequence, requestor, property, time});
    xcb_flush(im.conn);
    return;
  }

  if (target == im.clipboard.atom_targets) {
    std::vector<xcb_atom_t> atoms{im.clipboard.atom_targets, im.clipboard.atom_multiple};
    atoms.reserve(im.clipboard.outgoing_atom_to_mime.size() + 2);
    for (const auto &[atom, mime] : im.clipboard.outgoing_atom_to_mime) {
      (void)mime;
      atoms.push_back(atom);
    }
    xcb_change_property(im.conn, XCB_PROP_MODE_REPLACE, requestor, property,
                        static_cast<xcb_atom_t>(XCB_ATOM_ATOM), 32,
                        static_cast<uint32_t>(atoms.size()), atoms.data());
    xcb_selection_notify_event_t note{};
    note.response_type = XCB_SELECTION_NOTIFY;
    note.time = time;
    note.requestor = requestor;
    note.selection = im.clipboard.atom_clipboard;
    note.target = target;
    note.property = property;
    xcb_send_event(im.conn, 0, requestor, XCB_EVENT_MASK_NO_EVENT,
                   reinterpret_cast<const char *>(&note));
    xcb_flush(im.conn);
    return;
  }

  if (!start_clipboard_send(im, requestor, property, target))
    refuse();
}

void X11Backend::Impl::dispatch_clipboard_property_notify(
    Impl &im, xcb_property_notify_event_t *event) {
  if (event->state == XCB_PROPERTY_DELETE) {
    const uint64_t key = incr_key(event->window, event->atom);
    auto it = im.clipboard.pending_incr_sends.find(key);
    if (it != im.clipboard.pending_incr_sends.end()) {
      auto &transfer = it->second;
      if (transfer.offset < transfer.data.size()) {
        const std::size_t remaining = transfer.data.size() - transfer.offset;
        const uint32_t chunk = static_cast<uint32_t>(
            std::min<std::size_t>(remaining, kClipboardIncrChunkBytes));
        xcb_change_property(im.conn, XCB_PROP_MODE_REPLACE,
                            transfer.requestor, transfer.property,
                            transfer.target, 8, chunk,
                            transfer.data.data() + transfer.offset);
        transfer.offset += chunk;
        transfer.last_activity = std::chrono::steady_clock::now();
      } else {
        // A zero-length property terminates the INCR transfer.
        xcb_change_property(im.conn, XCB_PROP_MODE_REPLACE,
                            transfer.requestor, transfer.property,
                            transfer.target, 8, 0, nullptr);
        im.clipboard.pending_incr_sends.erase(it);
        // Keep PropertyChange selected on this requestor for the lifetime of
        // this X connection. Another concurrent INCR transfer to the same
        // requestor may still need notifications; selecting an already
        // selected event is harmless and avoids clobbering that transfer.
      }
      xcb_flush(im.conn);
      return;
    }
  }

  if (event->window == im.clipboard.selection_window &&
      event->state == XCB_PROPERTY_NEW_VALUE && im.clipboard.pending_incr_receive &&
      event->atom == im.clipboard.pending_incr_receive->property) {
    if (im.clipboard.pending_property_read) {
      // The owner can answer our delete and publish the next chunk before
      // poll_pending_property_read() consumes the previous GetProperty reply.
      // Remember that notification rather than losing the next chunk.
      im.clipboard.pending_incr_chunk_notify = true;
      return;
    }
    start_incr_receive_chunk_read(im);
  }
}

void X11Backend::Impl::start_incr_receive_chunk_read(Impl &im) {
  if (!im.clipboard.pending_incr_receive || im.clipboard.pending_property_read)
    return;
  im.clipboard.pending_incr_chunk_notify = false;
  const xcb_atom_t property = im.clipboard.pending_incr_receive->property;
  const auto cookie = xcb_get_property(
      im.conn, 1, im.clipboard.selection_window, property,
      static_cast<xcb_atom_t>(XCB_GET_PROPERTY_TYPE_ANY), 0,
      kClipboardMaxBytes / 4);
  im.clipboard.pending_property_read = PendingPropertyRead{
      cookie.sequence, property, PendingClipboardReceive{}, true};
  xcb_flush(im.conn);
}

// True if, as of the X server's current state, our own window owns CLIPBOARD.
// SelectionClear and XFixes SelectionNotify are queued notifications about
// *past* ownership transitions. When a client releases and immediately
// re-acquires the selection (wl_data_source.destroy of the current source,
// then wl_data_device.set_selection), the stale "owner became None" /
// "we lost it" events arrive after we already own it again. Treating them as
// an external takeover cancels the new Wayland source.
static bool we_own_clipboard_now(xcb_connection_t *conn,
                                 xcb_window_t selection_window,
                                 xcb_atom_t clipboard) {
  xcb_get_selection_owner_reply_t *r = xcb_get_selection_owner_reply(
      conn, xcb_get_selection_owner(conn, clipboard), nullptr);
  const bool ours = r && r->owner == selection_window;
  free(r);
  return ours;
}

void X11Backend::Impl::dispatch_selection_clear(Impl &im,
                                                 xcb_generic_event_t *ev) {
  auto *e = reinterpret_cast<xcb_selection_clear_event_t *>(ev);
  if (e->selection != im.clipboard.atom_clipboard)
    return;
  if (we_own_clipboard_now(im.conn, im.clipboard.selection_window, im.clipboard.atom_clipboard)) {
    WW_LOG_DEBUG("[x11-clipboard] ignoring stale SelectionClear "
                 "(we own CLIPBOARD again)");
    return;
  }
  WW_LOG_DEBUG("[x11-clipboard] lost CLIPBOARD ownership");
  im.clipboard.owned_mime_types.clear();
  im.clipboard.outgoing_atom_to_mime.clear();
  // Whoever just took it over is now the owner — refresh so Wayland
  // clients can still paste it. XFixes (if present) will also fire for
  // this same transition; refreshing here too keeps this correct even
  // without the extension.
  refresh_incoming_clipboard_targets(im);
}

// -----------------------------------------------------------------------
// Requestor side: someone else owns CLIPBOARD, we read from them.
// -----------------------------------------------------------------------

void X11Backend::set_clipboard_owner_changed_callback(
    ClipboardOwnerChangedCallback cb) {
  impl_->clipboard.clipboard_owner_changed_cb = std::move(cb);
}

void X11Backend::request_clipboard_data(const std::string &mime_type,
                                        ClipboardDataCallback on_data) {
  auto &im = *impl_;
  if (im.clipboard.incoming_mime_to_atom.find(mime_type) ==
      im.clipboard.incoming_mime_to_atom.end()) {
    on_data({});
    return;
  }
  PendingClipboardReceive req;
  req.is_targets_query = false;
  req.mime_type = mime_type;
  req.on_data = std::move(on_data);
  im.clipboard.receive_queue.push_back(std::move(req));
  if (!im.clipboard.receive_in_flight)
    Impl::start_next_clipboard_receive(im);
}

void X11Backend::Impl::refresh_incoming_clipboard_targets(Impl &im) {
  PendingClipboardReceive req;
  req.is_targets_query = true;
  im.clipboard.receive_queue.push_back(std::move(req));
  if (!im.clipboard.receive_in_flight)
    start_next_clipboard_receive(im);
}

void X11Backend::Impl::start_next_clipboard_receive(Impl &im) {
  while (!im.clipboard.receive_queue.empty()) {
    auto &req = im.clipboard.receive_queue.front();
    xcb_atom_t target = im.clipboard.atom_targets;
    if (!req.is_targets_query) {
      auto it = im.clipboard.incoming_mime_to_atom.find(req.mime_type);
      if (it == im.clipboard.incoming_mime_to_atom.end()) {
        if (req.on_data)
          req.on_data({});
        im.clipboard.receive_queue.pop_front();
        continue;
      }
      target = it->second;
    }
    im.clipboard.receive_in_flight = true;
    xcb_convert_selection(im.conn, im.clipboard.selection_window, im.clipboard.atom_clipboard,
                          target, im.clipboard.atom_ww_selection, XCB_CURRENT_TIME);
    xcb_flush(im.conn);
    return;
  }
  im.clipboard.receive_in_flight = false;
}

void X11Backend::Impl::finish_clipboard_targets(
    Impl &im, PendingClipboardTargets pending) {
  if (pending.has_utf8 || pending.has_plain_atom || pending.has_string) {
    const xcb_atom_t chosen = pending.has_utf8 ? im.clipboard.atom_utf8_string
                              : pending.has_plain_atom
                                  ? im.clipboard.atom_text_plain_utf8
                                  : static_cast<xcb_atom_t>(XCB_ATOM_STRING);
    for (const char *mime :
         {"text/plain;charset=utf-8", "UTF8_STRING", "text/plain", "STRING"}) {
      pending.mime_to_atom[mime] = chosen;
      if (std::find(pending.mimes.begin(), pending.mimes.end(), mime) ==
          pending.mimes.end())
        pending.mimes.emplace_back(mime);
    }
  }

  im.clipboard.incoming_mime_to_atom = std::move(pending.mime_to_atom);
  WW_LOG_DEBUG("[x11-clipboard] CLIPBOARD owner offers {} usable mime type(s)",
               pending.mimes.size());
  if (im.clipboard.clipboard_owner_changed_cb)
    im.clipboard.clipboard_owner_changed_cb(std::move(pending.mimes));
  start_next_clipboard_receive(im);
}

void X11Backend::Impl::poll_pending_clipboard_targets(Impl &im) {
  if (!im.clipboard.pending_clipboard_targets)
    return;

  auto &pending = *im.clipboard.pending_clipboard_targets;
  for (size_t i = 0; i < pending.names_pending.size();) {
    void *reply_ptr = nullptr;
    xcb_generic_error_t *error_ptr = nullptr;
    const auto sequence = pending.names_pending[i].sequence;
    if (!xcb_poll_for_reply(im.conn, sequence, &reply_ptr, &error_ptr)) {
      ++i;
      continue;
    }

    auto *reply = static_cast<xcb_get_atom_name_reply_t *>(reply_ptr);
    if (reply) {
      const int name_len = xcb_get_atom_name_name_length(reply);
      const char *name = xcb_get_atom_name_name(reply);
      if (name && name_len > 0) {
        std::string mime(name, static_cast<size_t>(name_len));
        if (mime.find('/') != std::string::npos) {
          const xcb_atom_t atom = pending.names_pending[i].atom;
          pending.mime_to_atom[mime] = atom;
          if (std::find(pending.mimes.begin(), pending.mimes.end(), mime) ==
              pending.mimes.end())
            pending.mimes.emplace_back(std::move(mime));
        }
      }
    }
    ::free(reply);
    ::free(error_ptr);
    pending.names_pending[i] = pending.names_pending.back();
    pending.names_pending.pop_back();
  }

  if (!pending.names_pending.empty())
    return;

  PendingClipboardTargets completed = std::move(pending);
  im.clipboard.pending_clipboard_targets.reset();
  finish_clipboard_targets(im, std::move(completed));
}

void X11Backend::Impl::finish_clipboard_receive(Impl &im,
                                                PendingClipboardReceive req,
                                                std::vector<uint8_t> data) {
  if (req.is_targets_query) {
    PendingClipboardTargets pending;
    if (data.size() >= sizeof(xcb_atom_t)) {
      const auto *atoms = reinterpret_cast<const xcb_atom_t *>(data.data());
      const size_t count = data.size() / sizeof(xcb_atom_t);
      for (size_t i = 0; i < count; ++i) {
        const xcb_atom_t atom = atoms[i];
        if (atom == im.clipboard.atom_utf8_string) {
          pending.has_utf8 = true;
          continue;
        }
        if (atom == static_cast<xcb_atom_t>(XCB_ATOM_STRING)) {
          pending.has_string = true;
          continue;
        }
        if (atom == im.clipboard.atom_text_plain_utf8) {
          pending.has_plain_atom = true;
          pending.mime_to_atom["text/plain;charset=utf-8"] = atom;
          if (std::find(pending.mimes.begin(), pending.mimes.end(),
                        "text/plain;charset=utf-8") == pending.mimes.end())
            pending.mimes.emplace_back("text/plain;charset=utf-8");
          continue;
        }
        if (atom == im.clipboard.atom_targets || atom == XCB_ATOM_NONE)
          continue;

        // Resolve MIME-named atoms without blocking the event loop. The
        // pending TARGETS result is published after these replies arrive.
        const auto cookie = xcb_get_atom_name(im.conn, atom);
        pending.names_pending.push_back({cookie.sequence, atom});
      }
    }

    if (!pending.names_pending.empty()) {
      im.clipboard.pending_clipboard_targets = std::move(pending);
      xcb_flush(im.conn);
      return; // keep receive processing serialized until names are ready
    }
    finish_clipboard_targets(im, std::move(pending));
    return;
  }

  if (req.on_data)
    req.on_data(std::move(data));
  start_next_clipboard_receive(im);
}

void X11Backend::Impl::dispatch_selection_notify(Impl &im,
                                                  xcb_generic_event_t *ev) {
  auto *e = reinterpret_cast<xcb_selection_notify_event_t *>(ev);
  if (e->requestor != im.clipboard.selection_window || e->selection != im.clipboard.atom_clipboard)
    return;
  if (im.clipboard.receive_queue.empty())
    return; // stray reply — nothing waiting on it
  // A SelectionNotify is only expected for the ConvertSelection we have in
  // flight. If a property read / INCR receive / target-name resolution is
  // still outstanding, this one is a duplicate (any X client can SendEvent
  // one at us); acting on it would pop an unrelated queued request and
  // overwrite pending_property_read, dropping the first request's callback.
  if (im.clipboard.pending_property_read || im.clipboard.pending_incr_receive ||
      im.clipboard.pending_clipboard_targets) {
    WW_LOG_WARN("[x11-clipboard] ignoring unexpected SelectionNotify while a "
                "clipboard read is still in progress");
    return;
  }

  PendingClipboardReceive req = std::move(im.clipboard.receive_queue.front());
  im.clipboard.receive_queue.pop_front();

  if (e->property == static_cast<xcb_atom_t>(XCB_ATOM_NONE)) {
    // Owner declined (empty property) — nothing to read, finish now.
    finish_clipboard_receive(im, std::move(req), {});
    return;
  }

  // Fire the GetProperty and come back for it from poll_pending_property_read
  // (called every pass through process_events) rather than blocking on
  // xcb_get_property_reply() here — see PendingPropertyRead's comment in
  // X11Backend_impl.hpp for why a blocking wait at this call site is
  // specifically risky.
  xcb_get_property_cookie_t c =
      xcb_get_property(im.conn, 0, im.clipboard.selection_window, e->property,
                       static_cast<xcb_atom_t>(XCB_GET_PROPERTY_TYPE_ANY), 0,
                       kClipboardMaxBytes / 4);
  im.clipboard.pending_property_read =
      PendingPropertyRead{c.sequence, e->property, std::move(req)};
  xcb_flush(im.conn);
}

void X11Backend::Impl::poll_pending_property_read(Impl &im) {
  if (!im.clipboard.pending_property_read)
    return;

  void *reply_ptr = nullptr;
  xcb_generic_error_t *error_ptr = nullptr;
  if (!xcb_poll_for_reply(im.conn, im.clipboard.pending_property_read->sequence,
                          &reply_ptr, &error_ptr))
    return;

  PendingPropertyRead pending = std::move(*im.clipboard.pending_property_read);
  im.clipboard.pending_property_read.reset();
  auto *r = static_cast<xcb_get_property_reply_t *>(reply_ptr);
  std::vector<uint8_t> data;
  xcb_atom_t type = XCB_ATOM_NONE;
  if (r) {
    type = r->type;
    const auto *val = static_cast<const uint8_t *>(xcb_get_property_value(r));
    const int len = xcb_get_property_value_length(r);
    if (val && len > 0)
      data.assign(val, val + static_cast<size_t>(len));
    ::free(r);
  }
  ::free(error_ptr);

  if (pending.incr_chunk) {
    if (!im.clipboard.pending_incr_receive)
      return;
    auto &transfer = *im.clipboard.pending_incr_receive;
    if (data.empty()) {
      PendingClipboardIncrReceive completed = std::move(transfer);
      im.clipboard.pending_incr_receive.reset();
      im.clipboard.pending_incr_chunk_notify = false;
      finish_clipboard_receive(im, std::move(completed.req),
                               std::move(completed.data));
      return;
    }
    if (transfer.data.size() + data.size() > kClipboardMaxBytes) {
      WW_LOG_WARN("[x11-clipboard] incoming INCR exceeds {} bytes; aborting",
                  kClipboardMaxBytes);
      PendingClipboardIncrReceive failed = std::move(transfer);
      im.clipboard.pending_incr_receive.reset();
      im.clipboard.pending_incr_chunk_notify = false;
      xcb_delete_property(im.conn, im.clipboard.selection_window, pending.property);
      xcb_flush(im.conn);
      finish_clipboard_receive(im, std::move(failed.req), {});
      return;
    }
    transfer.data.insert(transfer.data.end(), data.begin(), data.end());
    transfer.last_activity = std::chrono::steady_clock::now();
    // The get_property request used delete=1. The owner observes this delete
    // and writes the next chunk, which generates another PropertyNotify. If
    // that event arrived while this reply was still pending, start its read
    // now instead of waiting for an event that has already been consumed.
    if (im.clipboard.pending_incr_chunk_notify)
      start_incr_receive_chunk_read(im);
    xcb_flush(im.conn);
    return;
  }

  if (type == im.clipboard.atom_incr) {
    // The INCR property's 32-bit value is the owner's size estimate. It is
    // advisory; chunks are accumulated until the owner sends an empty one.
    im.clipboard.pending_incr_receive = PendingClipboardIncrReceive{
        std::move(pending.req), pending.property, {}};
    xcb_delete_property(im.conn, im.clipboard.selection_window, pending.property);
    xcb_flush(im.conn);
    return;
  }

  xcb_delete_property(im.conn, im.clipboard.selection_window, pending.property);
  xcb_flush(im.conn);
  finish_clipboard_receive(im, std::move(pending.req), std::move(data));
}

void X11Backend::Impl::dispatch_xfixes_selection_notify(
    Impl &im, xcb_generic_event_t *ev) {
  auto *e = reinterpret_cast<xcb_xfixes_selection_notify_event_t *>(ev);
  if (e->selection != im.clipboard.atom_clipboard)
    return;
  if (e->owner == im.clipboard.selection_window)
    return; // that's us — set_clipboard_owner() already handled it
  if (we_own_clipboard_now(im.conn, im.clipboard.selection_window, im.clipboard.atom_clipboard))
    return; // stale: a release we followed by re-acquiring (see above)
  // The owner we were reading an INCR transfer from just died or gave up the
  // selection; the next chunk / terminator will never come.
  if (im.clipboard.pending_incr_receive)
    abort_incr_receive(im, "CLIPBOARD owner changed during INCR receive");
  WW_LOG_TRACE("[x11-clipboard] CLIPBOARD owner changed externally "
              "(owner={})",
              e->owner);
  refresh_incoming_clipboard_targets(im);
}

void X11Backend::Impl::abort_incr_receive(Impl &im, const char *why) {
  if (!im.clipboard.pending_incr_receive)
    return;
  WW_LOG_DEBUG("[x11-clipboard] aborting INCR receive: {}", why);
  PendingClipboardIncrReceive aborted = std::move(*im.clipboard.pending_incr_receive);
  im.clipboard.pending_incr_receive.reset();
  im.clipboard.pending_incr_chunk_notify = false;
  // Complete (not drop) the request: its callback is what closes the Wayland
  // client's pipe. Partial data is deliberately not delivered.
  finish_clipboard_receive(im, std::move(aborted.req), {});
}

void X11Backend::Impl::dispatch_destroy_notify(Impl &im,
                                               xcb_generic_event_t *ev) {
  auto *e = reinterpret_cast<xcb_destroy_notify_event_t *>(ev);
  for (auto it = im.clipboard.pending_incr_sends.begin();
       it != im.clipboard.pending_incr_sends.end();) {
    if (it->second.requestor == e->window) {
      WW_LOG_DEBUG("[x11-clipboard] INCR requestor {} destroyed; dropping "
                   "{} byte(s) of pending data",
                   e->window, it->second.data.size());
      it = im.clipboard.pending_incr_sends.erase(it);
    } else {
      ++it;
    }
  }
}

void X11Backend::Impl::expire_stale_incr_transfers(Impl &im) {
  const auto now = std::chrono::steady_clock::now();
  for (auto it = im.clipboard.pending_incr_sends.begin();
       it != im.clipboard.pending_incr_sends.end();) {
    if (now - it->second.last_activity > kClipboardIncrIdleTimeout) {
      WW_LOG_DEBUG("[x11-clipboard] INCR send to requestor {} idle for {}s; "
                   "abandoning",
                   it->second.requestor, kClipboardIncrIdleTimeout.count());
      it = im.clipboard.pending_incr_sends.erase(it);
    } else {
      ++it;
    }
  }
  if (im.clipboard.pending_incr_receive &&
      now - im.clipboard.pending_incr_receive->last_activity > kClipboardIncrIdleTimeout)
    abort_incr_receive(im, "INCR owner stalled");
}

} // namespace ww
