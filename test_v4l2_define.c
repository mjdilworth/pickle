#include <stdio.h>

int main() {
#if defined(USE_V4L2_DECODER)
    printf("USE_V4L2_DECODER is DEFINED\n");
    return 0;
#else
    printf("USE_V4L2_DECODER is NOT DEFINED\n");
    return 1;
#endif
}