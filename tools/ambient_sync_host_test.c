#include <assert.h>
#include <stdio.h>

#include "ambient_sync.h"

int main(void) {
    assert(ambient_sync_next_delay(12 * 3600, 3600, 23, 7) == 3600);
    assert(ambient_sync_next_delay(22 * 3600, 2 * 3600, 23, 7) == 9 * 3600);
    assert(ambient_sync_next_delay(1 * 3600, 30 * 60, 23, 7) == 6 * 3600);
    assert(ambient_sync_next_delay(23 * 3600 + 30 * 60, 3600, 8, 20) == 3600);
    assert(ambient_sync_next_delay(10, 3600, 8, 8) == 3600);
    assert(ambient_sync_next_delay(10, 0, 23, 7) == 0);
    assert(ambient_sync_is_quiet(2 * 3600, 23, 7));
    assert(ambient_sync_is_quiet(23 * 3600, 23, 7));
    assert(!ambient_sync_is_quiet(7 * 3600, 23, 7));
    puts("ambient sync host tests passed");
    return 0;
}
