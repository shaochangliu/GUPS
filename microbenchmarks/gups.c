/*
 * =====================================================================================
 *
 *       Filename:  gups.c
 *
 *    Description:
 *
 *        Version:  1.0
 *        Created:  02/21/2018 02:36:27 PM
 *       Revision:  none
 *       Compiler:  gcc
 *
 *         Author:  YOUR NAME (),
 *   Organization:
 *
 * =====================================================================================
 */

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <unistd.h>
#include <sys/time.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <math.h>
#include <string.h>
#include <pthread.h>
#include <sys/mman.h>
#include <errno.h>
#include <stdint.h>

#include "../timer.h"
//#include "../hemem.h"


#include "gups.h"

#define MAX_THREADS     64

#define GUPS_PAGE_SIZE      (4 * 1024)
#define PAGE_NUM            3
#define PAGES               2048

#ifdef HOTSPOT
extern uint64_t hotset_start;
extern double hotset_fraction;
#endif

int threads;

uint64_t hot_start = 0;
uint64_t hotsize = 0;
uint64_t hot_offset_page = 0;

struct gups_args {
  int tid;                      // thread id
  uint64_t *indices;       // array of indices to access
  void* field;                  // pointer to start of thread's region
  uint64_t iters;          // iterations to perform
  uint64_t size;           // size of region
  uint64_t elt_size;       // size of elements
};

_Atomic uint64_t thread_gups[MAX_THREADS];

static unsigned long updates, nelems;

static void *print_instantaneous_gups()
{
  uint64_t last_second_gups[threads];
  FILE *f[threads];
  char fname[20];

  for (int i = 0; i < threads; i++) {
    last_second_gups[i] = 0;
    snprintf(fname, 20, "gups_%d.txt", i);
    printf("file name: %s\n", fname);
    f[i] = fopen(fname, "w");
    if (f[i] == NULL) {
      perror("fopen");
      assert(0);
    }
  }

  for (;;) {
    for (int i = 0; i < threads; i++) {
      fprintf(f[i], "%.10f\n", (1.0 * (abs(thread_gups[i] - last_second_gups[i]))) / (1.0e9));
      last_second_gups[i] = thread_gups[i];
    }
    sleep(1);
    //printf("GUPS: %.10f\n", (1.0 * (abs(thread_gups[0]- last_second_gups))) / (1.0e9));
    //last_second_gups = thread_gups[0];
    //sleep(1);
  }

  return NULL;
}


static uint64_t lfsr_fast(uint64_t lfsr)
{
  lfsr ^= lfsr >> 7;
  lfsr ^= lfsr << 9;
  lfsr ^= lfsr >> 13;
  return lfsr;
}

char *filename = "indices1.txt";

static void *do_gups(void *arguments)
{
  //printf("do_gups entered\n");
  struct gups_args *args = (struct gups_args*)arguments;
  char *field = (char*)(args->field);
  uint64_t i, j;
  uint64_t index1, index2;
  uint64_t elt_size = args->elt_size;
  char data[elt_size];
  uint64_t lfsr;
  uint64_t hot_num;
  FILE *indexfile;
  uint64_t offset;

  printf("thread %d starting GUPS\n", args->tid);

  indexfile = fopen(filename, "w");
  if (indexfile == NULL) {
    perror("fopen");
    assert(0);
  }

  srand(0);
  lfsr = rand();

  index1 = 0;
  index2 = 0;

  for (i = 0; i < args->iters; i++) {
    hot_num = lfsr % 4 + 8;
    for (j = 0; j < hot_num; j++) {
      lfsr = lfsr_fast(lfsr);
      index1 = hot_start + (lfsr % hotsize);
      offset = index1 * elt_size;
      if (offset >= PAGE_NUM * GUPS_PAGE_SIZE && offset < PAGE_NUM * GUPS_PAGE_SIZE * PAGES) {
        index1 += ((hot_offset_page * GUPS_PAGE_SIZE) / elt_size);
      }
      memcpy(data, &field[index1 * elt_size], elt_size);
      memset(data, data[0] + j, elt_size);
      memcpy(&field[index1 * elt_size], data, elt_size);
      thread_gups[args->tid]++;
      fprintf(indexfile, "%lu\n", index1);
    }
    i += hot_num;

    lfsr = lfsr_fast(lfsr);
    index2 = lfsr % (args->size);
    memcpy(data, &field[index2 * elt_size], elt_size);
    memset(data, data[0] + i, elt_size);
    memcpy(&field[index2 * elt_size], data, elt_size);
    thread_gups[args->tid]++;
    fprintf(indexfile, "%lu\n", index1);
  }
/*
  //printf("Thread [%d] starting: field: [%llx]\n", args->tid, field);
  for (i = 0; i < args->iters; i++) {
    index = args->indices[i];
    memcpy(data, &field[index * elt_size], elt_size);
    memset(data, data[0] + i, elt_size);
    memcpy(&field[index * elt_size], data, elt_size);
  }
*/
  return 0;
}
/*
static void *calc_indices_thread(void *arg)
{
  struct gups_args *gai = (struct gups_args*)arg;
  calc_indices(gai->indices, updates, nelems);
  return NULL;
}
*/
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
    fprintf(stderr, "Usage: %s [threads] [updates per thread] [exponent] [data size (bytes)] [noremap/remap]\n", argv[0]);
    fprintf(stderr, "  threads\t\t\tnumber of threads to launch\n");
    fprintf(stderr, "  updates per thread\t\tnumber of updates per thread\n");
    fprintf(stderr, "  exponent\t\t\tlog size of region\n");
    fprintf(stderr, "  data size\t\t\tsize of data in array (in bytes)\n");
    fprintf(stderr, "  hot size\t\t\tlog size of hot set\n");
    return 0;
  }

  gettimeofday(&starttime, NULL);

  threads = atoi(argv[1]);
  assert(threads <= MAX_THREADS);
  ga = (struct gups_args**)malloc(threads * sizeof(struct gups_args*));
  fprintf(stderr, "size of ga: %lu\n", sizeof(ga));

  updates = atol(argv[2]);
  updates -= updates % 256;
  expt = atoi(argv[3]);
  assert(expt > 8);
  assert(updates > 0 && (updates % 256 == 0));
  size = (unsigned long)(1) << expt;
  size -= (size % 256);
  assert(size > 0 && (size % 256 == 0));
  elt_size = atoi(argv[4]);
  log_hot_size = atof(argv[5]);
  tot_hot_size = (unsigned long)(1) << log_hot_size;

  fprintf(stderr, "%lu updates per thread (%d threads)\n", updates, threads);
  fprintf(stderr, "field of 2^%lu (%lu) bytes\n", expt, size);
  fprintf(stderr, "%ld byte element size (%ld elements total)\n", elt_size, size / elt_size);

  p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS | MAP_HUGETLB | MAP_POPULATE, -1, 0);
  if (p == MAP_FAILED) {
    perror("mmap");
    assert(0);
  }

  gettimeofday(&stoptime, NULL);
  fprintf(stderr, "Init took %.4f seconds\n", elapsed(&starttime, &stoptime));
  fprintf(stderr, "Region address: %p - %p\t size: %ld\n", p, (p + size), size);
  
  nelems = (size / threads) / elt_size; // number of elements per thread
  fprintf(stderr, "Elements per thread: %lu\n", nelems);

  memset(thread_gups, 0, sizeof(thread_gups));

  fprintf(stderr, "initializing thread data\n");
  for (i = 0; i < threads; ++i) {
    ga[i] = (struct gups_args*)malloc(sizeof(struct gups_args));
    ga[i]->tid = i;
    ga[i]->field = p + (i * nelems * elt_size);
    //printf("Thread [%d] starting address: %llx\n", i, ga[i]->field);
    //printf("thread %d start address: %llu\n", i, (unsigned long)td[i].field);
    /*
    ga[i]->indices = (unsigned long*)malloc(updates * sizeof(unsigned long));
    if (ga[i]->indices == NULL) {
      perror("malloc");
      exit(1);
    }
    int r = pthread_create(&t[i], NULL, calc_indices_thread, (void*)ga[i]);
    assert(r == 0);
    */
  }

  // wait for worker threads
  //for (i = 0; i < threads; i++) {
    //int r = pthread_join(t[i], NULL);
    //assert(r == 0);
  //}

  gettimeofday(&stoptime, NULL);
  secs = elapsed(&starttime, &stoptime);
  fprintf(stderr, "Initialization time: %.4f seconds.\n", secs);

  pthread_t print_thread;
  int pt = pthread_create(&print_thread, NULL, print_instantaneous_gups, NULL);
  assert(pt == 0);

  hot_start = 0;
  hotsize = (tot_hot_size / threads) / elt_size;
  printf("hot_start: %lu\thot_size: %lu\n", hot_start, hotsize);

  gettimeofday(&starttime, NULL);

  // run through gups once to touch all memory
  // spawn gups worker threads
  for (i = 0; i < threads; i++) {
    //printf("starting thread [%d]\n", i);
    ga[i]->tid = i;
    ga[i]->iters = updates;
    ga[i]->size = nelems;
    ga[i]->elt_size = elt_size;
    //printf("  tid: [%d]  iters: [%llu]  size: [%llu]  elt size: [%llu]\n", ga[i]->tid, ga[i]->iters, ga[i]->size, ga[i]->elt_size);
    int r = pthread_create(&t[i], NULL, do_gups, (void*)ga[i]);
    assert(r == 0);
  }

  // wait for worker threads
  for (i = 0; i < threads; i++) {
    int r = pthread_join(t[i], NULL);
    assert(r == 0);
  }
  //hemem_print_stats();

  gettimeofday(&stoptime, NULL);

  secs = elapsed(&starttime, &stoptime);
  printf("Elapsed time: %.4f seconds.\n", secs);
  gups = threads * ((double)updates) / (secs * 1.0e9);
  printf("GUPS = %.10f\n", gups);
  memset(thread_gups, 0, sizeof(thread_gups));

  filename = "indices2.txt";

  fprintf(stderr, "Timing.\n");
  gettimeofday(&starttime, NULL);

  //hemem_clear_stats();
  // spawn gups worker threads
  for (i = 0; i < threads; i++) {
    //printf("starting thread [%d]\n", i);
    ga[i]->tid = i;
    ga[i]->iters = updates;
    ga[i]->size = nelems;
    ga[i]->elt_size = elt_size;
    //printf("  tid: [%d]  iters: [%llu]  size: [%llu]  elt size: [%llu]\n", ga[i]->tid, ga[i]->iters, ga[i]->size, ga[i]->elt_size);
    int r = pthread_create(&t[i], NULL, do_gups, (void*)ga[i]);
    assert(r == 0);
  }

  // wait for worker threads
  for (i = 0; i < threads; i++) {
    int r = pthread_join(t[i], NULL);
    assert(r == 0);
  }
  gettimeofday(&stoptime, NULL);
  //hemem_print_stats();
  //hemem_clear_stats();

  secs = elapsed(&starttime, &stoptime);
  printf("Elapsed time: %.4f seconds.\n", secs);
  gups = threads * ((double)updates) / (secs * 1.0e9);
  printf("GUPS = %.10f\n", gups);

  memset(thread_gups, 0, sizeof(thread_gups));

#ifdef HOTSPOT
  hot_offset_page = 33554432;
  //hot_start = (16UL * 1024UL * 1024UL * 1024UL) / elt_size;              // 16GB to the right;
  printf("hot_start: %lu\thot_size: %lu\n", hot_start, hotsize);

  filename = "indices3.txt";

  printf("Timing.\n");
  gettimeofday(&starttime, NULL);

  // spawn gups worker threads
  for (i = 0; i < threads; i++) {
    ga[i]->tid = i;
    ga[i]->iters = updates;
    ga[i]->size = nelems;
    ga[i]->elt_size = elt_size;
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
  printf("Elapsed time: %.4f seconds.\n", secs);
  gups = threads * ((double)updates) / (secs * 1.0e9);
  printf("GUPS = %.10f\n", gups);

  //hemem_print_stats();
#endif

  for (i = 0; i < threads; i++) {
    //free(ga[i]->indices);
    free(ga[i]);
  }
  free(ga);

  munmap(p, size);

  return 0;
}

