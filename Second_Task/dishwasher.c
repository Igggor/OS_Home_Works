#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>

#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/shm.h>
#include <sys/sem.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <semaphore.h>
#include <time.h>

#define MAX_TYPE 64
#define MAX_ITEMS 10000
#define PATH_FTOK "/tmp"
#define PROJ_ID 42


typedef struct {
    char type[MAX_TYPE];
    int time;
} type_time_t;

typedef struct {
    char type[MAX_TYPE];
    int count;
} dirty_entry_t;

typedef struct {
    type_time_t *arr;
    int n;
} time_map_t;

time_map_t read_time_file(const char *path) {
    time_map_t map = {NULL,0};
    FILE *f = fopen(path,"r");
    if (!f) {
        perror("fopen time file");
        exit(1);
    }
    map.arr = malloc(sizeof(type_time_t)*256);
    map.n = 0;
    char line[256];
    while (fgets(line,sizeof(line),f)) {
        char *p = strchr(line,':');
        if (!p) continue;
        *p = 0;

        char *name = line;
        char *num = p+1;
        while(*name && isspace((unsigned char)*name))
            ++name;

        char *end = name + strlen(name)-1;
        while(end > name && isspace((unsigned char)*end))
            *end--=0;

        while(*num && isspace((unsigned char)*num))
            ++num;
        int t = atoi(num);
        if (t<=0) continue;
        strncpy(map.arr[map.n].type, name, MAX_TYPE-1);
        map.arr[map.n].time = t;
        map.n++;
    }
    fclose(f);
    return map;
}

int lookup_time(time_map_t *m, const char *type) {
    for(int i=0; i < m->n; ++i)
        if (strcmp(m->arr[i].type,type)==0)
            return m->arr[i].time;
    return -1;
}

typedef struct {
    dirty_entry_t *arr;
    int n;
} dirty_list_t;

dirty_list_t read_dirty_file(const char *path) {
    FILE *f = fopen(path,"r");
    if (!f) {
        perror("fopen dirty");
        exit(1);
    }
    dirty_list_t dl = {NULL,0};
    dl.arr = malloc(sizeof(dirty_entry_t)*1024);
    dl.n = 0;
    char line[256];
    while (fgets(line,sizeof(line),f)) {
        char *p = strchr(line,':');
        if (!p) continue;
        *p=0;

        char *name=line;
        char *num=p+1;
        while(*name && isspace((unsigned char)*name))
            ++name;

        char *end = name + strlen(name)-1;
        while(end>name && isspace((unsigned char)*end))
            *end--=0;

        while(*num && isspace((unsigned char)*num))
            ++num;

        int c = atoi(num);
        if (c <= 0) continue;

        strncpy(dl.arr[dl.n].type,name,MAX_TYPE-1);
        dl.arr[dl.n].count=c;
        dl.n++;
    }

    fclose(f);
    return dl;
}

int get_table_limit() {
    char *s = getenv("TABLE_LIMIT");
    if (!s) {
        fprintf(stderr,"Please set TABLE_LIMIT environment variable > 0\n");
        exit(1);
    }

    int v = atoi(s);
    if (v<=0) {
        fprintf(stderr,"TABLE_LIMIT must be >0\n");
        exit(1);
    }

    return v;
}


#define MODE_PIPE 1
#define MODE_FIFO 2
#define MODE_MSG  3
#define MODE_SHM  4
#define MODE_SOCK 5
#define MODE_FILESEM 6

int mode_global = 0;
int table_limit = 0;
pid_t parent_pid = 0;

sem_t *gen_mutex = NULL;
sem_t *gen_empty = NULL;
sem_t *gen_full = NULL;
char gen_mutex_name[128];
char gen_empty_name[128];
char gen_full_name[128];

int sem_gen_init(int cap) {
    snprintf(gen_mutex_name, sizeof(gen_mutex_name), "/dish_gen_mutex_%d", parent_pid);
    snprintf(gen_empty_name, sizeof(gen_empty_name), "/dish_gen_empty_%d", parent_pid);
    snprintf(gen_full_name, sizeof(gen_full_name), "/dish_gen_full_%d", parent_pid);

    sem_unlink(gen_mutex_name);
    sem_unlink(gen_empty_name);
    sem_unlink(gen_full_name);

    gen_mutex = sem_open(gen_mutex_name, O_CREAT|O_EXCL, 0600, 1);
    gen_empty = sem_open(gen_empty_name, O_CREAT|O_EXCL, 0600, cap);
    gen_full  = sem_open(gen_full_name, O_CREAT|O_EXCL, 0600, 0);

    if (gen_mutex == SEM_FAILED || gen_empty == SEM_FAILED || gen_full == SEM_FAILED) {
        if (gen_mutex == SEM_FAILED) 
            gen_mutex = sem_open(gen_mutex_name, 0);
        if (gen_empty == SEM_FAILED) 
            gen_empty = sem_open(gen_empty_name, 0);
        if (gen_full  == SEM_FAILED) 
            gen_full  = sem_open(gen_full_name, 0);
    }

    if (gen_mutex == SEM_FAILED || gen_empty == SEM_FAILED || gen_full == SEM_FAILED) {
        perror("sem_open gen");
        return -1;
    }
    return 0;
}

int sem_gen_cleanup() {
    if (gen_mutex) { 
        sem_close(gen_mutex); 
        gen_mutex = NULL; 
    }
    if (gen_empty) { 
        sem_close(gen_empty); 
        gen_empty = NULL; 
    }
    if (gen_full)  { 
        sem_close(gen_full); 
        gen_full = NULL; 
    }

    sem_unlink(gen_mutex_name);
    sem_unlink(gen_empty_name);
    sem_unlink(gen_full_name);
    return 0;
}

int sem_gen_before_put() {
    if (!gen_empty || !gen_mutex) return 0;
    if (sem_wait(gen_empty) < 0) { perror("sem_wait gen_empty"); return -1; }
    if (sem_wait(gen_mutex) < 0) { perror("sem_wait gen_mutex"); return -1; }
    return 0;
}
int sem_gen_after_put() {
    if (!gen_mutex || !gen_full) return 0;
    if (sem_post(gen_mutex) < 0) { perror("sem_post gen_mutex"); return -1; }
    if (sem_post(gen_full) < 0) { perror("sem_post gen_full"); return -1; }
    return 0;
}
int sem_gen_before_get() {
    if (!gen_full || !gen_mutex) return 0;
    if (sem_wait(gen_full) < 0) { perror("sem_wait gen_full"); return -1; }
    if (sem_wait(gen_mutex) < 0) { perror("sem_wait gen_mutex"); return -1; }
    return 0;
}
int sem_gen_after_get() {
    if (!gen_mutex || !gen_empty) return 0;
    if (sem_post(gen_mutex) < 0) { perror("sem_post gen_mutex"); return -1; }
    if (sem_post(gen_empty) < 0) { perror("sem_post gen_empty"); return -1; }
    return 0;
}


int pipe_fds[2];

int ipc_pipe_init(int limit) {
    (void)limit;
    if (pipe(pipe_fds) < 0) {
        perror("pipe");
        return -1;
    }
    return 0;
}
int ipc_pipe_cleanup() {
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    return 0;
}
int ipc_pipe_put(const char *s) {
    size_t L = strlen(s)+1;
    if (write(pipe_fds[1], s, L) != (ssize_t)L) {
        perror("write pipe");
        return -1;
    }
    return 0;
}
int ipc_pipe_get(char *out) {
    ssize_t r = read(pipe_fds[0], out, MAX_TYPE);
    if (r <= 0) return -1;
    out[r > 0 ? r : 0]=0;
    return 0;
}

char fifo_path[256];
int fifo_fd_w = -1, fifo_fd_r = -1;

int ipc_fifo_init(int limit) {
    (void)limit;
    char path[256];
    snprintf(path,sizeof(path),"/tmp/dish_fifo_%d",parent_pid);
    unlink(path);
    if (mkfifo(path, 0600) < 0) {
        perror("mkfifo");
        return -1;
    }
    strncpy(fifo_path,path,sizeof(fifo_path)-1);
    fifo_path[sizeof(fifo_path)-1]=0;
    return 0;
}
int ipc_fifo_cleanup() {
    if (fifo_fd_r>=0)
        close(fifo_fd_r);
    if (fifo_fd_w>=0)
        close(fifo_fd_w);
    unlink(fifo_path);
    return 0;
}
int ipc_fifo_put(const char *s) {
    if (fifo_fd_w < 0) {
        fifo_fd_w = open(fifo_path,O_WRONLY);
        if (fifo_fd_w < 0) {
            perror("open fifo w");
            return -1;
        }
    }
    size_t L = strlen(s)+1;
    if (write(fifo_fd_w, s, L) != (ssize_t)L) {
        perror("write fifo");
        return -1;
    }
    return 0;
}
int ipc_fifo_get(char *out) {
    if (fifo_fd_r < 0) {
        fifo_fd_r = open(fifo_path,O_RDONLY);
        if (fifo_fd_r < 0) {
            perror("open fifo r");
            return -1;
        }
    }
    ssize_t r = read(fifo_fd_r, out, MAX_TYPE);
    if (r <= 0)
        return -1;
    out[r > 0 ? r : 0] = 0;
    return 0;
}

int msqid = -1;
struct dish_msg {
    long mtype;
    char mtext[MAX_TYPE];
};

int ipc_msg_init(int limit) {
    (void)limit;
    key_t key = ftok(PATH_FTOK, PROJ_ID);
    if (key == -1) {
        perror("ftok msg");
        return -1;
    }
    msqid = msgget(key, IPC_CREAT | 0666);
    if (msqid < 0) {
        perror("msgget");
        return -1;
    }
    return 0;
}
int ipc_msg_cleanup() {
    if (msqid >= 0) {
        msgctl(msqid, IPC_RMID, NULL);
        msqid=-1;
    }
    return 0;
}
int ipc_msg_put(const char *s) {
    struct dish_msg mb;
    mb.mtype = 1;
    strncpy(mb.mtext, s, MAX_TYPE-1);
    if (msgsnd(msqid, &mb, strlen(mb.mtext)+1, 0) < 0) {
        perror("msgsnd");
        return -1;
    }
    return 0;
}
int ipc_msg_get(char *out) {
    struct dish_msg mb;
    if (msgrcv(msqid, &mb, MAX_TYPE, 0, 0) < 0) {
        perror("msgrcv");
        return -1;
    }
    strncpy(out, mb.mtext, MAX_TYPE-1);
    return 0;
}

int shm_id = -1;
int sem_id = -1;
char *shm_area = NULL;

struct shm_header {
    int in;
    int out;
    int cap;
};

int ipc_shm_init(int cap) {
    table_limit = cap;
    key_t key = ftok(PATH_FTOK, PROJ_ID+1);

    if (key == -1) {
        perror("ftok shm");
        return -1;
    }

    size_t total = sizeof(struct shm_header) + cap * MAX_TYPE;
    shm_id = shmget(key, total, IPC_CREAT | 0666);
    if (shm_id < 0) {
        perror("shmget");
        return -1;
    }
    shm_area = shmat(shm_id, NULL, 0);
    if (shm_area == (char*)-1) {
        perror("shmat");
        return -1;
    }

    struct shm_header *h = (struct shm_header*)shm_area;
    h->in = 0; 
    h->out = 0; 
    h->cap = cap;

    key_t skey = ftok(PATH_FTOK, PROJ_ID+2);
    if (skey == -1) {
        perror("ftok sem");
        return -1;
    }

    sem_id = semget(skey, 3, IPC_CREAT | 0666);
    if (sem_id < 0) {
        perror("semget");
        return -1;
    }

    unsigned short vals[3]; 
    vals[0]=1; 
    vals[1]=cap; 
    vals[2]=0;

    
    if (semctl(sem_id, 0, SETALL, vals) < 0) {
        perror("semctl setall");
        return -1;
    }
    return 0;
}
int ipc_shm_cleanup() {
    if (shm_area) {
        shmdt(shm_area);
        shm_area=NULL;
    }

    if (shm_id >= 0) {
        shmctl(shm_id, IPC_RMID, NULL);
        shm_id=-1;
    }

    if (sem_id >= 0) {
        semctl(sem_id, 0, IPC_RMID);
        sem_id=-1;
    }
    return 0;
}

int ipc_shm_put(const char *s) {
    struct sembuf ops[2];

    // empty--
    ops[0].sem_num = 1;
    ops[0].sem_op = -1;
    ops[0].sem_flg = 0;

    // mutex--
    ops[1].sem_num = 0;
    ops[1].sem_op = -1;
    ops[1].sem_flg = 0;


    if (semop(sem_id, ops, 2) < 0) {
        perror("semop put");
        return -1;
    }
    struct shm_header *h = (struct shm_header*)shm_area;
    char *slots = shm_area + sizeof(struct shm_header);
    int idx = h->in;
    char *dst = slots + idx * MAX_TYPE;
    strncpy(dst, s, MAX_TYPE-1);
    h->in = (h->in + 1) % h->cap;
    struct sembuf ops2[2];


    ops2[0].sem_num = 0;
    ops2[0].sem_op = 1;
    ops2[0].sem_flg = 0;


    ops2[1].sem_num = 2;
    ops2[1].sem_op = 1;
    ops2[1].sem_flg = 0;

    if (semop(sem_id, ops2, 2) < 0) {
        perror("semop put2");
        return -1;
    }
    return 0;
}
int ipc_shm_get(char *out) {
    struct sembuf ops[2];
    // full--
    ops[0].sem_num = 2;
    ops[0].sem_op = -1;
    ops[0].sem_flg = 0;


    // mutex--
    ops[1].sem_num = 0;
    ops[1].sem_op = -1;
    ops[1].sem_flg = 0;


    if (semop(sem_id, ops, 2) < 0) {
        perror("semop get");
        return -1;
    }

    struct shm_header *h = (struct shm_header*)shm_area;
    char *slots = shm_area + sizeof(struct shm_header);
    int idx = h->out;
    char *src = slots + idx * MAX_TYPE;
    strncpy(out, src, MAX_TYPE-1);
    out[MAX_TYPE-1]=0;
    h->out = (h->out + 1) % h->cap;
    struct sembuf ops2[2];
    // mutex++
    ops2[0].sem_num = 0;
    ops2[0].sem_op = 1;
    ops2[0].sem_flg = 0;

    // empty++
    ops2[1].sem_num = 1;
    ops2[1].sem_op = 1;
    ops2[1].sem_flg = 0;


    if (semop(sem_id, ops2, 2) < 0) {
        perror("semop get2");
        return -1;
    }
    return 0;
}

int sockpair_fds[2];

int ipc_sock_init(int limit) {
    (void)limit;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockpair_fds) < 0) {
        perror("socketpair");
        return -1;
    }
    return 0;
}
int ipc_sock_cleanup() {
    close(sockpair_fds[0]);
    close(sockpair_fds[1]);
    return 0;
}
int ipc_sock_put(const char *s) {
    size_t L = strlen(s)+1;
    if (write(sockpair_fds[0], s, L) != (ssize_t)L) {
        perror("sock write");
        return -1;
    }
    return 0;
}
int ipc_sock_get(char *out) {
    ssize_t r = read(sockpair_fds[1], out, MAX_TYPE);
    if (r <= 0)
        return -1;
    out[r > 0 ? r : 0] = 0;
    return 0;
}

char filebuf_path[256];
sem_t *file_sem_mutex=NULL, *file_sem_empty=NULL, *file_sem_full=NULL;

int ipc_filesem_init(int cap) {
    table_limit = cap;

    snprintf(filebuf_path, sizeof(filebuf_path), "/tmp/dish_buf_%d.bin", parent_pid);

    int fd = open(filebuf_path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        perror("open filebuf");
        return -1;
    }
    struct {
        int in; 
        int out; 
        int cap;
    } hdr;
    
    hdr.in=0; 
    hdr.out=0; 
    hdr.cap=cap;

    if (write(fd, &hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr)) {
        perror("write hdr");
        close(fd);
        return -1;
    }
    char z[MAX_TYPE]; memset(z,0,MAX_TYPE);

    for(int i=0; i < cap; ++i) {
        if (write(fd, z, MAX_TYPE) != MAX_TYPE) {
            perror("write slot");
            close(fd);
            return -1;
        }
    }
    close(fd);

    char name[64];
    snprintf(name,sizeof(name),"/dish_mutex_%d", parent_pid);
    sem_unlink(name);

    file_sem_mutex = sem_open(name, O_CREAT|O_EXCL, 0600, 1);
    snprintf(name,sizeof(name),"/dish_empty_%d", parent_pid);
    sem_unlink(name);
    file_sem_empty = sem_open(name, O_CREAT|O_EXCL, 0600, cap);
    snprintf(name,sizeof(name),"/dish_full_%d", parent_pid);
    sem_unlink(name);
    file_sem_full = sem_open(name, O_CREAT|O_EXCL, 0600, 0);
    if (file_sem_mutex==SEM_FAILED || file_sem_empty==SEM_FAILED || file_sem_full==SEM_FAILED) {
        perror("sem_open");
        return -1;
    }

    return 0;
}
int ipc_filesem_cleanup() {
    if (file_sem_mutex) {
        sem_close(file_sem_mutex);
        file_sem_mutex = NULL;
    }

    if (file_sem_empty) {
        sem_close(file_sem_empty);
        file_sem_empty = NULL;
    }

    if (file_sem_full) {
        sem_close(file_sem_full);
        file_sem_full = NULL;
    }

    char name[64];
    snprintf(name,sizeof(name),"/dish_mutex_%d", parent_pid); sem_unlink(name);
    snprintf(name,sizeof(name),"/dish_empty_%d", parent_pid); sem_unlink(name);
    snprintf(name,sizeof(name),"/dish_full_%d", parent_pid); sem_unlink(name);
    unlink(filebuf_path);
    return 0;
}
int ipc_filesem_put(const char *s) {
    sem_wait(file_sem_empty);
    sem_wait(file_sem_mutex);

    int fd = open(filebuf_path, O_RDWR);
    if (fd < 0) {
        perror("open file put");
        return -1;
    }

    struct {
        int in;
        int out;
        int cap;
    } hdr;

    if (pread(fd, &hdr, sizeof(hdr), 0) != (ssize_t)sizeof(hdr)) {
        perror("pread hdr");
        close(fd);
        return -1;
    }

    int idx = hdr.in;
    off_t off = sizeof(hdr) + idx * MAX_TYPE;

    char buf[MAX_TYPE]; memset(buf,0,MAX_TYPE); strncpy(buf, s, MAX_TYPE-1);
    if (pwrite(fd, buf, MAX_TYPE, off) != MAX_TYPE) {
        perror("pwrite slot");
        close(fd);
        return -1;
    }

    hdr.in = (hdr.in + 1) % hdr.cap;
    if (pwrite(fd, &hdr, sizeof(hdr), 0) != (ssize_t)sizeof(hdr)) {
        perror("pwrite hdr");
        close(fd);
        return -1;
    }

    close(fd);
    sem_post(file_sem_mutex);
    sem_post(file_sem_full);
    return 0;
}
int ipc_filesem_get(char *out) {
    sem_wait(file_sem_full);
    sem_wait(file_sem_mutex);

    int fd = open(filebuf_path, O_RDWR);
    if (fd < 0) {
        perror("open file get");
        return -1;
    }
    struct {
        int in;
        int out;
        int cap;
    } hdr;

    if (pread(fd, &hdr, sizeof(hdr), 0) != (ssize_t)sizeof(hdr)) {
        perror("pread hdr");
        close(fd);
        return -1;
    }

    int idx = hdr.out;
    off_t off = sizeof(hdr) + idx * MAX_TYPE;
    char buf[MAX_TYPE];

    if (pread(fd, buf, MAX_TYPE, off) != MAX_TYPE) {
        perror("pread slot");
        close(fd);
        return -1;
    }
    strncpy(out, buf, MAX_TYPE-1); out[MAX_TYPE-1]=0;
    hdr.out = (hdr.out + 1) % hdr.cap;
    if (pwrite(fd, &hdr, sizeof(hdr), 0) != (ssize_t)sizeof(hdr)) {
        perror("pwrite hdr");
        close(fd);
        return -1;
    }

    close(fd);
    sem_post(file_sem_mutex);
    sem_post(file_sem_empty);
    return 0;
}


int ipc_init(int mode, int cap) {
    switch(mode) {
        case MODE_PIPE:
            if (ipc_pipe_init(cap) < 0) 
                return -1;
            if (sem_gen_init(cap) < 0) 
                return -1;
            return 0;
        case MODE_FIFO:
            if (ipc_fifo_init(cap) < 0) 
                return -1;
            if (sem_gen_init(cap) < 0) 
                return -1;
            return 0;
        case MODE_MSG:
            if (ipc_msg_init(cap) < 0) 
                return -1;
            if (sem_gen_init(cap) < 0) 
                return -1;
            return 0;
        case MODE_SHM:
            return ipc_shm_init(cap);
        case MODE_SOCK:
            if (ipc_sock_init(cap) < 0) 
                return -1;
            if (sem_gen_init(cap) < 0) 
                return -1;
            return 0;
        case MODE_FILESEM:
            return ipc_filesem_init(cap);
        default: return -1;
    }
}

int ipc_cleanup(int mode) {
    switch(mode) {
        case MODE_PIPE:
            sem_gen_cleanup();
            return ipc_pipe_cleanup();
        case MODE_FIFO:
            sem_gen_cleanup();
            return ipc_fifo_cleanup();
        case MODE_MSG:
            sem_gen_cleanup();
            return ipc_msg_cleanup();
        case MODE_SHM:
            return ipc_shm_cleanup();
        case MODE_SOCK:
            sem_gen_cleanup();
            return ipc_sock_cleanup();
        case MODE_FILESEM:
            return ipc_filesem_cleanup();
        default: return -1;
    }
}

int ipc_put(int mode, const char *s) {
    int ret = -1;
    if (mode == MODE_PIPE || mode == MODE_FIFO || mode == MODE_MSG || mode == MODE_SOCK) {
        if (sem_gen_before_put() < 0) 
            return -1;
        
        
        if (mode == MODE_PIPE) 
            ret = ipc_pipe_put(s);
        else if (mode == MODE_FIFO) 
            ret = ipc_fifo_put(s);
        else if (mode == MODE_MSG) 
            ret = ipc_msg_put(s);
        else if (mode == MODE_SOCK) 
            ret = ipc_sock_put(s);
        
        
        if (ret < 0) {
            sem_post(gen_mutex);
            sem_post(gen_empty);
            return -1;
        }
        if (sem_gen_after_put() < 0) 
            return -1;
        return ret;
    } 
    else {
        if (mode == MODE_SHM) 
            return ipc_shm_put(s);
        if (mode == MODE_FILESEM) 
            return ipc_filesem_put(s);
    }
    return -1;
}

int ipc_get(int mode, char *out) {
    int ret = -1;
    if (mode == MODE_PIPE || mode == MODE_FIFO || mode == MODE_MSG || mode == MODE_SOCK) {
        if (sem_gen_before_get() < 0) 
            return -1;

        
        if (mode == MODE_PIPE) 
            ret = ipc_pipe_get(out);
        else if (mode == MODE_FIFO) 
            ret = ipc_fifo_get(out);
        else if (mode == MODE_MSG) 
            ret = ipc_msg_get(out);
        else if (mode == MODE_SOCK) 
            ret = ipc_sock_get(out);

        
        if (ret < 0) {
            sem_post(gen_mutex);
            sem_post(gen_full);
            return -1;
        }
        if (sem_gen_after_get() < 0) 
            return -1;
        return ret;
    } 
    else {
        if (mode == MODE_SHM) 
            return ipc_shm_get(out);
        if (mode == MODE_FILESEM) 
            return ipc_filesem_get(out);
    }
    return -1;
}


volatile sig_atomic_t stop_flag = 0;
void sigchld_handler(int s) {
    (void)s;
    while(waitpid(-1,NULL,WNOHANG) > 0);
}

void washer_proc(time_map_t *wash_map, dirty_list_t *dl) {
    for(int i=0;i<dl->n;i++) {
        for(int k=0;k<dl->arr[i].count;k++) {
            const char *type = dl->arr[i].type;
            int t = lookup_time(wash_map, type);

            if (t < 0) {
                fprintf(stderr,"[washer] unknown type '%s' in wash times, skipping\n", type);
                continue;
            }

            printf("[washer] washing %s (%d s)\n", type, t);
            fflush(stdout);
            sleep((unsigned)t);

            if (ipc_put(mode_global, type) < 0)
                fprintf(stderr,"[washer] ipc_put failed\n");
            else
                printf("[washer] placed %s on table\n", type);
        }
    }

    char end_tag[] = "___EOF___";

    if (ipc_put(mode_global, end_tag) < 0)
        fprintf(stderr,"[washer] failed to send EOF marker\n");
    else
        printf("[washer] all items produced, sent EOF marker\n");
    fflush(stdout);
}

void dryer_proc(time_map_t *dry_map) {
    char buf[MAX_TYPE];
    while(1) {
        if (ipc_get(mode_global, buf) < 0) {
            usleep(100000);
            continue;
        }
        if (strcmp(buf, "___EOF___")==0) {
            printf("[dryer] received EOF, exiting\n");
            break;
        }
        int t = lookup_time(dry_map, buf);
        if (t < 0) {
            fprintf(stderr,"[dryer] unknown type '%s' in dry times, skipping\n", buf);
            continue;
        }

        printf("[dryer] received %s, drying (%d s)\n", buf, t);
        fflush(stdout);
        sleep(t);
        printf("[dryer] done %s\n", buf);
        fflush(stdout);
    }
}


int main(int argc, char **argv) {
    if (argc != 5) {
        fprintf(stderr,"Usage: %s <mode> wash_times.txt dry_times.txt dirty.txt\nModes: pipe fifo msg shm sock file_sem\n", argv[0]);
        exit(1);
    }

    parent_pid = getpid(); 

    const char *mode = argv[1];
    const char *wash_file = argv[2];
    const char *dry_file = argv[3];
    const char *dirty_file = argv[4];

    time_map_t wash_map = read_time_file(wash_file);
    time_map_t dry_map = read_time_file(dry_file);
    dirty_list_t dirty = read_dirty_file(dirty_file);

    int cap = get_table_limit();
    table_limit = cap;

    if (strcmp(mode,"pipe")==0)
        mode_global = MODE_PIPE;
    else if (strcmp(mode,"fifo")==0)
        mode_global = MODE_FIFO;
    else if (strcmp(mode,"msg")==0)
        mode_global = MODE_MSG;
    else if (strcmp(mode,"shm")==0)
        mode_global = MODE_SHM;
    else if (strcmp(mode,"sock")==0)
        mode_global = MODE_SOCK;
    else if (strcmp(mode,"file_sem")==0)
        mode_global = MODE_FILESEM;
    else {
        fprintf(stderr,"Unknown mode %s\n", mode);
        exit(1);
    }

    if (ipc_init(mode_global, cap) < 0) {
        fprintf(stderr,"ipc init failed\n");
        exit(1);
    }

    struct sigaction sa;
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGCHLD, &sa, NULL);

    pid_t pid = fork();

    if (pid < 0) {
        perror("fork");
        ipc_cleanup(mode_global);
        exit(1);
    }

    if (pid == 0) {
        sleep(1);
        dryer_proc(&dry_map);
        ipc_cleanup(mode_global);
        exit(0);
    }
    else {
        washer_proc(&wash_map, &dirty);
        wait(NULL);
        ipc_cleanup(mode_global);
    }
    return 0;
}
