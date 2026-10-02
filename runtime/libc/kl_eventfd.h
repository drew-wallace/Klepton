#ifndef KL_EVENTFD_H
#define KL_EVENTFD_H
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/uio.h>

int klb_eventfd(unsigned initial, int flags);
int klb_eventfd_read(int fd, uint64_t *value);
int klb_eventfd_write(int fd, uint64_t value);
ssize_t kl_eventfd_read(int fd, void *buffer, size_t size, int *handled);
ssize_t kl_eventfd_write(int fd, const void *buffer, size_t size, int *handled);
int kl_eventfd_getfl(int fd, int *handled);
int kl_eventfd_setfl(int fd, int flags, int *handled);
int kl_eventfd_close(int fd, int *handled);
// to=-1: dup; to=-2: F_DUPFD with minimum; otherwise dup2.
int kl_eventfd_dup(int from, int to, int minimum, int cloexec);
ssize_t klb_write(int fd, const void *buffer, size_t size);
ssize_t klb_readv(int fd, const struct iovec *iov, int count);
ssize_t klb_writev(int fd, const struct iovec *iov, int count);
int klb_dup(int fd);
#endif
