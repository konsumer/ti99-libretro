#pragma once

// ti99sim's common.hpp does `#include <endian.h>` whenever it sees __GNUC__,
// which is true for clang on macOS -- but Apple ships the definitions in
// <machine/endian.h> instead. This shim sits ahead of upstream/include on the
// include path so the vendored tree stays pristine. Elsewhere it defers to the
// real <endian.h>.
#if defined ( __APPLE__ ) || defined ( __MACH__ )
    #include <machine/endian.h>
#else
    #include_next <endian.h>
#endif
