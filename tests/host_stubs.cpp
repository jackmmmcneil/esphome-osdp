#include <sys/select.h>
#include <cstdint>
#include <unistd.h>
namespace esphome {
void wake_loop_threadsafe() {}
void wake_setup() {}
namespace internal {
int g_wake_socket_fd = -1;
fd_set g_read_fds;
void wakeable_delay(uint32_t ms) { usleep(ms * 1000); }
}  // namespace internal
}  // namespace esphome
void setup() {}
void loop() {}
