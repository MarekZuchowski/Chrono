#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <mqueue.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <spawn.h>
#include <time.h>
#include <signal.h>
#include <pthread.h>
#include "logger.h"

#define __FILENAME__ (strrchr(__FILE__, '/') ? strrchr(__FILE__, '/') + 1 : __FILE__)

static char *data;
void* get_dump_data();

enum command_t {ADD, CANCEL, DISPLAY, STOP};
const char *commands[] = {"add", "cancel", "display", "stop"};
static pthread_mutex_t mutex;

struct query_t {
    enum command_t command;
    char timer_spec[256];
    char task[256];
};

struct response_t {
    long task_id;
    char time_spec[256];
    char task[256];
};

struct task_t {
    long task_id;
    timer_t timer_id;
    char time_spec[256];
    char **argv;
    int is_cyclic;
    int is_done;
};

struct node_t {
    struct task_t *timer_task;
    struct node_t *next;
};

struct linked_list_t {
    struct node_t *head;
    struct node_t *tail;
};

struct linked_list_t* ll_create();
int ll_push_back(struct linked_list_t* ll, struct task_t **new_task);
int ll_size(const struct linked_list_t* ll);
void ll_remove(struct linked_list_t* ll, long index);
void ll_clear(struct linked_list_t* ll);

void timer_notification_thread(union sigval arg);
void send_task_list(const struct linked_list_t *ll);
void get_argv_for_task(struct task_t *timer_task, struct query_t query);
int get_task_time(struct query_t *query, long *task_execution_time, long *interval_time);

void run_client(const mqd_t *mq_queries_to_server, int argc, char **argv);
void fill_add_query(int argc, char** argv, struct query_t *query);
void display_task_list();

int main(int argc, char **argv) {
    mqd_t mq_queries_to_server = mq_open("/mq_queries_queue", O_WRONLY);

    if(mq_queries_to_server == -1) {
        if(fork() != 0) {
            struct mq_attr attr;
            attr.mq_maxmsg = 10;
            attr.mq_msgsize = sizeof(struct query_t);
            attr.mq_flags = 0;

            int dump_sig_no = 36;
            int log_sig_no = 37;
            char* log_filename = "logger.log";
            size_t size = 50;
            data = calloc(size, sizeof(char));
            memset(data, '1', sizeof(char) * size);

            logger_init(log_sig_no, log_filename, dump_sig_no, &get_dump_data, size);
            logger_log(3, "(%s:%d) %s", __FILENAME__, __LINE__, "Server has started.");

            mqd_t mq_queries_from_clients = mq_open("/mq_queries_queue", O_CREAT | O_RDONLY, 0444, &attr);
            printf("Server has started with PID:%d.\n", getpid());
            printf("Waiting for tasks...\n");

            struct linked_list_t *ll;
            ll = ll_create();

            pthread_mutex_init(&mutex, NULL);
            long sequence = 1;
            struct query_t query;

            int is_stopped = 0;

            while (!is_stopped) {
                mq_receive(mq_queries_from_clients, (char *) &query, sizeof(struct query_t), NULL);
                switch (query.command) {
                    case ADD:
                        printf("TASK: add %s %s\n", query.timer_spec, query.task);
                        logger_log(2, "(%s:%d) TASK: add %s %s", __FILENAME__, __LINE__, query.timer_spec, query.task);
                        struct task_t *new_task = (struct task_t*) malloc(sizeof(struct task_t));

                        new_task->task_id = sequence++;
                        get_argv_for_task(new_task, query);

                        strcpy(new_task->time_spec, query.timer_spec);
                        new_task->is_cyclic = 0;
                        new_task->is_done = 0;
                        long task_execution_time;
                        long interval_time;
                        int is_absolute = get_task_time(&query, &task_execution_time, &interval_time);
                        new_task->is_cyclic = interval_time > 0 ? 1 : 0;

                        timer_t timer_id;
                        struct sigevent event;
                        event.sigev_notify = SIGEV_THREAD;
                        event.sigev_notify_function = timer_notification_thread;
                        event.sigev_value.sival_ptr = new_task;
                        event.sigev_notify_attributes = NULL;
                        timer_create(CLOCK_REALTIME, &event, &timer_id);
                        new_task->timer_id = timer_id;
                        ll_push_back(ll, &new_task);

                        struct itimerspec spec;
                        spec.it_value.tv_sec = task_execution_time;
                        spec.it_value.tv_nsec = 0;
                        spec.it_interval.tv_sec = interval_time;
                        spec.it_interval.tv_nsec = 0;
                        timer_settime(timer_id, is_absolute ? TIMER_ABSTIME : 0, &spec, NULL);
                        break;
                    case CANCEL:;
                        long id = strtol(query.task, NULL, 10);
                        printf("TASK: cancel %ld\n", id);
                        logger_log(1, "(%s:%d) TASK: cancel %ld", __FILENAME__, __LINE__, id);
                        ll_remove(ll, id);
                        break;
                    case DISPLAY:
                        printf("TASK: display\n");
                        logger_log(3, "(%s:%d) TASK: display", __FILENAME__, __LINE__);
                        send_task_list(ll);
                        break;
                    case STOP:
                        printf("TASK: stop\n");
                        logger_log(1, "(%s:%d) TASK: stop", __FILENAME__, __LINE__);
                        is_stopped = 1;
                        break;
                }

            }

            printf("Server has terminated.\n");
            ll_clear(ll);
            free(ll);
            mq_close(mq_queries_from_clients);
            mq_unlink("/mq_queries_queue");
            pthread_mutex_destroy(&mutex);
            logger_log(3, "(%s:%d) Server has terminated.", __FILENAME__, __LINE__);
            logger_destroy();
        }
        else {
            if(argc > 1) {
                do {
                    mq_queries_to_server = mq_open("/mq_queries_queue", O_WRONLY);
                    sleep(1);
                } while (mq_queries_to_server == -1);
                run_client(&mq_queries_to_server, argc, argv);
            }
        }
    }
    else {
        run_client(&mq_queries_to_server, argc, argv);
    }


    return 0;
}

void timer_notification_thread(union sigval arg) {
    pthread_mutex_lock(&mutex);
    struct task_t *timer_task = (struct task_t*) arg.sival_ptr;
    if(!timer_task->is_cyclic)
        timer_task->is_done = 1;

    pid_t child_pid;
    posix_spawn(&child_pid, *timer_task->argv, NULL, NULL, timer_task->argv, NULL);
    pthread_mutex_unlock(&mutex);
}

void send_task_list(const struct linked_list_t *ll) {
    mqd_t mq_response_to_client;
    do {
        mq_response_to_client = mq_open("/mq_response_queue", O_WRONLY);
        sleep(1);
    } while (mq_response_to_client == -1);

    struct response_t response;
    pthread_mutex_lock(&mutex);
    for(struct node_t* current = ll->head; current != NULL; current = current->next) {
        if(!current->timer_task->is_done) {
            response.task_id = current->timer_task->task_id;
            strcpy(response.time_spec, current->timer_task->time_spec);
            strcpy(response.task, "");
            for (int i = 0; *(current->timer_task->argv + i) != NULL; i++) {
                strcat(response.task, *(current->timer_task->argv + i));
                strcat(response.task, " ");
            }
            mq_send(mq_response_to_client, (char *) &response, sizeof(struct response_t), 0);
        }
    }
    pthread_mutex_unlock(&mutex);
    strcpy(response.task, "");
    mq_send(mq_response_to_client, (char*) &response, sizeof(struct response_t), 0);
    mq_close(mq_response_to_client);
}

void get_argv_for_task(struct task_t *timer_task, struct query_t query) {
    int argc = 1;
    for(int i = 0; i < strlen(query.task); i++) {
        if (*(query.task + i) == ' ')
            argc++;
    }

    timer_task->argv = realloc(NULL, sizeof(char *) * argc);
    char *temp;
    temp = strtok(query.task, " ");
    *timer_task->argv = calloc(sizeof(char), strlen(temp) + 1);
    strcpy(*timer_task->argv, temp);
    int counter = 1;
    temp = strtok(NULL, " ");
    while (temp) {
        *(timer_task->argv + counter) = calloc(sizeof(char), strlen(temp) + 1);
        strcpy(*(timer_task->argv + counter), temp);
        temp = strtok(NULL, " ");
        counter++;

    }
    *(timer_task->argv + counter) = NULL;
}

int get_task_time(struct query_t *query, long *task_execution_time, long *interval_time) {
    char* timer_spec = query->timer_spec;
    *task_execution_time = 0;
    *interval_time = 0;
    int is_absolute = 0;
    char* temp = strtok(timer_spec, " ");
    if(strcmp(temp, "-r") == 0) {
        temp = strtok(NULL, "-");
        *task_execution_time += strtol(temp, NULL, 10) * 365 * 24 * 60 * 60;
        temp = strtok(NULL, "-");
        *task_execution_time += strtol(temp, NULL, 10) * 24 * 60 * 60;
        temp = strtok(NULL, "-");
        *task_execution_time += strtol(temp, NULL, 10) * 60 * 60;
        temp = strtok(NULL, "-");
        *task_execution_time += strtol(temp, NULL, 10) * 60;
        temp = strtok(NULL, " ");
        *task_execution_time += strtol(temp, NULL, 10);
    }
    else {
        if(strcmp(temp, "-a") == 0) {
            is_absolute = 1;
            time_t now = time(NULL);
            struct tm *lt = localtime(&now);
            struct tm at;
            temp = strtok(NULL, ".");
            at.tm_mday = (int) strtol(temp, NULL, 10);
            temp = strtok(NULL, ".");
            at.tm_mon = (int) strtol(temp, NULL, 10) - 1;
            temp = strtok(NULL, "-");
            at.tm_year = (int) strtol(temp, NULL, 10) - 1900;
            temp = strtok(NULL, ":");
            at.tm_hour = (int) strtol(temp, NULL, 10);
            temp = strtok(NULL, ":");
            at.tm_min = (int) strtol(temp, NULL, 10);
            temp = strtok(NULL, " ");
            at.tm_sec = (int) strtol(temp, NULL, 10);
            at.tm_zone = lt->tm_zone;
            *task_execution_time = mktime(&at);
        }
    }

    temp = strtok(NULL, " ");
    if(temp != NULL) {
        if (strcmp(temp, "-i") == 0) {
            temp = strtok(NULL, "-");
            *interval_time += strtol(temp, NULL, 10) * 365 * 24 * 60 * 60;
            temp = strtok(NULL, "-");
            *interval_time += strtol(temp, NULL, 10) * 24 * 60 * 60;
            temp = strtok(NULL, "-");
            *interval_time += strtol(temp, NULL, 10) * 60 * 60;
            temp = strtok(NULL, "-");
            *interval_time += strtol(temp, NULL, 10) * 60;
            temp = strtok(NULL, " ");
            *interval_time += strtol(temp, NULL, 10);
        }
    }

    return is_absolute;
}

void run_client(const mqd_t *mq_queries_to_server, int argc, char **argv) {
    printf("CLIENT\n");

    if(argc > 1) {

        struct query_t query;

        if(strcmp(argv[1], commands[0]) == 0) {
            fill_add_query(argc, argv, &query);
            mq_send(*mq_queries_to_server, (char *) &query, sizeof(struct query_t), 0);

            printf("SENT: %s %s %s\n", commands[query.command], query.timer_spec, query.task);
        }
        else if(strcmp(argv[1], commands[1]) == 0) {
            query.command = CANCEL;
            strcpy(query.task, argv[2]);
            mq_send(*mq_queries_to_server, (char *) &query, sizeof(struct query_t), 0);

            printf("SENT: %s %s\n", commands[query.command], argv[2]);
        }
        else if(strcmp(argv[1], commands[2]) == 0) {
            query.command = DISPLAY;
            mq_send(*mq_queries_to_server, (char *) &query, sizeof(struct query_t), 0);
            printf("SENT: %s\n", commands[query.command]);

            display_task_list();
        }
        else if(strcmp(argv[1], commands[3]) == 0) {
            query.command = STOP;
            mq_send(*mq_queries_to_server, (char *) &query, sizeof(struct query_t), 0);

            printf("SENT: %s\n", commands[query.command]);
        }
        else {
            printf("Incorrect command!\n");
        }

        mq_close(*mq_queries_to_server);
    }
}

void fill_add_query(int argc, char** argv, struct query_t *query) {
    query->command = ADD;
    int index = 4;
    if(strcmp(argv[4], "-i") == 0) {
        sprintf(query->timer_spec, "%s %s %s %s", argv[2], argv[3], argv[4], argv[5]);
        index = 6;
    }
    else {
        sprintf(query->timer_spec, "%s %s", argv[2], argv[3]);
    }

    sprintf(query->task, "%s ", argv[index]);
    for(int i = index + 1; i < argc; i++) {
        strcat(query->task, argv[i]);
        strcat(query->task, " ");
    }
}

void display_task_list() {
    struct mq_attr attr;

    attr.mq_maxmsg = 10;
    attr.mq_msgsize = sizeof(struct response_t);
    attr.mq_flags = 0;

    int counter = 0;
    struct response_t response;
    mqd_t mq_response_from_server = mq_open("/mq_response_queue", O_CREAT | O_RDONLY, 0444, &attr);
    while(1) {
        mq_receive(mq_response_from_server, (char*) &response, sizeof(struct response_t), 0);
        if(strcmp(response.task, "") == 0)
            break;
        printf("ID: %ld %s %s\n", (long)response.task_id, response.time_spec, response.task);
        counter++;
    }
    if(counter < 1)
        printf("Task list is empty.\n");

    mq_close(mq_response_from_server);
    mq_unlink("/mq_response_queue");
}

void* get_dump_data() {
    return data;
}

struct linked_list_t* ll_create() {
    return calloc(1, sizeof(struct linked_list_t));
}

int ll_push_back(struct linked_list_t* ll, struct task_t **new_task) {
    pthread_mutex_lock(&mutex);
    if(ll == NULL || (ll->head == NULL && ll->tail != NULL) || (ll->head != NULL && ll->tail == NULL)) {
        pthread_mutex_unlock(&mutex);
        return 1;
    }

    struct node_t* node = calloc(1, sizeof(struct node_t));
    if(node == NULL) {
        pthread_mutex_unlock(&mutex);
        return 2;
    }
    node->timer_task = *new_task;

    if(ll->head == NULL) {
        ll->head = ll->tail = node;
    }
    else {
        ll->tail->next = node;
        ll->tail = node;
    }

    pthread_mutex_unlock(&mutex);
    return 0;
}

int ll_size(const struct linked_list_t* ll) {
    if(ll == NULL || (ll->head == NULL && ll->tail != NULL) || (ll->head != NULL && ll->tail == NULL))
        return -1;

    int counter = 0;
    for(struct node_t* current = ll->head; current != NULL; current = current->next)
        counter++;

    return counter;
}

void free_node(struct node_t * node) {
    for(int i = 0; *(node->timer_task->argv + i) != NULL; i++)
        free(*(node->timer_task->argv + i));
    free(node->timer_task->argv);
    free(node->timer_task);
    free(node);
}

void ll_remove(struct linked_list_t* ll, long index) {
    pthread_mutex_lock(&mutex);

    int size = ll_size(ll);
    if(size < 1) {
        pthread_mutex_unlock(&mutex);
        return;
    }

    if(ll->head == ll->tail) {
        if(ll->head->timer_task->task_id == index) {
            timer_delete(ll->head->timer_task->timer_id);
            free_node(ll->head);
            ll->head = ll->tail = NULL;
        }
    }
    else {
        if(ll->head->timer_task->task_id == index) {
            struct node_t* temp = ll->head->next;
            timer_delete(ll->head->timer_task->timer_id);
            free_node(ll->head);
            ll->head = temp;
        }
        else {
            struct node_t *current;
            for (current = ll->head; current->next != NULL; current = current->next) {
                if (current->next->timer_task->task_id == index) {
                    struct node_t *temp = current->next->next;
                    timer_delete(current->next->timer_task->timer_id);
                    free_node(current->next);
                    current->next = temp;
                    if (temp == NULL) {
                        ll->tail = current;
                        break;
                    }
                }
            }
        }
    }

    pthread_mutex_unlock(&mutex);
}

void ll_clear(struct linked_list_t* ll) {
    pthread_mutex_lock(&mutex);
    if(ll == NULL || ll->head == NULL || ll->tail == NULL) {
        pthread_mutex_unlock(&mutex);
        return;
    }

    for(struct node_t* current = ll->head; current!= NULL;) {
        struct node_t* temp = current->next;
        timer_delete(current->timer_task->timer_id);
        free_node(current);
        current = temp;
    }

    ll->head = ll->tail = NULL;
    pthread_mutex_unlock(&mutex);
}