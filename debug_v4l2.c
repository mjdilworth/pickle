#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>
#include <errno.h>
#include <string.h>

int main() {
    const char *dev_paths[] = {
        "/dev/video0", "/dev/video1", "/dev/video10", "/dev/video11", 
        "/dev/video19", NULL
    };
    
    for (int i = 0; dev_paths[i] != NULL; i++) {
        printf("Testing device: %s\n", dev_paths[i]);
        
        int fd = open(dev_paths[i], O_RDWR);
        if (fd < 0) {
            printf("  - Cannot open: %s\n", strerror(errno));
            continue;
        }
        
        struct v4l2_capability cap;
        if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
            printf("  - Driver: %s\n", cap.driver);
            printf("  - Card: %s\n", cap.card);
            printf("  - Capabilities: 0x%08x\n", cap.capabilities);
            
            if (cap.capabilities & V4L2_CAP_VIDEO_M2M_MPLANE) {
                printf("  - HAS V4L2_CAP_VIDEO_M2M_MPLANE ✓\n");
            }
            if (cap.capabilities & V4L2_CAP_VIDEO_M2M) {
                printf("  - HAS V4L2_CAP_VIDEO_M2M ✓\n");
            }
            
            if ((cap.capabilities & V4L2_CAP_VIDEO_M2M_MPLANE) || 
                (cap.capabilities & V4L2_CAP_VIDEO_M2M)) {
                printf("  - *** HARDWARE DECODER SUPPORTED! ***\n");
            }
        } else {
            printf("  - VIDIOC_QUERYCAP failed: %s\n", strerror(errno));
        }
        
        close(fd);
        printf("\n");
    }
    
    return 0;
}