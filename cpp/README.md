# C++ source boundary

Cobalt's native application remains C-first. Keep C translation units in the existing `src/` module tree; put new C++ translation units in this top-level `cpp/` directory.

The root Makefile deliberately discovers `.c` files only under `SOURCES` and `.cpp` files only under `CPPSOURCES`. This keeps language ownership visible in the build and avoids quietly mixing C++ into the C modules. C++ uses the existing `CXXFLAGS` policy in the Makefile (GNU C++17, with exceptions and RTTI disabled for binary size). Do not move existing C files merely to make the split look symmetrical.

Use C++ only where it materially helps, such as UI/state management or RAII for resource cleanup. Keep platform and SDK interfaces compatible with WUT and the existing C libraries.