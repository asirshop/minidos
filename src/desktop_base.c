/* =============================================================
 *  MiniDOS Desktop 1.0  —  自研 Framebuffer 图形桌面
 *  不依赖 X11 / Wayland / SDL / GTK, 直接绘制显卡帧缓冲
 *  结构: 绘图层 -> 字体 -> 控件 -> 窗口管理器 -> 应用
 * ============================================================= */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <time.h>
#include <math.h>

/* ---------- 屏幕 ---------- */
static int SW, SH, BPP, LINE;
static unsigned char *FB, *BUF;
static int bytespp, needs_swap;

/* ---------- 颜色 (RGB888 简化, 写入时按 bpp 转换) ---------- */
typedef unsigned int C;
#define RGB(r,g,b) (0xFF000000u | (((int)(r)&0xff)<<16) | (((int)(g)&0xff)<<8) | ((int)(b)&0xff))
#define COL_BG      RGB(24,28,42)
#define COL_BG2     RGB(32,38,56)
#define COL_TITLE   RGB(46,54,80)
#define COL_TITLE_A RGB(70,120,200)
#define COL_BORDER  RGB(90,105,140)
#define COL_TEXT    RGB(225,232,245)
#define COL_DIM     RGB(130,140,165)
#define COL_ACCENT  RGB(90,200,160)
#define COL_BUTTON  RGB(52,62,90)
#define COL_BTNH    RGB(74,90,130)
#define COL_TASKBAR RGB(18,21,32)
#define COL_SEL     RGB(60,110,180)

#define CLAMP(v) ((v)<0?0:((v)>255?255:(v)))

/* 像素写入（双缓冲到 FB） */
static inline void putpx(unsigned char *dst, int x, int y, C c){
    if(x<0||y<0||x>=SW||y>=SH) return;
    unsigned char *p = dst + y*LINE + x*bytespp;
    int r=(c>>16)&0xff, g=(c>>8)&0xff, b=c&0xff;
    if(bytespp==4){ p[0]=b; p[1]=g; p[2]=r; p[3]=0; }
    else if(bytespp==3){ p[0]=b; p[1]=g; p[2]=r; }
    else if(bytespp==2){ unsigned short v=((r>>3)<<11)|((g>>2)<<5)|(b>>3); *(unsigned short*)p=v; }
}
static inline int get_bpp_px(int x,int y){
    if(x<0||y<0||x>=SW||y>=SH) return 0;
    unsigned char *p = BUF + y*LINE + x*bytespp;
    if(bytespp==4) return p[0]|(p[1]<<8)|(p[2]<<16);
    if(bytespp==3) return p[0]|(p[1]<<8)|(p[2]<<16);
    return 0;
}
/* 半透明混合 */
static void blend_rect(int x,int y,int w,int h,C c,float a){
    int r=(c>>16)&0xff, g=(c>>8)&0xff, b=c&0xff;
    for(int yy=y; yy<y+h; yy++){
        if(yy<0||yy>=SH) continue;
        for(int xx=x; xx<x+w; xx++){
            if(xx<0||xx>=SW) continue;
            unsigned char *p = BUF + yy*LINE + xx*bytespp;
            int dr,dg,db;
            if(bytespp>=3){ dr=p[2]; dg=p[1]; db=p[0]; }
            else { unsigned short v=*(unsigned short*)p; dr=((v>>11)&0x1f)*8; dg=((v>>5)&0x3f)*4; db=(v&0x1f)*8; }
            int nr=dr+(int)((r-dr)*a), ng=dg+(int)((g-dg)*a), nb=db+(int)((b-db)*a);
            putpx(BUF,xx,yy,RGB(CLAMP(nr),CLAMP(ng),CLAMP(nb)));
        }
    }
}
static void fill_rect(int x,int y,int w,int h,C c){
    if(w<=0||h<=0) return;
    for(int yy=y; yy<y+h; yy++)
        for(int xx=x; xx<x+w; xx++) putpx(BUF,xx,yy,c);
}
static void rect_outline(int x,int y,int w,int h,C c){
    fill_rect(x,y,w,1,c); fill_rect(x,y+h-1,w,1,c);
    fill_rect(x,y,1,h,c); fill_rect(x+w-1,y,1,h,c);
}
static void round_rect(int x,int y,int w,int h,int r,C c){
    for(int yy=0; yy<h; yy++) for(int xx=0; xx<w; xx++){
        int dx = xx<r? r-xx : (xx>=w-r? xx-(w-r-1) : 0);
        int dy = yy<r? r-yy : (yy>=h-r? yy-(h-r-1) : 0);
        if(dx&&dy && dx*dx+dy*dy > r*r) continue;
        putpx(BUF,x+xx,y+yy,c);
    }
}
static void round_frame(int x,int y,int w,int h,int r,C c){
    for(int yy=0; yy<h; yy++) for(int xx=0; xx<w; xx++){
        int dx = xx<r? r-xx : (xx>=w-r? xx-(w-r-1) : 0);
        int dy = yy<r? r-yy : (yy>=h-r? yy-(h-r-1) : 0);
        int d2 = dx*dx+dy*dy;
        if(d2>r*r) continue;
        int on_edge = (xx==0||xx==w-1||yy==0||yy==h-1);
        if(on_edge || d2 > (r-1)*(r-1))
            putpx(BUF,x+xx,y+yy,c);
    }
}
static void gradient(int x,int y,int w,int h,C c1,C c2){
    int r1=(c1>>16)&0xff,g1=(c1>>8)&0xff,b1=c1&0xff;
    int r2=(c2>>16)&0xff,g2=(c2>>8)&0xff,b2=c2&0xff;
    for(int yy=0; yy<h; yy++){
        float t=(float)yy/(h>1?h-1:1);
        C c=RGB(r1+(r2-r1)*t, g1+(g2-g1)*t, b1+(b2-b1)*t);
        fill_rect(x,y+yy,w,1,c);
    }
}

/* ---------- 5x7 位图字体 ---------- */
static const unsigned char FONT[95][7] = {
{0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* space */
{0x04,0x04,0x04,0x04,0x04,0x00,0x04}, /* ! */
{0x0A,0x0A,0x00,0x00,0x00,0x00,0x00}, /* " */
{0x0A,0x0A,0x1F,0x0A,0x1F,0x0A,0x0A}, /* # */
{0x04,0x0F,0x14,0x0E,0x05,0x1E,0x04}, /* $ */
{0x18,0x19,0x02,0x04,0x08,0x13,0x03}, /* % */
{0x0C,0x12,0x14,0x08,0x15,0x12,0x0D}, /* & */
{0x04,0x04,0x00,0x00,0x00,0x00,0x00}, /* ' */
{0x02,0x04,0x08,0x08,0x08,0x04,0x02}, /* ( */
{0x08,0x04,0x02,0x02,0x02,0x04,0x08}, /* ) */
{0x00,0x04,0x15,0x0E,0x15,0x04,0x00}, /* * */
{0x00,0x04,0x04,0x1F,0x04,0x04,0x00}, /* + */
{0x00,0x00,0x00,0x00,0x06,0x04,0x08}, /* , */
{0x00,0x00,0x00,0x1F,0x00,0x00,0x00}, /* - */
{0x00,0x00,0x00,0x00,0x00,0x06,0x06}, /* . */
{0x01,0x02,0x02,0x04,0x08,0x08,0x10}, /* / */
{0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, /* 0 */
{0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}, /* 1 */
{0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, /* 2 */
{0x1F,0x02,0x04,0x02,0x01,0x11,0x0E}, /* 3 */
{0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, /* 4 */
{0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}, /* 5 */
{0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, /* 6 */
{0x1F,0x01,0x02,0x04,0x08,0x08,0x08}, /* 7 */
{0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, /* 8 */
{0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}, /* 9 */
{0x00,0x06,0x06,0x00,0x06,0x06,0x00}, /* : */
{0x00,0x06,0x06,0x00,0x06,0x04,0x08}, /* ; */
{0x02,0x04,0x08,0x10,0x08,0x04,0x02}, /* < */
{0x00,0x00,0x1F,0x00,0x1F,0x00,0x00}, /* = */
{0x08,0x04,0x02,0x01,0x02,0x04,0x08}, /* > */
{0x0E,0x11,0x01,0x02,0x04,0x00,0x04}, /* ? */
{0x0E,0x11,0x01,0x0D,0x15,0x15,0x0E}, /* @ */
{0x0E,0x11,0x11,0x11,0x1F,0x11,0x11}, /* A */
{0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}, /* B */
{0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}, /* C */
{0x1C,0x12,0x11,0x11,0x11,0x12,0x1C}, /* D */
{0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, /* E */
{0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}, /* F */
{0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}, /* G */
{0x11,0x11,0x11,0x1F,0x11,0x11,0x11}, /* H */
{0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}, /* I */
{0x07,0x02,0x02,0x02,0x02,0x12,0x0C}, /* J */
{0x11,0x12,0x14,0x18,0x14,0x12,0x11}, /* K */
{0x10,0x10,0x10,0x10,0x10,0x10,0x1F}, /* L */
{0x11,0x1B,0x15,0x15,0x11,0x11,0x11}, /* M */
{0x11,0x11,0x19,0x15,0x13,0x11,0x11}, /* N */
{0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, /* O */
{0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}, /* P */
{0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}, /* Q */
{0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}, /* R */
{0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}, /* S */
{0x1F,0x04,0x04,0x04,0x04,0x04,0x04}, /* T */
{0x11,0x11,0x11,0x11,0x11,0x11,0x0E}, /* U */
{0x11,0x11,0x11,0x11,0x11,0x0A,0x04}, /* V */
{0x11,0x11,0x11,0x15,0x15,0x1B,0x11}, /* W */
{0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}, /* X */
{0x11,0x11,0x0A,0x04,0x04,0x04,0x04}, /* Y */
{0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}, /* Z */
{0x0E,0x08,0x08,0x08,0x08,0x08,0x0E}, /* [ */
{0x10,0x08,0x08,0x04,0x02,0x02,0x01}, /* \ */
{0x0E,0x02,0x02,0x02,0x02,0x02,0x0E}, /* ] */
{0x04,0x0A,0x11,0x00,0x00,0x00,0x00}, /* ^ */
{0x00,0x00,0x00,0x00,0x00,0x00,0x1F}, /* _ */
{0x08,0x04,0x00,0x00,0x00,0x00,0x00}, /* ` */
{0x00,0x00,0x0E,0x01,0x0F,0x11,0x0F}, /* a */
{0x10,0x10,0x1E,0x11,0x11,0x11,0x1E}, /* b */
{0x00,0x00,0x0E,0x10,0x10,0x11,0x0E}, /* c */
{0x01,0x01,0x0F,0x11,0x11,0x11,0x0F}, /* d */
{0x00,0x00,0x0E,0x11,0x1F,0x10,0x0E}, /* e */
{0x06,0x09,0x08,0x1C,0x08,0x08,0x08}, /* f */
{0x00,0x0F,0x11,0x11,0x0F,0x01,0x0E}, /* g */
{0x10,0x10,0x1E,0x11,0x11,0x11,0x11}, /* h */
{0x04,0x00,0x0C,0x04,0x04,0x04,0x0E}, /* i */
{0x02,0x00,0x06,0x02,0x02,0x12,0x0C}, /* j */
{0x10,0x10,0x12,0x14,0x18,0x14,0x12}, /* k */
{0x0C,0x04,0x04,0x04,0x04,0x04,0x0E}, /* l */
{0x00,0x00,0x1A,0x15,0x15,0x15,0x15}, /* m */
{0x00,0x00,0x1E,0x11,0x11,0x11,0x11}, /* n */
{0x00,0x00,0x0E,0x11,0x11,0x11,0x0E}, /* o */
{0x00,0x00,0x1E,0x11,0x11,0x1E,0x10}, /* p */
{0x00,0x00,0x0F,0x11,0x11,0x0F,0x01}, /* q */
{0x00,0x00,0x16,0x19,0x10,0x10,0x10}, /* r */
{0x00,0x00,0x0F,0x10,0x0E,0x01,0x1E}, /* s */
{0x08,0x08,0x1C,0x08,0x08,0x09,0x06}, /* t */
{0x00,0x00,0x11,0x11,0x11,0x13,0x0D}, /* u */
{0x00,0x00,0x11,0x11,0x11,0x0A,0x04}, /* v */
{0x00,0x00,0x11,0x11,0x15,0x15,0x0A}, /* w */
{0x00,0x00,0x11,0x0A,0x04,0x0A,0x11}, /* x */
{0x00,0x00,0x11,0x11,0x0F,0x01,0x0E}, /* y */
{0x00,0x00,0x1F,0x02,0x04,0x08,0x1F}, /* z */
{0x02,0x04,0x04,0x08,0x04,0x04,0x02}, /* { */
{0x04,0x04,0x04,0x04,0x04,0x04,0x04}, /* | */
{0x08,0x04,0x04,0x02,0x04,0x04,0x08}, /* } */
{0x00,0x00,0x08,0x15,0x02,0x00,0x00} /* ~ */
};

#include "font16.c"
#include "font16_map.h"

/* 二分查找汉字索引 */
static int cn_index(unsigned int uni){
    int lo=0, hi=FONT16_N-1;
    while(lo<=hi){
        int m=(lo+hi)/2;
        unsigned int cp = CN_CP[m];
        if(cp==uni) return m;
        if(cp<uni) lo=m+1; else hi=m-1;
    }
    return -1;
}
static void draw_cn(int x,int y,unsigned int uni,C color,int scale){
    int idx = cn_index(uni);
    if(idx<0) return;
    const unsigned char *g = FONT16 + idx*32;
    for(int row=0; row<16; row++){
        unsigned int bits = (g[row*2]<<8) | g[row*2+1];
        if(!bits) continue;
        for(int col=0; col<16; col++)
            if(bits & (0x8000>>col)){
                if(scale==1) putpx(BUF,x+col,y+row,color);
                else fill_rect(x+col*scale,y+row*scale,scale,scale,color);
            }
    }
}
/* 解析 UTF-8 字符: 返回 codepoint, n=字节数 */
static unsigned int utf8_next(const unsigned char **p,int *n){
    unsigned char c=**p;
    if(c<0x80){ (*p)++; *n=1; return c; }
    if((c&0xE0)==0xC0){ *n=2; unsigned int v=c&0x1F; (*p)++; v=(v<<6)|(**p&0x3F); (*p)++; return v; }
    if((c&0xF0)==0xE0){ *n=3; unsigned int v=c&0x0F; (*p)++; v=(v<<6)|(**p&0x3F); (*p)++; v=(v<<6)|(**p&0x3F); (*p)++; return v; }
    (*p)++; *n=1; return '?';
}
static int char_w_utf8(const unsigned char **p){
    unsigned char c=**p; int n;
    utf8_next(p,&n);
    return n>1?16:6;
}
static void draw_char(int x,int y,char ch,C color,int scale){
    if(ch<32||ch>126) ch='?';
    const unsigned char *g = FONT[(int)ch-32];
    for(int row=0; row<7; row++)
        for(int col=0; col<5; col++)
            if(g[row] & (0x10>>col)){
                if(scale==1) putpx(BUF,x+col,y+row,color);
                else fill_rect(x+col*scale,y+row*scale,scale,scale,color);
            }
}
static void draw_str(int x,int y,const char *s,C color,int scale){
    const unsigned char *p=(const unsigned char*)s;
    while(*p){
        int n; unsigned int uni=utf8_next(&p,&n);
        if(n>1){ draw_cn(x,y,uni,color,scale); x+=16*scale; }
        else { draw_char(x,y,(char)uni,color,scale); x+=6*scale; }
    }
}
static int str_w(const char *s,int scale){
    int w=0; const unsigned char *p=(const unsigned char*)s;
    while(*p) w += char_w_utf8(&p)*scale;
    return w;
}
/* 截断绘制（防溢出） */
static void draw_str_clip(int x,int y,int maxw,const char *s,C color,int scale){
    int cx=x; const unsigned char *p=(const unsigned char*)s;
    while(*p){
        const unsigned char *start=p;
        int n; utf8_next(&p,&n);
        int cw=(n>1?16:6)*scale;
        if(cx+cw > x+maxw) break;
        char one[8]; int m=0;
        for(const unsigned char *q=start;q<p&&m<7;q++) one[m++]=*q;
        one[m]=0;
        draw_str(cx,y,one,color,scale);
        cx+=cw;
    }
}

/* ---------- 输入 ---------- */
static int Mx=0, My=0, mouse_fd=-1, kb_fd=-1;
static int mouse_down=0, mouse_click=0, mouse_click_x=0, mouse_click_y=0;
static int mouse_wheel=0;
static int key_queue[64]; static int kq_n=0;

static void push_key(int k){ if(kq_n<64) key_queue[kq_n++]=k; }

/* ---------- 窗口 ---------- */
#define MAX_WIN 8
typedef struct {
    int open, x, y, w, h, z;
    const char *title;
    int type;           /* 0=terminal 1=files 2=editor 3=clock 4=calc 5=about 6=browser */
    /* 终端内容 */
    char lines[40][96];
    int nlines;
    char input[128];
    /* 文件列表 */
    int sel;
    /* 编辑器 */
    char text[20][100];
    int curline;
    /* 计算器 */
    char expr[64];
    int dragging, drag_dx, drag_dy;
} Win;
static Win wins[MAX_WIN];
static int ztop=0, active_win=-1;

/* ---------- 应用函数前置声明 ---------- */
static void app_terminal_draw(Win *w);
static void app_files_draw(Win *w);
static void app_editor_draw(Win *w);
static void app_clock_draw(Win *w);
static void app_calc_draw(Win *w);
static void app_about_draw(Win *w);
static void app_browser_draw(Win *w);

/* 文字系统命令输出（终端用） */
static char CMD_BUF[8192];

static void term_add(Win *w, const char *s){
    if(w->nlines>=40){
        memmove(w->lines[0], w->lines[1], sizeof(w->lines[0])*(39));
        w->nlines=39;
    }
    strncpy(w->lines[w->nlines], s, 95); w->lines[w->nlines][95]=0;
    w->nlines++;
}


/* =============================================================
 *  小游戏 (图形版) — 从文字版移植到窗口内绘制
 *  type: 10=SNAKE 11=G2048 12=GUESS 13=TICTAC 14=MUSIC
 * ============================================================= */

/* ---- 通用按钮 ---- */
static int btn(int x,int y,int w,int h,const char *label,int hover){
    round_rect(x,y,w,h,5, hover?COL_BTNH:COL_BUTTON);
    draw_str(x+(w-str_w(label,1))/2, y+(h-7)/2, label, COL_TEXT, 1);
    return hover;
}

/* ================= 贪吃蛇 ================= */
#define SN 18          /* 网格 18x18 */
static int sn_body[SN*SN][2], sn_len, sn_dx, sn_dy, sn_fx, sn_fy, sn_score, sn_dead, sn_pause;
static int sn_step_ms=130; static unsigned long sn_last;

static void snake_init(void){
    sn_len=3; sn_dx=1; sn_dy=0; sn_score=0; sn_dead=0; sn_pause=0;
    for(int i=0;i<sn_len;i++){ sn_body[i][0]=9-i; sn_body[i][1]=9; }
    sn_fx=13; sn_fy=9;
    sn_last=0;
}
static void snake_food(void){
    do{ sn_fx=rand()%SN; sn_fy=rand()%SN;
        int bad=0; for(int i=0;i<sn_len;i++) if(sn_body[i][0]==sn_fx&&sn_body[i][1]==sn_fy) bad=1;
        if(!bad) break;
    } while(1);
}
static void snake_tick(void){
    if(sn_dead||sn_pause) return;
    int hx=(sn_body[0][0]+sn_dx+SN)%SN, hy=(sn_body[0][1]+sn_dy+SN)%SN;
    for(int i=0;i<sn_len;i++) if(sn_body[i][0]==hx&&sn_body[i][1]==hy){ sn_dead=1; return; }
    for(int i=sn_len-1;i>0;i--){ sn_body[i][0]=sn_body[i-1][0]; sn_body[i][1]=sn_body[i-1][1]; }
    sn_body[0][0]=hx; sn_body[0][1]=hy;
    if(hx==sn_fx&&hy==sn_fy){ if(sn_len<SN*SN-2) sn_len++; sn_score++; snake_food(); if(sn_score%5==0&&sn_step_ms>70) sn_step_ms-=10; }
}
static void app_snake_draw(Win *w){
    int px=w->x+8, py=w->y+34, area=w->h-96;
    int cell = area/SN; if(cell*w->w/SN < cell) {}
    if(cell<6) cell=6;
    int gw=cell*SN;
    int gx=w->x+(w->w-gw)/2, gy=py;
    fill_rect(gx,gy,gw,gw,RGB(10,26,16));
    /* 网格线 */
    for(int i=1;i<SN;i++){
        for(int y=0;y<gw;y++) putpx(BUF,gx+i*cell,gy+y,RGB(18,40,26));
        for(int x=0;x<gw;x++) putpx(BUF,gx+x,gy+i*cell,RGB(18,40,26));
    }
    /* 食物 */
    fill_rect(gx+sn_fx*cell+cell/4, gy+sn_fy*cell+cell/4, cell/2+1, cell/2+1, RGB(240,90,90));
    /* 蛇 */
    for(int i=0;i<sn_len;i++){
        C c = i==0? RGB(180,255,190) : RGB(60,220,120);
        fill_rect(gx+sn_body[i][0]*cell+1, gy+sn_body[i][1]*cell+1, cell-2, cell-2, c);
    }
    /* 状态 */
    char sc[32]; snprintf(sc,sizeof sc,"SCORE %d",sn_score);
    draw_str(gx, gy+gw+10, sc, COL_ACCENT, 1);
    draw_str(gx+100, gy+gw+10, "WASD/方向键", COL_DIM, 1);
    if(sn_dead){
        blend_rect(gx,gy,gw,gw,RGB(0,0,0),0.65f);
        int tx=gx+gw/2-str_w("GAME OVER",2)/2;
        draw_str(tx, gy+gw/2-20, "GAME OVER", RGB(255,120,120), 2);
        char s2[40]; snprintf(s2,sizeof s2,"得分 %d",sn_score);
        draw_str(gx+gw/2-str_w(s2,1)/2, gy+gw/2+8, s2, COL_TEXT, 1);
        draw_str(gx+gw/2-str_w("按 R 重来",1)/2, gy+gw/2+24, "按 R 重来", COL_DIM, 1);
    }
    if(sn_pause && !sn_dead){
        blend_rect(gx,gy,gw,gw,RGB(0,0,0),0.5f);
        draw_str(gx+gw/2-str_w("PAUSED",2)/2, gy+gw/2-8, "PAUSED", COL_TEXT, 2);
    }
}

/* ================= 2048 ================= */
static int g2[4][4], g2_score, g2_over;
static void g2048_init(void){
    memset(g2,0,sizeof g2); g2_score=0; g2_over=0;
    for(int k=0;k<2;k++){ int i=rand()%4,j=rand()%4; if(!g2[i][j]) g2[i][j]=2; else k--; }
}
static void g2_add(void){
    int e[16][2],n=0;
    for(int i=0;i<4;i++)for(int j=0;j<4;j++) if(!g2[i][j]){e[n][0]=i;e[n][1]=j;n++;}
    if(n){ int k=rand()%n; g2[e[k][0]][e[k][1]] = rand()%10<9?2:4; }
}
static int g2_slide(int r[4]){
    int t[4],n=0,moved=0;
    for(int i=0;i<4;i++) if(r[i]) t[n++]=r[i];
    for(int i=0;i<n-1;i++) if(t[i]==t[i+1]){ t[i]*=2; for(int j=i+1;j<n-1;j++) t[j]=t[j+1]; t[--n]=0; }
    for(int i=0;i<4;i++){ if(r[i]!=t[i]) moved=1; r[i]=t[i]; }
    return moved;
}
static int g2_move(char d){
    int moved=0;
    for(int i=0;i<4;i++){
        int line[4];
        for(int j=0;j<4;j++) line[j]= d=='w'?g2[j][i] : d=='s'?g2[3-j][i] : d=='a'?g2[i][j] : g2[i][3-j];
        if(g2_slide(line)){ moved=1;
            for(int j=0;j<4;j++){
                if(d=='w') g2[j][i]=line[j];
                else if(d=='s') g2[3-j][i]=line[j];
                else if(d=='a') g2[i][j]=line[j];
                else g2[i][3-j]=line[j];
            }
        }
    }
    return moved;
}
static int g2_canmove(void){
    int bak[4][4]; memcpy(bak,g2,sizeof g2);
    int ok = g2_move('w')|g2_move('a')|g2_move('s')|g2_move('d');
    memcpy(g2,bak,sizeof g2);
    return ok;
}
static C tile_color(int v){
    switch(v){
        case 2: return RGB(238,228,218); case 4: return RGB(237,224,200);
        case 8: return RGB(242,177,121); case 16: return RGB(245,149,99);
        case 32: return RGB(246,124,95); case 64: return RGB(246,94,59);
        case 128: return RGB(237,207,114); case 256: return RGB(237,204,97);
        case 512: return RGB(237,200,80); case 1024: return RGB(237,197,63);
        default: return RGB(237,194,46);
    }
}
static void app_2048_draw(Win *w){
    int gw=w->w-40; if(gw>w->h-110) gw=w->h-110;
    int gx=w->x+(w->w-gw)/2, gy=w->y+52;
    int pad=6, cell=(gw-5*pad)/4;
    fill_rect(gx,gy,gw,gw,RGB(187,173,160));
    for(int i=0;i<4;i++) for(int j=0;j<4;j++){
        int cx=gx+pad+j*(cell+pad), cy=gy+pad+i*(cell+pad);
        int v=g2[i][j];
        round_rect(cx,cy,cell,cell,4, v? tile_color(v):RGB(205,193,180));
        if(v){
            char s[8]; snprintf(s,sizeof s,"%d",v);
            int sc = v<100?2:1;
            if(v>=1000) sc=1;
            int tw=str_w(s,sc);
            draw_str(cx+(cell-tw)/2, cy+(cell-7*sc)/2, s, v<=4?RGB(120,110,100):RGB(255,255,255), sc);
        }
    }
    char sc2[40]; snprintf(sc2,sizeof sc2,"SCORE %d",g2_score);
    draw_str(gx, w->y+36, sc2, COL_ACCENT, 1);
    if(g2_over){
        blend_rect(gx,gy,gw,gw,RGB(0,0,0),0.6f);
        draw_str(gx+gw/2-str_w("GAME OVER",2)/2, gy+gw/2-10, "GAME OVER", RGB(255,120,120), 2);
        draw_str(gx+gw/2-str_w("按 R 重来",1)/2, gy+gw/2+18, "按 R 重来", COL_TEXT, 1);
    }
}

/* ================= 猜数字 ================= */
static int gs_ans, gs_tries, gs_lo, gs_hi, gs_msgtype; /* 0无 1低 2高 3中 */
static char gs_last[64];
static void guess_init(void){ gs_ans=rand()%100+1; gs_tries=0; gs_lo=1; gs_hi=100; gs_msgtype=0; strcpy(gs_last,""); }
static void app_guess_draw(Win *w){
    char buf[64];
    draw_str(w->x+20, w->y+46, "我想了一个 1~100 的数", COL_TEXT, 1);
    snprintf(buf,sizeof buf,"范围缩小到: %d ~ %d", gs_lo, gs_hi);
    draw_str(w->x+20, w->y+64, buf, COL_DIM, 1);
    /* 大数字显示当前输入 */
    fill_rect(w->x+20,w->y+88,w->w-40,54,RGB(12,14,20));
    draw_str(w->x+30, w->y+98, w->input[0]?w->input:"?", COL_ACCENT, 4);
    /* 提示信息 */
    if(gs_msgtype==1) draw_str(w->x+20,w->y+156,"太小了! 往上猜 ↑", RGB(120,180,255),1);
    else if(gs_msgtype==2) draw_str(w->x+20,w->y+156,"太大了! 往下猜 ↓", RGB(255,180,90),1);
    else if(gs_msgtype==3){ draw_str(w->x+20,w->y+156,"答对了! 按 R 再来", COL_ACCENT,1); }
    if(gs_last[0]) draw_str(w->x+20,w->y+174,gs_last,COL_DIM,1);
    /* 快捷按钮 */
    int by=w->y+w->h-92, bw=(w->w-60)/3;
    btn(w->x+20+0*(bw+10), by, bw, 30, "1-33", 0);
    btn(w->x+20+1*(bw+10), by, bw, 30, "34-66", 0);
    btn(w->x+20+2*(bw+10), by, bw, 30, "67-100", 0);
    draw_str(w->x+20, by+40, "输入数字后按 Enter 提交", COL_DIM, 1);
}

/* ================= 井字棋 ================= */
static char tt[9]; static int tt_over; static char tt_winner;
static void tic_init(void){ memset(tt,0,9); tt_over=0; tt_winner=0; }
static const int TTL[8][3]={{0,1,2},{3,4,5},{6,7,8},{0,3,6},{1,4,7},{2,5,8},{0,4,8},{2,4,6}};
static char tic_who(void){
    for(int i=0;i<8;i++){ int a=TTL[i][0],b=TTL[i][1],c=TTL[i][2];
        if(tt[a]&&tt[a]==tt[b]&&tt[a]==tt[c]) return tt[a]; }
    for(int i=0;i<9;i++) if(!tt[i]) return 0;
    return '-';
}
static void tic_ai(void){
    for(int i=0;i<8;i++){ int a=TTL[i][0],b=TTL[i][1],c=TTL[i][2];
        int cnt=0,emp=-1;
        int v[3]={tt[a],tt[b],tt[c]};
        for(int k=0;k<3;k++){ if(v[k]=='O')cnt++; if(!v[k])emp=k; }
        if(cnt==2&&emp>=0){ tt[TTL[i][emp]]='O'; return; }
    }
    for(int i=0;i<8;i++){ int a=TTL[i][0],b=TTL[i][1],c=TTL[i][2];
        int cnt=0,emp=-1;
        int v[3]={tt[a],tt[b],tt[c]};
        for(int k=0;k<3;k++){ if(v[k]=='X')cnt++; if(!v[k])emp=k; }
        if(cnt==2&&emp>=0){ tt[TTL[i][emp]]='O'; return; }
    }
    if(!tt[4]){ tt[4]='O'; return; }
    int e[9],n=0;
    for(int i=0;i<9;i++) if(!tt[i]) e[n++]=i;
    if(n) tt[e[rand()%n]]='O';
}
static void app_tictac_draw(Win *w){
    int size=w->w-60; if(size>w->h-130) size=w->h-130;
    int gx=w->x+(w->w-size)/2, gy=w->y+48;
    int cell=size/3;
    fill_rect(gx,gy,size,size,RGB(30,34,48));
    for(int i=1;i<3;i++){
        fill_rect(gx+i*cell-1,gy,2,size,COL_BORDER);
        fill_rect(gx,gy+i*cell-1,size,2,COL_BORDER);
    }
    for(int i=0;i<9;i++){
        int cx=gx+(i%3)*cell, cy=gy+(i/3)*cell;
        if(tt[i]=='X'){
            int m=cell/4;
            for(int t=0;t<3;t++) for(int s=0;s<cell-2*m;s++){ putpx(BUF,cx+m+s,cy+m+s+t,RGB(120,200,255)); putpx(BUF,cx+cell-m-s,cy+m+s+t,RGB(120,200,255)); }
        } else if(tt[i]=='O'){
            int rad=cell/2-8, cx2=cx+cell/2, cy2=cy+cell/2;
            for(int a=0;a<360;a+=2){
                float rr=a*3.14159f/180;
                for(int t=0;t<3;t++) putpx(BUF, cx2+(int)(sinf(rr)*(rad-t)), cy2+(int)(cosf(rr)*(rad-t)), RGB(255,190,100));
            }
        }
        tt[i]=tt[i]; /* noop */
    }
    const char *st = tt_over ? (tt_winner=='X'?"你赢了! 按 R 重来": tt_winner=='O'?"AI 赢了, 按 R 重来":"平局! 按 R 重来")
                             : (tt[0]==0&&tt[1]==0&&tt[2]==0&&tt[3]==0?"点击格子落子 X":"轮到你 (X)");
    draw_str(gx+size/2-str_w(st,1)/2, gy+size+14, st, tt_over?COL_ACCENT:COL_DIM, 1);
}

/* ================= 音乐 (彩蛋) ================= */
static void app_music_draw(Win *w){
    draw_str(w->x+20,w->y+46,"♪ MiniDOS Music Player",COL_ACCENT,2);
    draw_str(w->x+20,w->y+80,"正在播放: 小星星 (Twinkle)",COL_TEXT,1);
    /* 简单的跳动条 */
    static int bars[16];
    time_t t=time(0);
    for(int i=0;i<16;i++){
        int h=(int)((sinf((float)t*0.6f+i*0.7f)+1.2f)*24);
        if(h<4) h=4;
        fill_rect(w->x+20+i*18, w->y+150-h, 12, h, i%2?COL_ACCENT:RGB(90,160,255));
    }
    draw_str(w->x+20,w->y+170,"按 P 播放 / S 停止",COL_DIM,1);
}


/* =============================================================
 *  窗口管理器 + 桌面 + 任务栏 + 主循环
 * ============================================================= */
#define TASKBAR_H 34
#define TITLE_H 24

typedef struct { const char *name; const char *sub; int type; int app; C icon_col; int x,y; } DeskIcon;
static DeskIcon icons[] = {
    {"TERMINAL", "命令行",  0, 0, RGB(90,200,160), 0,0},
    {"SNAKE",    "贪吃蛇", 10, 0, RGB(60,220,120), 0,0},
    {"2048",     "数字拼图",11, 0, RGB(240,170,80), 0,0},
    {"GUESS",    "猜数字", 12, 0, RGB(120,170,255), 0,0},
    {"TICTAC",   "井字棋", 13, 0, RGB(255,140,140), 0,0},
    {"FILES",    "文件",    1, 0, RGB(200,190,120), 0,0},
    {"EDITOR",   "编辑器",  2, 0, RGB(230,230,230), 0,0},
    {"CLOCK",    "时钟",    3, 0, RGB(140,200,255), 0,0},
    {"CALC",     "计算器",  4, 0, RGB(180,180,255), 0,0},
    {"MUSIC",    "音乐",    14, 0, RGB(255,140,220), 0,0},
    {"ABOUT",    "关于",    5, 0, RGB(160,160,180), 0,0},
};
#define NICON ((int)(sizeof icons/sizeof icons[0]))

static void win_open(int type);
static void open_app_by_name(const char *n){
    for(int i=0;i<NICON;i++) if(!strcasecmp(n,icons[i].name)) { win_open(icons[i].type); return; }
    if(!strcasecmp(n,"files")) win_open(1);
    if(!strcasecmp(n,"editor")) win_open(2);
    if(!strcasecmp(n,"calc")) win_open(4);
}

static void win_setup(Win *w,int type){
    memset(w,0,sizeof *w);
    w->open=1; w->type=type;
    w->w = SW*58/100; w->h = SH*62/100;
    if(w->w>720) w->w=720; if(w->h>520) w->h=520;
    if(w->w<300) w->w=300; if(w->h<260) w->h=260;
    w->x = (SW-w->w)/2 + (rand()%80-40);
    w->y = (SH-TASKBAR_H-w->h)/2 + (rand()%60-30);
    if(w->x<0)w->x=0; if(w->y<0)w->y=0;
    if(w->x+w->w>SW) w->x=SW-w->w;
    if(w->y+w->h>SH-TASKBAR_H) w->y=SH-TASKBAR_H-w->h;
    w->z = ++ztop;
    switch(type){
        case 0: w->title="MiniDOS 终端"; term_add(w,"MiniDOS Desktop 1.0"); term_add(w,"输入 help 查看命令"); break;
        case 1: w->title="文件管理器"; break;
        case 2: w->title="文本编辑器"; strcpy(w->text[0],"欢迎使用 MiniDOS 图形系统。"); strcpy(w->text[1],"这里可以输入文字。"); w->curline=1; break;
        case 3: w->title="时钟"; break;
        case 4: w->title="计算器"; break;
        case 5: w->title="关于本系统"; break;
        case 10: w->title="贪吃蛇"; sn_step_ms=130; snake_init(); break;
        case 11: w->title="2048"; g2048_init(); break;
        case 12: w->title="猜数字"; guess_init(); break;
        case 13: w->title="井字棋"; tic_init(); break;
        case 14: w->title="音乐播放器"; break;
    }
}
static void win_open(int type){
    for(int i=0;i<MAX_WIN;i++){
        if(wins[i].open && wins[i].type==type){
            wins[i].z=++ztop; active_win=i;
            if(type==10) snake_init();
            if(type==11) g2048_init();
            if(type==12) guess_init();
            if(type==13) tic_init();
            return;
        }
    }
    for(int i=0;i<MAX_WIN;i++){
        if(!wins[i].open){ win_setup(&wins[i],type); active_win=i; return; }
    }
}

/* 桌面壁纸 */
static void draw_wallpaper(void){
    gradient(0,0,SW,SH,RGB(20,26,44),RGB(38,32,60));
    /* 装饰: 柔光圆点 */
    for(int i=0;i<40;i++){
        int x=(i*137)%SW, y=(i*89)%SH;
        for(int r=0;r<60;r+=4){
            for(int a=0;a<360;a+=12){
                float rad=a*3.14159f/180;
                putpx(BUF,x+(int)(sinf(rad)*r), y+(int)(cosf(rad)*r), RGB(28+i%6,34+i%5,58+i%7));
            }
        }
    }
    /* 底部渐暗 */
    for(int y=SH-TASKBAR_H-120; y<SH-TASKBAR_H; y++){
        float t=(float)(y-(SH-TASKBAR_H-120))/120;
        blend_rect(0,y,SW,1,RGB(10,12,20),t*0.55f);
    }
}

/* 图标绘制 */
static void draw_icon(DeskIcon *ic,int hov){
    int s=44;
    int x=ic->x, y=ic->y;
    if(hov){ round_rect(x-6,y-6,s+12,s+34,6,RGB(255,255,255)); blend_rect(x-6,y-6,s+12,s+34,RGB(255,255,255),0.12f); }
    round_rect(x,y,s,s,8,ic->icon_col);
    /* 简单图形标识 */
    if(!strcmp(ic->name,"SNAKE")){
        for(int i=0;i<4;i++) fill_rect(x+10+i*7, y+18+(i%2)*6, 6, 6, RGB(20,40,25));
    } else if(!strcmp(ic->name,"2048")){
        draw_str(x+10,y+12,"2",RGB(90,60,20),2); draw_str(x+26,y+24,"4",RGB(90,60,20),2);
    } else if(!strcmp(ic->name,"TERMINAL")){
        draw_str(x+8,y+10,">",RGB(10,30,20),2); fill_rect(x+22,y+20,14,3,RGB(10,30,20));
    } else if(!strcmp(ic->name,"TICTAC")){
        fill_rect(x+20,y+8,2,28,RGB(60,20,20)); fill_rect(x+8,y+20,28,2,RGB(60,20,20));
    } else if(!strcmp(ic->name,"CLOCK")){
        for(int a=0;a<360;a+=10){ float r=a*3.14159f/180;
            putpx(BUF,x+22+(int)(sinf(r)*13),y+22-(int)(cosf(r)*13),RGB(20,40,70)); }
        fill_rect(x+21,y+16,2,7,RGB(20,40,70));
    } else if(!strcmp(ic->name,"MUSIC")){
        fill_rect(x+16,y+12,3,18,RGB(60,20,45)); fill_rect(x+16,y+12,12,3,RGB(60,20,45)); fill_rect(x+25,y+15,3,12,RGB(60,20,45));
    } else if(!strcmp(ic->name,"GUESS")){
        draw_str(x+16,y+10,"?",RGB(20,35,70),3);
    } else if(!strcmp(ic->name,"FILES")){
        round_rect(x+9,y+12,26,22,3,RGB(60,55,25)); fill_rect(x+9,y+12,12,5,RGB(60,55,25));
    } else if(!strcmp(ic->name,"EDITOR")){
        for(int i=0;i<5;i++) fill_rect(x+10,y+12+i*6,24,2,RGB(80,80,90));
    } else if(!strcmp(ic->name,"CALC")){
        fill_rect(x+10,y+9,24,10,RGB(60,60,90));
        for(int i=0;i<3;i++)for(int j=0;j<3;j++) fill_rect(x+11+j*8,y+23+i*6,5,4,RGB(60,60,90));
    } else if(!strcmp(ic->name,"ABOUT")){
        draw_str(x+18,y+9,"i",RGB(40,40,50),4);
    } else {
        draw_str(x+16,y+14,"?",RGB(20,20,30),2);
    }
    /* 标签 */
    int tw=str_w(ic->name,1);
    draw_str_clip(x+s/2-tw/2, y+s+6, s+16, ic->name, RGB(235,240,255),1);
}

/* 任务栏 */
static void draw_taskbar(void){
    int ty=SH-TASKBAR_H;
    fill_rect(0,ty,SW,TASKBAR_H,COL_TASKBAR);
    fill_rect(0,ty,SW,1,RGB(50,60,85));
    draw_str(10,ty+11,"MiniDOS",COL_ACCENT,1);
    fill_rect(80,ty+10,1,14,RGB(50,60,85));
    /* 窗口按钮 */
    int bx=90;
    for(int i=0;i<MAX_WIN;i++){
        if(!wins[i].open) continue;
        int bw=str_w(wins[i].title,1)+20;
        if(bx+bw>SW-140) break;
        int act = (i==active_win);
        round_rect(bx,ty+6,bw,TASKBAR_H-12,4, act?COL_TITLE_A:COL_BUTTON);
        draw_str_clip(bx+8,ty+13,bw-14,wins[i].title,COL_TEXT,1);
        bx+=bw+6;
    }
    /* 右侧时钟 */
    time_t t=time(0); struct tm *tm=localtime(&t);
    char clk[16]; snprintf(clk,sizeof clk,"%02d:%02d",tm->tm_hour,tm->tm_min);
    draw_str(SW-str_w(clk,1)-12,ty+11,clk,COL_TEXT,1);
}

/* 窗口绘制 */
static void draw_window(Win *w,int is_active){
    /* 阴影 */
    for(int i=6;i>0;i--) blend_rect(w->x-i,w->y-i+2,w->w+2*i,w->h+2*i,RGB(0,0,0),0.05f);
    /* 主体 */
    round_rect(w->x,w->y,w->w,w->h,6,COL_BG2);
    round_frame(w->x,w->y,w->w,w->h,6, is_active?COL_TITLE_A:COL_BORDER);
    /* 标题栏 */
    int th=TITLE_H;
    fill_rect(w->x+1,w->y+1,w->w-2,th, is_active?COL_TITLE_A:COL_TITLE);
    /* 圆角贴合 */
    round_rect(w->x,w->y,w->w,th+6,6, is_active?COL_TITLE_A:COL_TITLE);
    round_frame(w->x,w->y,w->w,th+6,6, is_active?COL_TITLE_A:COL_BORDER);
    draw_str(w->x+10,w->y+9,w->title,COL_TEXT,1);
    /* 关闭按钮 */
    int cx=w->x+w->w-20, cy=w->y+7;
    round_rect(cx,cy,14,12,3,RGB(220,80,80));
    draw_str(cx+4,cy+2,"x",RGB(255,255,255),1);
    /* 内容 */
    switch(w->type){
        case 0: app_terminal_draw(w); break;
        case 1: app_files_draw(w); break;
        case 2: app_editor_draw(w); break;
        case 3: app_clock_draw(w); break;
        case 4: app_calc_draw(w); break;
        case 5: app_about_draw(w); break;
        case 10: app_snake_draw(w); break;
        case 11: app_2048_draw(w); break;
        case 12: app_guess_draw(w); break;
        case 13: app_tictac_draw(w); break;
        case 14: app_music_draw(w); break;
    }
}

/* 鼠标光标 */
static void draw_cursor(void){
    C c1=RGB(255,255,255), c2=RGB(0,0,0);
    for(int i=0;i<14;i++){
        for(int j=0;j<=i;j++){
            putpx(BUF,Mx+j,My+i,c1);
            if(j==0||j==i) putpx(BUF,Mx+j+1,My+i,c2);
        }
    }
    for(int i=14;i<20;i++) for(int j=i-6;j<=14;j++) putpx(BUF,Mx+j,My+i,c1);
    for(int i=0;i<20;i++) putpx(BUF,Mx-1,My+i,c2);
}

/* 命中检测 */
static int icon_at(int x,int y,int *idx){
    for(int i=0;i<NICON;i++){
        DeskIcon *ic=&icons[i];
        if(x>=ic->x-6&&x<=ic->x+50&&y>=ic->y-6&&y<=ic->y+60){ *idx=i; return 1; }
    }
    return 0;
}
static int win_at(int x,int y){
    int best=-1; int bz=-1;
    for(int i=0;i<MAX_WIN;i++){
        if(!wins[i].open) continue;
        Win *w=&wins[i];
        if(x>=w->x&&x<w->x+w->w&&y>=w->y&&y<w->y+w->h){
            if(w->z>bz){ bz=w->z; best=i; }
        }
    }
    return best;
}
static void close_win(int i){
    wins[i].open=0;
    if(active_win==i){ active_win=-1;
        int bz=-1; for(int k=0;k<MAX_WIN;k++) if(wins[k].open&&wins[k].z>bz){bz=wins[k].z;active_win=k;}
    }
}

/* 布局图标 (列优先) */
static void layout_icons(void){
    int x=24, y=24, row=0;
    for(int i=0;i<NICON;i++){
        icons[i].x=x; icons[i].y=y+row*74;
        row++;
        if(icons[i].y+74 > SH-TASKBAR_H-80){ row=0; x+=76; }
    }
}

/* 时间 */
static unsigned long now_ms(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
    return ts.tv_sec*1000 + ts.tv_nsec/1000000;
}

/* ============ 应用实现 ============ */
static const char *FILES[]={"README.TXT","DESKTOP.C","SHELL.BIN","NOTES.TXT","minidos.iso","WELCOME.TXT"};
static const int FSIZE[]={1,68,44,0,66,1};
static void term_add_impl(Win *w, const char *s){
    if(w->nlines>=40){
        for(int i=1;i<40;i++) memcpy(w->lines[i-1],w->lines[i],96);
        w->nlines=39;
    }
    snprintf(w->lines[w->nlines],96,"%s",s); w->nlines++;
}
static double p_sum(void);
static const char *gp; static int gerr;
static double p_atom(void){
    while(*gp==' ') gp++;
    if(*gp=='('){ gp++; double v=p_sum(); while(*gp==' ')gp++; if(*gp==')')gp++; return v; }
    if(*gp=='-'){ gp++; return -p_atom(); }
    if(*gp=='+'){ gp++; return p_atom(); }
    double v=0; int any=0;
    while((*gp>='0'&&*gp<='9')){ any=1; v=v*10+(*gp-'0'); gp++; }
    if(*gp=='.'){ any=1; gp++; double f=0.1; while(*gp>='0'&&*gp<='9'){ v+=(*gp-'0')*f; f/=10; gp++; } }
    if(!any){ gerr=1; return 0; }
    return v;
}
static double p_mul(void){
    double v=p_atom();
    for(;;){ while(*gp==' ')gp++;
        if(*gp=='*'){ gp++; v*=p_atom(); }
        else if(*gp=='/'){ gp++; double d=p_atom(); if(d==0){gerr=2;return 0;} v/=d; }
        else break; }
    return v;
}
static double p_sum(void){
    double v=p_mul();
    for(;;){ while(*gp==' ')gp++;
        if(*gp=='+'){ gp++; v+=p_mul(); }
        else if(*gp=='-'){ gp++; v-=p_mul(); }
        else break; }
    return v;
}
static double calc_eval(const char *e, int *err){
    gp=e; gerr=0;
    double r=p_sum();
    while(*gp==' ')gp++;
    if(*gp) gerr=1;
    *err=gerr;
    return r;
}
static const char* eval_expr(const char *e){
    static char out[64];
    int err=0; double v=calc_eval(e,&err);
    if(err==1) strcpy(out,"语法错误"); else if(err==2) strcpy(out,"除数不能为0");
    else {
        if(v==(long long)v) snprintf(out,sizeof out,"=%lld",(long long)v);
        else snprintf(out,sizeof out,"=%.4f",v);
    }
    return out;
}

static void app_terminal_draw(Win *w){
    int px=w->x+10, py=w->y+TITLE_H+8;
    int bh=w->h-TITLE_H-42;
    fill_rect(px,py,w->w-20,bh,RGB(10,12,18));
    int y=py+6, maxl=bh/11;
    int start = w->nlines>maxl? w->nlines-maxl : 0;
    for(int i=start;i<w->nlines;i++){ draw_str_clip(px+6,y,w->w-36,w->lines[i],COL_ACCENT,1); y+=11; }
    int iy=w->y+w->h-30;
    draw_str(px+6,iy,">",COL_TEXT,1);
    draw_str_clip(px+20,iy,w->w-40,w->input,COL_TEXT,1);
    fill_rect(px+20+str_w(w->input,1),iy-1,5,9,COL_ACCENT);
}

static void app_files_draw(Win *w){
    int x=w->x+10, y=w->y+TITLE_H+8, wd=w->w-20, ht=w->h-TITLE_H-42;
    fill_rect(x,y,wd,ht,RGB(14,17,26));
    int iy=y+6;
    for(int i=0;i<6;i++){
        if(iy+18>y+ht) break;
        if(i==w->sel) fill_rect(x+2,iy-3,wd-6,16,COL_SEL);
        round_rect(x+6,iy-1,12,12,2,RGB(200,190,120));
        draw_str(x+24,iy,FILES[i],COL_TEXT,1);
        char sz[24]; snprintf(sz,sizeof sz,"%d KB",FSIZE[i]);
        draw_str(x+wd-70,iy,sz,COL_DIM,1);
        iy+=18;
    }
}

static void app_editor_draw(Win *w){
    int x=w->x+10, y=w->y+TITLE_H+8, wd=w->w-20, ht=w->h-TITLE_H-42;
    fill_rect(x,y,wd,ht,RGB(250,248,238));
    int iy=y+6, maxr=ht/12;
    for(int i=0;i<20&&i<maxr;i++){
        char n[6]; snprintf(n,sizeof n,"%2d",i+1);
        draw_str(x+4,iy,n,RGB(150,140,120),1);
        draw_str(x+24,iy,w->text[i],RGB(30,30,40),1);
        iy+=12;
    }
    if(w->curline>=0&&w->curline<maxr){
        int cy=y+6+w->curline*12;
        fill_rect(x+24+str_w(w->text[w->curline],1),cy-1,4,9,RGB(60,110,200));
    }
}

static void app_clock_draw(Win *w){
    time_t t=time(0); struct tm *tm=localtime(&t);
    int cx=w->x+w->w/2;
    char buf[16]; snprintf(buf,sizeof buf,"%02d:%02d:%02d",tm->tm_hour,tm->tm_min,tm->tm_sec);
    draw_str(cx-str_w(buf,3)/2, w->y+TITLE_H+18, buf, COL_ACCENT, 3);
    char ds[32]; snprintf(ds,sizeof ds,"%04d-%02d-%02d",tm->tm_year+1900,tm->tm_mon+1,tm->tm_mday);
    draw_str(cx-str_w(ds,1)/2, w->y+TITLE_H+48, ds, COL_DIM, 1);
    int cy=w->y+w->h-100, r=52;
    if(w->h<300) r=40;
    for(int a=0;a<360;a+=6){
        float rad=a*3.14159f/180;
        int x1=cx+(int)(sinf(rad)*(r-5)), y1=cy-(int)(cosf(rad)*(r-5));
        int x2=cx+(int)(sinf(rad)*r), y2=cy-(int)(cosf(rad)*r);
        C c=(a%90==0)?COL_TEXT:COL_DIM;
        for(int s=0;s<3;s++){ putpx(BUF,x1,y1+s,c); putpx(BUF,x2,y2+s,c); }
    }
    int hh=tm->tm_hour%12, mm=tm->tm_min, ss=tm->tm_sec;
    float ha=(hh*30+mm*0.5f)*3.14159f/180, ma=mm*6*3.14159f/180, sa=ss*6*3.14159f/180;
    for(int s=0;s<r/2;s++) putpx(BUF,cx+(int)(sinf(ha)*s),cy-(int)(cosf(ha)*s),COL_TEXT);
    for(int s=0;s<r*3/4;s++) putpx(BUF,cx+(int)(sinf(ma)*s),cy-(int)(cosf(ma)*s),COL_TEXT);
    for(int s=0;s<r-6;s++) putpx(BUF,cx+(int)(sinf(sa)*s),cy-(int)(cosf(sa)*s),RGB(255,90,90));
    fill_rect(cx-2,cy-2,4,4,COL_ACCENT);
}

static void app_calc_draw(Win *w){
    int x=w->x+10, y=w->y+TITLE_H+8, wd=w->w-20;
    fill_rect(x,y,wd,40,RGB(12,14,20));
    draw_str_clip(x+8,y+8,wd-16,w->expr[0]?w->expr:"0",COL_TEXT,2);
    const char *k[5][4]={{"7","8","9","/"},{"4","5","6","*"},{"1","2","3","-"},{"0",".","=","+"},{"C","(",")","<"}};
    int bh=(w->h-TITLE_H-42-50-16)/5 - 4;
    int bw=(wd-24)/4;
    if(bh<18) bh=18;
    for(int r=0;r<5&&y+50+r*(bh+4)+bh<w->y+w->h-30;r++) for(int c=0;c<4;c++){
        int bx=x+4+c*(bw+4), by=y+50+r*(bh+4);
        int iseq = !strcmp(k[r][c],"=");
        round_rect(bx,by,bw,bh,4, iseq?COL_ACCENT:COL_BUTTON);
        draw_str(bx+bw/2-3,by+bh/2-4,k[r][c],iseq?RGB(15,40,30):COL_TEXT,1);
    }
}

static void app_about_draw(Win *w){
    int x=w->x+24, y=w->y+TITLE_H+20;
    draw_str(x,y,"MiniDOS Desktop 1.0",COL_ACCENT,2);
    y+=28;
    draw_str(x,y,"自研 Framebuffer 图形层",COL_TEXT,1); y+=14;
    draw_str(x,y,"不使用 X11 / Wayland / SDL",COL_DIM,1); y+=14;
    draw_str(x,y,"全部绘制代码约 800 行 C",COL_DIM,1); y+=24;
    draw_str(x,y,"内核  : Linux 6.x",COL_TEXT,1); y+=14;
    draw_str(x,y,"图形  : /dev/fb0 直接绘制",COL_TEXT,1); y+=14;
    draw_str(x,y,"输入  : /dev/input/mice + evdev",COL_TEXT,1); y+=14;
    draw_str(x,y,"字体  : 内置 5x7 位图字库",COL_TEXT,1); y+=24;
    draw_str(x,y,"点击桌面图标启动应用",COL_DIM,1);
}

/* ---------- 事件 ---------- */
static void focus_click(int i){ if(i>=0){ wins[i].z=++ztop; active_win=i; } }

static void handle_click(int x,int y){
    /* 任务栏 */
    if(y>=SH-TASKBAR_H){
        int bx=90;
        for(int i=0;i<MAX_WIN;i++){
            if(!wins[i].open) continue;
            int bw=str_w(wins[i].title,1)+20;
            if(x>=bx&&x<=bx+bw){ focus_click(i); return; }
            bx+=bw+6;
        }
        return;
    }
    int wi=win_at(x,y);
    if(wi>=0){
        Win *w=&wins[wi];
        /* 关闭按钮 */
        if(x>=w->x+w->w-20&&x<=w->x+w->w-6&&y>=w->y+7&&y<=w->y+19){ close_win(wi); return; }
        /* 标题栏拖动 */
        if(y<w->y+TITLE_H){ focus_click(wi); w->dragging=1; w->drag_dx=x-w->x; w->drag_dy=y-w->y; return; }
        focus_click(wi);
        /* 内容点击 */
        int cx=x-w->x, cy=y-w->y;
        switch(w->type){
            case 4: { /* 计算器 */
                int wd=w->w-20, bh=(w->h-TITLE_H-42-50-16)/5-4, bw=(wd-24)/4;
                if(bh<18)bh=18;
                int lx=cx-14, ly=cy-(TITLE_H+16+50);
                if(lx>=0&&ly>=0){
                    int c=lx/(bw+4), r=ly/(bh+4);
                    if(c>=0&&c<4&&r>=0&&r<5){
                        const char *k[5][4]={{"7","8","9","/"},{"4","5","6","*"},{"1","2","3","-"},{"0",".","=","+"},{"C","(",")","<"}};
                        const char *key=k[r][c];
                        int L=strlen(w->expr);
                        if(!strcmp(key,"C")) w->expr[0]=0;
                        else if(!strcmp(key,"<")){ if(L>0) w->expr[L-1]=0; }
                        else if(!strcmp(key,"=")){
                            int err=0; double v=calc_eval(w->expr,&err);
                            if(!err){ if(v==(long long)v) snprintf(w->expr,sizeof w->expr,"%lld",(long long)v); else snprintf(w->expr,sizeof w->expr,"%.4f",v); }
                        } else if(L<60){ strncat(w->expr,key,1); }
                    }
                }
                break; }
            case 1: { if(cy>TITLE_H+8&&cy<w->h-30){ int idx=(cy-TITLE_H-8-3)/18; if(idx>=0&&idx<6) w->sel=idx; } break; }
            case 13: { /* 井字棋 */
                if(tt_over) break;
                int size=w->w-60; if(size>w->h-130) size=w->h-130;
                int gx=(w->w-size)/2, gy=TITLE_H+26;
                if(cx>=gx&&cx<gx+size&&cy>=gy&&cy<gy+size){
                    int cell=size/3, idx=((cy-gy)/cell)*3+((cx-gx)/cell);
                    if(idx>=0&&idx<9&&!tt[idx]){ tt[idx]='X'; tic_ai(); char W=tic_who(); if(W){ tt_over=1; tt_winner=W; } }
                }
                break; }
            case 12: { /* 猜数字快捷 */
                int by=w->h-92, bw=(w->w-60)/3, ly=cy-by, lx=cx-20;
                if(ly>=0&&ly<=30&&lx>=0&&lx<3*(bw+10)){
                    int which=lx/(bw+10);
                    int g[3]={17,50,84};
                    if(which>=0&&which<3){
                        int n=g[which]; gs_tries++;
                        if(n<gs_ans){ gs_msgtype=1; if(n+1>gs_lo)gs_lo=n+1; snprintf(gs_last,sizeof gs_last,"猜了 %d, 第 %d 次",n,gs_tries);}
                        else if(n>gs_ans){ gs_msgtype=2; if(n-1<gs_hi)gs_hi=n-1; snprintf(gs_last,sizeof gs_last,"猜了 %d, 第 %d 次",n,gs_tries);}
                        else { gs_msgtype=3; snprintf(gs_last,sizeof gs_last,"用了 %d 次",gs_tries); }
                    }
                }
                break; }
            default: break;
        }
        return;
    }
    /* 桌面图标 */
    int idx;
    if(icon_at(x,y,&idx)) win_open(icons[idx].type);
}

/* ---------- 键盘 ---------- */
static void handle_key(int k,int shift){
    if(active_win<0) return;
    Win *w=&wins[active_win];
    /* 游戏按键 */
    if(w->type==10){
        switch(k){
            case KEY_UP: if(sn_dy==0){sn_dx=0;sn_dy=-1;} break;
            case KEY_DOWN: if(sn_dy==0){sn_dx=0;sn_dy=1;} break;
            case KEY_LEFT: if(sn_dx==0){sn_dx=-1;sn_dy=0;} break;
            case KEY_RIGHT: if(sn_dx==0){sn_dx=1;sn_dy=0;} break;
            case KEY_R: snake_init(); break;
            case KEY_P: sn_pause=!sn_pause; break;
            case KEY_Q: close_win(active_win); break;
        }
        return;
    }
    if(w->type==11){
        char d=0;
        if(k==KEY_UP) d='w'; if(k==KEY_DOWN) d='s';
        if(k==KEY_LEFT) d='a'; if(k==KEY_RIGHT) d='d';
        if(k==KEY_R){ g2048_init(); return; }
        if(k==KEY_Q){ close_win(active_win); return; }
        if(d&&!g2_over){ if(g2_move(d)){ g2_add(); if(!g2_canmove()) g2_over=1; } }
        return;
    }
    if(w->type==13){
        if(k==KEY_R){ tic_init(); return; }
        if(k>=KEY_1&&k<=KEY_9&&!tt_over){
            int idx=k-KEY_1;
            if(!tt[idx]){ tt[idx]='X'; tic_ai(); char W=tic_who(); if(W){tt_over=1;tt_winner=W;} }
        }
        return;
    }
    if(w->type==12){
        if(k==KEY_R){ guess_init(); return; }
        return;
    }
    if(w->type==14){
        return;
    }
    if(w->type==5||w->type==1||w->type==3){ return; }
    /* 文本输入 */
    char ch=0;
    if(k>=KEY_A&&k<=KEY_Z) ch = 'A'+(k-KEY_A);
    else if(k>=KEY_1&&k<=KEY_9) ch='1'+(k-KEY_1);
    else if(k==KEY_0) ch='0';
    else if(k==KEY_SPACE) ch=' ';
    else if(k==KEY_DOT) ch='.';
    else if(k==KEY_MINUS) ch='-';
    else if(k==KEY_EQUAL) ch='=';
    else if(k==KEY_SLASH) ch='/';
    else if(k==KEY_APOSTROPHE) ch='\'';
    else if(k==KEY_SEMICOLON) ch=';';
    else if(k==KEY_COMMA) ch=',';
    else if(k==KEY_LEFTBRACE) ch='[';
    else if(k==KEY_RIGHTBRACE) ch=']';
    else if(k==KEY_BACKSLASH) ch='\\';
    else if(k==KEY_GRAVE) ch='`';
    else if(k==KEY_102ND) ch='\\';
    if(ch&&!shift&&ch>='A'&&ch<='Z') ch = ch-'A'+'a';
    if(shift){
        if(ch>='a'&&ch<='z') ch=ch-'a'+'A';
        else if(k==KEY_1) ch='!'; else if(k==KEY_2) ch='@'; else if(k==KEY_3) ch='#';
        else if(k==KEY_4) ch='$'; else if(k==KEY_5) ch='%'; else if(k==KEY_6) ch='^';
        else if(k==KEY_7) ch='&'; else if(k==KEY_8) ch='*'; else if(k==KEY_9) ch='(';
        else if(k==KEY_0) ch=')'; else if(k==KEY_MINUS) ch='_'; else if(k==KEY_EQUAL) ch='+';
    }
    if(w->type==4){ /* 计算器 */
        int L=strlen(w->expr);
        if(k==KEY_BACKSPACE){ if(L>0) w->expr[L-1]=0; }
        else if(k==KEY_ENTER){ int err=0; double v=calc_eval(w->expr,&err);
            if(!err){ if(v==(long long)v) snprintf(w->expr,sizeof w->expr,"%lld",(long long)v); else snprintf(w->expr,sizeof w->expr,"%.4f",v);} }
        else if(ch&&strchr("0123456789+-*/().=",ch)&&L<60){ w->expr[L]=ch; w->expr[L+1]=0; }
        return;
    }
    if(w->type==2){ /* 编辑器 */
        int L=strlen(w->text[w->curline]);
        if(k==KEY_ENTER){ if(w->curline<19){ w->curline++; w->text[w->curline][0]=0; } }
        else if(k==KEY_BACKSPACE){ if(L>0) w->text[w->curline][L-1]=0; else if(w->curline>0){ w->curline--; w->text[w->curline][strlen(w->text[w->curline])]=0; } }
        else if(k==KEY_UP&&w->curline>0) w->curline--;
        else if(k==KEY_DOWN&&w->curline<19) w->curline++;
        else if(ch&&L<95){ w->text[w->curline][L]=ch; w->text[w->curline][L+1]=0; }
        return;
    }
    if(w->type==0){ /* 终端 */
        int L=strlen(w->input);
        if(k==KEY_ENTER||k==96||k==KEY_KPENTER){
            char cmd[128]; strcpy(cmd,w->input);
            w->input[0]=0;
            if(cmd[0]){
                char l[200]; snprintf(l,sizeof l,"> %s",cmd); term_add_impl(w,l);
                /* 执行 */
                char c0[64]; sscanf(cmd,"%63s",c0);
                if(!strcasecmp(c0,"help")){
                    term_add_impl(w,"命令: help dir ls date time clear free uname");
                    term_add_impl(w,"      neofetch ver calc open exit");
                } else if(!strcmp(c0,"clear")||!strcmp(c0,"cls")) w->nlines=0;
                else if(!strcmp(c0,"ls")||!strcmp(c0,"dir")){
                    term_add_impl(w,"README.TXT   DESKTOP.C   SHELL.BIN");
                    term_add_impl(w,"NOTES.TXT    minidos.iso");
                } else if(!strcmp(c0,"date")){ time_t t=time(0); struct tm *tm=localtime(&t);
                    snprintf(l,sizeof l,"%04d-%02d-%02d",tm->tm_year+1900,tm->tm_mon+1,tm->tm_mday); term_add_impl(w,l);
                } else if(!strcmp(c0,"time")){ time_t t=time(0); struct tm *tm=localtime(&t);
                    snprintf(l,sizeof l,"%02d:%02d:%02d",tm->tm_hour,tm->tm_min,tm->tm_sec); term_add_impl(w,l);
                } else if(!strcmp(c0,"ver")){ term_add_impl(w,"MiniDOS Desktop 1.0 (Framebuffer GUI)"); }
                else if(!strcmp(c0,"uname")){ term_add_impl(w,"MiniDOS 1.0 x86_64"); }
                else if(!strcmp(c0,"free")){
                    long mt=0,ma=0; FILE *f=fopen("/proc/meminfo","r");
                    if(f){ char key[64],unit[16]; long v;
                        while(fscanf(f,"%63s %ld %15s",key,&v,unit)==3){
                            if(!strcmp(key,"MemTotal:")) mt=v/1024;
                            if(!strcmp(key,"MemAvailable:")) ma=v/1024; } fclose(f); }
                    snprintf(l,sizeof l,"Mem: %ldMB/ %ldMB",ma,mt); term_add_impl(w,l);
                } else if(!strcmp(c0,"neofetch")){
                    term_add_impl(w,"   ___  ___   MiniDOS 1.0");
                    term_add_impl(w,"  /  |/  /   GUI: Framebuffer");
                    term_add_impl(w," /_/  /_/    Shell: msh");
                } else if(!strcmp(c0,"calc")){ term_add_impl(w, eval_expr(cmd+4)); }
                else if(!strcmp(c0,"open")){
                    char what[32]={0}; sscanf(cmd+4,"%31s",what);
                    open_app_by_name(what);
                    snprintf(l,sizeof l,"open: %s",what[0]?what:"(none)"); term_add_impl(w,l);
                } else if(!strcmp(c0,"exit")){ close_win(active_win); return; }
                else { snprintf(l,sizeof l,"bad command '%s' - try help",c0); term_add_impl(w,l); }
            }
            return;
        }
        if(k==KEY_BACKSPACE){ if(L>0) w->input[L-1]=0; return; }
        if(ch&&L<120){ w->input[L]=ch; w->input[L+1]=0; }
        return;
    }
}

/* ---------- 初始化 ---------- */
static int fb_init(void){
    int fd=open("/dev/fb0",O_RDWR);
    if(fd<0) return -1;
    struct fb_var_screeninfo vi; struct fb_fix_screeninfo fi;
    if(ioctl(fd,FBIOGET_VSCREENINFO,&vi)<0) return -2;
    ioctl(fd,FBIOGET_FSCREENINFO,&fi);
    SW=vi.xres; SH=vi.yres; BPP=vi.bits_per_pixel; LINE=fi.line_length;
    bytespp = BPP/8;
    /* 检测是否需要 BGR->RGB 交换 */
    needs_swap = (vi.red.offset==0 && vi.blue.offset==16);
    size_t sz=(size_t)LINE*SH;
    FB=mmap(0,sz,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    if(FB==MAP_FAILED) return -3;
    BUF=malloc(sz);
    memset(BUF,0,sz);
    return fd;
}

int main(int argc,char **argv){
    srand(time(0));
    int fb=fb_init();
    if(fb<0){
        fprintf(stderr,"MiniDOS Desktop: 无法打开 /dev/fb0 (%d)\n",fb);
        fprintf(stderr,"请确认内核已加载 simpledrm/vesafb 且启动参数含 video=\n");
        return 1;
    }
    if(bytespp==4 && needs_swap) bytespp=4; /* 统一按 BGR 写, 后面可调整 */
    /* 输入设备 */
    mouse_fd=open("/dev/input/mice",O_RDONLY|O_NONBLOCK);
    kb_fd=open("/dev/input/event0",O_RDONLY|O_NONBLOCK);
    if(kb_fd<0) kb_fd=open("/dev/input/event1",O_RDONLY|O_NONBLOCK);
    layout_icons();
    /* 默认打开终端, 让人看到"系统起来了" */
    win_setup(&wins[0],0); active_win=0;
    win_setup(&wins[1],10); active_win=1;   /* 贪吃蛇演示 */
    wins[1].x = wins[0].x+60; wins[1].y = wins[0].y+40;
    wins[0].z = 1; wins[1].z = 2;
    active_win = 1;
    int last_mx=Mx,last_my=My;
    unsigned long last=now_ms();
    for(;;){
        /* 事件 */
        unsigned char mb[3];
        while(mouse_fd>=0 && read(mouse_fd,mb,3)==3){
            int dx=(signed char)mb[1], dy=(signed char)mb[2];
            int btn=mb[0]&7, wheel=(signed char)mb[2];
            if(mb[0]&0xC0) continue;
            Mx+=dx; My+=dy;
            if(Mx<0)Mx=0; if(My<0)My=0; if(Mx>=SW)Mx=SW-1; if(My>=SH)My=SH-1;
            int down=btn&1;
            if(down&&!mouse_down){ mouse_click=1; mouse_click_x=Mx; mouse_click_y=My; }
            mouse_down=down;
        }
        struct input_event ev;
        while(kb_fd>=0 && read(kb_fd,&ev,sizeof ev)==(int)sizeof ev){
            if(ev.type==EV_KEY){
                if(ev.value==1){
                    int shift = 0;
                    /* 简单追踪 Shift */
                    static int shift_held=0;
                    if(ev.code==KEY_LEFTSHIFT||ev.code==KEY_RIGHTSHIFT){ shift_held=1; continue; }
                    if(ev.code==KEY_LEFTCTRL||ev.code==KEY_LEFTALT) continue;
                    push_key(ev.code);
                    (void)shift;
                    handle_key(ev.code, shift_held);
                } else if(ev.value==0){
                    if(ev.code==KEY_LEFTSHIFT||ev.code==KEY_RIGHTSHIFT){
                        /* shift released */
                    }
                }
            }
        }
        kq_n=0;
        /* 拖动 */
        for(int i=0;i<MAX_WIN;i++){
            Win *w=&wins[i];
            if(w->open&&w->dragging){
                if(mouse_down){ w->x=Mx-w->drag_dx; w->y=My-w->drag_dy;
                    if(w->x<0)w->x=0; if(w->y<0)w->y=0;
                    if(w->x+w->w>SW)w->x=SW-w->w;
                    if(w->y+w->h>SH-TASKBAR_H)w->y=SH-TASKBAR_H-w->h;
                } else w->dragging=0;
            }
        }
        if(mouse_click){ handle_click(mouse_click_x,mouse_click_y); mouse_click=0;
            for(int i=0;i<MAX_WIN;i++) if(wins[i].open&&wins[i].dragging&&!mouse_down) wins[i].dragging=0;
        }
        /* 游戏逻辑 */
        unsigned long t=now_ms();
        for(int i=0;i<MAX_WIN;i++){
            if(wins[i].open&&wins[i].type==10){
                if(t-sn_last>= (unsigned long)sn_step_ms){ sn_last=t; snake_tick(); }
            }
        }
        /* 渲染 */
        draw_wallpaper();
        for(int i=0;i<NICON;i++){
            int hov = (Mx>=icons[i].x-6&&Mx<=icons[i].x+50&&My>=icons[i].y-6&&My<=icons[i].y+60);
            draw_icon(&icons[i],hov);
        }
        /* 按 z 序绘制窗口 */
        for(int z=1;z<=ztop;z++)
            for(int i=0;i<MAX_WIN;i++)
                if(wins[i].open&&wins[i].z==z) draw_window(&wins[i], i==active_win);
        draw_taskbar();
        draw_cursor();
        memcpy(FB,BUF,(size_t)LINE*SH);
        usleep(16000);
    }
    return 0;
}
