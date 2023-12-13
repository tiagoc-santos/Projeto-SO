#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "constants.h"
#include "operations.h"
#include "parser.h"

//mutex to make sure each thread only reads one line at a time
pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_rwlock_t barrier_mut = PTHREAD_RWLOCK_INITIALIZER;
pthread_rwlock_t wait_mut = PTHREAD_RWLOCK_INITIALIZER;

unsigned int delay_thread = 0, thread_id = 0;
//global variable to inform other threads that a barrier has been encountered
int barrier = 0;

struct Args{
  int out_fd, fd, index;
};

int thread_fn(void *arg) {
  unsigned int event_id;
  size_t num_rows, num_columns, num_coords;
  size_t xs[MAX_RESERVATION_SIZE], ys[MAX_RESERVATION_SIZE];
  int end_file = 0;
  int inside_wait = 0;

  struct Args *args = (struct Args *)arg;
  int fd = args->fd;
  int out_fd = args->out_fd;
  int index = args->index;

  while (!end_file) {
    pthread_rwlock_rdlock(&wait_mut);
    if (index == (int)thread_id && delay_thread > 0) {
      pthread_rwlock_unlock(&wait_mut);
      printf("Waiting...\n");
      pthread_rwlock_wrlock(&wait_mut);
      thread_id = 0;
      pthread_rwlock_unlock(&wait_mut);
      inside_wait = 1;
      ems_wait(delay_thread);
    }
    if(!inside_wait){
      pthread_rwlock_unlock(&wait_mut);
    }
    pthread_rwlock_rdlock(&barrier_mut);
    if (barrier) {
      pthread_rwlock_unlock(&barrier_mut);
      return 1;
    }
    pthread_rwlock_unlock(&barrier_mut);
    pthread_mutex_lock(&file_mutex);  
    switch (get_next(fd)) {
    case CMD_CREATE:
      if (parse_create(fd, &event_id, &num_rows, &num_columns) != 0) {
        pthread_mutex_unlock(&file_mutex);
        fprintf(stderr, "Invalid command. See HELP for usage\n");
        continue;
      }
      pthread_mutex_unlock(&file_mutex);
      if (ems_create(event_id, num_rows, num_columns)) {
        fprintf(stderr, "Failed to create event\n");
      }

      break;

    case CMD_RESERVE:
      num_coords = parse_reserve(fd, MAX_RESERVATION_SIZE, &event_id, xs, ys);
      pthread_mutex_unlock(&file_mutex);
      if (num_coords == 0) {
        fprintf(stderr, "Invalid command. See HELP for usage\n");
        continue;
      }

      if (ems_reserve(event_id, num_coords, xs, ys)) {
        fprintf(stderr, "Failed to reserve seats\n");
      }
      break;

    case CMD_SHOW:
      if (parse_show(fd, &event_id) != 0) {
        pthread_mutex_unlock(&file_mutex);
        fprintf(stderr, "Invalid command. See HELP for usage\n");
        continue;
      }
      pthread_mutex_unlock(&file_mutex);
      if (ems_show(event_id, out_fd)) {
        fprintf(stderr, "Failed to show event\n");
      }

      break;

    case CMD_LIST_EVENTS:
      pthread_mutex_unlock(&file_mutex);
      if (ems_list_events(out_fd)) {
        fprintf(stderr, "Failed to list events\n");
      }
      break;

    case CMD_WAIT:
      pthread_rwlock_wrlock(&wait_mut);
      if (parse_wait(fd, &delay_thread, &thread_id) == -1) {
        pthread_rwlock_unlock(&wait_mut);
        pthread_mutex_unlock(&file_mutex);
        fprintf(stderr, "Invalid command. See HELP for usage\n");
        continue;
      }
      pthread_rwlock_unlock(&wait_mut);
      if (delay_thread > 0 && thread_id == 0) {
        printf("Waiting...\n");
        ems_wait(delay_thread);
        pthread_rwlock_wrlock(&wait_mut);
        delay_thread = 0;
        pthread_rwlock_unlock(&wait_mut);
        pthread_mutex_unlock(&file_mutex);
        break;
      }
      pthread_mutex_unlock(&file_mutex);
      break;

    case CMD_INVALID:
      pthread_mutex_unlock(&file_mutex);
      fprintf(stderr, "Invalid command. See HELP for usage\n");
      break;

    case CMD_HELP:
      pthread_mutex_unlock(&file_mutex);
      printf("Available commands:\n"
             "  CREATE <event_id> <num_rows> <num_columns>\n"
             "  RESERVE <event_id> [(<x1>,<y1>) (<x2>,<y2>) ...]\n"
             "  SHOW <event_id>\n"
             "  LIST\n"
             "  WAIT <delay_ms> [thread_id]\n"
             "  BARRIER\n"
             "  HELP\n");

      break;

    case CMD_BARRIER:
      pthread_rwlock_wrlock(&barrier_mut);
      barrier = 1;
      pthread_rwlock_unlock(&barrier_mut);
      pthread_mutex_unlock(&file_mutex);
      return 1;
    case CMD_EMPTY:
      pthread_mutex_unlock(&file_mutex);
      break;

    case EOC:
      end_file = 1;
      pthread_mutex_unlock(&file_mutex);
      break;
    }
  }
  return 0;
}

int main(int argc, char *argv[]) {
  unsigned int state_access_delay_ms = STATE_ACCESS_DELAY_MS;
  
  if (argc > 4) {
    char *endptr;
    unsigned long int delay = strtoul(argv[4], &endptr, 10);

    if (*endptr != '\0' || delay > UINT_MAX) {
      fprintf(stderr, "Invalid delay value or value too large\n");
      return 1;
    }
    state_access_delay_ms = (unsigned int)delay;
  }

  char *endptr;
  unsigned long int MAX_PROC = strtoul(argv[2], &endptr, 10);
  unsigned long int MAX_THREADS = strtoul(argv[3], &endptr, 10);

  if (ems_init(state_access_delay_ms)) {
    fprintf(stderr, "Failed to initialize EMS\n");
    return 1;
  }

  DIR *dirpath = opendir(argv[1]);

  if (dirpath == NULL) {
    fprintf(stderr, "The directory '%s' does not exist or cannot be accessed.",
            argv[1]);
    return 1;
  }

  struct dirent *file;
  unsigned long int child_count = 0;
  unsigned long int total_child = 0;
  int status;

  while ((file = readdir(dirpath)) != NULL) {
    char file_path[BUFFER_LEN];
    pid_t pid;

    if (strcmp(file->d_name, ".") == 0 || strcmp(file->d_name, "..") == 0 || strstr(file->d_name, ".out"))
      continue;

    if (child_count >= MAX_PROC) {
      wait(NULL);
      child_count--;
    }

    pid = fork();
    if (pid == -1) {
      fprintf(stderr, "Error creating process");
      exit(EXIT_FAILURE);
    }
    child_count++;
    total_child++;
    if (pid == 0) {
      snprintf(file_path, sizeof(file_path), "%s/%s", argv[1], file->d_name);

      int fd = open(file_path, O_RDONLY);
      memset(file_path, 0, sizeof(file_path));
      snprintf(file_path, sizeof(file_path), "%s/%s", argv[1],
               strcat(strtok(file->d_name, "."), ".out"));
      int out_fd =
          open(file_path, O_CREAT | O_TRUNC | O_WRONLY, S_IRUSR | S_IWUSR);

      if (fd < 0 || out_fd < 0) {
        fprintf(stderr, "Open error: %s\n", strerror(errno));
        return 1;
      }
      pthread_t thread_ids[MAX_THREADS + 1];
      for (int i = 1; i <= (int)MAX_THREADS; i++) {
        int *index = malloc(sizeof(int));
        struct Args * args = malloc(sizeof(struct Args));
        *index = i;
        args->fd = fd;
        args->out_fd =out_fd;
        args->index = *index;
        if (pthread_create(&thread_ids[i], NULL, (void *)thread_fn,
                           (void *)args) != 0) {
          fprintf(stderr, "Error creating thread.");
          exit(EXIT_FAILURE);
        }
        free(index);
      }
      int relaunch = 1;
      while(1){
        for (int i = 1; i <= (int)MAX_THREADS; i++){
          int j;
          pthread_join(thread_ids[i], (void*) &j);
          if(j == 0){ 
            relaunch = 0;
          }
        }
        if(!relaunch){
          break;
        }
        else{
          barrier = 0;
          memset(thread_ids, 0, sizeof(thread_ids));
          for (int i = 1; i <= (int)MAX_THREADS; i++) {
            int *index = malloc(sizeof(int));
            struct Args * args = malloc(sizeof(struct Args));
            *index = i;
            args->fd = fd;
            args->out_fd =out_fd;
            args->index = *index;
            if (pthread_create(&thread_ids[i], NULL, (void *)thread_fn,
                              (void *)args) != 0) {
              fprintf(stderr, "Error creating thread.");
              exit(EXIT_FAILURE);
            }
            free(index);
          }
        }
      }
      if (close(fd) < 0 || close(out_fd) < 0) {
        fprintf(stderr, "Close error: %s\n", strerror(errno));
        return -1;
      }
      ems_terminate();
      if (closedir(dirpath) < 0) {
        fprintf(stderr, "Error closing the directory: %s\n", strerror(errno));
      }
      exit(EXIT_SUCCESS);
    }
  }
  while (total_child > 0) {
    int pid = wait(&status);
    if (WIFEXITED(status)) {
      fprintf(stdout, "Child process %d exited with status: %d\n", pid, status);
    } else {
      fprintf(stdout, "Child process %d exited abnormally", pid);
    }
    total_child--;
  }
  ems_terminate();
  if (closedir(dirpath) < 0) {
    fprintf(stderr, "Error closing the directory: %s\n", strerror(errno));
  }
  return 0;
}