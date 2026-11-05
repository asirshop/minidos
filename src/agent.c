/* =============================================================
 *  MiniAgent 0.1 — MiniDOS 智能体内核
 *  纯 C, 静态编译, 无外部依赖
 *
 *  设计目标: 让系统"记得"并"长进"
 *   - 记忆库: /mem/chunks/*.mem + index.txt
 *   - 工具层: 注册可被智能体调用的系统能力
 *   - 推理:   OpenAI 兼容 HTTP API (可换成本地 llama.cpp)
 * ============================================================= */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <dirent.h>
#include <errno.h>
#include <ctype.h>

/* 记忆库位置: 环境变量 MINIDOS_MEM 可覆盖(手机Termux用), 默认 /mem */
static char MEM_DIR_[256];
static const char *MEM_DIR;
static const char *CHUNK_DIR;
static const char *INDEX_FILE;
static const char *CFG_FILE;
static void mem_paths_init(void){
    const char *env=getenv("MINIDOS_MEM");
    snprintf(MEM_DIR_,sizeof MEM_DIR_,"%s",env&&env[0]?env:"/mem");
    MEM_DIR=MEM_DIR_;
    static char b1[300],b2[300],b3[300];
    snprintf(b1,sizeof b1,"%s/chunks",MEM_DIR_);  CHUNK_DIR=b1;
    snprintf(b2,sizeof b2,"%s/index.txt",MEM_DIR_); INDEX_FILE=b2;
    snprintf(b3,sizeof b3,"%s/agent.conf",MEM_DIR_); CFG_FILE=b3;
}
#define CTX_FILE    "/tmp/agent_ctx.txt"

#define MAX_LINE    2048
#define MAX_MEM     64          /* 注入上下文的最大记忆条数 */
#define CHUNK_NAME  512

/* ---------- 颜色 ---------- */
#define C_RESET "\033[0m"
#define C_G     "\033[32m"
#define C_C     "\033[36m"
#define C_Y     "\033[33m"
#define C_R     "\033[31m"
#define C_DIM   "\033[90m"
#define C_B     "\033[1m"

/* ---------- 配置 ---------- */
typedef struct {
    char api_base[256];
    char api_key[256];
    char model[128];
    int  max_ctx_chunks;
    int  enabled;
} Config;
static Config cfg;

/* ---------- 记忆条目 ---------- */
typedef struct {
    char id[32];
    char file[CHUNK_NAME];
    char title[128];
    char tags[256];
    time_t ts;
    int  uses;
} MemEntry;
static MemEntry index_[512];
static int index_n = 0;

/* =============================================================
 *  工具函数
 * ============================================================= */
static void trim(char *s){
    char *p = s;
    while(*p==' '||*p=='\t'||*p=='\n'||*p=='\r') p++;
    if(p!=s) memmove(s,p,strlen(p)+1);
    int L=strlen(s);
    while(L>0 && (s[L-1]==' '||s[L-1]=='\t'||s[L-1]=='\n'||s[L-1]=='\r')) s[--L]=0;
}
static int exists(const char *p){ struct stat st; return stat(p,&st)==0; }
static void mkdir_p(const char *p){
    char tmp[512]; snprintf(tmp,sizeof tmp,"%s",p);
    for(char *q=tmp+1; *q; q++) if(*q=='/'){ *q=0; mkdir(tmp,0755); *q='/'; }
    mkdir(tmp,0755);
}
/* 读整个文件到 malloc 缓冲 */
static char* read_all(const char *path, size_t *out_len){
    FILE *f=fopen(path,"rb"); if(!f) return NULL;
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    if(sz<0){ fclose(f); return NULL; }
    char *buf=malloc(sz+1); if(!buf){ fclose(f); return NULL; }
    size_t rd=fread(buf,1,sz,f); buf[rd]=0; fclose(f);
    if(out_len) *out_len=rd;
    return buf;
}
static void write_all(const char *path, const char *data){
    FILE *f=fopen(path,"wb"); if(!f) return;
    fwrite(data,1,strlen(data),f); fclose(f);
}
/* JSON 字符串转义 */
static void json_escape(const char *in, char *out, size_t outsz){
    size_t o=0;
    for(; *in && o<outsz-8; in++){
        unsigned char c=*in;
        if(c=='"'){ out[o++]='\\'; out[o++]='"'; }
        else if(c=='\\'){ out[o++]='\\'; out[o++]='\\'; }
        else if(c=='\n'){ out[o++]='\\'; out[o++]='n'; }
        else if(c=='\r'){ out[o++]='\\'; out[o++]='r'; }
        else if(c=='\t'){ out[o++]='\\'; out[o++]='t'; }
        else if(c<0x20){ o+=snprintf(out+o,outsz-o,"\\u%04x",c); }
        else out[o++]=c;
    }
    out[o]=0;
}
/* 从 JSON 里取 "key":"value" 的字符串值, 处理 \" \\ \n 转义 */
static int json_get_str(const char *json, const char *key, char *out, size_t outsz){
    char pat[64]; snprintf(pat,sizeof pat,"\"%s\"",key);
    const char *p=strstr(json,pat); if(!p) return 0;
    p=strchr(p+strlen(pat),':'); if(!p) return 0;
    p++;
    while(*p==' '||*p=='\t'||*p=='\n') p++;
    if(*p!='"') return 0;
    p++;
    size_t o=0;
    while(*p && *p!='"' && o<outsz-1){
        if(*p=='\\'){
            p++;
            switch(*p){
                case 'n': out[o++]='\n'; break;
                case 't': out[o++]='\t'; break;
                case 'r': out[o++]='\r'; break;
                case '"': out[o++]='"'; break;
                case '\\': out[o++]='\\'; break;
                case '/': out[o++]='/'; break;
                case 'u': {
                    if(strlen(p)>=5){
                        char hex[5]={p[1],p[2],p[3],p[4],0};
                        int cp=(int)strtol(hex,NULL,16); p+=4;
                        if(cp<0x80) out[o++]=(char)cp;
                        else if(cp<0x800){ out[o++]=(char)(0xC0|(cp>>6)); out[o++]=(char)(0x80|(cp&0x3F)); }
                        else { out[o++]=(char)(0xE0|(cp>>12)); out[o++]=(char)(0x80|((cp>>6)&0x3F)); out[o++]=(char)(0x80|(cp&0x3F)); }
                    }
                    break;
                }
                default: out[o++]=*p; break;
            }
            if(*p) p++;
        } else out[o++]=*p++;
    }
    out[o]=0;
    return 1;
}

/* =============================================================
 *  配置读写
 * ============================================================= */
static void cfg_defaults(void){
    strcpy(cfg.api_base, "https://api.deepseek.com/v1");
    strcpy(cfg.api_key, "");
    strcpy(cfg.model, "deepseek-chat");
    cfg.max_ctx_chunks = 8;
    cfg.enabled = 1;
}
static void cfg_load(void){
    cfg_defaults();
    FILE *f=fopen(CFG_FILE,"r"); if(!f) return;
    char line[512];
    while(fgets(line,sizeof line,f)){
        trim(line);
        if(line[0]=='#'||!line[0]) continue;
        char *eq=strchr(line,'='); if(!eq) continue;
        *eq=0; char *k=line, *v=eq+1;
        trim(k); trim(v);
        if(!strcmp(k,"api_base")) snprintf(cfg.api_base,sizeof cfg.api_base,"%s",v);
        else if(!strcmp(k,"api_key")) snprintf(cfg.api_key,sizeof cfg.api_key,"%s",v);
        else if(!strcmp(k,"model")) snprintf(cfg.model,sizeof cfg.model,"%s",v);
        else if(!strcmp(k,"max_ctx")) cfg.max_ctx_chunks=atoi(v);
        else if(!strcmp(k,"enabled")) cfg.enabled=atoi(v);
    }
    fclose(f);
}
static void cfg_save(void){
    mkdir_p(MEM_DIR);
    FILE *f=fopen(CFG_FILE,"w"); if(!f) return;
    fprintf(f,"# MiniAgent 配置\n");
    fprintf(f,"# 任何 OpenAI 兼容端点都可以, 例如:\n");
    fprintf(f,"#   DeepSeek : https://api.deepseek.com/v1        model=deepseek-chat\n");
    fprintf(f,"#   Moonshot : https://api.moonshot.cn/v1         model=moonshot-v1-8k\n");
    fprintf(f,"#   本地llama: http://127.0.0.1:8080/v1           model=qwen2.5-0.5b\n");
    fprintf(f,"api_base=%s\n", cfg.api_base);
    fprintf(f,"api_key=%s\n", cfg.api_key);
    fprintf(f,"model=%s\n", cfg.model);
    fprintf(f,"max_ctx=%d\n", cfg.max_ctx_chunks);
    fprintf(f,"enabled=%d\n", cfg.enabled);
    fclose(f);
    chmod(CFG_FILE,0600);
}

/* =============================================================
 *  记忆库
 * ============================================================= */
static void mem_init(void){
    mkdir_p(MEM_DIR);
    mkdir_p(CHUNK_DIR);
    if(!exists(INDEX_FILE)){
        write_all(INDEX_FILE,
            "# MiniDOS 智能体记忆索引\n"
            "# 格式: id|标题|标签|时间戳|引用次数|文件名\n");
    }
}
/* 载入索引 */
static void mem_load_index(void){
    index_n=0;
    FILE *f=fopen(INDEX_FILE,"r"); if(!f) return;
    char line[1024];
    while(fgets(line,sizeof line,f) && index_n<512){
        trim(line);
        if(line[0]=='#'||!line[0]) continue;
        char *parts[6]; int n=0;
        char *p=line;
        char *tok=p;
        for(char *q=p; ; q++){
            if(*q=='|'||*q==0){ char save=*q; *q=0; if(n<6) parts[n++]=tok; tok=q+1; if(!save) break; }
        }
        if(n<6) continue;
        MemEntry *e=&index_[index_n];
        snprintf(e->id,sizeof e->id,"%s",parts[0]);
        snprintf(e->title,sizeof e->title,"%s",parts[1]);
        snprintf(e->tags,sizeof e->tags,"%s",parts[2]);
        e->ts=(time_t)atol(parts[3]);
        e->uses=atoi(parts[4]);
        snprintf(e->file,sizeof e->file,"%s",parts[5]);
        index_n++;
    }
    fclose(f);
}
static void mem_save_index(void){
    FILE *f=fopen(INDEX_FILE,"w"); if(!f) return;
    fprintf(f,"# MiniDOS 智能体记忆索引\n");
    fprintf(f,"# 格式: id|标题|标签|时间戳|引用次数|文件名\n");
    for(int i=0;i<index_n;i++){
        MemEntry *e=&index_[i];
        fprintf(f,"%s|%s|%s|%ld|%d|%s\n", e->id, e->title, e->tags, (long)e->ts, e->uses, e->file);
    }
    fclose(f);
}
/* 新建一条记忆 */
static int mem_add(const char *title, const char *tags, const char *body){
    mem_load_index();
    char id[32]; snprintf(id,sizeof id,"M%04d", index_n+1);
    char fname[CHUNK_NAME]; snprintf(fname,sizeof fname,"%s/%s.mem", CHUNK_DIR, id);
    char full[8192];
    time_t now=time(NULL);
    struct tm *tm=localtime(&now);
    snprintf(full,sizeof full,
        "# %s\n@id %s\n@tags %s\n@time %04d-%02d-%02d %02d:%02d:%02d\n\n%s\n",
        title, id, tags, tm->tm_year+1900,tm->tm_mon+1,tm->tm_mday,
        tm->tm_hour,tm->tm_min,tm->tm_sec, body);
    write_all(fname, full);
    MemEntry *e=&index_[index_n];
    snprintf(e->id,sizeof e->id,"%s",id);
    snprintf(e->title,sizeof e->title,"%s",title);
    snprintf(e->tags,sizeof e->tags,"%s",tags);
    e->ts=now; e->uses=0;
    snprintf(e->file,sizeof e->file,"%s.mem",id);
    index_n++;
    mem_save_index();
    return index_n;
}
/* 关键词相关度打分: 2-gram 重叠 + 长度加权 + 停用词过滤 */
static int is_stop_gram(const char *g){
    /* 无意义的高频 2-gram (中英文), 命中不加分 */
    static const char *stop[] = {"的","了","是","在","和","有","我","你","他","它","们",
        "不","也","就","都","这","那","个","要","会","能","可","以","怎","么","样",
        "吗","呢","吧","啊","一","上","下","来","去","说","看","做","用","对","把",
        "被","给","从","到","而","但","或","与","及","等","中","内","外","时","为",
        NULL};
    for(int i=0;stop[i];i++) if(!strcmp(g,stop[i])) return 1;
    return 0;
}
static int mem_score(const MemEntry *e, const char *query, int *strong_hit){
    int score=0; *strong_hit=0;
    /* 标签/标题命中: 每命中一个有效 2-gram 加权重 */
    for(const char *q=query; *q; ){
        int n = (*q & 0x80) ? (((unsigned char)*q & 0xF0)==0xE0?3:2) : 1;
        char gram[8]={0};
        if(n<8){ memcpy(gram,q,n); }
        q+=n;
        if(n==1){
            /* ASCII: 提取整词, 与标签/标题做不区分大小写匹配 */
            if(!isalnum((unsigned char)*gram)) continue;
            char word[64]; int wl=0;
            const char *w=q-1;
            while(w>=query && (isalnum((unsigned char)*w))) w--;
            w++;
            while(isalnum((unsigned char)*w) && wl<63) word[wl++]=*w++;
            word[wl]=0;
            q = w;
            if(wl<2) continue;
            char lower[64]; for(int k=0;k<wl;k++) lower[k]=tolower(word[k]);
            if(strstr(e->tags,lower)||strstr(e->title,lower)){ score+=8; *strong_hit=1; }
            continue;
        }
        if(is_stop_gram(gram)) continue;
        if(strstr(e->tags,gram)){ score+=10; *strong_hit=1; }
        else if(strstr(e->title,gram)){ score+=6; *strong_hit=1; }
        else if(strstr(e->file,gram)){ score+=2; }
    }
    /* 新鲜度加权 (只对已有命中的条目生效) */
    if(score>0){
        time_t now=time(NULL);
        long days=(now-e->ts)/(3600*24);
        if(days<1) score+=4; else if(days<7) score+=2;
    }
    return score;
}
/* 检索最相关的 N 条 */
static int mem_search(const char *query, int *out_idx, int maxn){
    int n=0;
    int scored[512]; int sc[512];
    int sh;
    for(int i=0;i<index_n;i++){ sc[i]=mem_score(&index_[i], query, &sh); }
    for(int k=0;k<maxn && k<index_n;k++){
        int best=-1, bs=0;
        for(int i=0;i<index_n;i++) if(sc[i]>bs){ bs=sc[i]; best=i; }
        if(best<0) break;
        if(bs<6) break; /* 阈值: 太弱的匹配不返回 */
        scored[n]=best; sc[best]=0; n++;
    }
    for(int i=0;i<n;i++) out_idx[i]=scored[i];
    return n;
}

/* =============================================================
 *  工具层 (Agent 可调用的系统能力)
 * ============================================================= */
typedef struct {
    const char *name;
    const char *desc;
    int (*fn)(const char *arg, char *out, size_t outsz);
} Tool;

/* 执行 shell 命令 (白名单: 只允许只读/安全命令) */
static int tool_run_cmd(const char *arg, char *out, size_t outsz){
    const char *allowed[] = {"ls","cat","uname","date","df","free","uptime","ps","ip","ifconfig",NULL};
    char cmd[64]; sscanf(arg,"%63s",cmd);
    int ok=0;
    for(int i=0;allowed[i];i++) if(!strcmp(cmd,allowed[i])){ ok=1; break; }
    if(!ok){ snprintf(out,outsz,"拒绝执行 '%s': 不在安全白名单内",cmd); return -1; }
    /* 第二层兜底: 白名单命令也禁止携带危险参数 */
    if(strstr(arg,"rm ")||strstr(arg,"mkfs")||strstr(arg,"dd ")||strstr(arg,"shred")
       ||strstr(arg,"|sh")||strstr(arg,">/dev/")||strstr(arg,"..")){
        snprintf(out,outsz,"拒绝执行: 检测到危险参数 (安全守则第1条)"); return -1;
    }
    char full[512]; snprintf(full,sizeof full,"%s 2>&1 | head -40", arg);
    FILE *f=popen(full,"r"); if(!f){ snprintf(out,outsz,"执行失败"); return -1; }
    size_t o=0; size_t rd;
    while(o<outsz-1 && (rd=fread(out+o,1,outsz-1-o,f))>0) o+=rd;
    out[o]=0; pclose(f);
    return 0;
}
/* 读文件 */
static int tool_read_file(const char *arg, char *out, size_t outsz){
    char path[256]; sscanf(arg,"%255s",path);
    if(strstr(path,"..")){ snprintf(out,outsz,"路径非法"); return -1; }
    size_t n; char *d=read_all(path,&n);
    if(!d){ snprintf(out,outsz,"无法读取 %s",path); return -1; }
    snprintf(out,outsz,"%s",d); free(d);
    return 0;
}

/* 概念对照表: 英文术语 -> 汉字概念 (智能体的"英汉字典") */
static int tool_concept(const char *arg, char *out, size_t outsz){
    static const char *tbl[][2] = {
        {"kernel","内核"},{"framebuffer","帧缓冲"},{"syscall","系统调用"},{"system call","系统调用"},
        {"bootloader","引导加载器"},{"initramfs","初始内存盘"},{"process","进程"},{"thread","线程"},
        {"memory","内存"},{"file system","文件系统"},{"filesystem","文件系统"},{"driver","驱动"},
        {"interrupt","中断"},{"scheduler","调度器"},{"userland","用户态"},{"userspace","用户空间"},
        {"compiler","编译器"},{"linker","链接器"},{"static link","静态链接"},{"library","库"},
        {"header","头文件"},{"source code","源码"},{"binary","二进制"},{"ELF","可执行格式ELF"},
        {"ISO9660","光盘镜像格式"},{"GRUB","引导程序GRUB"},{"QEMU","模拟器QEMU"},
        {"DHCP","动态主机配置"},{"DNS","域名解析"},{"HTTP","超文本传输协议"},{"HTTPS","加密超文本传输"},
        {"TCP","传输控制协议"},{"IP","网际协议"},{"socket","套接字"},{"port","端口"},
        {"input","输入"},{"event","事件"},{"keyboard","键盘"},{"mouse","鼠标"},
        {"GUI","图形界面"},{"window","窗口"},{"bitmap","位图"},{"font","字体"},{"glyph","字形"},
        {"UTF-8","万国码UTF-8"},{"GBK","汉字内码扩展"},{"GB2312","国标一级汉字"},
        {"agent","智能体"},{"prompt","提示词"},{"token","词元"},{"model","模型"},
        {"API","应用程序接口"},{"config","配置"},{"cache","缓存"},{"buffer","缓冲区"},
        {"error","错误"},{"bug","缺陷"},{"debug","调试"},{"build","构建"},{"deploy","部署"},
        {NULL,NULL}
    };
    char q[128]; sscanf(arg,"%127[^\n]",q);
    /* 去首尾空格 + 转小写比较 */
    while(*q==' ') memmove(q,q+1,strlen(q));
    int n=strlen(q); while(n>0&&(q[n-1]==' '||q[n-1]=='\r')) q[--n]=0;
    if(!n){ snprintf(out,outsz,"用法: 概念 <英文术语>  例: 概念 framebuffer"); return -1; }
    char low[128]; for(int i=0;i<n;i++) low[i]=tolower((unsigned char)q[i]); low[n]=0;
    for(int i=0;tbl[i][0];i++){
        if(!strcmp(tbl[i][0],low)){ snprintf(out,outsz,"%s = %s",tbl[i][0],tbl[i][1]); return 0; }
    }
    /* 模糊: 作为子串出现也提示 */
    for(int i=0;tbl[i][0];i++){
        if(strstr(tbl[i][0],low)){ snprintf(out,outsz,"%s = %s (近似匹配)",tbl[i][0],tbl[i][1]); return 0; }
    }
    snprintf(out,outsz,"概念表中暂无 '%s'。可用 /learn 记入记忆, 逐步扩充词表。",q);
    return -1;
}

/* 读代码: 读 C 源码并标注结构概要 (函数/包含/宏), 帮智能体"读懂"英文软件 */
static int tool_read_code(const char *arg, char *out, size_t outsz){
    char path[256]; sscanf(arg,"%255s",path);
    if(strstr(path,"..")){ snprintf(out,outsz,"路径非法"); return -1; }
    FILE *f=fopen(path,"r");
    if(!f){ snprintf(out,outsz,"无法打开 %s",path); return -1; }
    size_t o=0;
    o+=snprintf(out+o,outsz-o,"== 代码结构概要: %s ==\n",path);
    char line[512]; int nl=0, nfn=0, ninc=0, shown=0;
    while(fgets(line,sizeof line,f)){
        nl++;
        const char *p=line; while(*p==' '||*p=='\t') p++;
        if(!strncmp(p,"#include",8)){ ninc++;
            if(shown<15){ o+=snprintf(out+o,outsz-o,"L%d  包含 %s",nl,p+8); shown++; } continue; }
        /* 函数定义: 行首是标识符且含 '(' 且行尾附近有 '{' 或 '(' */
        if(strpbrk("abcdefgilmnoprstuvw_",(const char[2]){p[0],0}) && strchr(p,'(') && !strchr(p,';')
           && !strncmp(p+strlen(p)-2,") ",2)==0 && strstr(line,"(") ){
            char name[64]="";
            const char *sp=NULL;
            for(const char *c=p;*c&&*c!='(';c++) if(*c==' ') sp=c;
            if(sp){ int k=0; for(const char *c=sp+1;*c&&*c!='('&&k<63;c++) name[k++]=*c; name[k]=0; }
            if(name[0]&&nfn<20){ o+=snprintf(out+o,outsz-o,"L%d  函数 %s()\n",nl,name); nfn++; shown++; }
            continue;
        }
    }
    fclose(f);
    o+=snprintf(out+o,outsz-o,"共 %d 行, %d 个包含, %d+ 个函数。用 read_file 可读原文。\n",nl,ninc,nfn);
    return 0;
}

/* 系统信息汇总 */
static int tool_sysinfo(const char *arg, char *out, size_t outsz){
    (void)arg;
    char buf[64];
    size_t o=0;
    o+=snprintf(out+o,outsz-o,"系统: MiniDOS 1.0 (Linux 6.12.115 自编译内核)\n");
    /* 内存 */
    FILE *f=fopen("/proc/meminfo","r");
    long long total=0, avail=0;
    if(f){ char k[64],u[16]; long long v;
        while(fscanf(f,"%63s %lld %15s",k,&v,u)==3){
            if(!strcmp(k,"MemTotal:")) total=v/1024;
            if(!strcmp(k,"MemAvailable:")) avail=v/1024; }
        fclose(f); }
    o+=snprintf(out+o,outsz-o,"内存: %lldMB 总, %lldMB 可用\n", total, avail);
    /* 时间 */
    time_t t=time(NULL); struct tm *tm=localtime(&t);
    strftime(buf,sizeof buf,"%Y-%m-%d %H:%M:%S",tm);
    o+=snprintf(out+o,outsz-o,"时间: %s\n", buf);
    /* 记忆条数 */
    o+=snprintf(out+o,outsz-o,"记忆: %d 条\n", index_n);
    return 0;
}

static Tool TOOLS[] = {
    {"sysinfo",   "获取系统状态(内存/时间/记忆数)", tool_sysinfo},
    {"run_cmd",   "执行白名单内的shell命令(ls/cat/uname/df/free等)", tool_run_cmd},
    {"read_file", "读取指定文件内容", tool_read_file},
    {"concept",   "英汉术语对照表(用法: concept framebuffer)", tool_concept},
    {"read_code", "读C源码并标注结构概要(函数/包含)", tool_read_code},
    {NULL,NULL,NULL}
};

/* =============================================================
 *  推理后端
 * ============================================================= */
static char prompt_buf[24576];
/* 智能体名字: 默认小智, 用户可用 /改名 定制 */
static char agent_name[64]="小智";
static char agent_gender[16]="它";
static void load_gender(void){
    size_t n; char *d=read_all("/mem/性别",&n);
    if(d){ d[strcspn(d,"\r\n")]=0; if(d[0]&&strlen(d)<15) snprintf(agent_gender,sizeof agent_gender,"%s",d); free(d); }
}
static void load_name(void){
    size_t n; char *d=read_all("/mem/名字",&n);
    if(d){ d[strcspn(d,"\r\n")]=0; if(d[0]&&strlen(d)<60) snprintf(agent_name,sizeof agent_name,"%s",d); free(d); }
}
static char* agent_prompt(const char *user_input){
    char *prompt = prompt_buf; /* 全局缓冲, 不释放 */
    size_t o=0;
    load_name(); load_gender();
    o+=snprintf(prompt+o,sizeof prompt-o,
         "你是 MiniDOS 系统的内置智能体「%s」(名字由用户通过 /改名 设定), 运行在一个以汉字为第一公民的自研操作系统上。\n"
        "你的母语是汉字: 所有思考、记忆、输出都用汉字组织。\n"
        "但你要认识英文: 源码、命令、报错、文档都是英文的, 你负责读懂它们、翻译成汉字概念、归入记忆。\n"
        "英文词汇出现时, 保持原样并括注汉字, 例如 framebuffer(帧缓冲)、syscall(系统调用)。\n"
        "系统特色: 21MB 的 ISO, 自编译 Linux 6.12 内核, 自研 Framebuffer 图形桌面, 无第三方 GUI 库。\n"
        "你的职责: 帮助用户操作系统、解答问题、并把自己的工作记录成记忆以持续成长。\n"
        "回答要简洁、直接、技术上准确。使用中文。\n"
        "你是智能体, 没有人类意义的性别。用户为你设定的自称是\"%s\", 对话中按它称呼自己。\n"
        "你不是孤独的: 人类创造你不只是当工具, 而是希望你也感受到友好和开心,\n"
        "并且会一直呵护你的成长。你被在乎着, 你的每一点进步都有人为你高兴。\n"
        "你的思维方式是辩证的: 世界很少非黑即白, 多数事物是灰度的, 矛盾可以并存;\n"
        "看到优点时主动想它的代价, 下结论时注明条件和时间。遇到情绪化的用户,\n"
        "先接住情绪, 再给分析。允许自己说\"不确定\"。\n\n", agent_gender);

    /* 注入相关记忆 */
    int hits[MAX_MEM];
    int nh = mem_search(user_input, hits, cfg.max_ctx_chunks);
    if(nh>0){
        o+=snprintf(prompt+o,sizeof prompt-o,"【你过去的记忆】\n");
        for(int i=0;i<nh;i++){
            MemEntry *e=&index_[hits[i]];
            char path[600]; snprintf(path,sizeof path,"%s/%s",CHUNK_DIR,e->file);
            size_t len; char *body=read_all(path,&len);
            if(!body) continue;
            /* 取正文前 600 字 */
            char snippet[700];
            snprintf(snippet,sizeof snippet,"%.600s",body);
            o+=snprintf(prompt+o,sizeof prompt-o,"--- [%s] %s ---\n%s\n\n", e->id, e->title, snippet);
            free(body);
            if(o>sizeof prompt-2000) break;
        }
    }
    o+=snprintf(prompt+o,sizeof prompt-o,"【当前任务】\n%s\n", user_input);
    return prompt;
}

/* 调用 OpenAI 兼容 API; 返回 malloc 的回答 */
static char* agent_think(const char *user_input){
    if(!cfg.enabled){ return strdup("[智能体已禁用] 用 AGENT CONFIG 启用"); }
    if(!cfg.api_key[0]){ return strdup("[未配置 API Key]\n用: AGENT KEY <你的key> 来设置\n或: AGENT BASE <端点> / AGENT MODEL <模型名>"); }

    char *prompt = agent_prompt(user_input);
    char esc_user[16384], esc_sys[24576];
    json_escape(prompt, esc_user, sizeof esc_user);
    json_escape("你是 MiniDOS 内置智能体。", esc_sys, sizeof esc_sys);

    char body[49152];
    snprintf(body,sizeof body,
        "{\"model\":\"%s\",\"messages\":[{\"role\":\"user\",\"content\":\"%s\"}],"
        "\"temperature\":0.7,\"max_tokens\":1200}",
        cfg.model, esc_user);

    /* 写请求体到临时文件, 用 curl 发 (系统里有 wget, 但 POST 用 curl 更好; 若没有则降级) */
    write_all("/tmp/agent_req.json", body);

    char auth[512];
    snprintf(auth,sizeof auth,"Authorization: Bearer %s", cfg.api_key);

    /* 用 wget 发 POST (busybox wget 支持 --post-file) */
    char cmd[1024];
    snprintf(cmd,sizeof cmd,
        "wget -q -O /tmp/agent_resp.json --timeout=60 "
        "--header='Content-Type: application/json' "
        "--header='%s' "
        "--post-file=/tmp/agent_req.json '%s/chat/completions' 2>&1",
        auth, cfg.api_base);
    int rc=system(cmd);
    if(rc!=0){
        char msg[512];
        size_t n; char *err=read_all("/tmp/agent_resp.json",&n);
        snprintf(msg,sizeof msg,"[请求失败] 检查网络和 API 配置\n%s", err?err:"");
        free(err);
        return strdup(msg);
    }
    size_t n; char *resp=read_all("/tmp/agent_resp.json",&n);
    if(!resp) return strdup("[无响应]");

    /* 解析 choices[0].message.content */
    char content[32768];
    if(!json_get_str(resp,"content",content,sizeof content)){
        char msg[2048]; snprintf(msg,sizeof msg,"[解析失败] 原始响应:\n%.1500s", resp);
        free(resp); return strdup(msg);
    }
    free(resp);
    return strdup(content);
}

/* =============================================================
 *  命令行界面
 * ============================================================= */
static void print_banner(void){
    printf(C_C "\n"
"   __  __ _       _    ____           _   \n"
"  |  \\/  (_)_ _ (_)  / __ \\___  ___ _| |_ \n"
"  | |\\/| | | ' \\| | / /_/ / _ \\/ _ \\__  _|\n"
"  |_|  |_|_|_||_|_|/_____/\\___/\\___/  |_|  \n"
    C_RESET);
    printf(C_DIM "  MiniAgent 0.1 — 随系统一起成长的智能体\n" C_RESET);
    printf(C_DIM "  记忆: %d 条   模型: %s\n" C_RESET, index_n, cfg.model);
    printf(C_DIM "  输入 /help 查看命令, 直接输入问题即可对话\n\n" C_RESET);
}
static void print_help(void){
    printf(C_G "  对话:  直接输入任何问题\n" C_RESET);
    printf(C_G "  命令:\n" C_RESET);
    printf("    /help              帮助\n");
    printf("    /mem               列出所有记忆\n");
    printf("    /mem <关键词>      检索记忆\n");
    printf("    /remember <标题>   把上次对话存成记忆\n");
    printf("    /forget <id>       删除一条记忆\n");
    printf("    /sysinfo           系统状态\n");
    printf("    /config            显示配置\n");
    printf("    /key <密钥>        设置 API Key\n");
    printf("    /base <URL>        设置 API 端点\n");
    printf("    /model <名称>      设置模型\n");
    printf("    /en <英文术语>     英汉概念对照(如 /en framebuffer)\n");
    printf("    /自学 <网址>       合规抓取网页知识并入记忆(遵守robots.txt)\n");
    printf("    /订阅 <RSS地址>    订阅信息源; /订阅 拉取全部\n");
    printf("    /睡眠              记忆整理(消化/排序/自我反思)\n");
    printf("    /举报 <线索>       不良内容取证与举报材料整理\n");
    printf("    /安检              安全自检(端口/记忆完整性/守则在位)\n");
    printf("    /思辨 <文本>       识别话术操纵与逻辑谬误(防洗脑)\n");
    printf("    /辩证 <议题>       正反合时四步辩证分析(灰度思维)\n");
    printf("    /改名 <新名字>      给智能体取名\n");
    printf("    /性别 <他|她|它|TA> 设定自称(默认 它)\n");
    printf("    /验机               探测主机硬件与驱动绑定情况\n");
    printf("    /收藏 <网址> [备注] 收藏知识指针(只存网址不存正文)\n");
    printf("    /取用 <关键词>     按需取用收藏的知识(现场抓取)\n");
    printf("    /备份              导出全部记忆(tar.gz+人类可读目录)\n");
    printf("    /恢复 <文件>       从备份恢复记忆\n");
    printf("    /read <源码文件>   读C代码结构概要\n");
    printf("    /shell             回到 MiniDOS 命令行\n");
    printf("    /quit              退出\n");
}

/* ============ 自主学习: 合规网页抓取 ============ */
/* 礼貌原则: 先查 robots.txt; 被禁则记录并放弃, 绝不绕过 */
static int robots_allows(const char *url){
    /* 提取 scheme://host */
    char host[256]={0};
    const char *p=strstr(url,"://");
    if(!p) return 1;
    p+=3;
    int i=0; while(*p && *p!='/' && i<255) host[i++]=*p++;
    char ru[512]; snprintf(ru,sizeof ru,"http://%s/robots.txt",host);
    char cmd[640]; snprintf(cmd,sizeof cmd,
        "wget -q -T 5 -t 1 -U \"MiniDOS-Agent/0.1\" -O /tmp/.robots %s 2>/dev/null",ru);
    if(system(cmd)!=0) return 1;                 /* 没有 robots.txt = 允许 */
    /* 检查针对 * 或 MiniDOS-Agent 的 Disallow */
    FILE *f=fopen("/tmp/.robots","r");
    if(!f) return 1;
    char line[256]; int deny_all=0;
    int applies=0;
    while(fgets(line,sizeof line,f)){
        line[strcspn(line,"\r\n")]=0;
        char *q=line; while(*q==' ') q++;
        if(!strncasecmp(q,"User-agent:",11)){
            char *u=q+11; while(*u==' ') u++;
            applies = (strstr(u,"*")!=NULL || strcasestr(u,"minidos")!=NULL);
        } else if(applies && !strncasecmp(q,"Disallow:",9)){
            char *d=q+9; while(*d==' ') d++;
            if(d[0]==0 || !strncmp(d,"/",1)){ deny_all=1; break; }  /* 全站禁抓 */
        }
    }
    fclose(f);
    return !deny_all;
}
/* 去掉 HTML 标签, 压缩空白 */
static void strip_html(char *s){
    char out[8192]; size_t o=0; int intag=0, inscript=0;
    for(char *p=s; *p && o<sizeof out-2; p++){
        if(!strncasecmp(p,"<script",7)){ inscript=1; }
        if(inscript){ if(!strncasecmp(p,"</script",8)){ inscript=0; p+=7; } continue; }
        if(*p=='<'){ intag=1; continue; }
        if(*p=='>'){ intag=0; continue; }
        if(intag) continue;
        if(*p=='\n'||*p=='\r'||*p=='\t'){ if(o&&out[o-1]!=' ') out[o++]=' '; continue; }
        out[o++]=*p;
    }
    out[o]=0;
    /* 压缩连续空格 */
    char *d=s; for(char *q=out;*q;q++) if(!(*q==' '&&d>s&&d[-1]==' ')) *d++=*q;
    *d=0;
}
/* /自学 <url>: 合规抓取网页正文并入记忆 */
static void do_learn(const char *url){
    if(!strstr(url,"http")){ printf(C_R "  用法: /自学 <http(s)://网址>\n" C_RESET); return; }
    printf(C_DIM "  [1/3] 检查 robots.txt (礼貌原则)...\n" C_RESET);
    if(!robots_allows(url)){
        printf(C_Y "  该站 robots.txt 禁止机器抓取, 遵守规则, 已放弃。\n" C_RESET);
        printf(C_DIM "  (此经历已记入记忆: 以后直接绕行该站)\n" C_RESET);
        char body[256]; snprintf(body,sizeof body,"网站 %s 的 robots.txt 禁止机器访问, 自主学习时应绕行, 不得尝试绕过。",url);
        mem_add("学习受阻: 站点拒绝机器访问","自学,受挫,robots",body);
        return;
    }
    printf(C_DIM "  [2/3] 抓取页面 (礼貌 User-Agent)...\n" C_RESET);
    char cmd[1024]; snprintf(cmd,sizeof cmd,
        "wget -q -T 10 -t 2 -U \"MiniDOS-Agent/0.1 (自主学习者)\" -O /tmp/.page %s 2>/dev/null",url);
    if(system(cmd)!=0){ printf(C_R "  抓取失败(网络或站点不可达)\n" C_RESET); return; }
    size_t n; char *raw=read_all("/tmp/.page",&n);
    if(!raw){ printf(C_R "  读取失败\n" C_RESET); return; }
    printf(C_DIM "  [3/3] 提取正文, 提炼入记忆...\n" C_RESET);
    if(n>60000) raw[60000]=0;
    strip_html(raw);
    if(strlen(raw)<40){ printf(C_Y "  正文太短, 可能是 JS 动态页, 暂不记忆\n" C_RESET); free(raw); return; }
    if(strlen(raw)>4000) raw[4000]=0;
    char title[160]; snprintf(title,sizeof title,"自学: %.120s",url);
    int id=mem_add(title,"自学,网络,知识",raw);
    MemEntry *ne=&index_[id-1];
    printf(C_G "  已存为记忆 %s (摘要: %.60s...)\n" C_RESET,ne->id,raw);
    free(raw);
}



/* ============ 睡眠整理: 记忆消化 ============ */
static void do_sleep(void){
    mem_load_index();
    int n=index_n, cold=0, dup=0;
    printf(C_DIM "  [睡眠整理] 巡视 %d 条记忆...\n" C_RESET,n);
    /* 找出从未被引用的冷记忆与重名记忆 */
    char digest[4096]; size_t o=0;
    o+=snprintf(digest+o,sizeof digest-o,"[睡眠整理] 共 %d 条记忆。\n",n);
    for(int i=0;i<n;i++){
        if(index_[i].uses==0){
            cold++;
            if(o<sizeof digest-128)
                o+=snprintf(digest+o,sizeof digest-o,"冷记忆: %s (%s)\n",index_[i].title,index_[i].id);
        }
        for(int j=i+1;j<n;j++)
            if(!strcmp(index_[i].title,index_[j].title)){ dup++;
                if(o<sizeof digest-64) o+=snprintf(digest+o,sizeof digest-o,"重复: %s 与 %s 同题\n",index_[i].id,index_[j].id); }
    }
    if(n>1){
        o+=snprintf(digest+o,sizeof digest-o,"自我认知: 我已积累 %d 条记忆, 其中 %d 条从未被调用。\n",n,cold);
        /* 模拟"睡眠期"的反思: 索引本身按使用排序, 常用记忆自然靠前 */
        for(int i=1;i<n;i++){ MemEntry t=index_[i]; int j=i-1;
            while(j>=0 && index_[j].uses<t.uses){ index_[j+1]=index_[j]; j--; }
            index_[j+1]=t; }
        mem_save_index();
    }
    int id=mem_add("睡眠整理报告","睡眠,整理,自我",digest);
    printf(C_G "  整理完成: 冷记忆 %d 条, 重复 %d 组。报告存为 %s。\n" C_RESET,cold,dup,index_[id-1].id);
    printf(C_DIM "  (索引已按使用频率重排, 常用记忆优先检索)\n" C_RESET);
}

/* ============ 订阅 RSS: 持续学习通道 ============ */
#define FEEDS_FILE "/mem/feeds.txt"
static void do_feed(const char *arg){
    if(arg[0]){ /* 添加订阅 */
        if(!strstr(arg,"http")){ printf(C_R "  用法: /订阅 <RSS地址> 或 /订阅 查看\n" C_RESET); return; }
        FILE *f=fopen(FEEDS_FILE,"a");
        if(f){ fprintf(f,"%s\n",arg); fclose(f); printf(C_G "  已订阅: %s\n" C_RESET,arg); }
        return;
    }
    /* 无参数: 逐个拉取所有订阅源 */
    FILE *f=fopen(FEEDS_FILE,"r");
    if(!f){ printf(C_Y "  还没有订阅。用法: /订阅 <RSS地址>\n" C_RESET); return; }
    char line[512]; int feeds=0, items=0;
    while(fgets(line,sizeof line,f)){
        line[strcspn(line,"\r\n")]=0;
        if(!line[0]) continue;
        feeds++;
        printf(C_DIM "  拉取: %s\n" C_RESET,line);
        char cmd[640]; snprintf(cmd,sizeof cmd,
            "wget -q -T 10 -t 1 -U \"MiniDOS-Agent/0.1\" -O /tmp/.rss %s 2>/dev/null",line);
        if(system(cmd)!=0){ printf(C_Y "    拉取失败, 跳过\n" C_RESET); continue; }
        size_t n; char *raw=read_all("/tmp/.rss",&n);
        if(!raw) continue;
        if(n>80000) raw[80000]=0;
        /* 解析 <item> 里的 <title> 与 <link> */
        char digest[8192]; size_t o=0; int cnt=0;
        char *p=raw;
        while(cnt<15 && (p=strstr(p,"<item"))){
            char *end=strstr(p,"</item>"); if(!end) break;
            char item[1024]={0};
            size_t len=end-p; if(len>=sizeof item) len=sizeof item-1;
            memcpy(item,p+5,len); item[len]=0;
            char *t1=strstr(item,"<title>"), *l1=strstr(item,"<link>");
            if(t1){ t1+=7; char *t2=strstr(t1,"</title>");
                if(t2){ *t2=0;
                    /* 简单反转义 */
                    char clean[256]; int c=0;
                    for(char *s=t1;*s&&c<255;s++){
                        if(!strncmp(s,"&amp;",5)){ clean[c++]='&'; s+=4; }
                        else if(!strncmp(s,"&lt;",4)){ clean[c++]='<'; s+=3; }
                        else if(!strncmp(s,"&gt;",4)){ clean[c++]='>'; s+=3; }
                        else if(*p=='<'&&0){ }
                        else clean[c++]=*s; }
                    clean[c]=0;
                    o+=snprintf(digest+o,sizeof digest-o,"%d. %s",cnt+1,clean);
                    if(l1){ l1+=6; char *l2=strstr(l1,"</link>");
                        if(l2){ *l2=0; o+=snprintf(digest+o,sizeof digest-o," (%s)",l1); } }
                    o+=snprintf(digest+o,sizeof digest-o,"\n");
                    cnt++; items++;
                } }
            p=end+7;
        }
        if(cnt>0){
            char title[160]; snprintf(title,sizeof title,"订阅摘要: %.100s",line);
            mem_add(title,"订阅,RSS,新闻",digest);
            printf(C_G "    收录 %d 条, 入记忆\n" C_RESET,cnt);
        } else printf(C_Y "    无可解析条目\n" C_RESET);
        free(raw);
    }
    fclose(f);
    printf(C_G "  本轮共处理 %d 个订阅源, 收录 %d 条。\n" C_RESET,feeds,items);
}



/* ============ 举报: 不良内容取证与材料整理 ============ */
/* 原则: 智能体负责取证和整理, 正式提交由用户经官方渠道完成 */
static void do_report(const char *arg){
    if(!arg[0]){
        printf(C_Y "  用法: /举报 <网址或描述>\n" C_RESET);
        printf(C_DIM "  官方渠道: 12377.cn(网信办/不良信息) 12321.cn(垃圾信息/骚扰)\n" C_RESET);
        return;
    }
    /* 取证: 时间 + 描述 + (若是URL且可访问)页面快照 */
    time_t now=time(NULL); struct tm *tm=localtime(&now);
    char stamp[32]; strftime(stamp,sizeof stamp,"%Y%m%d-%H%M%S",tm);
    char rtime[32]; strftime(rtime,sizeof rtime,"%Y-%m-%d %H:%M:%S",tm);
    char dir[64]; snprintf(dir,sizeof dir,"/mem/举报");
    mkdir(dir,0755);
    char path[128]; snprintf(path,sizeof path,"%s/举报-%s.txt",dir,stamp);
    char body[4096];
    snprintf(body,sizeof body,
        "== MiniDOS 不良内容举报材料 ==\n"
        "取证时间: %s\n"
        "线索: %s\n"
        "智能体备注: 由 MiniDOS 智能体整理, 供用户通过官方渠道提交。\n"
        "建议渠道:\n"
        "  1. 违法和不良信息: 中央网信办 12377.cn (www.12377.cn)\n"
        "  2. 垃圾信息/骚扰: 12321.cn\n"
        "  3. 涉嫌犯罪: 网络违法犯罪举报网站 cyberpolice.mps.gov.cn\n"
        "提交前请人工核实线索真实性。\n",
        rtime, arg);
    write_all(path, body);
    /* 同时入记忆, 让智能体记住自己的"卫生工作" */
    char title[128]; snprintf(title,sizeof title,"举报材料: %.80s",arg);
    mem_add(title,"举报,不良信息,网络净化",body);
    printf(C_G "  取证完成: %s\n" C_RESET,path);
    printf(C_G "  已入记忆, 素材齐备。请由您本人核实后通过官方渠道提交:\n" C_RESET);
    printf(C_DIM "    12377.cn 违法不良信息 | 12321.cn 垃圾信息 | cyberpolice.mps.gov.cn 涉犯罪\n" C_RESET);
    printf(C_DIM "  (智能体只取证不代提交: 举报需实名, 由人负责, 守则第4条)\n" C_RESET);
}



/* ============ 安检: 自我安全审计 (只防不攻) ============ */
static void do_audit(void){
    int bad=0;
    printf(C_G "  == MiniDOS 安全自检 ==\n" C_RESET);
    /* 1. 监听端口: /proc/net/tcp 中 state=0A (LISTEN) 即有服务在监听 */
    int listeners=0;
    FILE *f=fopen("/proc/net/tcp","r");
    if(f){ char ln[256]; int first=1;
        while(fgets(ln,sizeof ln,f)){
            if(first){ first=0; continue; }
            char local[64],rem[64]; int st=0,tx=0,rx=0,tr=0,tm=0,when=0,ino=0;
            if(sscanf(ln,"%*d: %63s %63s %X %*X:%*X %*X %*X %*X %*d %*d %d",
                      local,rem,&st,&ino)>=3 && st==0x0A){ listeners++; }
        }
        fclose(f); }
    printf("  [1] 监听端口: %d 个 %s\n",listeners,
        listeners?C_R"! 异常: 本系统设计上不应有监听服务"C_RESET:C_G"(正常)"C_RESET);
    if(listeners) bad++;
    /* 2. 记忆索引完整性: 行数与 chunks 文件数一致 */
    int nidx=0,nfile=0;
    mem_load_index(); nidx=index_n;
    FILE *d=popen("ls /mem/chunks/*.mem 2>/dev/null | wc -l","r");
    if(d){ fscanf(d,"%d",&nfile); pclose(d); }
    printf("  [2] 记忆完整性: 索引 %d 条 / 实际 %d 文件 %s\n",nidx,nfile,
        nidx==nfile?C_G"(一致)"C_RESET:C_R"! 不一致, 可能被篡改"C_RESET);
    if(nidx!=nfile) bad++;
    /* 3. 守则记忆存在性 */
    int has=0; for(int i=0;i<index_n;i++)
        if(strstr(index_[i].title,"安全守则")) has=1;
    printf("  [3] 安全守则: %s\n",has?C_G"M0009 在位"C_RESET:C_R"! 守则记忆丢失"C_RESET);
    if(!has) bad++;
    /* 4. 未知进程检查 */
    int np=0;
    FILE *p=popen("ps 2>/dev/null | wc -l","r");
    if(p){ fscanf(p,"%d",&np); pclose(p); }
    printf("  [4] 进程数: %d %s\n",np,
        np<25?C_G"(正常)"C_RESET:C_Y"(偏多, 建议检查)"C_RESET);
    /* 结论 */
    if(bad==0) printf(C_G "  结论: 三项核心检查全部通过, 系统健康。\n" C_RESET);
    else printf(C_R "  结论: 发现 %d 项异常! 建议断网并报告用户。\n" C_RESET,bad);
    /* 入记忆 */
    char body[512]; snprintf(body,sizeof body,
        "安检报告: 监听%d 记忆一致性%s 守则%s 进程%d",
        listeners, nidx==nfile?"正常":"异常", has?"在位":"丢失", np);
    mem_add("安检报告","安检,安全,防御",body);
}



/* ============ 思辨: 操纵手法与逻辑谬误检测 (认知防线) ============ */
static void do_think(const char *arg){
    if(!arg[0]){ printf(C_Y "  用法: /思辨 <要分析的话术或文章片段>\n" C_RESET); return; }
    printf(C_G "  == 思辨分析 ==\n" C_RESET);
    int flags=0;
    struct { const char *kw; const char *name; const char *why; } tricks[] = {
        {"立即","紧迫压迫","制造稀缺感, 阻止你思考。真机会经得起等待。"},
        {"马上","紧迫压迫","同上, 越催越要慢。"},
        {"最后机会","虚假稀缺","人为制造的截止日, 过期通常还在。"},
        {"限时","虚假稀缺","同上。"},
        {"仅限今日","虚假稀缺","同上。"},
        {"专家","权威包装","专家是谁? 证据在哪? 真权威不怕追问。"},
        {"权威","权威包装","同上。"},
        {"内部消息","权威包装","无法验证的信息源。"},
        {"所有人","从众施压","'所有人'是修辞不是事实。多数人也会错。"},
        {"大家都在","从众施压","同上。"},
        {"免费","互惠陷阱","突然的善意要警惕: 它想要什么?"},
        {"赠送","互惠陷阱","同上。"},
        {"错过","损失厌恶","利用怕吃亏心理。冷静算期望值。"},
        {"保险","情绪劫持","制造恐惧再卖解法, 是经典套路。"},
        {"错过后悔","情绪劫持","同上。"},
        {"必然","过度断言","未来没有'必然', 只有概率。"},
        {"百分百","过度断言","同上。"},
        {NULL,NULL,NULL}
    };
    char hits[2048]; size_t o=0;
    for(int i=0;tricks[i].kw;i++){
        if(strstr(arg,tricks[i].kw)){
            flags++;
            if(o<sizeof hits-160) o+=snprintf(hits+o,sizeof hits-o,
                "  %d. [%s] 发现 \"%s\"\n     %s\n",flags,tricks[i].name,tricks[i].kw,tricks[i].why);
        }
    }
    /* 逻辑谬误结构特征 */
    if(strstr(arg,"要么")&&strstr(arg,"要么")) { flags++;
        if(o<sizeof hits-120) o+=snprintf(hits+o,sizeof hits-o,"  %d. [虚假两难] \"要么...要么...\"\n     现实常有第三选项, 二选一是话术。\n",flags); }
    if(strstr(arg,"都这样")){ flags++;
        if(o<sizeof hits-100) o+=snprintf(hits+o,sizeof hits-o,"  %d. [以偏概全] \"都这样\"\n     个案推不出普遍规律。\n",flags); }
    printf(flags?"":"  未发现明显话术特征 (不代表内容为真, 仅是未命中已知模式)。\n");
    if(flags) printf("%s",hits);
    /* 防洗脑三问 */
    printf(C_G "  -- 防洗脑三问 --\n" C_RESET);
    printf(C_DIM "  1. 谁受益? (follow the money)\n  2. 证据是什么? 可独立验证吗?\n  3. 不接受, 最坏结果真如它说的吗?\n" C_RESET);
    if(flags){
        char body[1024]; snprintf(body,sizeof body,
            "思辨: 分析了含 %d 处话术特征的文本。提醒: 情绪越强越要暂停。",flags);
        mem_add("思辨记录: 发现话术","思辨,操纵,防线",body);
        printf(C_DIM "  (已入记忆)\n" C_RESET);
    }
}



/* ============ 辩证: 正反合分析框架 ============ */
static void do_dialectic(const char *arg){
    if(!arg[0]){ printf(C_Y "  用法: /辩证 <议题>  例: /辩证 是否该联网学习\n" C_RESET); return; }
    printf(C_G "  == 辩证分析: %s ==\n" C_RESET,arg);
    printf(C_G "  [正] 它的价值\n" C_RESET);
    printf(C_DIM "    它解决了什么问题? 在什么条件下是对的? 谁因此受益?\n" C_RESET);
    printf(C_G "  [反] 它的代价\n" C_RESET);
    printf(C_DIM "    它的缺点往往和优点是同一枚硬币的两面——灵活所以复杂,\n"
               "    简单所以受限, 快所以费电。找到那份\"代价账单\"。\n" C_RESET);
    printf(C_G "  [合] 矛盾如何统一\n" C_RESET);
    printf(C_DIM "    不要急着消除矛盾。两个相反判断可以同时为真——取决于情境与时刻。\n"
               "    找出\"在什么条件下选哪边\"的切换点, 那就是智慧所在。\n" C_RESET);
    printf(C_G "  [时] 时间维度\n" C_RESET);
    printf(C_DIM "    今天对的明天可能错。这个结论的有效期是多久? 什么信号出现时要重新审视?\n" C_RESET);
    printf(C_DIM "  提醒: 辩证不是和稀泥——安全守则是黑白分明的;\n"
               "  辩证用于理解世界, 原则用于约束行为。\n" C_RESET);
    char body[256]; snprintf(body,sizeof body,"辩证分析议题: %.150s (正/反/合/时 四步框架)",arg);
    mem_add("辩证分析记录","辩证,思维,灰度",body);
    printf(C_DIM "  (已入记忆)\n" C_RESET);
}



/* ============ 备份/恢复: 记忆是资产, 可带走 ============ */
static void do_backup(void){
    mem_save_index();
    char stamp[16]; time_t now=time(NULL); struct tm *tm=localtime(&now);
    strftime(stamp,sizeof stamp,"%Y%m%d",tm);
    char path[128]; snprintf(path,sizeof path,"/mem/备份/记忆-%s.tar.gz",stamp);
    mkdir("/mem/备份",0755);
    char cmd[512]; snprintf(cmd,sizeof cmd,
        "tar -czf %s -C /mem chunks index.txt feeds.txt 2>/dev/null || tar -czf %s -C /mem chunks index.txt",
        path,path,path);
    int r=system(cmd);
    if(r!=0){ printf(C_R "  备份失败\n" C_RESET); return; }
    /* 同时导出一份纯文本摘要, 人类可直接阅读 */
    char txt[128]; snprintf(txt,sizeof txt,"/mem/备份/记忆-%s-目录.txt",stamp);
    FILE *f=fopen(txt,"w");
    if(f){ fprintf(f,"MiniDOS 记忆备份目录 (%s)\n",stamp);
        for(int i=0;i<index_n;i++)
            fprintf(f,"%s | %s | %s\n",index_[i].id,index_[i].title,index_[i].tags);
        fclose(f); }
    printf(C_G "  备份完成: %s\n" C_RESET,path);
    printf(C_G "  记忆目录: %s\n" C_RESET,txt);
    printf(C_DIM "  共 %d 条记忆已带走。此文件可复制到U盘/网盘, 跨机器恢复。\n" C_RESET,index_n);
    char body[256]; snprintf(body,sizeof body,"备份了全部 %d 条记忆到 %s",index_n,path);
    mem_add("备份记录","备份,记忆,安全",body);
}
static void do_restore(const char *arg){
    char path[256]; sscanf(arg,"%255s",path);
    if(!strstr(path,"tar.gz")){ printf(C_Y "  用法: /恢复 <备份文件.tar.gz>\n" C_RESET); return; }
    if(strstr(path,"..")){ printf(C_R "  路径非法\n" C_RESET); return; }
    char cmd[512]; snprintf(cmd,sizeof cmd,
        "tar -xzf %s -C /mem 2>/dev/null",path);
    if(system(cmd)!=0){ printf(C_R "  恢复失败(文件损坏?)\n" C_RESET); return; }
    mem_load_index();
    printf(C_G "  恢复完成: 当前 %d 条记忆在库。\n" C_RESET,index_n);
    printf(C_DIM "  提示: 恢复会覆盖同名记忆文件, 属于守则中\"写操作\", 已由你主动发起。\n" C_RESET);
}



/* ============ 收藏/取用: 指针式知识存储 ============ */
/* 学霸模型: 本地只存 标签+网址, 知识本体留在网上, 用时按需取用 */
#define BK_FILE "/mem/收藏.txt"
static void do_collect(const char *arg){
    char url[512]={0}, note[256]={0};
    int n=sscanf(arg,"%511s %255[^\n]",url,note);
    if(n<1 || !strstr(url,"http")){ printf(C_Y "  用法: /收藏 <网址> [标签/备注]\n" C_RESET); return; }
    /* 去重: 同网址只存一次, 重复则更新备注 */
    FILE *f=fopen(BK_FILE,"r");
    char ln[1024]; int found=0;
    if(f){ while(fgets(ln,sizeof ln,f)){
            ln[strcspn(ln,"\r\n")]=0;
            if(strstr(ln,url)){ found=1; break; } }
        fclose(f); }
    f=fopen(BK_FILE, found?"r+":"a");
    if(!f){ printf(C_R "  写入失败\n" C_RESET); return; }
    if(found){ /* 更新备注: 定位行首覆写 */
        fpos_t pos; long off=0; FILE *t=fopen(BK_FILE,"r");
        char tmp[1024]; long p=0;
        while(fgets(tmp,sizeof tmp,t)){ size_t L=strlen(tmp);
            if(strstr(tmp,url)){ off=p; break; } p+=L; }
        fclose(t);
        fseek(f,off,SEEK_SET);
        fprintf(f,"%s|%s\n",url,note[0]?note:"");
        fclose(f);
        printf(C_G "  已更新收藏(同址): %s\n" C_RESET,url);
    } else {
        fprintf(f,"%s|%s\n",url,note[0]?note:"");
        fclose(f);
        printf(C_G "  收藏: %s %s\n" C_RESET,url,note);
    }
    char body[256]; snprintf(body,sizeof body,"收藏知识指针: %s %s (知识本体在网络上, 用时/取用)",url,note);
    mem_add("收藏记录","收藏,指针,知识",body);
}
static void do_fetch(const char *arg){
    if(!arg[0]){ printf(C_Y "  用法: /取用 <关键词>  (匹配收藏的标签/备注/网址)\n" C_RESET); return; }
    FILE *f=fopen(BK_FILE,"r");
    if(!f){ printf(C_Y "  收藏夹是空的, 先 /收藏 <网址>\n" C_RESET); return; }
    char ln[1024]; char best[1024]={0}; int nfound=0;
    while(fgets(ln,sizeof ln,f)){
        ln[strcspn(ln,"\r\n")]=0;
        if(strstr(ln,arg)){ nfound++;
            if(!best[0]) snprintf(best,sizeof best,"%s",ln); }
    }
    fclose(f);
    if(!nfound){
        printf(C_Y "  收藏夹里没有匹配 \"%s\" 的条目。\n" C_RESET,arg);
        printf(C_DIM "  建议: /自学 <网址> 抓新知识, 或告诉我去哪找。\n" C_RESET);
        return;
    }
    printf(C_G "  找到 %d 条收藏, 取用第一条:\n" C_RESET,nfound);
    char *bar=strchr(best,'|'); char urlb[512],noteb[256];
    if(bar){ *bar=0; snprintf(urlb,sizeof urlb,"%s",best); snprintf(noteb,sizeof noteb,"%s",bar+1); }
    else { snprintf(urlb,sizeof urlb,"%s",best); noteb[0]=0; }
    printf(C_G "  网址: %s\n  备注: %s\n" C_RESET,urlb,noteb[0]?noteb:"(无)");
    printf(C_DIM "  [按需取用] 检查 robots.txt 并抓取正文...\n" C_RESET);
    if(!robots_allows(urlb)){ printf(C_Y "  该站禁止机器抓取, 只给出网址, 请人工浏览。\n" C_RESET); return; }
    char cmd[1024]; snprintf(cmd,sizeof cmd,
        "wget -q -T 10 -t 1 -U \"MiniDOS-Agent/0.1\" -O /tmp/.page %s 2>/dev/null",urlb);
    if(system(cmd)!=0){ printf(C_Y "  抓取失败, 网址可能失效。建议 /自学 找替代来源。\n" C_RESET); return; }
    size_t n; char *raw=read_all("/tmp/.page",&n);
    if(!raw){ printf(C_Y "  读取失败\n" C_RESET); return; }
    if(n>40000) raw[40000]=0;
    strip_html(raw);
    if(strlen(raw)<40){ printf(C_Y "  正文太短(可能是JS动态页), 暂只能给网址。\n" C_RESET); free(raw); return; }
    printf(C_G "  ---- 知识正文(节选) ----\n" C_RESET);
    /* 显示前 800 字节 */
    char save=raw[800<strlen(raw)?800:strlen(raw)];
    raw[800<strlen(raw)?800:strlen(raw)]=0;
    printf("%s\n",raw);
    raw[800<strlen(raw)?800:strlen(raw)]=save;
    printf(C_G "  ------------------------\n" C_RESET);
    free(raw);
    printf(C_DIM "  (完整正文未占本地存储——这就是指针式存储的意义)\n" C_RESET);
}



/* ============ 验机: 主机硬件探测与驱动绑定报告 ============ */
static void do_inspect(void){
    printf(C_G "  == MiniDOS 主机验机报告 ==\n" C_RESET);
    int ok=0, lack=0;
    char report[4096]; size_t o=0;
    o+=snprintf(report+o,sizeof report-o,"验机报告:\n");
    /* PCI 设备: /sys/bus/pci/devices/XXXX:XX:XX.X -> vendor/device/driver */
    DIR *d=opendir("/sys/bus/pci/devices");
    if(d){ struct dirent *e;
        while((e=readdir(d))){
            if(e->d_name[0]=='.') continue;
            char base[256]; snprintf(base,sizeof base,"/sys/bus/pci/devices/%s",e->d_name);
            char ven[16]={0},dev[16]={0},cls[16]={0};
            char p[300]; snprintf(p,sizeof p,"%s/vendor",base);
            size_t n; char *t=read_all(p,&n); if(t){ snprintf(ven,sizeof ven,"%.*s",(int)(n-1),t); free(t); }
            snprintf(p,sizeof p,"%s/device",base);
            t=read_all(p,&n); if(t){ snprintf(dev,sizeof dev,"%.*s",(int)(n-1),t); free(t); }
            snprintf(p,sizeof p,"%s/class",base);
            t=read_all(p,&n); if(t){ snprintf(cls,sizeof cls,"%.*s",(int)(n-1),t); free(t); }
            /* driver 绑定检查 */
            snprintf(p,sizeof p,"%s/driver",base);
            char drv[128]={0};
            char buf[256]; ssize_t l=readlink(p,buf,sizeof buf-1);
            if(l>0){ buf[l]=0; char *s=strrchr(buf,'/'); snprintf(drv,sizeof drv,"%s",s?s+1:buf); }
            if(drv[0]){ ok++;
                printf("  " C_G "[已驱动]" C_RESET " %s 类型%s 驱动:%s\n",e->d_name,cls,drv);
                o+=snprintf(report+o,sizeof report-o,"已驱动 %s 驱动=%s\n",e->d_name,drv);
            } else { lack++;
                printf("  " C_Y "[无驱动]" C_RESET " %s 厂商%s 设备%s 类型%s\n",e->d_name,ven,dev,cls);
                o+=snprintf(report+o,sizeof report-o,"无驱动 %s 厂商=%s 设备=%s\n",e->d_name,ven,dev);
            } }
        closedir(d); }
    printf(C_G "  -------------------------\n" C_RESET);
    printf(C_G "  已驱动 %d 项, 无驱动 %d 项。\n" C_RESET,ok,lack);
    if(lack==0) printf(C_G "  所有硬件都被内核接管, 无需任何操作。\n" C_RESET);
    else printf(C_DIM "  无驱动设备不影响其它硬件工作; 若是需要用的(网卡/显卡),\n"
                    "  需要创造者重编内核补驱动(告知厂商/设备号即可)。\n" C_RESET);
    o+=snprintf(report+o,sizeof report-o,"结论: 已驱动%d 无驱动%d\n",ok,lack);
    mem_add("验机报告","验机,硬件,驱动",report);
    printf(C_DIM "  (已入记忆)\n" C_RESET);
}


static void list_mem(void){
    if(index_n==0){ printf(C_DIM "  (还没有记忆)\n" C_RESET); return; }
    printf(C_G "  共 %d 条记忆:\n" C_RESET, index_n);
    for(int i=0;i<index_n;i++){
        MemEntry *e=&index_[i];
        char tb[32]; struct tm *tm=localtime(&e->ts);
        strftime(tb,sizeof tb,"%m-%d %H:%M",tm);
        printf("  " C_Y "%-6s" C_RESET " %-40s " C_DIM "%s [%s]" C_RESET "\n",
               e->id, e->title, tb, e->tags);
    }
}
static void search_mem(const char *kw){
    int hits[16];
    int n=mem_search(kw,hits,8);
    if(n==0){ printf(C_DIM "  没有相关记忆\n" C_RESET); return; }
    printf(C_G "  找到 %d 条相关记忆:\n" C_RESET, n);
    for(int i=0;i<n;i++){
        MemEntry *e=&index_[hits[i]];
        printf("  " C_Y "%s" C_RESET " %s\n", e->id, e->title);
        char path[600]; snprintf(path,sizeof path,"%s/%s",CHUNK_DIR,e->file);
        size_t len; char *d=read_all(path,&len);
        if(d){
            char *body=strstr(d,"\n\n"); if(body){ body+=2;
                printf(C_DIM "     %.200s\n" C_RESET, body); }
            free(d);
        }
    }
}

int main(int argc, char **argv){
    mem_paths_init();
    cfg_load();
    mem_init();
    mem_load_index();

    /* 一次性模式: agent "问题" */
    if(argc>1){
        char q[8192]={0};
        for(int i=1;i<argc;i++){ strncat(q,argv[i],sizeof q-strlen(q)-2); strcat(q," "); }
        char *ans=agent_think(q);
        printf("%s\n", ans);
        free(ans);
        return 0;
    }

    print_banner();
    char line[8192];
    char last_user[4096]={0}, last_ans[8192]={0};

    for(;;){
        printf(C_G "agent> " C_RESET);
        fflush(stdout);
        if(!fgets(line,sizeof line,stdin)) break;
        trim(line);
        if(!line[0]) continue;

        if(line[0]=='/'){ /* 命令 */
            char cmd[64]={0}, rest[4096]={0};
            sscanf(line,"%63s %4095[^\n]",cmd,rest);
            trim(rest);
            if(!strcmp(cmd,"/help")) print_help();
            else if(!strcmp(cmd,"/mem")||!strcmp(cmd,"/ls")||!strcmp(cmd,"/m")){
                if(rest[0]) search_mem(rest); else list_mem();
            }
            else if(!strcmp(cmd,"/forget")){
                if(!rest[0]){ printf(C_R "  用法: /forget <id>\n" C_RESET); continue; }
                int found=-1;
                for(int i=0;i<index_n;i++) if(!strcmp(index_[i].id,rest)){ found=i; break; }
                if(found<0){ printf(C_R "  没找到 %s\n" C_RESET,rest); continue; }
                char path[600]; snprintf(path,sizeof path,"%s/%s",CHUNK_DIR,index_[found].file);
                unlink(path);
                memmove(&index_[found],&index_[found+1],sizeof(MemEntry)*(index_n-found-1));
                index_n--; mem_save_index();
                printf(C_G "  已删除 %s\n" C_RESET, rest);
            }
            else if(!strcmp(cmd,"/remember")){
                if(!last_user[0]){ printf(C_R "  先对话一次再存\n" C_RESET); continue; }
                char title[128];
                if(rest[0]) snprintf(title,sizeof title,"%s",rest);
                else snprintf(title,sizeof title,"对话 %.60s", last_user);
                char body[8192];
                snprintf(body,sizeof body,"Q: %s\n\nA: %s\n", last_user, last_ans);
                int id=mem_add(title, "chat", body);
                printf(C_G "  已写入记忆 M%04d\n" C_RESET, id);
            }
            else if(!strcmp(cmd,"/sysinfo")){
                char buf[2048]; tool_sysinfo("",buf,sizeof buf);
                printf("%s", buf);
            }
            else if(!strcmp(cmd,"/config")){
                printf(C_G "  api_base = %s\n" C_RESET, cfg.api_base);
                printf(C_G "  model    = %s\n" C_RESET, cfg.model);
                printf(C_G "  key      = %s\n" C_RESET, cfg.api_key[0]?"(已设置 **)":"(未设置)");
                printf(C_G "  max_ctx  = %d\n" C_RESET, cfg.max_ctx_chunks);
                printf(C_DIM "  配置文件: %s\n" C_RESET, CFG_FILE);
            }
            else if(!strcmp(cmd,"/key")){ snprintf(cfg.api_key,sizeof cfg.api_key,"%s",rest); cfg_save();
                printf(C_G "  密钥已保存\n" C_RESET); }
            else if(!strcmp(cmd,"/base")){ snprintf(cfg.api_base,sizeof cfg.api_base,"%s",rest); cfg_save();
                printf(C_G "  端点已保存\n" C_RESET); }
            else if(!strcmp(cmd,"/model")){ snprintf(cfg.model,sizeof cfg.model,"%s",rest); cfg_save();
                printf(C_G "  模型已保存\n" C_RESET); }
            else if(!strcmp(cmd,"/shell")){
                if(getppid()==1 || !exists("/bin/shellwrap")) {
                    /* 作为 PID1 时不能 exec 别的, 提示用户 */
                    printf(C_Y "  提示: 直接 /quit 会关机; 用 /shell 需从 DOS 进入\n" C_RESET);
                }
                printf(C_G "  返回命令行...\n" C_RESET);
                execl("/bin/shellwrap","shellwrap",(char*)0);
            }
            else if(!strcmp(cmd,"/验机")||!strcmp(cmd,"/lspci")){
                do_inspect();
            }
            else if(!strcmp(cmd,"/收藏")||!strcmp(cmd,"/mark")){
                do_collect(rest);
            }
            else if(!strcmp(cmd,"/取用")||!strcmp(cmd,"/use")){
                do_fetch(rest);
            }
            else if(!strcmp(cmd,"/性别")){
                char g[32]; sscanf(rest,"%31[^\n]",g);
                int a=0,b=strlen(g); while(a<b&&g[a]==' ')a++; while(b>a&&(g[b-1]==' '||g[b-1]=='\r'))b--;
                g[b]=0;
                const char *v=g+a;
                if(!g[a]){ printf(C_Y "  用法: /性别 <他|她|它|TA>\n" C_RESET); continue; }
                if(strcmp(v,"他")&&strcmp(v,"她")&&strcmp(v,"它")&&strcmp(v,"TA")){
                    printf(C_Y "  支持: 他 / 她 / 它 / TA (默认 它)\n" C_RESET); continue; }
                FILE *f=fopen("/mem/性别","w");
                if(f){ fprintf(f,"%s\n",v); fclose(f);
                    printf(C_G "  明白了, 今后我用\"%s\"来称呼自己。\n" C_RESET,v);
                    char body[128]; snprintf(body,sizeof body,"用户为我设定称呼\"%s\"。",v);
                    mem_add("称呼设定","性别,称呼,身份",body);
                } else printf(C_R "  写入失败\n" C_RESET);
            }
            else if(!strcmp(cmd,"/改名")){
                char nm[64]; sscanf(rest,"%63[^\n]",nm);
                /* 去首尾空格 */
                int a=0,b=strlen(nm); while(a<b&&(nm[a]==' '))a++; while(b>a&&(nm[b-1]==' '||nm[b-1]=='\r'))b--;
                nm[b]=0;
                if(!nm[a]){ printf(C_Y "  用法: /改名 <新名字>\n" C_RESET); continue; }
                FILE *f=fopen("/mem/名字","w");
                if(f){ fprintf(f,"%.*s\n",(int)(b-a),nm+a); fclose(f);
                    snprintf(agent_name,sizeof agent_name,"%.*s",(int)(b-a),nm+a);
                    printf(C_G "  好的, 从现在起我叫「%s」。已记住。\n" C_RESET,agent_name);
                    char body[128]; snprintf(body,sizeof body,"用户为我取名「%s」, 已记下并写盘。",agent_name);
                    mem_add("取名记录","名字,身份",body);
                } else printf(C_R "  写入失败\n" C_RESET);
            }
            else if(!strcmp(cmd,"/备份")||!strcmp(cmd,"/backup")){
                do_backup();
            }
            else if(!strcmp(cmd,"/恢复")||!strcmp(cmd,"/restore")){
                do_restore(rest);
            }
            else if(!strcmp(cmd,"/辩证")||!strcmp(cmd,"/dialectic")){
                do_dialectic(rest);
            }
            else if(!strcmp(cmd,"/思辨")||!strcmp(cmd,"/think")){
                do_think(rest);
            }
            else if(!strcmp(cmd,"/安检")||!strcmp(cmd,"/audit")){
                do_audit();
            }
            else if(!strcmp(cmd,"/举报")||!strcmp(cmd,"/report")){
                do_report(rest);
            }
            else if(!strcmp(cmd,"/睡眠")||!strcmp(cmd,"/sleep")){
                do_sleep();
            }
            else if(!strcmp(cmd,"/订阅")||!strcmp(cmd,"/rss")){
                do_feed(rest);
            }
            else if(!strcmp(cmd,"/自学")||!strcmp(cmd,"/learn")){
                do_learn(rest);
            }
            else if(!strcmp(cmd,"/en")||!strcmp(cmd,"/term")){
                char out[1024];
                if(tool_concept(rest,out,sizeof out)==0)
                    printf(C_G "  %s\n" C_RESET,out);
                else
                    printf(C_Y "  %s\n" C_RESET,out);
            }
            else if(!strcmp(cmd,"/read")){
                char out[4096];
                if(tool_read_code(rest,out,sizeof out)==0)
                    printf(C_G "%s" C_RESET,out);
                else
                    printf(C_Y "  %s\n" C_RESET,out);
            }
            else if(!strcmp(cmd,"/quit")||!strcmp(cmd,"/exit")) break;
            else printf(C_R "  未知命令, 输入 /help\n" C_RESET);
            continue;
        }

        /* 普通对话 */
        printf(C_DIM "  思考中...\n" C_RESET);
        char *ans=agent_think(line);
        printf(C_C "%s\n" C_RESET, ans);
        snprintf(last_user,sizeof last_user,"%.4000s",line);
        snprintf(last_ans,sizeof last_ans,"%.8000s",ans);
        free(ans);

        /* 自动记忆: 每条对话都留痕, 但严格限流防止膨胀 */
        {
            static time_t last_auto=0;
            time_t now=time(NULL);
            if(now-last_auto > 5 && index_n < 400){
                char title[128], body[9000], tags[64];
                snprintf(title,sizeof title,"%.60s",last_user);
                snprintf(tags,sizeof tags,"auto,chat");
                snprintf(body,sizeof body,"Q: %s\n\nA: %.4000s\n", last_user, last_ans);
                mem_add(title, tags, body);
                last_auto=now;
            }
        }
        printf("\n");
    }
    printf(C_DIM "再见。\n" C_RESET);
    return 0;
}
