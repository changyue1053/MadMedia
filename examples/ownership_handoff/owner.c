#include <madmedia/madmedia_c.h>
#include <stdio.h>

static int signal_byte(int value) {
  return putchar(value) == value && fflush(stdout) == 0;
}

static int is_idle(madmedia_handle handle) {
  size_t required = 99;
  if (madmedia_poll(handle, NULL, 0, &required) != MADMEDIA_EMPTY || required != 0)
    return 0;
  required = 99;
  return madmedia_query_persisted(handle, "absent_fixture", NULL, 0, &required)
      == MADMEDIA_NOT_FOUND && required == 0;
}

int main(int argc, char** argv) {
  if (argc != 2 || madmedia_abi_version() != MADMEDIA_ABI_VERSION) return 1;
  /* Construction requires a nonempty string; no service or task is configured. */
  madmedia_config_v2 config = {
      sizeof(config), MADMEDIA_CONFIG_V2_VERSION, "ownership-fixture-not-a-url", argv[1],
      1, 0, MADMEDIA_JOURNAL_FULL, 0
  };
  madmedia_handle handle = 0;
  if (madmedia_create_v2(&config, &handle) != MADMEDIA_OK || handle == 0) return 2;
  int ready = is_idle(handle) && signal_byte('R') && getchar() == 'C';
  if (madmedia_destroy(handle) != MADMEDIA_OK) return 3;
  if (!ready || madmedia_destroy(handle) != MADMEDIA_CLOSED || !signal_byte('D')) return 4;
  /* Remain alive until the contender reopens A, so process exit cannot
     substitute for explicit C ABI destruction. */
  return getchar() == 'Q' && signal_byte('Z') ? 0 : 5;
}
