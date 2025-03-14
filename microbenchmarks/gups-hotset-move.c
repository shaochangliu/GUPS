#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <math.h>
#include <string.h>
#include <pthread.h>
#include <sys/mman.h>
#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/syscall.h>

#include "../src/timer.h"

#include "gups.h"

#define PROC_PATH "/proc/swap_log_ctl"

#define MAX_THREADS     64

int threads;

bool move_hotset1 = false;

uint64_t hot_start = 0;
uint64_t hotsize = 0;

struct gups_args {
  int tid;                 // thread id (0 to threads-1)
  uint64_t *indices;       // array of indices to access
  void* field;             // pointer to start of thread's region
  uint64_t iters;          // iterations to perform
  uint64_t size;           // number of elements
  uint64_t elt_size;       // size of each element
  uint64_t hot_start;      // start index of hot set, set to 0 for all threads
  uint64_t hotsize;        // number of hot set elements
  bool exec_stage;         // execution stage or not
};


static inline uint64_t rdtscp(void)   //read the time stamp counter
{
    uint32_t eax, edx;
    // why is "ecx" in clobber list here, anyway? -SG&MH,2017-10-05
    __asm volatile ("rdtscp" : "=a" (eax), "=d" (edx) :: "ecx", "memory");
    return ((uint64_t)edx << 32) | eax;
}

uint64_t thread_gups[MAX_THREADS];

static unsigned long updates, nelems;

bool stop = false;

static void *timing_thread()    //timer, unit: second
{
  uint64_t tic = -1;
  bool printed1 = false;
  for (;;) {
    tic++;
    if (tic >= 150 && tic < 300) {
      if (!printed1) {
        move_hotset1 = true;
        fprintf(stderr, "moved hotset1\n");
        printed1 = true;
      }
    }
    if (tic >= 250) {
      stop = true;
    }
    sleep(1);
  }
  return 0;
}

uint64_t tot_updates = 0;

static void *print_instantaneous_gups()   //print GUPS per second
{
  FILE *tot;
  uint64_t tot_gups;

  tot = fopen("tot_gups.txt", "w");
  if (tot == NULL) {
    perror("fopen");
  }

  for (;;) {
    tot_gups = 0;
    for (int i = 0; i < threads; i++) {
      tot_gups += thread_gups[i];
    }
    fprintf(tot, "GUPS in last second: %.10f\n", (1.0 * (abs(tot_gups - tot_updates))) / (1.0e9));
    tot_updates = tot_gups;
    sleep(1);   //may not be accurate if the execution time isn't much bigger than 1s
  }

  return NULL;
}


static uint64_t lfsr_fast(uint64_t lfsr)  //emulate the random number generator?
{
  lfsr ^= lfsr >> 7;
  lfsr ^= lfsr << 9;
  lfsr ^= lfsr >> 13;
  return lfsr;
}

FILE *hotsetfile = NULL;

static void *do_gups(void *arguments)
{
  //printf("do_gups entered\n");
  struct gups_args *args = (struct gups_args*)arguments;
  uint64_t *field = (uint64_t*)(args->field);   //pointer to the start of the thread's region
  uint64_t i;
  uint64_t index1, index2;
  uint64_t elt_size = args->elt_size;   //size of elements
  char data[elt_size];
  uint64_t lfsr;
  uint64_t hot_num;
  uint64_t tmp;
  uint64_t start, end;

  srand(args->tid);   //use tid as the seed of random number generator
  lfsr = rand();

  index1 = 0;
  index2 = 0;

  fprintf(hotsetfile, "Thread %d region: %p - %p\thot set: %p - %p\n", args->tid, field, field + args->size, field + args->hot_start, field + args->hot_start + args->hotsize);   

  for (i = 0; i < args->iters; i++) {   //every iteration is an memory access
    hot_num = lfsr_fast(lfsr) % 100;

    if (hot_num < 90) {   //90% of the time, access hot set
      lfsr = lfsr_fast(lfsr);
      index1 = args->hot_start + (lfsr % args->hotsize);  //hot element index

      //start = rdtscp();   //count the clock cycles this access takes
      if (elt_size == 8) {    //each access include one read, one write
        uint64_t  tmp = field[index1];
        tmp = tmp + i;
        field[index1] = tmp;
      } else {    //if data size is not 8 bytes, use memcpy to access memory
        memcpy(data, &field[index1 * elt_size], elt_size);
        memset(data, data[0] + i, elt_size);
        memcpy(&field[index1 * elt_size], data, elt_size);
      }
      //end = rdtscp();
    } else {      //10% of the time, don't care whether the element is hot or cold
      lfsr = lfsr_fast(lfsr);
      index2 = lfsr % (args->size);

      //start = rdtscp();
      if (elt_size == 8) {
        uint64_t tmp = field[index2];
        tmp = tmp + i;
        field[index2] = tmp;
      } else {
        memcpy(data, &field[index2 * elt_size], elt_size);
        memset(data, data[0] + i, elt_size);
        memcpy(&field[index2 * elt_size], data, elt_size);
      }
      //end = rdtscp();
    }

    if (i % 10000 == 0) {
      thread_gups[args->tid] += 10000;  //count the number of iterations (GUPS ops)
    }
  }

  if (args->exec_stage) { // statistics from warm-up phase are automatically cleared by pthread_join
    pid_t pid = getpid();
    pid_t tid = syscall(SYS_gettid);
    char command[256];
    snprintf(command, sizeof(command), "cat /proc/%d/task/%d/page_reclaim_breakdown >> gups_tid_%d.log", pid, tid, args->tid);
    system(command);
  }

  return 0;
}

int main(int argc, char **argv)
{
  unsigned long expt;
  unsigned long size, elt_size;
  unsigned long tot_hot_size;
  int log_hot_size;
  struct timeval starttime, stoptime;
  double secs, gups;
  int i;
  void *p;
  struct gups_args** ga;
  pthread_t t[MAX_THREADS];

  if (argc != 6) {
    fprintf(stderr, "Usage: %s [threads] [updates per thread] [exponent of region] [data size (bytes)] [exponent of hot set]\n", argv[0]);
    fprintf(stderr, "  threads\t\t\tnumber of threads to launch\n");
    fprintf(stderr, "  updates per thread\t\tnumber of updates per thread\n");
    fprintf(stderr, "  exponent\t\t\tlog size of region\n");
    fprintf(stderr, "  data size\t\t\tsize of data in array (in bytes)\n");
    fprintf(stderr, "  hot size\t\t\tlog size of hot set\n");
    return 0;
  }

  gettimeofday(&starttime, NULL);   //start initialization

  threads = atoi(argv[1]);
  assert(threads <= MAX_THREADS);
  ga = (struct gups_args**)malloc(threads * sizeof(struct gups_args*));

  updates = atol(argv[2]);
  updates -= updates % 256;    //updates per thread, multiple of 256
  assert(updates > 0 && (updates % 256 == 0));

  expt = atoi(argv[3]);
  assert(expt > 8);
  size = (unsigned long)(1) << expt;
  size -= (size % 256);       //size of region, multiple of 256
  assert(size > 0 && (size % 256 == 0));

  elt_size = atoi(argv[4]);   //size of each element
  log_hot_size = atof(argv[5]);
  tot_hot_size = (unsigned long)(1) << log_hot_size;    //size of hot set

  fprintf(stderr, "%lu updates per thread (%d threads)\n", updates, threads);
  fprintf(stderr, "field of 2^%lu (%lu) bytes\n", expt, size);
  fprintf(stderr, "%ld byte element size (%ld elements total)\n", elt_size, size / elt_size);

  p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
  if (p == MAP_FAILED) {
    perror("mmap");
    assert(0);
  }
  fprintf(stderr, "Region address: %p - %p\t size: %ld\n", p, (p + size), size);
  
  nelems = (size / threads) / elt_size; // number of elements per thread
  fprintf(stderr, "Elements per thread: %lu\n", nelems);

  memset(thread_gups, 0, sizeof(thread_gups));

  hotsetfile = fopen("hotsets.txt", "a");
  if (hotsetfile == NULL) {
    perror("fopen");
    assert(0);
  }
  
  hot_start = 0;
  hotsize = (tot_hot_size / threads) / elt_size;    //size of hot set per thread

  for (i = 0; i < threads; i++) {   //specify parameters for each thread
    ga[i] = (struct gups_args*)malloc(sizeof(struct gups_args));
    ga[i]->tid = i;
    ga[i]->field = p + (i * nelems * elt_size);
    ga[i]->iters = updates;
    ga[i]->size = nelems;
    ga[i]->elt_size = elt_size;
    ga[i]->hot_start = 0;        // hot set at start of thread's region
    ga[i]->hotsize = hotsize;
    ga[i]->exec_stage = false;
  }

  gettimeofday(&stoptime, NULL);    //end initialization
  secs = elapsed(&starttime, &stoptime);
  fprintf(stderr, "Initialization time: %.4f seconds.\n", secs);

  gettimeofday(&starttime, NULL);
  
  // run through gups once to touch all memory
  // lsc: actually, not all the memory is touched, as GUPS itself performs random accesses
  // spawn gups worker threads
  for (i = 0; i < threads; i++) {
    int r = pthread_create(&t[i], NULL, do_gups, (void*)ga[i]);
    assert(r == 0);
  }

  // wait for worker threads
  for (i = 0; i < threads; i++) {
    int r = pthread_join(t[i], NULL);
    assert(r == 0);
  }

  gettimeofday(&stoptime, NULL);

  secs = elapsed(&starttime, &stoptime);
  printf("Memory warm-up time: %.4f seconds.\n", secs);
  gups = threads * ((double)updates) / (secs * 1.0e9);
  printf("GUPS = %.10f\n", gups);
  fprintf(stderr, "swap space usage after warm-up: ");
  system("cat /sys/fs/cgroup/swap_log/memory.swap.current");

  memset(thread_gups, 0, sizeof(thread_gups));

  /*
  pthread_t print_thread;   //print GUPS per second
  int pt = pthread_create(&print_thread, NULL, print_instantaneous_gups, NULL);
  assert(pt == 0);
  */

  for (i = 0; i < threads; i++) {
    ga[i]->exec_stage = true;
  }

  // start writing swap log
  system("echo 2 > /proc/swap_log_ctl");

  fprintf(stderr, "Start timing.\n");
  gettimeofday(&starttime, NULL);

  // spawn gups worker threads
  for (i = 0; i < threads; i++) {
    //ga[i]->iters = updates * 2;   //lsc: why 2x updates?
    int r = pthread_create(&t[i], NULL, do_gups, (void*)ga[i]);
    assert(r == 0);
  }

  // wait for worker threads
  for (i = 0; i < threads; i++) {
    int r = pthread_join(t[i], NULL);
    assert(r == 0);
  }

  gettimeofday(&stoptime, NULL);

  secs = elapsed(&starttime, &stoptime);
  printf("Elapsed time for real benchmark: %.4f seconds.\n", secs);
  //gups = ((double)tot_updates) / (secs * 1.0e9);
  gups = threads * ((double)updates) / (secs * 1.0e9);
  printf("GUPS = %.10f\n", gups);

  memset(thread_gups, 0, sizeof(thread_gups));

  // free resources
  for (i = 0; i < threads; i++) {
    //free(ga[i]->indices);
    free(ga[i]);
  }
  free(ga);
  munmap(p, size);

  return 0;
}

