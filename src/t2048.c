/* MiniDOS 2048 — ANSI 终端版 */
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>
#include <time.h>
#include <string.h>

static struct termios oldt;
static void raw_on(int fd){ tcgetattr(fd,&oldt); struct termios t=oldt; t.c_lflag&=~(ICANON|ECHO); t.c_cc[VMIN]=1; tcsetattr(fd,TCSANOW,&t); }
static void raw_off(int fd){ tcsetattr(fd,TCSANOW,&oldt); }

int g[4][4];
void addnew(void){
    int e[16][2],n=0;
    for(int i=0;i<4;i++)for(int j=0;j<4;j++) if(!g[i][j]){e[n][0]=i;e[n][1]=j;n++;}
    if(n){ int k=rand()%n; g[e[k][0]][e[k][1]] = rand()%10<9?2:4; }
}
void show(int score){
    printf("\033[H\033[32m MiniDOS 2048   SCORE: %d   [WASD] Q退出\033[0m\n",score);
    for(int i=0;i<4;i++){
        printf("+------+------+------+------+\n");
        for(int j=0;j<4;j++){
            printf("|");
            if(g[i][j]) printf("\033[33m%4d\033[0m  ",g[i][j]); else printf("      ");
        }
        printf("|\n");
    }
    printf("+------+------+------+------+\n");
    fflush(stdout);
}
int slide(int *r){ /* 返回是否移动 */
    int t[4],n=0,moved=0;
    for(int i=0;i<4;i++) if(r[i]) t[n++]=r[i];
    for(int i=0;i<n-1;i++) if(t[i]==t[i+1]){ t[i]*=2; for(int j=i+1;j<n-1;j++)t[j]=t[j+1]; t[--n]=0; }
    for(int i=0;i<4;i++){ if(r[i]!=t[i])moved=1; r[i]=t[i]; }
    return moved;
}
int move(int dir){
    int moved=0;
    for(int i=0;i<4;i++){
        int line[4];
        for(int j=0;j<4;j++)
            line[j]= dir=='w'?g[j][i] : dir=='s'?g[3-j][i] : dir=='a'?g[i][j] : g[i][3-j];
        if(slide(line)){
            moved=1;
            for(int j=0;j<4;j++){
                if(dir=='w') g[j][i]=line[j];
                else if(dir=='s') g[3-j][i]=line[j];
                else if(dir=='a') g[i][j]=line[j];
                else g[i][3-j]=line[j];
            }
        }
    }
    return moved;
}
int canmove(void){ int b[4][4]; memcpy(b,g,sizeof g);
    for(char d="wsad"[0];;){ if(move('w')||move('a')||move('s')||move('d')){ memcpy(g,b,sizeof g); return 1; } break; }
    return 0;
}
int main(void){
    srand(time(0)); memset(g,0,sizeof g); addnew(); addnew();
    int score=0; raw_on(STDIN_FILENO);
    printf("\033[2J\033[?25l");
    show(score);
    char c;
    while(read(STDIN_FILENO,&c,1)==1){
        if(c=='q'||c=='Q') break;
        if(c=='w'||c=='a'||c=='s'||c=='d'){
            if(move(c)){ addnew(); score+=2; }
            show(score);
            if(!canmove()){ printf("\n  没有可移动的方向了, 游戏结束! 得分 %d\n",score); break; }
        }
    }
    raw_off(STDIN_FILENO);
    printf("\033[?25h");
    return 0;
}
