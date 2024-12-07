#include "syscall.h"
#include "string.h"
#include "stdlib.h"

void main(void) {
    int result;

    if (_fork()) {
#if 1
        // Open ser1 device as fd=0
        result = _devopen(0, "ser", 1);

        if (result < 0) {
            _msgout("_devopen failed ser1");
            char err[10];
            itoa(result, err, 10);
            _msgout(err);
            _exit();
        }

        // exec trek

        result = _fsopen(1, "trek");

        if (result < 0) {
            _msgout("_fsopen failed trek");
            _exit();
        }

        _exec(1);
#else
        _wait(0);
#endif
    } else {
#if 1
        // Open ser1 device as fd=0
        result = _devopen(0, "ser", 2);

        if (result < 0) {
            _msgout("_devopen failed ser2");
            _exit();
        }

        // exec trek

        result = _fsopen(1, "rule30");

        if (result < 0) {
            _msgout("_fsopen failed rule30");
            _exit();
        }

        _exec(1);
#endif
    }
}
