# Quick start — C / C++

## Prerequisites

**Linux**
```bash
# Ubuntu/Debian
sudo apt install cmake ninja-build gcc g++ libssl-dev

# Fedora/RHEL
sudo dnf install cmake ninja-build gcc gcc-c++ openssl-devel
```

**Windows**
- Visual Studio 2022 (Desktop C++ workload)
- [vcpkg](https://vcpkg.io) — `vcpkg install openssl zlib:x64-windows-static-md`
- CMake 3.19+ in PATH (bundled with VS or standalone)

---

## C (plain API)

```c
#include "medut.h"
#include <stdio.h>
#include <string.h>

static void on_event(const medut_event_t *ev, void *ud) {
    if (ev->type == MEDUT_EV_REG_STATE &&
        ev->u.reg.state == MEDUT_REG_REGISTERED) {
        puts("Registered!");
    }
    if (ev->type == MEDUT_EV_INCOMING_CALL) {
        printf("Incoming from %s\n", ev->u.incoming.from_uri);
        medut_call_answer(ev->u.incoming.call);
    }
}

int main(void) {
    medut_config_t cfg;
    medut_config_init(&cfg);
    cfg.log_level  = 1;
    cfg.event_cb   = on_event;

    medut_init(&cfg);

    medut_account_config_t acfg = {
        .uri      = "alice@pbx.example.com",
        .password = "secret",
        .transport = MEDUT_TRANSPORT_TLS,
    };
    medut_account_handle_t acct;
    medut_account_create(&acfg, &acct);
    medut_account_register(acct);

    /* ... wait for events via your own event loop ... */

    medut_account_destroy(acct);
    medut_shutdown();
    return 0;
}
```

**Linux / macOS**
```bash
# 1. Build SDK
bash scripts/build-linux.sh    # or build-macos.sh

# 2. Compile — .so has OpenSSL/zlib/pthreads baked in; no extra -l flags needed
gcc main.c -I dist/linux/x86_64/include \
    dist/linux/x86_64/medut.so -o demo

# 3. Run
./demo
```

**Windows (PowerShell)**
```powershell
# 1. Build SDK
.\scripts\build-windows.ps1
# Output: dist\windows\x64\medut.dll  +  medut.lib  +  bare.lib

# 2. Compile — link against the import library (medut.lib), ship medut.dll alongside the exe
cl main.c /I dist\windows\x64\include /link dist\windows\x64\medut.lib /out:demo.exe
```

---

## C++ (header-only wrapper)

### One-command setup

**Linux / macOS**
```bash
bash bindings/cpp/build.sh
```
Builds the SDK if needed, compiles all examples in `bindings/cpp/examples/`, and places the binary and `medut.so` together so no `LD_LIBRARY_PATH` is needed.

**Windows (PowerShell)**
```powershell
cd bindings\cpp
.\build-examples.ps1
# Output: build\Release\quickstart.exe  (medut.dll copied alongside)
```

### Manual compile

**Linux / macOS**
```bash
# After running bash scripts/build-linux.sh
g++ -std=c++17 main.cpp \
    -I dist/linux/x86_64/include -I bindings/cpp \
    dist/linux/x86_64/medut.so \
    -o demo
./demo
```

**Windows**
```powershell
# After running .\scripts\build-windows.ps1
# Use CMake (recommended) or compile directly with cl:
cl /std:c++17 main.cpp ^
   /I dist\windows\x64\include /I bindings\cpp ^
   /link dist\windows\x64\medut.lib /out:demo.exe
```

### Code

```cpp
#include "bindings/cpp/medut.hpp"
#include <iostream>

int main() {
    Medut::SDK sdk;
    sdk.config().log_level = 1;

    sdk.on_event([](const medut_event_t& ev) {
        if (ev.type == MEDUT_EV_REG_STATE &&
            ev.u.reg.state == MEDUT_REG_REGISTERED)
            std::cout << "Registered!\n";

        if (ev.type == MEDUT_EV_INCOMING_CALL) {
            Medut::Call c(ev.u.incoming.call);
            c.answer();
        }
    });

    auto acct = sdk.create_account("alice@pbx.example.com", "secret",
                                   MEDUT_TRANSPORT_TLS);
    acct.register_account();

    std::this_thread::sleep_for(std::chrono::seconds(30));
    return 0;
}
```

---

## Debugging init issues

If `medut_init()` hangs or crashes, set `MEDUT_DEBUG_INIT=1` to see which of the 14 init stages is the culprit:

```bash
MEDUT_DEBUG_INIT=1 ./demo                  # Linux / macOS
```

```powershell
$env:MEDUT_DEBUG_INIT=1; .\demo.exe        # Windows
```

See [debugging guide](../guides/debugging.md) for the full set of diagnostic options.

---

## See also
- Full C++ example: [bindings/cpp/examples/quickstart.cpp](../../bindings/cpp/examples/quickstart.cpp)
- API reference: [accounts](../api/accounts.md) · [calls](../api/calls.md) · [events](../api/events.md)
