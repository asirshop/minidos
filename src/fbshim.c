// 测试用: 拦截 framebuffer 访问, 把桌面渲染结果输出为 PPM
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

static int W=1280, H=720;
static unsigned char *fake=NULL;
static void dump_fb(void) __attribute__((destructor));
static void dump_fb(void){
    if(!fake) return;
    FILE *f=fopen("/tmp/fakefb.img","wb");
    if(f){ fwrite(fake,W*H*4,1,f); fclose(f); }
}

int open(const char *path, int flags, ...){
    static int (*real)(const char*,int,...);
    if(!real) real=dlsym(RTLD_NEXT,"open");
    va_list ap; va_start(ap,flags); int mode=va_arg(ap,int); va_end(ap);
    if(path && strstr(path,"/dev/fb0")){
        if(!fake) fake=calloc(W*H*4,1);
        int fd=real("/tmp/fakefb.img", O_RDWR|O_CREAT, 0644);
        ftruncate(fd, (off_t)W*H*4);
        return fd;
    }
    if(path && strstr(path,"/dev/input")) return -1; /* 无输入设备 */
    return real(path,flags,mode);
}
int ioctl(int fd, unsigned long req, ...){
    static int (*real)(int,unsigned long,...);
    if(!real) real=dlsym(RTLD_NEXT,"ioctl");
    va_list ap; va_start(ap,req); void *arg=va_arg(ap,void*); va_end(ap);
    if(req==FBIOGET_VSCREENINFO){
        struct fb_var_screeninfo *vi=arg;
        memset(vi,0,sizeof *vi);
        vi->xres=W; vi->yres=H; vi->bits_per_pixel=32;
        vi->red.offset=16; vi->green.offset=8; vi->blue.offset=0;
        vi->red.length=8; vi->green.length=8; vi->blue.length=8;
        return 0;
    }
    if(req==FBIOGET_FSCREENINFO){
        struct fb_fix_screeninfo *fi=arg;
        memset(fi,0,sizeof *fi);
        fi->line_length=W*4;
        strcpy(fi->id,"fakefb");
        return 0;
    }
    return real(fd,req,arg);
}
void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off){
    static void* (*real)(void*,size_t,int,int,int,off_t);
    if(!real) real=dlsym(RTLD_NEXT,"mmap");
    (void)fake;
    return real(addr,len,prot,flags,fd,off);
}
