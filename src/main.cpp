#include "wwindows/wwindows.hpp"
#include "wwindows/core/event_loop.hpp"
#include <wwindows/support/log.hpp>
#include <csignal>
#include <cstdlib>

static ww::EventLoop *g_event_loop = nullptr;

static void handle_shutdown_signal(int sig) {
  if (g_event_loop) {
    WW_LOG_INFO("[wwindows] received signal {}, initiating graceful shutdown", sig);
    g_event_loop->stop(0);
  }
}

int main() {
  std::signal(SIGPIPE, SIG_IGN);

  // Install handlers for graceful shutdown on session end (SIGTERM/SIGHUP).
  // SA_RESTART ensures interrupted syscalls are retried.
  struct sigaction sa{};
  sa.sa_handler = handle_shutdown_signal;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESTART;
  ::sigaction(SIGTERM, &sa, nullptr);
  ::sigaction(SIGHUP, &sa, nullptr);

  auto server = ww::WWindows::create();
  if (!server) {
    WW_LOG_ERROR("wwindows: startup failed: {}", server.error().message);
    return EXIT_FAILURE;
  }
  g_event_loop = &(*server)->event_loop();
  return (*server)->run();
}
