#if defined(BUILD_TARGET_AARCH64)
#if WASM_ENABLE_SIMD != 0
#include "invokeNative_aarch64_simd.s"
#else
#include "invokeNative_aarch64.s"
#endif
#elif defined(BUILD_TARGET_X86_64) || defined(BUILD_TARGET_AMD_64)
#if WASM_ENABLE_SIMD != 0
#include "invokeNative_em64_simd.s"
#else
#include "invokeNative_em64.s"
#endif
#else
#error "unsupported darwin build target"
#endif
