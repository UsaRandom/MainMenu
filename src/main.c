#include <libdragon.h>

#include "app.h"
#include "boot/boot.h"


int main (void) {
    boot_params_t boot_params;

    app_run(&boot_params);

    disable_interrupts();

    boot(&boot_params);

    assertf(false, "Unexpected return from 'boot' function");

    while (true) {
        // Shouldn't get here
    }
}
