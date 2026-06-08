#ifndef VIDEO_H
#define VIDEO_H

#define FB_IOCTL_GET_INFO 0

struct framebuffer_info {
    unsigned int width;
    unsigned int height;
    unsigned int bpp;  // byte per pixel
};

void video_init(void);
void video_bmp_display(unsigned int* bmp_image, unsigned int width, unsigned int height);
void video_get_info(struct framebuffer_info* info);
void video_flush(void* addr, unsigned long len);
void* video_get_base(void);

#endif // VIDEO_H
