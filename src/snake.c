/* MiniDOS Snake — ANSI 终端贪吃蛇 */
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <string.h>

#define W 32
#define H 16
static struct termios oldt;
static void raw_on(int fd){ tcgetattr(fd,&oldt); struct termios t=oldt; t.c_lflag&=~(ICANON|ECHO); t.c_cc[VMIN]=0; t.c_cc[VTIME]=0; tcsetattr(fd,TCSANOW,&t); }
static void raw_off(int fd){ tcsetattr(fd,TCSANOW,&oldt); }

int main(void){
    int fd=STDIN_FILENO; fcntl(fd,F_SETFL,O_NONBLOCK);
    raw_on(fd);
    int sx=W/2, sy=H/2, dx=1, dy=0, len=3, score=0, dead=0;
    int bx[512], by[512]; /* 蛇身队列 */
    memset(bx,0,sizeof bx); memset(by,0,sizeof by);
    srand(time(0));
    int fx=rand()%W, fy=rand()%H;
    int map[H][W]; memset(map,0,sizeof map);
    printf("\033[2J\033[?25l");
    while(!dead){
        /* 输入 */
        char c;
        while(read(fd,&c,1)==1){
            if(c=='q'||c=='Q'||c==27){ if(c==27){ char b[2]; if(read(fd,b,2)<2){dead=2; break;} if(b[1]=='A'&&dy==0){dx=0;dy=-1;} else if(b[1]=='B'&&dy==0){dx=0;dy=1;} else if(b[1]=='C'&&dx==0){dx=1;dy=0;} else if(b[1]=='D'&&dx==0){dx=-1;dy=0;} } else dead=2; }
            else if((c=='w'||c=='W'||c=='k')&&dy==0){dx=0;dy=-1;}
            else if((c=='s'||c=='S'||c=='j')&&dy==0){dx=0;dy=1;}
            else if((c=='a'||c=='A'||c=='h')&&dx==0){dx=-1;dy=0;}
            else if((c=='d'||c=='D'||c=='l')&&dx==0){dx=1;dy=0;}
        }
        if(dead==2) break;
        sx+=dx; sy+=dy;
        if(sx<0||sy<0||sx>=W||sy>=H) dead=1;
        for(int i=0;i<len;i++) if(bx[i]==sx&&by[i]==sy) dead=1;
        if(dead) break;
        /* 身体跟随: 队列 */
        for(int i=len-1;i>0;i--){bx[i]=bx[i-1];by[i]=by[i-1];}
        bx[0]=sx; by[0]=sy;
        if(sx==fx&&sy==fy){
            if(len<510) len++;
            score+=10;
            do{ fx=rand()%W; fy=rand()%H; }while(0);
            for(int i=0;i<len;i++) if(bx[i]==fx&&by[i]==fy){fx=rand()%W;fy=rand()%H;i=-1;}
        }
        /* 绘制 */
        memset(map,0,sizeof map);
        map[fy][fx]=2;
        for(int i=0;i<len;i++) map[by[i]][bx[i]]=1;
        map[sy][sx]=3;
        printf("\033[H");
        printf("\033[32m MiniDOS Snake  SCORE: %d   [WASD/方向键] Q退出\033[0m\n+",score);
        for(int x=0;x<W;x++) printf("-");
        printf("+\n");
        for(int y=0;y<H;y++){
            printf("|");
            for(int x=0;x<W;x++){
                if(map[y][x]==2) printf("\033[31m@\033[32m");
                else if(map[y][x]==3) printf("O");
                else if(map[y][x]==1) printf("o");
                else printf(" ");
            }
            printf("|\n");
        }
        printf("+");
        for(int x=0;x<W;x++) printf("-");
        printf("+\n");
        fflush(stdout);
        usleep(130000);
    }
    raw_off(fd);
    printf("\033[?25h\033[0m\n  游戏结束! 得分: %d\n",score);
    return 0;
}
