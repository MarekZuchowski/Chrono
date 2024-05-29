#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <pthread.h>
#include <unistd.h>
#include <stdatomic.h>
#include <semaphore.h>
#include <stdarg.h>
#include <time.h>
#include "logger.h"

static volatile sig_atomic_t current_level = 3;
static atomic_int initialized = 0;
static FILE* file;
static sem_t sem;
static pthread_mutex_t mutex;
static pthread_t dump_thread;
static pthread_t signal_thread;
static int dump_sig_num;
static int log_sig_num;
static const char *logs[3] = {"ERROR", "WARN", "INFO"};
struct dump_t {
    void* (*get_dump_data)();
    size_t size;
};
static struct dump_t* dump_data;

void log_sig_handler(int signo, siginfo_t* info, void* other);
void dump_sig_handler();
void* signal_receiver(void* arg);
void* dump(void* arg);


int logger_init(int log_sig_no, char* log_filename, int dump_sig_no,  void* (*get_dump_data_fun)(), size_t dump_size) {
    if(initialized)
        return 1;

    if((file = fopen(log_filename, "w")) == NULL)
        return 2;
    setbuf(file, NULL);

    dump_sig_num = dump_sig_no;
    log_sig_num = log_sig_no;
    dump_data = malloc(sizeof(struct dump_t));
    if(dump_data == NULL) {
        fclose(file);
        return 3;
    }
    dump_data->get_dump_data = get_dump_data_fun;
    dump_data->size = dump_size;

    sigset_t block_set;
    sigemptyset(&block_set);
    sigaddset(&block_set, dump_sig_num);
    sigaddset(&block_set, log_sig_no);
    pthread_sigmask(SIG_BLOCK, &block_set, NULL);

    if(sem_init(&sem, 0, 0)) {
        fclose(file);
        free(dump_data);
        return 4;
    }

    if(pthread_mutex_init(&mutex, NULL)) {
        fclose(file);
        free(dump_data);
        sem_destroy(&sem);
        return 5;
    }

    sigset_t set;
    sigfillset(&set);

    struct sigaction action;
    action.sa_sigaction = log_sig_handler;
    action.sa_mask = set;
    action.sa_flags = SA_SIGINFO;
    if(sigaction(log_sig_no, &action, NULL) != 0) {
        fclose(file);
        sem_destroy(&sem);
        pthread_mutex_destroy(&mutex);
        free(dump_data);
        return 6;
    }

    action.sa_handler = dump_sig_handler;
    action.sa_mask = set;
    action.sa_flags = 0;
    if(sigaction(dump_sig_no, &action, NULL)) {
        signal(log_sig_num, SIG_DFL);
        fclose(file);
        sem_destroy(&sem);
        pthread_mutex_destroy(&mutex);
        free(dump_data);
        return 7;
    }

    if(pthread_create(&dump_thread, NULL, dump, dump_data)) {
        signal(dump_sig_num, SIG_DFL);
        signal(log_sig_num, SIG_DFL);
        fclose(file);
        free(dump_data);
        sem_destroy(&sem);
        pthread_mutex_destroy(&mutex);
        return 8;
    }

    if(pthread_detach(dump_thread)) {
        pthread_cancel(dump_thread);
        signal(dump_sig_num, SIG_DFL);
        signal(log_sig_num, SIG_DFL);
        fclose(file);
        free(dump_data);
        sem_destroy(&sem);
        pthread_mutex_destroy(&mutex);
        return 9;
    }

    if(pthread_create(&signal_thread, NULL, signal_receiver, NULL)) {
        pthread_cancel(dump_thread);
        signal(dump_sig_num, SIG_DFL);
        signal(log_sig_num, SIG_DFL);
        fclose(file);
        free(dump_data);
        sem_destroy(&sem);
        pthread_mutex_destroy(&mutex);
        return 10;
    }

    if(pthread_detach(signal_thread)) {
        pthread_cancel(signal_thread);
        pthread_cancel(dump_thread);
        signal(dump_sig_num, SIG_DFL);
        signal(log_sig_num, SIG_DFL);
        fclose(file);
        free(dump_data);
        sem_destroy(&sem);
        pthread_mutex_destroy(&mutex);
        return 11;
    }

    initialized = 1;
    return 0;
}

void log_sig_handler(int signo, siginfo_t* info, void* other) {
    current_level = info->si_value.sival_int;
}

void dump_sig_handler() {
    sem_post(&sem);
}

void* signal_receiver(void* arg) {
    sigset_t set;
    sigfillset(&set);
    sigdelset(&set, dump_sig_num);
    sigdelset(&set, log_sig_num);
    pthread_sigmask(SIG_SETMASK, &set, NULL);

    while(1) {
        sleep(1000);
    }
}

void* dump(void* arg) {
    sigset_t set;
    sigfillset(&set);
    pthread_sigmask(SIG_SETMASK, &set, NULL);

    while(1) {
        sem_wait(&sem);
        char filename[50];
        char dump_time[30];
        time_t t = time(NULL);
        struct tm* tm = localtime(&t);
        strftime(dump_time, 30, "%Y-%m-%d %H-%M-%S", tm);
        sprintf(filename, "dump %s.txt", dump_time);
        FILE* dump_file = fopen(filename, "w");
        if(dump_file == NULL) {
            return NULL;
        }
        fwrite(dump_data->get_dump_data(), dump_data->size, sizeof(char), dump_file);
        fclose(dump_file);
    }
}

int logger_log(int level, const char* format, ...) {
    if(!initialized)
        return -1;

    if(level < 0 || level > 3)
        return 2;

    if(!current_level)
        return -3;

    char log_time[26];
    time_t t = time(NULL);
    struct tm* tm = localtime(&t);
    strftime(log_time, 26, "%Y-%m-%d %H:%M:%S", tm);

    int result = 0;
    if(current_level >= level) {
        pthread_mutex_lock(&mutex);
        fprintf(file, "(%s) (%s) ", logs[level - 1], log_time);
        va_list args;
        va_start(args, format);
        result = vfprintf(file, format, args);
        va_end(args);
        fprintf(file, "\n");
        pthread_mutex_unlock(&mutex);
    }

    return result;
}

void logger_destroy() {
    if(!initialized)
        return;

    fclose(file);
    pthread_cancel(signal_thread);
    pthread_cancel(dump_thread);
    free(dump_data);
    signal(dump_sig_num, SIG_DFL);
    signal(log_sig_num, SIG_DFL);
    sem_destroy(&sem);
    pthread_mutex_destroy(&mutex);
    initialized = 0;
}