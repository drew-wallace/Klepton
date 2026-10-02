#ifndef KL_FILE_H
#define KL_FILE_H
// Path has already passed through kl_guest_path. dirfd and flags use Linux
// numbering; the returned fd is native and works with mmap/dup/descriptor IPC.
int kl_open_mapped(int dirfd, const char *path, int flags, int mode);
#endif
