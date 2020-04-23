/*
 * =====================================================================================
 *
 *       Filename:  simple.c
 *
 *    Description:  
 *
 *        Version:  1.0
 *        Created:  02/04/2020 09:58:58 AM
 *       Revision:  none
 *       Compiler:  gcc
 *
 *         Author:  YOUR NAME (), 
 *   Organization:  
 *
 * =====================================================================================
 */
#include <stdlib.h>
#include <pthread.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdbool.h>
#include <pthread.h>
#include <assert.h>
#include <sys/time.h>

#include "hemem.h"
#include "paging.h"
#include "timer.h"

uint64_t fastmem = 0;
uint64_t slowmem = 0;
bool slowmem_switch = false;

struct hemem_page* simple_pagefault()
{
  struct hemem_page* page = hemem_get_free_page();
  struct timeval start, end;

  gettimeofday(&start, NULL);
  if (fastmem < DRAMSIZE) {
    page->in_dram = true;
    page->devdax_offset = fastmem;
    page->pt = pagesize_to_pt(PAGE_SIZE);
    fastmem += PAGE_SIZE;
    assert(fastmem <= DRAMSIZE);
  }
  else {
    assert(slowmem < NVMSIZE);
    page->in_dram = false;
    page->devdax_offset = slowmem;
    page->pt = pagesize_to_pt(PAGE_SIZE);
    slowmem += PAGE_SIZE;
    assert(slowmem <= NVMSIZE);
    if (!slowmem_switch) {
      LOG("Switched to allocating from slowmem\n");
      slowmem_switch = true;
    }
  }

  gettimeofday(&end, NULL);
  LOG_TIME("mem_policy_allocate_page: %f s\n", elapsed(&start, &end));
  
  return page;
}

void simple_init(void)
{
  LOG("Memory management policy is simple\n");
}
