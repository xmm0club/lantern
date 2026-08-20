#ifndef LANTERN_VFIO_H
#define LANTERN_VFIO_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

struct lt_syscall_ops {
    const char *name;
    int   (*open)(const char *path, int flags);
    int   (*close)(int fd);
    int   (*ioctl)(int fd, unsigned long request, void *arg);
    void *(*mmap)(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
    int   (*munmap)(void *addr, size_t length);
    ssize_t (*pread)(int fd, void *buf, size_t count, off_t offset);
    ssize_t (*pwrite)(int fd, const void *buf, size_t count, off_t offset);
    int   (*iommu_group_of)(const char *bdf);
};

typedef struct {
    const char *image_path;
    uint64_t capacity_bytes;
    uint32_t lba_bytes;
    uint32_t max_namespaces;
    uint32_t queue_latency_us;
    const char *serial;
    const char *model;
} lt_mock_config_t;

const struct lt_syscall_ops *lt_syscalls_real(void);
const struct lt_syscall_ops *lt_syscalls_mock(void);

void lt_mock_config_defaults(lt_mock_config_t *cfg);
int  lt_mock_arm(const lt_mock_config_t *cfg);
void lt_mock_disarm(void);
const char *lt_mock_last_error(void);

#endif
