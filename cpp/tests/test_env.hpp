#pragma once

// Compat tests : _putenv_s n'existe pas sous POSIX. Le shim reproduit la
// semantique utile aux tests ("" = effacer, comme l'intention des appels
// _putenv_s("K", "") ; le code produit teste toujours `getenv(k) && *getenv(k)`,
// identique dans les deux cas).

#include <cstdlib>

#ifndef _WIN32
static inline int test_putenv_s(const char* k, const char* v) {
    if (!k || !*k || !v) return -1;
    if (!*v) {
        unsetenv(k);
        return 0;
    }
    return setenv(k, v, 1);
}
#define _putenv_s test_putenv_s
#endif
