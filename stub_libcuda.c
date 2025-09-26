/* 
 * stub_libcuda.c - Stub implementation for libcuda.so.1
 * This prevents "Cannot load libcuda.so.1" errors on non-NVIDIA hardware
 */

// We need to export some symbols that libmpv might try to dlsym() for
void* cuInit() { return 0; }
void* cuCtxCreate() { return 0; }
void* cuCtxPushCurrent() { return 0; }
void* cuCtxPopCurrent() { return 0; }
void* cuStreamCreate() { return 0; }
void* cuMemAlloc() { return 0; }
void* cuMemFree() { return 0; }
void* cuMemcpy2D() { return 0; }
void* cuMemcpyDtoH() { return 0; }
void* cuMemcpyHtoD() { return 0; }
void* cuMemcpy() { return 0; }
void* cuDeviceGetCount() { return 0; }
void* cuDeviceGet() { return 0; }
void* cuDeviceGetName() { return 0; }
void* cuDeviceGetAttribute() { return 0; }