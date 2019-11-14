// libzfifo-test: DMA loopback test

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <omp.h>

#include "zfifo.h"

int main(){
  int fd = open("/dev/zfifo0", O_RDWR | O_SYNC);
  if (fd<0) {
    printf("Can't open /dev/zfifo0!\n");
    return -1;
  }

  unsigned size = 16*1024*1024;
  unsigned *send = (unsigned*)malloc(size * sizeof(unsigned));
  unsigned *recv = (unsigned*)malloc(size * sizeof(unsigned));
  
  for (unsigned i=0; i<size; i++){
    send[i] = i;
    recv[i] = 0;
  }

  // Send & recv in parallel because FIFO deadlocks
#pragma omp parallel for
  for(int i=0; i<2; i++){
    if (i==0){
      printf("Send on thread %d\n", omp_get_thread_num());
      zf_send(fd, (char*)send, sizeof(unsigned)*size);
    }
    if (i==1){
      printf("Recv on thread %d\n", omp_get_thread_num());
      zf_recv(fd, (char*)recv, sizeof(unsigned)*size);
    }
  }
  
  int errors=0;
  for (unsigned i=0; i<size; i++){
    if (send[i] != recv[i]){
      errors++;
      if (errors < 20)
        printf("[%u]: send %u != recv %u\n", i, send[i], recv[i]);
    }
  }  

  if (errors==0) printf("Data transferred correctly.\n");
  
  close(fd);
  return 0;
}
