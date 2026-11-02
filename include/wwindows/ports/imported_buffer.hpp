#pragma once

#include <array>
#include <cstdint>
#include <memory>

namespace ww {

class IWindowBackend;

// How far an RGB buffer has got through DRI3 import. NV12 uses the separate
// `ImportedBufferFormat::Nv12` CPU-conversion path and does not use this state.
//
// `fd` and `pixmap` alone cannot express it: once the single import attempt
// has been fired the fd is spent (-1), and until it resolves the pixmap is
// still 0 — byte-identical to an import that already failed for good. The
// two need opposite handling and deserve opposite log levels: an in-flight
// import is a benign, self-correcting race that resolves through its own
// callback a few microseconds later, while a failed one will never present
// anything, ever.
enum class DmabufImportStatus : uint8_t {
  NotStarted, // no attempt yet — `fd` is still live
  Pending,    // request sent, callback hasn't fired
  Failed,     // callback fired with pixmap == 0
};

// Transport-neutral presentation handle owned by a protocol adapter and
// consumed by the SurfaceManager through the backend port. No Wayland/XCB
// type leaks into the core representation.
enum class ImportedBufferFormat : uint8_t { Rgb, Nv12 };

struct ImportedDmabufPlane {
  int fd{-1};
  uint32_t offset{0};
  uint32_t stride{0};
  uint64_t modifier{0};
};

// Tracks source-memory reads that have been queued on the X11 connection.
// Kept separately from ImportedBuffer so asynchronous completion callbacks
// can safely finish after the wl_buffer resource itself is destroyed.
struct ImportedBufferUseState {
  uint32_t source_reads_in_flight{0};
  bool release_pending{false};
};

struct ImportedBuffer {
  int fd{-1};
  std::array<ImportedDmabufPlane, 4> planes{};
  uint8_t plane_count{0};
  ImportedBufferFormat format{ImportedBufferFormat::Rgb};
  uint32_t pixmap{0};
  DmabufImportStatus import_status{DmabufImportStatus::NotStarted};
  uint16_t width{0};
  uint16_t height{0};
  uint32_t depth{0};
  uint32_t bpp{0};
  uint32_t stride{0};
  uint64_t modifier{0};
  IWindowBackend *backend{nullptr};
  bool bgr_order{false};
  std::shared_ptr<bool> alive{std::make_shared<bool>(true)};
  std::shared_ptr<ImportedBufferUseState> use_state{
      std::make_shared<ImportedBufferUseState>()};
};

} // namespace ww
