/* Synthetic resident callback dispatcher. Test-only, not a vendor runtime. */
#include <stdlib.h>
static void (*late_callback)(void);
static void run_callback(void) { if(late_callback) late_callback(); }
void fixture_register_late_cleanup(void (*callback)(void)) {
    if(!late_callback) atexit(run_callback);
    late_callback=callback;
}
