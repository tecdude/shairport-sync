/*
 * This file is part of Shairport Sync.
 * Copyright (c) Mike Brady 2020 -- 2021
 * All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#define __STDC_FORMAT_MACROS
#include "common.h"
#include <inttypes.h>
#include <unistd.h>

#include "../nqptp/nqptp-shm-structures.h"
#include "ptp-utilities.h"

int shm_fd;
void *mapped_addr = NULL;

int failure_message_sent = 0;

static pthread_mutex_t ptp_access_mutex = PTHREAD_MUTEX_INITIALIZER;

// returns a copy of the shared memory data from the nqptp
// shared memory interface, so long as it's open.
int get_nqptp_data(struct shm_structure *nqptp_data) {
  int response = -1; // presume the worst. Fix it on success
  // the first part of the shared memory is a mutex lock, so use it to get
  // exclusive access while copying

  if ((mapped_addr != MAP_FAILED) && (mapped_addr != NULL)) {
    pthread_cleanup_debug_mutex_lock((pthread_mutex_t *)mapped_addr, 100000, 1);
    memcpy(nqptp_data, (char *)mapped_addr, sizeof(struct shm_structure));
    pthread_cleanup_pop(1); // release the mutex
    response = 0;
  }
  return response;
}

int ptp_get_clock_info(uint64_t *actual_clock_id, uint64_t *raw_offset) {
  int response = -1;
  pthread_cleanup_debug_mutex_lock(&ptp_access_mutex, 10000, 1);
  // 0 -> valid and working; -1 -> can't connect to nqptp
  if (actual_clock_id != NULL)
    *actual_clock_id = 0;
  if (raw_offset != NULL)
    *raw_offset = 0;

  if (ptp_shm_interface_open() == 0) {
    struct shm_structure nqptp_data;
    if (get_nqptp_data(&nqptp_data) == 0) {
      if (nqptp_data.version == NQPTP_SHM_STRUCTURES_VERSION) {
      if (actual_clock_id != NULL)
        *actual_clock_id = nqptp_data.master_clock_id;
      if (raw_offset != NULL)
        *raw_offset = nqptp_data.local_to_master_time_offset;
      response = 0;
      } else {
        if (failure_message_sent == 0) {
          warn("This version of Shairport Sync requires an NQPTP with a Shared Memory Interface Version %u, but the installed version is %u. Please install the correct version of NQPTP.", NQPTP_SHM_STRUCTURES_VERSION, nqptp_data.version);
          failure_message_sent = 1;
        }
      }
    }
    if (response != -1)
      response = ptp_shm_interface_close();
  } else {
    if (failure_message_sent == 0) {
      warn("Can't open the interface to nqptp. Is the service running?");
      failure_message_sent = 1;
    }
  }
  pthread_cleanup_pop(1); // release the mutex
  if (response == 0)
    failure_message_sent = 0;
  return response;
}

int ptp_shm_interface_open() {
  mapped_addr = NULL;
  int shared_memory_file_descriptor = shm_open("/nqptp", O_RDWR, 0);
  int response = -1;
  if (shared_memory_file_descriptor >= 0) {
    mapped_addr =
        // needs to be PROT_READ | PROT_WRITE to allow the mapped memory to be writable for the
        // mutex to lock and unlock
        mmap(NULL, sizeof(struct shm_structure), PROT_READ | PROT_WRITE, MAP_SHARED,
             shared_memory_file_descriptor, 0);
    if (mapped_addr == MAP_FAILED) {
      if (failure_message_sent == 0) {
        debug(1, "unable to open the shared memory interface with nqptp. Is the service running?");
        failure_message_sent = 1;
      }
    }
    if (close(shared_memory_file_descriptor) == -1) {
      if (failure_message_sent == 0) {
        debug(1, "error closing \"/nqptp\" after mapping it.");
        failure_message_sent = 1;
      }
    } else {
      response = 0;
    }
  }
  return response;
}

int ptp_shm_interface_close() {
  int response = -1;
  if ((mapped_addr != MAP_FAILED) && (mapped_addr != NULL)) {
    response = munmap(mapped_addr, sizeof(struct shm_structure));
    if (response != 0)
      debug(1, "error unmapping shared memory.");
  }
  mapped_addr = NULL;
  return response;
}

void ptp_send_control_message_string(const char *msg) {
  debug(2, "Send control message to NQPTP: \"%s\"", msg);
  int s;
  unsigned short port = htons(NQPTP_CONTROL_PORT);
  struct sockaddr_in server;

  /* Create a datagram socket in the internet domain and use the
   * default protocol (UDP).
   */
  if ((s = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
    die("Can't open a socket to NQPTP");
  }

  /* Set up the server name */
  server.sin_family = AF_INET; /* Internet Domain    */
  server.sin_port = port;      /* Server Port        */
  server.sin_addr.s_addr = 0;  /* Server's Address   */

  /* Send the message in buf to the server */
  if (sendto(s, msg, (strlen(msg) + 1), 0, (struct sockaddr *)&server, sizeof(server)) < 0) {
    die("error sending timing_peer_list to NQPTP");
  }
  /* Deallocate the socket */
  close(s);
}
