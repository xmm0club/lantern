#define _GNU_SOURCE
#include "vfio.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

static int real_open(const char *path, int flags)
{
    return open(path, flags);
}

static int real_close(int fd)
{
    return close(fd);
}

static int real_ioctl(int fd, unsigned long request, void *arg)
{
    return ioctl(fd, request, arg);
}

static void *real_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
    return mmap(addr, length, prot, flags, fd, offset);
}

static int real_munmap(void *addr, size_t length)
{
    return munmap(addr, length);
}

static ssize_t real_pread(int fd, void *buf, size_t count, off_t offset)
{
    return pread(fd, buf, count, offset);
}

static ssize_t real_pwrite(int fd, const void *buf, size_t count, off_t offset)
{
    return pwrite(fd, buf, count, offset);
}

static int real_iommu_group_of(const char *bdf)
{
    char link[PATH_MAX];
    char target[PATH_MAX];
    ssize_t n;
    const char *base;

    if (snprintf(link, sizeof(link), "/sys/bus/pci/devices/%s/iommu_group", bdf) >= (int)sizeof(link)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    n = readlink(link, target, sizeof(target) - 1);
    if (n < 0)
        return -1;
    target[n] = '\0';
    base = strrchr(target, '/');
    base = base ? base + 1 : target;
    return (int)strtol(base, NULL, 10);
}

static const struct lt_syscall_ops real_ops = {
    .name = "vfio",
    .open = real_open,
    .close = real_close,
    .ioctl = real_ioctl,
    .mmap = real_mmap,
    .munmap = real_munmap,
    .pread = real_pread,
    .pwrite = real_pwrite,
    .iommu_group_of = real_iommu_group_of,
};

const struct lt_syscall_ops *lt_syscalls_real(void)
{
    return &real_ops;
}
