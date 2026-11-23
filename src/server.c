#define _DEFAULT_SOURCE
#include "banking.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define MAX_SESSIONS 256
typedef struct { char token[33]; BankAccount a, b; pthread_rwlock_t gate; } Session;
static Session sessions[MAX_SESSIONS];
static int session_count;
static pthread_mutex_t sessions_lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_int clients;
static int listen_fd;
static volatile sig_atomic_t stopping;
static const char *public_dir = "public";

static void stop_server(int sig) { (void)sig; stopping = 1; close(listen_fd); }
static int send_all(int fd, const char *data, size_t n) {
    while (n) { ssize_t sent = send(fd,data,n,MSG_NOSIGNAL); if(sent <= 0) return -1; data += sent; n -= (size_t)sent; }
    return 0;
}
static void reply(int fd, int status, const char *type, const char *body, const char *cookie) {
    char header[1024]; size_t len = strlen(body);
    snprintf(header,sizeof(header),"HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nContent-Security-Policy: default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' data:; connect-src 'self'; object-src 'none'; base-uri 'none'; frame-ancestors 'none'\r\n%s\r\n",
             status,status == 200 ? "OK" : "Error",type,len,cookie ? cookie : "");
    if (!send_all(fd,header,strlen(header))) send_all(fd,body,len);
}
static void error_reply(int fd, int status, const char *message) {
    char json[256]; snprintf(json,sizeof(json),"{\"ok\":false,\"error\":\"%s\"}",message);
    reply(fd,status,"application/json; charset=utf-8",json,NULL);
}
static void escape_json(const char *in, char *out, size_t size) {
    size_t n = 0;
    for (; *in && n + 7 < size; in++) {
        unsigned char c = (unsigned char)*in;
        if (c == '"' || c == '\\') { out[n++] = '\\'; out[n++] = (char)c; }
        else if (c == '\n') { out[n++] = '\\'; out[n++] = 'n'; }
        else if (c == '\r') { out[n++] = '\\'; out[n++] = 'r'; }
        else if (c < 32) n += (size_t)snprintf(out+n,size-n,"\\u%04x",c);
        else out[n++] = (char)c;
    }
    out[n] = '\0';
}
static int get_header(const char *req, const char *key, char *value, size_t size) {
    const char *p = strstr(req,"\r\n"); size_t key_len = strlen(key);
    if (!p) return 0;
    p += 2;
    while (*p && strncmp(p,"\r\n",2)) {
        const char *end = strstr(p,"\r\n"); if(!end) return 0;
        if ((size_t)(end-p) > key_len && !strncasecmp(p,key,key_len) && p[key_len] == ':') {
            const char *v = p+key_len+1; while(v<end && (*v==' ' || *v=='\t')) v++;
            size_t n=(size_t)(end-v); if(n>=size) return -1; memcpy(value,v,n); value[n]='\0'; return 1;
        }
        p = end+2;
    }
    return 0;
}
static int form_value(const char *body, const char *key, char *out, size_t cap) {
    size_t key_len = strlen(key); const char *p = body;
    while (*p) {
        const char *end = strchr(p,'&'); if(!end) end=p+strlen(p);
        if ((size_t)(end-p)>key_len && !strncmp(p,key,key_len) && p[key_len]=='=') {
            size_t n=0; p+=key_len+1;
            while (p<end) {
                unsigned char c=(unsigned char)*p++;
                if(c=='+') c=' ';
                else if(c=='%') {
                    if(end-p<2 || !isxdigit((unsigned char)p[0]) || !isxdigit((unsigned char)p[1])) return -1;
                    char hex[3]={p[0],p[1],0}; c=(unsigned char)strtoul(hex,NULL,16); p+=2;
                }
                if(c<32 || c==127 || n+1>=cap) return -1;
                out[n++]=(char)c;
            }
            out[n]=0; return 1;
        }
        p=*end ? end+1 : end;
    }
    out[0]=0; return 0;
}
static Session *get_session(const char *req, char *cookie, size_t cap) {
    char cookies[2048]={0}, token[33]={0};
    if(get_header(req,"Cookie",cookies,sizeof(cookies))>0) {
        const char *p=cookies;
        while(*p) {
            while(*p==' ' || *p==';') p++;
            const char *end=strchr(p,';'); if(!end) end=p+strlen(p);
            if(end-p==37 && !strncmp(p,"bank=",5)) memcpy(token,p+5,32);
            p=end;
        }
    }
    pthread_mutex_lock(&sessions_lock);
    for(int i=0;i<session_count;i++) if(token[0] && !strcmp(sessions[i].token,token)) {
        pthread_mutex_unlock(&sessions_lock); return &sessions[i];
    }
    if(session_count==MAX_SESSIONS) { pthread_mutex_unlock(&sessions_lock); return NULL; }
    unsigned char bytes[16];
    if(getrandom(bytes,sizeof(bytes),0)!=(ssize_t)sizeof(bytes)) { pthread_mutex_unlock(&sessions_lock); return NULL; }
    Session *s=&sessions[session_count++];
    for(int i=0;i<16;i++) snprintf(s->token+i*2,3,"%02x",bytes[i]);
    init_account(&s->a,101,50000); init_account(&s->b,102,50000); pthread_rwlock_init(&s->gate,NULL);
    pthread_mutex_unlock(&sessions_lock);
    snprintf(cookie,cap,"Set-Cookie: bank=%s; Path=/; HttpOnly; SameSite=Strict\r\n",s->token);
    return s;
}
static void state_json(Session *s, char *out, size_t cap, const char *log) {
    char escaped[65536]; escape_json(log ? log : "",escaped,sizeof(escaped));
    pthread_mutex_lock(&s->a.lock); pthread_mutex_lock(&s->b.lock);
    double a=s->a.balance,b=s->b.balance;
    pthread_mutex_unlock(&s->b.lock); pthread_mutex_unlock(&s->a.lock);
    snprintf(out,cap,"{\"ok\":true,\"accounts\":[{\"id\":101,\"balance\":%.2f},{\"id\":102,\"balance\":%.2f}],\"total\":%.2f,\"log\":\"%s\"}",a,b,a+b,escaped);
}
static void static_file(int fd, const char *path) {
    const char *file,*type;
    if(!strcmp(path,"/") || !strcmp(path,"/index.html")) { file="index.html"; type="text/html; charset=utf-8"; }
    else if(!strcmp(path,"/app.js")) { file="app.js"; type="text/javascript; charset=utf-8"; }
    else if(!strcmp(path,"/style.css")) { file="style.css"; type="text/css; charset=utf-8"; }
    else { error_reply(fd,404,"Not found"); return; }
    char name[1024]; snprintf(name,sizeof(name),"%s/%s",public_dir,file);
    FILE *f=fopen(name,"rb"); if(!f) { error_reply(fd,500,"Frontend file missing"); return; }
    char *data=calloc(1,131073); if(!data) { fclose(f); error_reply(fd,503,"Server busy"); return; }
    size_t n=fread(data,1,131072,f); fclose(f); data[n]=0;
    reply(fd,200,type,data,NULL); free(data);
}
static void handle_request(int fd) {
    char req[8193]={0}; size_t used=0,header_len=0,body_len=0; char *body=NULL;
    struct timeval timeout={5,0}; setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
    setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
    while(used<sizeof(req)-1) {
        ssize_t n=recv(fd,req+used,sizeof(req)-1-used,0); if(n<=0) return;
        used+=(size_t)n; req[used]=0;
        if(!header_len && (body=strstr(req,"\r\n\r\n"))) {
            header_len=(size_t)(body-req)+4; body=req+header_len;
            char value[64];
            if(get_header(req,"Transfer-Encoding",value,sizeof(value))) { error_reply(fd,400,"Transfer encoding is unsupported"); return; }
            int h=get_header(req,"Content-Length",value,sizeof(value));
            if(h<0) { error_reply(fd,400,"Invalid content length"); return; }
            if(h) {
                char *end; errno=0; unsigned long parsed=strtoul(value,&end,10);
                if(errno || !*value || *end || parsed>2048) { error_reply(fd,413,"Request body too large or invalid"); return; }
                body_len=(size_t)parsed;
            }
        }
        if(header_len && used>=header_len+body_len) { req[header_len+body_len]=0; break; }
    }
    if(!header_len || used<header_len+body_len) { error_reply(fd,400,"Incomplete request"); return; }
    char method[12],path[256],version[16];
    if(sscanf(req,"%11s %255s %15s",method,path,version)!=3) { error_reply(fd,400,"Invalid request"); return; }
    if(!strcmp(path,"/health")) { reply(fd,200,"application/json","{\"ok\":true,\"backend\":\"C / POSIX pthreads\"}",NULL); return; }
    if(strncmp(path,"/api/",5)) {
        if(strcmp(method,"GET")) error_reply(fd,405,"Method not allowed"); else static_file(fd,path);
        return;
    }
    int write=!strcmp(method,"POST");
    if(strcmp(method,"GET") && !write) { error_reply(fd,405,"Method not allowed"); return; }
    if(write) {
        char marker[32];
        if(get_header(req,"X-Bank-Request",marker,sizeof(marker))!=1 || strcmp(marker,"1")) { error_reply(fd,403,"Missing request header"); return; }
    }
    if((!strcmp(path,"/api/state") && write) || (strcmp(path,"/api/state") && !write)) { error_reply(fd,405,"Method not allowed"); return; }
    if(strcmp(path,"/api/state") && strcmp(path,"/api/transaction") && strcmp(path,"/api/reset") && strcmp(path,"/api/demo")) { error_reply(fd,404,"Not found"); return; }
    char cookie[160]={0}; Session *s=get_session(req,cookie,sizeof(cookie));
    if(!s) { error_reply(fd,503,"Session capacity reached. Restart the demo service."); return; }
    char *json=malloc(70000); if(!json) { error_reply(fd,503,"Server busy"); return; }
    if(!strcmp(path,"/api/state")) {
        pthread_rwlock_rdlock(&s->gate); state_json(s,json,70000,NULL); pthread_rwlock_unlock(&s->gate);
    } else if(!strcmp(path,"/api/reset")) {
        pthread_rwlock_wrlock(&s->gate); s->a.balance=s->b.balance=50000; state_json(s,json,70000,"Demo accounts reset to ₹50,000 each.\n"); pthread_rwlock_unlock(&s->gate);
    } else if(!strcmp(path,"/api/demo")) {
        char kind[24];
        if(form_value(body,"kind",kind,sizeof(kind))!=1 || (strcmp(kind,"race") && strcmp(kind,"safe") && strcmp(kind,"transfer") && strcmp(kind,"assignment"))) { free(json); error_reply(fd,400,"Invalid demo"); return; }
        BankAccount a,b; Audit audit; double expected,actual;
        int deposit=!strcmp(kind,"race") || !strcmp(kind,"safe");
        init_account(&a,deposit?100:101,deposit?10000:50000); init_account(&b,102,50000); audit_init(&audit);
        int result=run_batch(&a,&b,kind,&audit,&expected,&actual);
        char escaped[65536]; escape_json(audit.text,escaped,sizeof(escaped));
        snprintf(json,70000,"{\"ok\":%s,\"kind\":\"%s\",\"expected\":%.2f,\"actual\":%.2f,\"account101\":%.2f,\"account102\":%.2f,\"log\":\"%s\"}",result?"false":"true",kind,expected,actual,a.balance,b.balance,escaped);
        pthread_mutex_destroy(&a.lock); pthread_mutex_destroy(&b.lock); pthread_mutex_destroy(&audit.lock);
    } else {
        char op[24]={0},account[12]={0},target[12]={0},value[64]={0},channel[32]={0};
        int fields=form_value(body,"operation",op,sizeof(op))==1 && form_value(body,"account",account,sizeof(account))==1;
        int channel_ok=form_value(body,"channel",channel,sizeof(channel));
        if(channel_ok==0) strcpy(channel,"Online-Web");
        int type=!strcmp(op,"deposit")?1:!strcmp(op,"withdraw")?2:!strcmp(op,"enquiry")?3:!strcmp(op,"transfer")?4:0;
        BankAccount *from=!strcmp(account,"101")?&s->a:!strcmp(account,"102")?&s->b:NULL,*to=NULL;
        double amount=0;
        if(type!=3) {
            if(form_value(body,"amount",value,sizeof(value))!=1) fields=0;
            else {
                char *end; errno=0; amount=strtod(value,&end);
                if(errno || !*value || *end || !isfinite(amount) || amount<=0 || amount>1000000 || fabs(amount*100-round(amount*100))>0.00001) fields=0;
            }
        }
        if(type==4) {
            if(form_value(body,"target",target,sizeof(target))!=1) fields=0;
            else to=!strcmp(target,"101")?&s->a:!strcmp(target,"102")?&s->b:NULL;
            if(!to || from==to) fields=0;
        }
        if(!fields || !type || !from || channel_ok<0 || (strcmp(channel,"ATM-01") && strcmp(channel,"ATM-02") && strcmp(channel,"Online-Web") && strcmp(channel,"Mobile-UPI"))) { free(json); error_reply(fd,400,"Invalid transaction, account or amount (use positive amounts up to 1000000, with at most two decimals)"); return; }
        Audit audit; audit_init(&audit);
        TransactionTask *task=create_task(from,to,amount,channel,type);
        if(!task) { pthread_mutex_destroy(&audit.lock); free(json); error_reply(fd,503,"Server busy"); return; }
        task->audit=&audit;
        pthread_rwlock_rdlock(&s->gate);
        /* This HTTP connection is already a real POSIX worker thread. */
        execute_transaction(task);
        state_json(s,json,70000,audit.text);
        pthread_rwlock_unlock(&s->gate);
        pthread_mutex_destroy(&audit.lock);
    }
    reply(fd,200,"application/json; charset=utf-8",json,cookie); free(json);
}
static void *client_thread(void *arg) {
    int fd=*(int *)arg; free(arg); handle_request(fd); close(fd); atomic_fetch_sub(&clients,1); return NULL;
}
int main(int argc,char **argv) {
    if(argc>1) public_dir=argv[1];
    const char *port_env=getenv("PORT");
    for(int i=2;i+1<argc;i++) if(!strcmp(argv[i],"--port")) port_env=argv[++i];
    char *end=NULL; long port=port_env?strtol(port_env,&end,10):10000;
    if(port<1 || port>65535 || (port_env && (!*port_env || *end))) { fprintf(stderr,"Invalid PORT\n"); return 1; }
    signal(SIGPIPE,SIG_IGN); signal(SIGTERM,stop_server); signal(SIGINT,stop_server);
    listen_fd=socket(AF_INET,SOCK_STREAM,0); int reuse=1;
    setsockopt(listen_fd,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));
    struct sockaddr_in addr={.sin_family=AF_INET,.sin_port=htons((unsigned short)port),.sin_addr={.s_addr=INADDR_ANY}};
    if(listen_fd<0 || bind(listen_fd,(struct sockaddr *)&addr,sizeof(addr)) || listen(listen_fd,64)) { perror("listen"); return 1; }
    printf("Banking / ATM C backend listening on 0.0.0.0:%ld\n",port); fflush(stdout);
    while(!stopping) {
        int fd=accept(listen_fd,NULL,NULL); if(fd<0) { if(stopping) break; continue; }
        if(atomic_fetch_add(&clients,1)>=32) { atomic_fetch_sub(&clients,1); error_reply(fd,503,"Server busy"); close(fd); continue; }
        int *arg=malloc(sizeof(int)); pthread_t thread;
        if(!arg) { atomic_fetch_sub(&clients,1); close(fd); continue; }
        *arg=fd;
        if(pthread_create(&thread,NULL,client_thread,arg)) { free(arg); atomic_fetch_sub(&clients,1); close(fd); }
        else pthread_detach(thread);
    }
    while(atomic_load(&clients)) usleep(10000);
    return 0;
}
