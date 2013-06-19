/*
 * Copyright (c) 2007, Neocleus Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 59 Temple
 * Place - Suite 330, Boston, MA 02111-1307 USA.
 *
 *
 *  Assign a PCI device from the host to a guest VM.
 *
 *  Adapted for KVM by Qumranet.
 *
 *  Copyright (c) 2007, Neocleus, Alex Novik (alex@neocleus.com)
 *  Copyright (c) 2007, Neocleus, Guy Zana (guy@neocleus.com)
 *  Copyright (C) 2008, Qumranet, Amit Shah (amit.shah@qumranet.com)
 *  Copyright (C) 2008, Red Hat, Amit Shah (amit.shah@redhat.com)
 *  Copyright (C) 2008, IBM, Muli Ben-Yehuda (muli@il.ibm.com)
 */
#include <stdio.h>
#include <unistd.h>
#include <sys/io.h>
#include <sys/types.h>
#include <sys/stat.h>
#include "qemu-kvm.h"
#include "hw.h"
#include "pc.h"
#include "qemu-error.h"
#include "console.h"
#include "device-assignment.h"
#include "loader.h"
#include "monitor.h"
#include "range.h"
#include "sysemu.h"

#define MSIX_PAGE_SIZE 0x1000

/* From linux/ioport.h */
#define IORESOURCE_IO       0x00000100  /* Resource type */
#define IORESOURCE_MEM      0x00000200
#define IORESOURCE_IRQ      0x00000400
#define IORESOURCE_DMA      0x00000800
#define IORESOURCE_PREFETCH 0x00002000  /* No side effects */

/* #define DEVICE_ASSIGNMENT_DEBUG 1 */

#ifdef DEVICE_ASSIGNMENT_DEBUG
#define DEBUG(fmt, ...)                                       \
    do {                                                      \
      fprintf(stderr, "%s: " fmt, __func__ , __VA_ARGS__);    \
    } while (0)
#else
#define DEBUG(fmt, ...) do { } while(0)
#endif

static void assigned_dev_load_option_rom(AssignedDevice *dev);

static void assigned_dev_unregister_msix_mmio(AssignedDevice *dev);

static uint64_t assigned_dev_ioport_rw(AssignedDevRegion *dev_region,
                                       target_phys_addr_t addr, int size,
                                       uint64_t *data)
{
    uint64_t val = 0;
    int fd = dev_region->region->resource_fd;

    if (fd >= 0) {
        if (data) {
            DEBUG("pwrite data=%x, size=%d, e_phys=%x, addr=%x\n",
                  *data, size, addr, addr);
            if (pwrite(fd, data, size, addr) != size) {
                fprintf(stderr, "%s - pwrite failed %s\n",
                        __func__, strerror(errno));
            }
        } else {
            if (pread(fd, &val, size, addr) != size) {
                fprintf(stderr, "%s - pread failed %s\n",
                        __func__, strerror(errno));
                val = (1UL << (size * 8)) - 1;
            }
            DEBUG("pread val=%x, size=%d, e_phys=%x, addr=%x\n",
                  val, size, addr, addr);
        }
    } else {
        uint32_t port = addr + dev_region->u.r_baseport;

        if (data) {
            DEBUG("out data=%x, size=%d, e_phys=%x, host=%x\n",
                  *data, size, addr, port);
            switch (size) {
                case 1:
                    outb(*data, port);
                    break;
                case 2:
                    outw(*data, port);
                    break;
                case 4:
                    outl(*data, port);
                    break;
            }
        } else {
            switch (size) {
                case 1:
                    val = inb(port);
                    break;
                case 2:
                    val = inw(port);
                    break;
                case 4:
                    val = inl(port);
                    break;
            }
            DEBUG("in data=%x, size=%d, e_phys=%x, host=%x\n",
                  val, size, addr, port);
        }
    }
    return val;
}

static void assigned_dev_ioport_write(void *opaque, target_phys_addr_t addr,
                                      uint64_t data, unsigned size)
{
    assigned_dev_ioport_rw(opaque, addr, size, &data);
}

static uint64_t assigned_dev_ioport_read(void *opaque,
                                         target_phys_addr_t addr, unsigned size)
{
    return assigned_dev_ioport_rw(opaque, addr, size, NULL);
}

static uint32_t slow_bar_readb(void *opaque, target_phys_addr_t addr)
{
    AssignedDevRegion *d = opaque;
    uint8_t *in = d->u.r_virtbase + addr;
    uint32_t r;

    r = *in;
    DEBUG("slow_bar_readl addr=0x" TARGET_FMT_plx " val=0x%08x\n", addr, r);

    return r;
}

static uint32_t slow_bar_readw(void *opaque, target_phys_addr_t addr)
{
    AssignedDevRegion *d = opaque;
    uint16_t *in = d->u.r_virtbase + addr;
    uint32_t r;

    r = *in;
    DEBUG("slow_bar_readl addr=0x" TARGET_FMT_plx " val=0x%08x\n", addr, r);

    return r;
}

static uint32_t slow_bar_readl(void *opaque, target_phys_addr_t addr)
{
    AssignedDevRegion *d = opaque;
    uint32_t *in = d->u.r_virtbase + addr;
    uint32_t r;

    r = *in;
    DEBUG("slow_bar_readl addr=0x" TARGET_FMT_plx " val=0x%08x\n", addr, r);

    return r;
}

static void slow_bar_writeb(void *opaque, target_phys_addr_t addr, uint32_t val)
{
    AssignedDevRegion *d = opaque;
    uint8_t *out = d->u.r_virtbase + addr;

    DEBUG("slow_bar_writeb addr=0x" TARGET_FMT_plx " val=0x%02x\n", addr, val);
    *out = val;
}

static void slow_bar_writew(void *opaque, target_phys_addr_t addr, uint32_t val)
{
    AssignedDevRegion *d = opaque;
    uint16_t *out = d->u.r_virtbase + addr;

    DEBUG("slow_bar_writew addr=0x" TARGET_FMT_plx " val=0x%04x\n", addr, val);
    *out = val;
}

static void slow_bar_writel(void *opaque, target_phys_addr_t addr, uint32_t val)
{
    AssignedDevRegion *d = opaque;
    uint32_t *out = d->u.r_virtbase + addr;

    DEBUG("slow_bar_writel addr=0x" TARGET_FMT_plx " val=0x%08x\n", addr, val);
    *out = val;
}

static const MemoryRegionOps slow_bar_ops = {
    .old_mmio = {
        .read = { slow_bar_readb, slow_bar_readw, slow_bar_readl, },
        .write = { slow_bar_writeb, slow_bar_writew, slow_bar_writel, },
    },
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void assigned_dev_iomem_setup(PCIDevice *pci_dev, int region_num,
                                     pcibus_t e_size)
{
    AssignedDevice *r_dev = DO_UPCAST(AssignedDevice, dev, pci_dev);
    AssignedDevRegion *region = &r_dev->v_addrs[region_num];
    PCIRegion *real_region = &r_dev->real_device.regions[region_num];

    if (e_size > 0) {
        memory_region_init(&region->container, "assigned-dev-container",
                           e_size);
        memory_region_add_subregion(&region->container, 0, &region->real_iomem);

        /* deal with MSI-X MMIO page */
        if (real_region->base_addr <= r_dev->msix_table_addr &&
                real_region->base_addr + real_region->size >
                r_dev->msix_table_addr) {
            int offset = r_dev->msix_table_addr - real_region->base_addr;

            memory_region_add_subregion_overlap(&region->container,
                                                offset,
                                                &r_dev->mmio,
                                                1);
        }
    }
}

static const MemoryRegionOps assigned_dev_ioport_ops = {
    .read = assigned_dev_ioport_read,
    .write = assigned_dev_ioport_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void assigned_dev_ioport_setup(PCIDevice *pci_dev, int region_num,
                                      pcibus_t size)
{
    AssignedDevice *r_dev = DO_UPCAST(AssignedDevice, dev, pci_dev);
    AssignedDevRegion *region = &r_dev->v_addrs[region_num];
    int r;

    region->e_size = size;

    if (region->region->resource_fd < 0) {
        r = kvm_add_ioport_region(region->u.r_baseport, region->r_size,
                                  pci_dev->qdev.hotplugged);
        if (r < 0) {
            fprintf(stderr, "%s: failed to enable ioport access (%m)\n",
                    __func__);
        }
    }
    memory_region_init(&region->container, "assigned-dev-container", size);
    memory_region_init_io(&region->real_iomem, &assigned_dev_ioport_ops,
                          r_dev->v_addrs + region_num,
                          "assigned-dev-iomem", size);
    memory_region_add_subregion(&region->container, 0, &region->real_iomem);
}

static uint32_t assigned_dev_pci_read(PCIDevice *d, int pos, int len)
{
    AssignedDevice *pci_dev = DO_UPCAST(AssignedDevice, dev, d);
    uint32_t val;
    ssize_t ret;
    int fd = pci_dev->real_device.config_fd;

again:
    ret = pread(fd, &val, len, pos);
    if (ret != len) {
	if ((ret < 0) && (errno == EINTR || errno == EAGAIN))
	    goto again;

	fprintf(stderr, "%s: pread failed, ret = %zd errno = %d\n",
		__func__, ret, errno);

	exit(1);
    }

    return val;
}

static uint8_t assigned_dev_pci_read_byte(PCIDevice *d, int pos)
{
    return (uint8_t)assigned_dev_pci_read(d, pos, 1);
}

static void assigned_dev_pci_write(PCIDevice *d, int pos, uint32_t val, int len)
{
    AssignedDevice *pci_dev = DO_UPCAST(AssignedDevice, dev, d);
    ssize_t ret;
    int fd = pci_dev->real_device.config_fd;

again:
    ret = pwrite(fd, &val, len, pos);
    if (ret != len) {
	if ((ret < 0) && (errno == EINTR || errno == EAGAIN))
	    goto again;

	fprintf(stderr, "%s: pwrite failed, ret = %zd errno = %d\n",
		__func__, ret, errno);

	exit(1);
    }

    return;
}

static void assigned_dev_emulate_config_read(AssignedDevice *dev,
                                             uint32_t offset, uint32_t len)
{
    memset(dev->emulate_config_read + offset, 0xff, len);
}

static void assigned_dev_direct_config_read(AssignedDevice *dev,
                                            uint32_t offset, uint32_t len)
{
    memset(dev->emulate_config_read + offset, 0, len);
}

static void assigned_dev_direct_config_write(AssignedDevice *dev,
                                             uint32_t offset, uint32_t len)
{
    memset(dev->emulate_config_write + offset, 0, len);
}

static uint8_t pci_find_cap_offset(PCIDevice *d, uint8_t cap, uint8_t start)
{
    int id;
    int max_cap = 48;
    int pos = start ? start : PCI_CAPABILITY_LIST;
    int status;

    status = assigned_dev_pci_read_byte(d, PCI_STATUS);
    if ((status & PCI_STATUS_CAP_LIST) == 0)
        return 0;

    while (max_cap--) {
        pos = assigned_dev_pci_read_byte(d, pos);
        if (pos < 0x40)
            break;

        pos &= ~3;
        id = assigned_dev_pci_read_byte(d, pos + PCI_CAP_LIST_ID);

        if (id == 0xff)
            break;
        if (id == cap)
            return pos;

        pos += PCI_CAP_LIST_NEXT;
    }
    return 0;
}

static int assigned_dev_register_regions(PCIRegion *io_regions,
                                         unsigned long regions_num,
                                         AssignedDevice *pci_dev)
{
    uint32_t i;
    PCIRegion *cur_region = io_regions;

    for (i = 0; i < regions_num; i++, cur_region++) {
        if (!cur_region->valid)
            continue;
        pci_dev->v_addrs[i].num = i;

        /* handle memory io regions */
        if (cur_region->type & IORESOURCE_MEM) {
            int t = cur_region->type & IORESOURCE_PREFETCH
                ? PCI_BASE_ADDRESS_MEM_PREFETCH
                : PCI_BASE_ADDRESS_SPACE_MEMORY;

            /* map physical memory */
            pci_dev->v_addrs[i].u.r_virtbase = mmap(NULL, cur_region->size,
                                                    PROT_WRITE | PROT_READ,
                                                    MAP_SHARED,
                                                    cur_region->resource_fd,
                                                    (off_t)0);

            if (pci_dev->v_addrs[i].u.r_virtbase == MAP_FAILED) {
                pci_dev->v_addrs[i].u.r_virtbase = NULL;
                fprintf(stderr, "%s: Error: Couldn't mmap 0x%x!"
                        "\n", __func__,
                        (uint32_t) (cur_region->base_addr));
                return -1;
            }

            pci_dev->v_addrs[i].r_size = cur_region->size;
            pci_dev->v_addrs[i].e_size = 0;

            /* add offset */
            pci_dev->v_addrs[i].u.r_virtbase +=
                (cur_region->base_addr & 0xFFF);

            if (cur_region->size & 0xFFF) {
                fprintf(stderr, "PCI region %d at address 0x%llx "
                        "has size 0x%x, which is not a multiple of 4K. "
                        "You might experience some performance hit "
                        "due to that.\n",
                        i, (unsigned long long)cur_region->base_addr,
                        cur_region->size);
                memory_region_init_io(&pci_dev->v_addrs[i].real_iomem,
                                      &slow_bar_ops, &pci_dev->v_addrs[i],
                                      "assigned-dev-slow-bar",
                                      cur_region->size);
            } else {
                void *virtbase = pci_dev->v_addrs[i].u.r_virtbase;
                char name[32];
                snprintf(name, sizeof(name), "%s.bar%d",
                         pci_dev->dev.qdev.info->name, i);
                memory_region_init_ram_ptr(&pci_dev->v_addrs[i].real_iomem,
                                           &pci_dev->dev.qdev,
                                           name, cur_region->size,
                                           virtbase);
            }

            assigned_dev_iomem_setup(&pci_dev->dev, i, cur_region->size);
            pci_register_bar((PCIDevice *) pci_dev, i, t,
                             &pci_dev->v_addrs[i].container);
            continue;
        } else {
            /* handle port io regions */
            uint32_t val;
            int ret;

            /* Test kernel support for ioport resource read/write.  Old
             * kernels return EIO.  New kernels only allow 1/2/4 byte reads
             * so should return EINVAL for a 3 byte read */
            ret = pread(pci_dev->v_addrs[i].region->resource_fd, &val, 3, 0);
            if (ret >= 0) {
                fprintf(stderr, "Unexpected return from I/O port read: %d\n",
                        ret);
                abort();
            } else if (errno != EINVAL) {
                fprintf(stderr, "Using raw in/out ioport access (sysfs - %s)\n",
                        strerror(errno));
                close(pci_dev->v_addrs[i].region->resource_fd);
                pci_dev->v_addrs[i].region->resource_fd = -1;
            }

            pci_dev->v_addrs[i].u.r_baseport = cur_region->base_addr;
            pci_dev->v_addrs[i].r_size = cur_region->size;
            pci_dev->v_addrs[i].e_size = 0;

            assigned_dev_ioport_setup(&pci_dev->dev, i, cur_region->size);
            pci_register_bar((PCIDevice *) pci_dev, i,
                             PCI_BASE_ADDRESS_SPACE_IO,
                             &pci_dev->v_addrs[i].container);
        }
    }

    /* success */
    return 0;
}

static int get_real_id(const char *devpath, const char *idname, uint16_t *val)
{
    FILE *f;
    char name[128];
    long id;

    snprintf(name, sizeof(name), "%s%s", devpath, idname);
    f = fopen(name, "r");
    if (f == NULL) {
        fprintf(stderr, "%s: %s: %m\n", __func__, name);
        return -1;
    }
    if (fscanf(f, "%li\n", &id) == 1) {
        *val = id;
    } else {
        return -1;
    }
    fclose(f);

    return 0;
}

static int get_real_vendor_id(const char *devpath, uint16_t *val)
{
    return get_real_id(devpath, "vendor", val);
}

static int get_real_device_id(const char *devpath, uint16_t *val)
{
    return get_real_id(devpath, "device", val);
}

static int get_real_device(AssignedDevice *pci_dev, uint16_t r_seg,
                           uint8_t r_bus, uint8_t r_dev, uint8_t r_func)
{
    char dir[128], name[128];
    int fd, r = 0, v;
    FILE *f;
    unsigned long long start, end, size, flags;
    uint16_t id;
    struct stat statbuf;
    PCIRegion *rp;
    PCIDevRegions *dev = &pci_dev->real_device;

    dev->region_number = 0;

    snprintf(dir, sizeof(dir), "/sys/bus/pci/devices/%04x:%02x:%02x.%x/",
	     r_seg, r_bus, r_dev, r_func);

    snprintf(name, sizeof(name), "%sconfig", dir);

    if (pci_dev->configfd_name && *pci_dev->configfd_name) {
        if (qemu_isdigit(pci_dev->configfd_name[0])) {
            dev->config_fd = strtol(pci_dev->configfd_name, NULL, 0);
        } else {
            dev->config_fd = monitor_get_fd(cur_mon, pci_dev->configfd_name);
            if (dev->config_fd < 0) {
                fprintf(stderr, "%s: (%s) unkown\n", __func__,
                        pci_dev->configfd_name);
                return 1;
            }
        }
    } else {
        dev->config_fd = open(name, O_RDWR);

        if (dev->config_fd == -1) {
            fprintf(stderr, "%s: %s: %m\n", __func__, name);
            return 1;
        }
    }
again:
    r = read(dev->config_fd, pci_dev->dev.config,
             pci_config_size(&pci_dev->dev));
    if (r < 0) {
        if (errno == EINTR || errno == EAGAIN)
            goto again;
        fprintf(stderr, "%s: read failed, errno = %d\n", __func__, errno);
    }

    /* Clear host resource mapping info.  If we choose not to register a
     * BAR, such as might be the case with the option ROM, we can get
     * confusing, unwritable, residual addresses from the host here. */
    memset(&pci_dev->dev.config[PCI_BASE_ADDRESS_0], 0, 24);
    memset(&pci_dev->dev.config[PCI_ROM_ADDRESS], 0, 4);

    snprintf(name, sizeof(name), "%sresource", dir);

    f = fopen(name, "r");
    if (f == NULL) {
        fprintf(stderr, "%s: %s: %m\n", __func__, name);
        return 1;
    }

    for (r = 0; r < PCI_ROM_SLOT; r++) {
	if (fscanf(f, "%lli %lli %lli\n", &start, &end, &flags) != 3)
	    break;

        rp = dev->regions + r;
        rp->valid = 0;
        rp->resource_fd = -1;
        size = end - start + 1;
        flags &= IORESOURCE_IO | IORESOURCE_MEM | IORESOURCE_PREFETCH;
        if (size == 0 || (flags & ~IORESOURCE_PREFETCH) == 0)
            continue;
        if (flags & IORESOURCE_MEM) {
            flags &= ~IORESOURCE_IO;
        } else {
            flags &= ~IORESOURCE_PREFETCH;
        }
        snprintf(name, sizeof(name), "%sresource%d", dir, r);
        fd = open(name, O_RDWR);
        if (fd == -1)
            continue;
        rp->resource_fd = fd;

        rp->type = flags;
        rp->valid = 1;
        rp->base_addr = start;
        rp->size = size;
        pci_dev->v_addrs[r].region = rp;
        DEBUG("region %d size %d start 0x%llx type %d resource_fd %d\n",
              r, rp->size, start, rp->type, rp->resource_fd);
    }

    fclose(f);

    /* read and fill vendor ID */
    v = get_real_vendor_id(dir, &id);
    if (v) {
        return 1;
    }
    pci_dev->dev.config[0] = id & 0xff;
    pci_dev->dev.config[1] = (id & 0xff00) >> 8;

    /* read and fill device ID */
    v = get_real_device_id(dir, &id);
    if (v) {
        return 1;
    }
    pci_dev->dev.config[2] = id & 0xff;
    pci_dev->dev.config[3] = (id & 0xff00) >> 8;

    /* dealing with virtual function device */
    snprintf(name, sizeof(name), "%sphysfn/", dir);
    if (!stat(name, &statbuf)) {
        /* always provide the written value on readout */
        assigned_dev_emulate_config_read(pci_dev, PCI_COMMAND, 2);
    }

    dev->region_number = r;
    return 0;
}

static QLIST_HEAD(, AssignedDevice) devs = QLIST_HEAD_INITIALIZER(devs);

static void free_dev_irq_entries(AssignedDevice *dev)
{
    int i;

    for (i = 0; i < dev->irq_entries_nr; i++)
        kvm_del_routing_entry(&dev->entry[i]);
    g_free(dev->entry);
    dev->entry = NULL;
    dev->irq_entries_nr = 0;
}

static void free_assigned_device(AssignedDevice *dev)
{
    int i;

    if (dev->cap.available & ASSIGNED_DEVICE_CAP_MSIX) {
        assigned_dev_unregister_msix_mmio(dev);
    }
    for (i = 0; i < dev->real_device.region_number; i++) {
        PCIRegion *pci_region = &dev->real_device.regions[i];
        AssignedDevRegion *region = &dev->v_addrs[i];

        if (!pci_region->valid) {
            continue;
        }
        if (pci_region->type & IORESOURCE_IO) {
            if (pci_region->resource_fd < 0) {
                kvm_remove_ioport_region(region->u.r_baseport, region->r_size,
                                         dev->dev.qdev.hotplugged);
            }
            memory_region_del_subregion(&region->container,
                                        &region->real_iomem);
            memory_region_destroy(&region->real_iomem);
            memory_region_destroy(&region->container);
        } else if (pci_region->type & IORESOURCE_MEM) {
            if (region->u.r_virtbase) {
                memory_region_del_subregion(&region->container,
                                            &region->real_iomem);

                /* Remove MSI-X table subregion */
                if (pci_region->base_addr <= dev->msix_table_addr &&
                    pci_region->base_addr + pci_region->size >
                    dev->msix_table_addr) {
                    memory_region_del_subregion(&region->container,
                                                &dev->mmio);
                }

                memory_region_destroy(&region->real_iomem);
                memory_region_destroy(&region->container);
                if (munmap(region->u.r_virtbase,
                           (pci_region->size + 0xFFF) & 0xFFFFF000)) {
                    fprintf(stderr,
                            "Failed to unmap assigned device region: %s\n",
                            strerror(errno));
                }
            }
        }
        if (pci_region->resource_fd >= 0) {
            close(pci_region->resource_fd);
        }
    }

    if (dev->real_device.config_fd >= 0) {
        close(dev->real_device.config_fd);
    }

    free_dev_irq_entries(dev);
}

static uint32_t calc_assigned_dev_id(AssignedDevice *dev)
{
    return (uint32_t)dev->h_segnr << 16 | (uint32_t)dev->h_busnr << 8 |
           (uint32_t)dev->h_devfn;
}

static void assign_failed_examine(AssignedDevice *dev)
{
    char name[PATH_MAX], dir[PATH_MAX], driver[PATH_MAX] = {}, *ns;
    uint16_t vendor_id, device_id;
    int r;

    sprintf(dir, "/sys/bus/pci/devices/%04x:%02x:%02x.%01x/",
            dev->host.seg, dev->host.bus, dev->host.dev, dev->host.func);

    sprintf(name, "%sdriver", dir);

    r = readlink(name, driver, sizeof(driver));
    if ((r <= 0) || r >= sizeof(driver) || !(ns = strrchr(driver, '/'))) {
        goto fail;
    }

    ns++;

    if (get_real_vendor_id(dir, &vendor_id) ||
        get_real_device_id(dir, &device_id)) {
        goto fail;
    }

    fprintf(stderr, "*** The driver '%s' is occupying your device "
                    "%04x:%02x:%02x.%x.\n",
            ns, dev->host.seg, dev->host.bus, dev->host.dev, dev->host.func);
    fprintf(stderr, "***\n");
    fprintf(stderr, "*** You can try the following commands to free it:\n");
    fprintf(stderr, "***\n");
    fprintf(stderr, "*** $ echo \"%04x %04x\" > /sys/bus/pci/drivers/pci-stub/"
                    "new_id\n", vendor_id, device_id);
    fprintf(stderr, "*** $ echo \"%04x:%02x:%02x.%x\" > /sys/bus/pci/drivers/"
                    "%s/unbind\n",
            dev->host.seg, dev->host.bus, dev->host.dev, dev->host.func, ns);
    fprintf(stderr, "*** $ echo \"%04x:%02x:%02x.%x\" > /sys/bus/pci/drivers/"
                    "pci-stub/bind\n",
            dev->host.seg, dev->host.bus, dev->host.dev, dev->host.func);
    fprintf(stderr, "*** $ echo \"%04x %04x\" > /sys/bus/pci/drivers/pci-stub"
                    "/remove_id\n", vendor_id, device_id);
    fprintf(stderr, "***\n");

    return;

fail:
    fprintf(stderr, "Couldn't find out why.\n");
}

static int assign_device(AssignedDevice *dev)
{
    struct kvm_assigned_pci_dev assigned_dev_data;
    int r;

    /* Only pass non-zero PCI segment to capable module */
    if (!kvm_check_extension(kvm_state, KVM_CAP_PCI_SEGMENT) &&
        dev->h_segnr) {
        fprintf(stderr, "Can't assign device inside non-zero PCI segment "
                "as this KVM module doesn't support it.\n");
        return -ENODEV;
    }

    memset(&assigned_dev_data, 0, sizeof(assigned_dev_data));
    assigned_dev_data.assigned_dev_id = calc_assigned_dev_id(dev);
    assigned_dev_data.segnr = dev->h_segnr;
    assigned_dev_data.busnr = dev->h_busnr;
    assigned_dev_data.devfn = dev->h_devfn;

    /* We always enable the IOMMU unless disabled on the command line */
    if (dev->features & ASSIGNED_DEVICE_USE_IOMMU_MASK) {
        if (!kvm_check_extension(kvm_state, KVM_CAP_IOMMU)) {
            fprintf(stderr, "No IOMMU found.  Unable to assign device \"%s\"\n",
                    dev->dev.qdev.id);
            return -ENODEV;
        }
        assigned_dev_data.flags |= KVM_DEV_ASSIGN_ENABLE_IOMMU;
    }
    if (!(dev->features & ASSIGNED_DEVICE_USE_IOMMU_MASK)) {
        fprintf(stderr,
                "WARNING: Assigning a device without IOMMU protection can "
                "cause host memory corruption if the device issues DMA write "
                "requests!\n");
    }

    r = kvm_assign_pci_device(kvm_state, &assigned_dev_data);
    if (r < 0) {
        fprintf(stderr, "Failed to assign device \"%s\" : %s\n",
                dev->dev.qdev.id, strerror(-r));

        switch (r) {
            case -EBUSY:
                assign_failed_examine(dev);
                break;
            default:
                break;
        }
    }
    return r;
}

static int assign_irq(AssignedDevice *dev)
{
    struct kvm_assigned_irq assigned_irq_data;
    int irq, r = 0;

    /* Interrupt PIN 0 means don't use INTx */
    if (assigned_dev_pci_read_byte(&dev->dev, PCI_INTERRUPT_PIN) == 0)
        return 0;

    irq = pci_map_irq(&dev->dev, dev->intpin);
    irq = piix_get_irq(irq);

    if (dev->girq == irq)
        return r;

    memset(&assigned_irq_data, 0, sizeof(assigned_irq_data));
    assigned_irq_data.assigned_dev_id = calc_assigned_dev_id(dev);
    assigned_irq_data.guest_irq = irq;
    assigned_irq_data.host_irq = dev->real_device.irq;
    if (dev->irq_requested_type) {
        assigned_irq_data.flags = dev->irq_requested_type;
        r = kvm_deassign_irq(kvm_state, &assigned_irq_data);
        if (r) {
            perror("assign_irq: deassign");
        }
        dev->irq_requested_type = 0;
    }

    assigned_irq_data.flags = KVM_DEV_IRQ_GUEST_INTX;
    if (dev->features & ASSIGNED_DEVICE_PREFER_MSI_MASK &&
        dev->cap.available & ASSIGNED_DEVICE_CAP_MSI)
        assigned_irq_data.flags |= KVM_DEV_IRQ_HOST_MSI;
    else
        assigned_irq_data.flags |= KVM_DEV_IRQ_HOST_INTX;

    r = kvm_assign_irq(kvm_state, &assigned_irq_data);
    if (r < 0) {
        fprintf(stderr, "Failed to assign irq for \"%s\": %s\n",
                dev->dev.qdev.id, strerror(-r));
        fprintf(stderr, "Perhaps you are assigning a device "
                "that shares an IRQ with another device?\n");
        return r;
    }

    dev->girq = irq;
    dev->irq_requested_type = assigned_irq_data.flags;
    return r;
}

static void deassign_device(AssignedDevice *dev)
{
    struct kvm_assigned_pci_dev assigned_dev_data;
    int r;

    memset(&assigned_dev_data, 0, sizeof(assigned_dev_data));
    assigned_dev_data.assigned_dev_id = calc_assigned_dev_id(dev);

    r = kvm_deassign_pci_device(kvm_state, &assigned_dev_data);
    if (r < 0)
	fprintf(stderr, "Failed to deassign device \"%s\" : %s\n",
                dev->dev.qdev.id, strerror(-r));
}

#if 0
AssignedDevInfo *get_assigned_device(int pcibus, int slot)
{
    AssignedDevice *assigned_dev = NULL;
    AssignedDevInfo *adev = NULL;

    QLIST_FOREACH(adev, &adev_head, next) {
        assigned_dev = adev->assigned_dev;
        if (pci_bus_num(assigned_dev->dev.bus) == pcibus &&
            PCI_SLOT(assigned_dev->dev.devfn) == slot)
            return adev;
    }

    return NULL;
}
#endif

/* The pci config space got updated. Check if irq numbers have changed
 * for our devices
 */
void assigned_dev_update_irqs(void)
{
    AssignedDevice *dev, *next;
    int r;

    dev = QLIST_FIRST(&devs);
    while (dev) {
        next = QLIST_NEXT(dev, next);
        if (dev->irq_requested_type & KVM_DEV_IRQ_HOST_INTX) {
            r = assign_irq(dev);
            if (r < 0) {
                qdev_unplug(&dev->dev.qdev);
            }
        }
        dev = next;
    }
}

static void assigned_dev_update_msi(PCIDevice *pci_dev)
{
    struct kvm_assigned_irq assigned_irq_data;
    AssignedDevice *assigned_dev = DO_UPCAST(AssignedDevice, dev, pci_dev);
    uint8_t ctrl_byte = pci_get_byte(pci_dev->config + pci_dev->msi_cap +
                                     PCI_MSI_FLAGS);
    int r;

    memset(&assigned_irq_data, 0, sizeof assigned_irq_data);
    assigned_irq_data.assigned_dev_id = calc_assigned_dev_id(assigned_dev);

    /* Some guests gratuitously disable MSI even if they're not using it,
     * try to catch this by only deassigning irqs if the guest is using
     * MSI or intends to start. */
    if ((assigned_dev->irq_requested_type & KVM_DEV_IRQ_GUEST_MSI) ||
        (ctrl_byte & PCI_MSI_FLAGS_ENABLE)) {

        assigned_irq_data.flags = assigned_dev->irq_requested_type;
        free_dev_irq_entries(assigned_dev);
        r = kvm_deassign_irq(kvm_state, &assigned_irq_data);
        /* -ENXIO means no assigned irq */
        if (r && r != -ENXIO)
            perror("assigned_dev_update_msi: deassign irq");

        assigned_dev->irq_requested_type = 0;
    }

    if (ctrl_byte & PCI_MSI_FLAGS_ENABLE) {
        uint8_t *pos = pci_dev->config + pci_dev->msi_cap;

        assigned_dev->entry = g_malloc0(sizeof(*(assigned_dev->entry)));
        assigned_dev->entry->u.msi.address_lo =
            pci_get_long(pos + PCI_MSI_ADDRESS_LO);
        assigned_dev->entry->u.msi.address_hi = 0;
        assigned_dev->entry->u.msi.data = pci_get_word(pos + PCI_MSI_DATA_32);
        assigned_dev->entry->type = KVM_IRQ_ROUTING_MSI;
        r = kvm_get_irq_route_gsi();
        if (r < 0) {
            perror("assigned_dev_update_msi: kvm_get_irq_route_gsi");
            return;
        }
        assigned_dev->entry->gsi = r;

        kvm_add_routing_entry(assigned_dev->entry);
        if (kvm_commit_irq_routes() < 0) {
            perror("assigned_dev_update_msi: kvm_commit_irq_routes");
            assigned_dev->cap.state &= ~ASSIGNED_DEVICE_MSI_ENABLED;
            return;
        }
	assigned_dev->irq_entries_nr = 1;

        assigned_irq_data.guest_irq = assigned_dev->entry->gsi;
	assigned_irq_data.flags = KVM_DEV_IRQ_HOST_MSI | KVM_DEV_IRQ_GUEST_MSI;
        if (kvm_assign_irq(kvm_state, &assigned_irq_data) < 0) {
            perror("assigned_dev_enable_msi: assign irq");
        }

        assigned_dev->girq = -1;
        assigned_dev->irq_requested_type = assigned_irq_data.flags;
    } else {
        assign_irq(assigned_dev);
    }
}

static int assigned_dev_update_msix_mmio(PCIDevice *pci_dev)
{
    AssignedDevice *adev = DO_UPCAST(AssignedDevice, dev, pci_dev);
    uint16_t entries_nr = 0, entries_max_nr;
    int pos = 0, i, r = 0;
    uint32_t msg_addr, msg_upper_addr, msg_data, msg_ctrl;
    struct kvm_assigned_msix_nr msix_nr;
    struct kvm_assigned_msix_entry msix_entry;
    void *va = adev->msix_table_page;

    pos = pci_find_capability(pci_dev, PCI_CAP_ID_MSIX);

    entries_max_nr = *(uint16_t *)(pci_dev->config + pos + 2);
    entries_max_nr &= PCI_MSIX_FLAGS_QSIZE;
    entries_max_nr += 1;

    /* Get the usable entry number for allocating */
    for (i = 0; i < entries_max_nr; i++) {
        memcpy(&msg_ctrl, va + i * 16 + 12, 4);
        memcpy(&msg_data, va + i * 16 + 8, 4);
        /* Ignore unused entry even it's unmasked */
        if (msg_data == 0)
            continue;
        entries_nr ++;
    }

    if (entries_nr == 0) {
        fprintf(stderr, "MSI-X entry number is zero!\n");
        return -EINVAL;
    }
    msix_nr.assigned_dev_id = calc_assigned_dev_id(adev);
    msix_nr.entry_nr = entries_nr;
    r = kvm_assign_set_msix_nr(kvm_state, &msix_nr);
    if (r != 0) {
        fprintf(stderr, "fail to set MSI-X entry number for MSIX! %s\n",
			strerror(-r));
        return r;
    }

    free_dev_irq_entries(adev);
    adev->irq_entries_nr = entries_nr;
    adev->entry = g_malloc0(entries_nr * sizeof(*(adev->entry)));

    msix_entry.assigned_dev_id = msix_nr.assigned_dev_id;
    entries_nr = 0;
    for (i = 0; i < entries_max_nr; i++) {
        if (entries_nr >= msix_nr.entry_nr)
            break;
        memcpy(&msg_ctrl, va + i * 16 + 12, 4);
        memcpy(&msg_data, va + i * 16 + 8, 4);
        if (msg_data == 0)
            continue;

        memcpy(&msg_addr, va + i * 16, 4);
        memcpy(&msg_upper_addr, va + i * 16 + 4, 4);

        r = kvm_get_irq_route_gsi();
        if (r < 0)
            return r;

        adev->entry[entries_nr].gsi = r;
        adev->entry[entries_nr].type = KVM_IRQ_ROUTING_MSI;
        adev->entry[entries_nr].flags = 0;
        adev->entry[entries_nr].u.msi.address_lo = msg_addr;
        adev->entry[entries_nr].u.msi.address_hi = msg_upper_addr;
        adev->entry[entries_nr].u.msi.data = msg_data;
        DEBUG("MSI-X data 0x%x, MSI-X addr_lo 0x%x\n!", msg_data, msg_addr);
	kvm_add_routing_entry(&adev->entry[entries_nr]);

        msix_entry.gsi = adev->entry[entries_nr].gsi;
        msix_entry.entry = i;
        r = kvm_assign_set_msix_entry(kvm_state, &msix_entry);
        if (r) {
            fprintf(stderr, "fail to set MSI-X entry! %s\n", strerror(-r));
            break;
        }
        DEBUG("MSI-X entry gsi 0x%x, entry %d\n!",
                msix_entry.gsi, msix_entry.entry);
        entries_nr ++;
    }

    if (r == 0 && kvm_commit_irq_routes() < 0) {
	    perror("assigned_dev_update_msix_mmio: kvm_commit_irq_routes");
	    return -EINVAL;
    }

    return r;
}

static void assigned_dev_update_msix(PCIDevice *pci_dev)
{
    struct kvm_assigned_irq assigned_irq_data;
    AssignedDevice *assigned_dev = DO_UPCAST(AssignedDevice, dev, pci_dev);
    uint16_t ctrl_word = pci_get_word(pci_dev->config + pci_dev->msix_cap +
                                      PCI_MSIX_FLAGS);
    int r;

    memset(&assigned_irq_data, 0, sizeof assigned_irq_data);
    assigned_irq_data.assigned_dev_id = calc_assigned_dev_id(assigned_dev);

    /* Some guests gratuitously disable MSIX even if they're not using it,
     * try to catch this by only deassigning irqs if the guest is using
     * MSIX or intends to start. */
    if ((assigned_dev->irq_requested_type & KVM_DEV_IRQ_GUEST_MSIX) ||
        (ctrl_word & PCI_MSIX_FLAGS_ENABLE)) {

        assigned_irq_data.flags = assigned_dev->irq_requested_type;
        free_dev_irq_entries(assigned_dev);
        r = kvm_deassign_irq(kvm_state, &assigned_irq_data);
        /* -ENXIO means no assigned irq */
        if (r && r != -ENXIO)
            perror("assigned_dev_update_msix: deassign irq");

        assigned_dev->irq_requested_type = 0;
    }

    if (ctrl_word & PCI_MSIX_FLAGS_ENABLE) {
        assigned_irq_data.flags = KVM_DEV_IRQ_HOST_MSIX |
                                  KVM_DEV_IRQ_GUEST_MSIX;

        if (assigned_dev_update_msix_mmio(pci_dev) < 0) {
            perror("assigned_dev_update_msix_mmio");
            return;
        }
        if (kvm_assign_irq(kvm_state, &assigned_irq_data) < 0) {
            perror("assigned_dev_enable_msix: assign irq");
            return;
        }
        assigned_dev->girq = -1;
        assigned_dev->irq_requested_type = assigned_irq_data.flags;
    } else {
        assign_irq(assigned_dev);
    }
}

static uint32_t assigned_dev_pci_read_config(PCIDevice *pci_dev,
                                             uint32_t address, int len)
{
    AssignedDevice *assigned_dev = DO_UPCAST(AssignedDevice, dev, pci_dev);
    uint32_t virt_val = pci_default_read_config(pci_dev, address, len);
    uint32_t real_val, emulate_mask, full_emulation_mask;

    emulate_mask = 0;
    memcpy(&emulate_mask, assigned_dev->emulate_config_read + address, len);
    emulate_mask = le32_to_cpu(emulate_mask);

    full_emulation_mask = 0xffffffff >> (32 - len * 8);

    if (emulate_mask != full_emulation_mask) {
        real_val = assigned_dev_pci_read(pci_dev, address, len);
        return (virt_val & emulate_mask) | (real_val & ~emulate_mask);
    } else {
        return virt_val;
    }
}

static void assigned_dev_pci_write_config(PCIDevice *pci_dev, uint32_t address,
                                          uint32_t val, int len)
{
    AssignedDevice *assigned_dev = DO_UPCAST(AssignedDevice, dev, pci_dev);
    uint32_t emulate_mask, full_emulation_mask;

    pci_default_write_config(pci_dev, address, val, len);

    if (assigned_dev->cap.available & ASSIGNED_DEVICE_CAP_MSI) {
        if (range_covers_byte(address, len,
                              pci_dev->msi_cap + PCI_MSI_FLAGS)) {
            assigned_dev_update_msi(pci_dev);
        }
    }
    if (assigned_dev->cap.available & ASSIGNED_DEVICE_CAP_MSIX) {
        if (range_covers_byte(address, len,
                              pci_dev->msix_cap + PCI_MSIX_FLAGS + 1)) {
            assigned_dev_update_msix(pci_dev);
        }
    }

    emulate_mask = 0;
    memcpy(&emulate_mask, assigned_dev->emulate_config_write + address, len);
    emulate_mask = le32_to_cpu(emulate_mask);

    full_emulation_mask = 0xffffffff >> (32 - len * 8);

    if (emulate_mask != full_emulation_mask) {
        if (emulate_mask) {
            val &= ~emulate_mask;
            val |= assigned_dev_pci_read(pci_dev, address, len) & emulate_mask;
        }
        assigned_dev_pci_write(pci_dev, address, val, len);
    }
}

static void assigned_dev_setup_cap_read(AssignedDevice *dev, uint32_t offset,
                                        uint32_t len)
{
    assigned_dev_direct_config_read(dev, offset, len);
    assigned_dev_emulate_config_read(dev, offset + PCI_CAP_LIST_NEXT, 1);
}

static int assigned_device_pci_cap_init(PCIDevice *pci_dev)
{
    AssignedDevice *dev = DO_UPCAST(AssignedDevice, dev, pci_dev);
    PCIRegion *pci_region = dev->real_device.regions;
    int ret, pos;

    /* Clear initial capabilities pointer and status copied from hw */
    pci_set_byte(pci_dev->config + PCI_CAPABILITY_LIST, 0);
    pci_set_word(pci_dev->config + PCI_STATUS,
                 pci_get_word(pci_dev->config + PCI_STATUS) &
                 ~PCI_STATUS_CAP_LIST);

    /* Expose MSI capability
     * MSI capability is the 1st capability in capability config */
    pos = pci_find_cap_offset(pci_dev, PCI_CAP_ID_MSI, 0);
    if (pos != 0 && kvm_check_extension(kvm_state, KVM_CAP_ASSIGN_DEV_IRQ)) {
        dev->cap.available |= ASSIGNED_DEVICE_CAP_MSI;
        /* Only 32-bit/no-mask currently supported */
        if ((ret = pci_add_capability(pci_dev, PCI_CAP_ID_MSI, pos, 10)) < 0) {
            return ret;
        }
        pci_dev->msi_cap = pos;

        pci_set_word(pci_dev->config + pos + PCI_MSI_FLAGS,
                     pci_get_word(pci_dev->config + pos + PCI_MSI_FLAGS) &
                     PCI_MSI_FLAGS_QMASK);
        pci_set_long(pci_dev->config + pos + PCI_MSI_ADDRESS_LO, 0);
        pci_set_word(pci_dev->config + pos + PCI_MSI_DATA_32, 0);

        /* Set writable fields */
        pci_set_word(pci_dev->wmask + pos + PCI_MSI_FLAGS,
                     PCI_MSI_FLAGS_QSIZE | PCI_MSI_FLAGS_ENABLE);
        pci_set_long(pci_dev->wmask + pos + PCI_MSI_ADDRESS_LO, 0xfffffffc);
        pci_set_word(pci_dev->wmask + pos + PCI_MSI_DATA_32, 0xffff);
    }
    /* Expose MSI-X capability */
    pos = pci_find_cap_offset(pci_dev, PCI_CAP_ID_MSIX, 0);
    /* Would really like to test kvm_check_extension(, KVM_CAP_DEVICE_MSIX),
     * but the kernel doesn't expose it.  Instead do a dummy call to
     * KVM_ASSIGN_SET_MSIX_NR to see if it exists. */
    if (pos != 0 && kvm_assign_set_msix_nr(kvm_state, NULL) == -EFAULT) {
        int bar_nr;
        uint32_t msix_table_entry;

        dev->cap.available |= ASSIGNED_DEVICE_CAP_MSIX;
        if ((ret = pci_add_capability(pci_dev, PCI_CAP_ID_MSIX, pos, 12)) < 0) {
            return ret;
        }
        pci_dev->msix_cap = pos;

        pci_set_word(pci_dev->config + pos + PCI_MSIX_FLAGS,
                     pci_get_word(pci_dev->config + pos + PCI_MSIX_FLAGS) &
                     PCI_MSIX_FLAGS_QSIZE);

        /* Only enable and function mask bits are writable */
        pci_set_word(pci_dev->wmask + pos + PCI_MSIX_FLAGS,
                     PCI_MSIX_FLAGS_ENABLE | PCI_MSIX_FLAGS_MASKALL);

        msix_table_entry = pci_get_long(pci_dev->config + pos + PCI_MSIX_TABLE);
        bar_nr = msix_table_entry & PCI_MSIX_FLAGS_BIRMASK;
        msix_table_entry &= ~PCI_MSIX_FLAGS_BIRMASK;
        dev->msix_table_addr = pci_region[bar_nr].base_addr + msix_table_entry;
    }

    /* Minimal PM support, nothing writable, device appears to NAK changes */
    if ((pos = pci_find_cap_offset(pci_dev, PCI_CAP_ID_PM, 0))) {
        uint16_t pmc;
        if ((ret = pci_add_capability(pci_dev, PCI_CAP_ID_PM, pos,
                                      PCI_PM_SIZEOF)) < 0) {
            return ret;
        }

        assigned_dev_setup_cap_read(dev, pos, PCI_PM_SIZEOF);

        pmc = pci_get_word(pci_dev->config + pos + PCI_CAP_FLAGS);
        pmc &= (PCI_PM_CAP_VER_MASK | PCI_PM_CAP_DSI);
        pci_set_word(pci_dev->config + pos + PCI_CAP_FLAGS, pmc);

        /* assign_device will bring the device up to D0, so we don't need
         * to worry about doing that ourselves here. */
        pci_set_word(pci_dev->config + pos + PCI_PM_CTRL,
                     PCI_PM_CTRL_NO_SOFT_RESET);

        pci_set_byte(pci_dev->config + pos + PCI_PM_PPB_EXTENSIONS, 0);
        pci_set_byte(pci_dev->config + pos + PCI_PM_DATA_REGISTER, 0);
    }

    if ((pos = pci_find_cap_offset(pci_dev, PCI_CAP_ID_EXP, 0))) {
        uint8_t version, size = 0;
        uint16_t type, devctl, lnksta;
        uint32_t devcap, lnkcap;

        version = pci_get_byte(pci_dev->config + pos + PCI_EXP_FLAGS);
        version &= PCI_EXP_FLAGS_VERS;
        if (version == 1) {
            size = 0x14;
        } else if (version == 2) {
            /*
             * Check for non-std size, accept reduced size to 0x34,
             * which is what bcm5761 implemented, violating the
             * PCIe v3.0 spec that regs should exist and be read as 0,
             * not optionally provided and shorten the struct size.
             */
            size = MIN(0x3c, PCI_CONFIG_SPACE_SIZE - pos);
            if (size < 0x34) {
                fprintf(stderr,
                        "%s: Invalid size PCIe cap-id 0x%x \n",
                        __func__, PCI_CAP_ID_EXP);
                return -EINVAL;
            } else if (size != 0x3c) {
                fprintf(stderr,
                        "WARNING, %s: PCIe cap-id 0x%x has "
                        "non-standard size 0x%x; std size should be 0x3c \n",
                         __func__, PCI_CAP_ID_EXP, size);
            }
        } else if (version == 0) {
            uint16_t vid, did;
            vid = pci_get_word(pci_dev->config + PCI_VENDOR_ID);
            did = pci_get_word(pci_dev->config + PCI_DEVICE_ID);
            if (vid == PCI_VENDOR_ID_INTEL && did == 0x10ed) {
                /*
                 * quirk for Intel 82599 VF with invalid PCIe capability
                 * version, should really be version 2 (same as PF)
                 */
                size = 0x3c;
            }
        }

        if (size == 0) {
            fprintf(stderr,
                    "%s: Unsupported PCI express capability version %d\n",
                    __func__, version);
            return -EINVAL;
        }

        if ((ret = pci_add_capability(pci_dev, PCI_CAP_ID_EXP,
                                      pos, size)) < 0) {
            return ret;
        }

        assigned_dev_setup_cap_read(dev, pos, size);

        type = pci_get_word(pci_dev->config + pos + PCI_EXP_FLAGS);
        type = (type & PCI_EXP_FLAGS_TYPE) >> 4;
        if (type != PCI_EXP_TYPE_ENDPOINT &&
            type != PCI_EXP_TYPE_LEG_END && type != PCI_EXP_TYPE_RC_END) {
            fprintf(stderr,
                    "Device assignment only supports endpoint assignment, "
                    "device type %d\n", type);
            return -EINVAL;
        }

        /* capabilities, pass existing read-only copy
         * PCI_EXP_FLAGS_IRQ: updated by hardware, should be direct read */

        /* device capabilities: hide FLR */
        devcap = pci_get_long(pci_dev->config + pos + PCI_EXP_DEVCAP);
        devcap &= ~PCI_EXP_DEVCAP_FLR;
        pci_set_long(pci_dev->config + pos + PCI_EXP_DEVCAP, devcap);

        /* device control: clear all error reporting enable bits, leaving
         *                 only a few host values.  Note, these are
         *                 all writable, but not passed to hw.
         */
        devctl = pci_get_word(pci_dev->config + pos + PCI_EXP_DEVCTL);
        devctl = (devctl & (PCI_EXP_DEVCTL_READRQ | PCI_EXP_DEVCTL_PAYLOAD)) |
                  PCI_EXP_DEVCTL_RELAX_EN | PCI_EXP_DEVCTL_NOSNOOP_EN;
        pci_set_word(pci_dev->config + pos + PCI_EXP_DEVCTL, devctl);
        devctl = PCI_EXP_DEVCTL_BCR_FLR | PCI_EXP_DEVCTL_AUX_PME;
        pci_set_word(pci_dev->wmask + pos + PCI_EXP_DEVCTL, ~devctl);

        /* Clear device status */
        pci_set_word(pci_dev->config + pos + PCI_EXP_DEVSTA, 0);

        /* Link capabilities, expose links and latencues, clear reporting */
        lnkcap = pci_get_long(pci_dev->config + pos + PCI_EXP_LNKCAP);
        lnkcap &= (PCI_EXP_LNKCAP_SLS | PCI_EXP_LNKCAP_MLW |
                   PCI_EXP_LNKCAP_ASPMS | PCI_EXP_LNKCAP_L0SEL |
                   PCI_EXP_LNKCAP_L1EL);
        pci_set_long(pci_dev->config + pos + PCI_EXP_LNKCAP, lnkcap);

        /* Link control, pass existing read-only copy.  Should be writable? */

        /* Link status, only expose current speed and width */
        lnksta = pci_get_word(pci_dev->config + pos + PCI_EXP_LNKSTA);
        lnksta &= (PCI_EXP_LNKSTA_CLS | PCI_EXP_LNKSTA_NLW);
        pci_set_word(pci_dev->config + pos + PCI_EXP_LNKSTA, lnksta);

        if (version >= 2) {
            /* Slot capabilities, control, status - not needed for endpoints */
            pci_set_long(pci_dev->config + pos + PCI_EXP_SLTCAP, 0);
            pci_set_word(pci_dev->config + pos + PCI_EXP_SLTCTL, 0);
            pci_set_word(pci_dev->config + pos + PCI_EXP_SLTSTA, 0);

            /* Root control, capabilities, status - not needed for endpoints */
            pci_set_word(pci_dev->config + pos + PCI_EXP_RTCTL, 0);
            pci_set_word(pci_dev->config + pos + PCI_EXP_RTCAP, 0);
            pci_set_long(pci_dev->config + pos + PCI_EXP_RTSTA, 0);

            /* Device capabilities/control 2, pass existing read-only copy */
            /* Link control 2, pass existing read-only copy */
        }
    }

    if ((pos = pci_find_cap_offset(pci_dev, PCI_CAP_ID_PCIX, 0))) {
        uint16_t cmd;
        uint32_t status;

        /* Only expose the minimum, 8 byte capability */
        if ((ret = pci_add_capability(pci_dev, PCI_CAP_ID_PCIX, pos, 8)) < 0) {
            return ret;
        }

        assigned_dev_setup_cap_read(dev, pos, 8);

        /* Command register, clear upper bits, including extended modes */
        cmd = pci_get_word(pci_dev->config + pos + PCI_X_CMD);
        cmd &= (PCI_X_CMD_DPERR_E | PCI_X_CMD_ERO | PCI_X_CMD_MAX_READ |
                PCI_X_CMD_MAX_SPLIT);
        pci_set_word(pci_dev->config + pos + PCI_X_CMD, cmd);

        /* Status register, update with emulated PCI bus location, clear
         * error bits, leave the rest. */
        status = pci_get_long(pci_dev->config + pos + PCI_X_STATUS);
        status &= ~(PCI_X_STATUS_BUS | PCI_X_STATUS_DEVFN);
        status |= (pci_bus_num(pci_dev->bus) << 8) | pci_dev->devfn;
        status &= ~(PCI_X_STATUS_SPL_DISC | PCI_X_STATUS_UNX_SPL |
                    PCI_X_STATUS_SPL_ERR);
        pci_set_long(pci_dev->config + pos + PCI_X_STATUS, status);
    }

    if ((pos = pci_find_cap_offset(pci_dev, PCI_CAP_ID_VPD, 0))) {
        /* Direct R/W passthrough */
        if ((ret = pci_add_capability(pci_dev, PCI_CAP_ID_VPD, pos, 8)) < 0) {
            return ret;
        }

        assigned_dev_setup_cap_read(dev, pos, 8);

        /* direct write for cap content */
        assigned_dev_direct_config_write(dev, pos + 2, 6);
    }

    /* Devices can have multiple vendor capabilities, get them all */
    for (pos = 0; (pos = pci_find_cap_offset(pci_dev, PCI_CAP_ID_VNDR, pos));
        pos += PCI_CAP_LIST_NEXT) {
        uint8_t len = pci_get_byte(pci_dev->config + pos + PCI_CAP_FLAGS);
        /* Direct R/W passthrough */
        if ((ret = pci_add_capability(pci_dev, PCI_CAP_ID_VNDR,
                                      pos, len)) < 0) {
            return ret;
        }

        assigned_dev_setup_cap_read(dev, pos, len);

        /* direct write for cap content */
        assigned_dev_direct_config_write(dev, pos + 2, len - 2);
    }

    /* If real and virtual capability list status bits differ, virtualize the
     * access. */
    if ((pci_get_word(pci_dev->config + PCI_STATUS) & PCI_STATUS_CAP_LIST) !=
        (assigned_dev_pci_read_byte(pci_dev, PCI_STATUS) &
         PCI_STATUS_CAP_LIST)) {
        dev->emulate_config_read[PCI_STATUS] |= PCI_STATUS_CAP_LIST;
    }

    return 0;
}

static uint32_t msix_mmio_readl(void *opaque, target_phys_addr_t addr)
{
    AssignedDevice *adev = opaque;
    unsigned int offset = addr & 0xfff;
    void *page = adev->msix_table_page;
    uint32_t val = 0;

    memcpy(&val, (void *)((char *)page + offset), 4);

    return val;
}

static uint32_t msix_mmio_readb(void *opaque, target_phys_addr_t addr)
{
    return ((msix_mmio_readl(opaque, addr & ~3)) >>
            (8 * (addr & 3))) & 0xff;
}

static uint32_t msix_mmio_readw(void *opaque, target_phys_addr_t addr)
{
    return ((msix_mmio_readl(opaque, addr & ~3)) >>
            (8 * (addr & 3))) & 0xffff;
}

static void msix_mmio_writel(void *opaque,
                             target_phys_addr_t addr, uint32_t val)
{
    AssignedDevice *adev = opaque;
    unsigned int offset = addr & 0xfff;
    void *page = adev->msix_table_page;

    DEBUG("write to MSI-X entry table mmio offset 0x%lx, val 0x%x\n",
		    addr, val);
    memcpy((void *)((char *)page + offset), &val, 4);
}

static void msix_mmio_writew(void *opaque,
                             target_phys_addr_t addr, uint32_t val)
{
    msix_mmio_writel(opaque, addr & ~3,
                     (val & 0xffff) << (8*(addr & 3)));
}

static void msix_mmio_writeb(void *opaque,
                             target_phys_addr_t addr, uint32_t val)
{
    msix_mmio_writel(opaque, addr & ~3,
                     (val & 0xff) << (8*(addr & 3)));
}

static const MemoryRegionOps msix_mmio_ops = {
    .old_mmio = {
        .read = { msix_mmio_readb, msix_mmio_readw, msix_mmio_readl, },
        .write = { msix_mmio_writeb, msix_mmio_writew, msix_mmio_writel, },
    },
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static int assigned_dev_register_msix_mmio(AssignedDevice *dev)
{
    dev->msix_table_page = mmap(NULL, 0x1000,
                                PROT_READ|PROT_WRITE,
                                MAP_ANONYMOUS|MAP_PRIVATE, 0, 0);
    if (dev->msix_table_page == MAP_FAILED) {
        fprintf(stderr, "fail allocate msix_table_page! %s\n",
                strerror(errno));
        return -EFAULT;
    }
    memset(dev->msix_table_page, 0, 0x1000);
    memory_region_init_io(&dev->mmio, &msix_mmio_ops, dev,
                          "assigned-dev-msix", MSIX_PAGE_SIZE);
    return 0;
}

static void assigned_dev_unregister_msix_mmio(AssignedDevice *dev)
{
    if (!dev->msix_table_page)
        return;

    memory_region_destroy(&dev->mmio);

    if (munmap(dev->msix_table_page, 0x1000) == -1) {
        fprintf(stderr, "error unmapping msix_table_page! %s\n",
                strerror(errno));
    }
    dev->msix_table_page = NULL;
}

static const VMStateDescription vmstate_assigned_device = {
    .name = "pci-assign",
    .unmigratable = 1,
};

static void reset_assigned_device(DeviceState *dev)
{
    PCIDevice *pci_dev = DO_UPCAST(PCIDevice, qdev, dev);
    AssignedDevice *adev = DO_UPCAST(AssignedDevice, dev, pci_dev);
    char reset_file[64];
    const char reset[] = "1";
    int fd, ret;

    snprintf(reset_file, sizeof(reset_file),
             "/sys/bus/pci/devices/%04x:%02x:%02x.%01x/reset",
             adev->host.seg, adev->host.bus, adev->host.dev, adev->host.func);

    /*
     * Issue a device reset via pci-sysfs.  Note that we use write(2) here
     * and ignore the return value because some kernels have a bug that
     * returns 0 rather than bytes written on success, sending us into an
     * infinite retry loop using other write mechanisms.
     */
    fd = open(reset_file, O_WRONLY);
    if (fd != -1) {
        ret = write(fd, reset, strlen(reset));
        (void)ret;
        close(fd);
    }

    /*
     * When a 0 is written to the command register, the device is logically
     * disconnected from the PCI bus. This avoids further DMA transfers.
     */
    assigned_dev_pci_write_config(pci_dev, PCI_COMMAND, 0, 2);
}

static int assigned_initfn(struct PCIDevice *pci_dev)
{
    AssignedDevice *dev = DO_UPCAST(AssignedDevice, dev, pci_dev);
    uint8_t e_intx;
    int r;

    if (!kvm_enabled()) {
        error_report("pci-assign: error: requires KVM support");
        return -1;
    }

    if (!dev->host.seg && !dev->host.bus && !dev->host.dev && !dev->host.func) {
        error_report("pci-assign: error: no host device specified");
        return -1;
    }

    /*
     * Set up basic config space access control. Will be further refined during
     * device initialization.
     */
    assigned_dev_emulate_config_read(dev, 0, PCI_CONFIG_SPACE_SIZE);
    assigned_dev_direct_config_read(dev, PCI_COMMAND, 2);
    assigned_dev_direct_config_read(dev, PCI_STATUS, 2);
    assigned_dev_direct_config_read(dev, PCI_REVISION_ID, 1);
    assigned_dev_direct_config_read(dev, PCI_CLASS_PROG, 3);
    assigned_dev_direct_config_read(dev, PCI_CACHE_LINE_SIZE, 1);
    assigned_dev_direct_config_read(dev, PCI_LATENCY_TIMER, 1);
    assigned_dev_direct_config_read(dev, PCI_HEADER_TYPE, 1);
    assigned_dev_direct_config_read(dev, PCI_BIST, 1);
    assigned_dev_direct_config_read(dev, PCI_CARDBUS_CIS, 4);
    assigned_dev_direct_config_read(dev, PCI_SUBSYSTEM_VENDOR_ID, 2);
    assigned_dev_direct_config_read(dev, PCI_SUBSYSTEM_ID, 2);
    assigned_dev_direct_config_read(dev, PCI_CAPABILITY_LIST + 1, 7);
    assigned_dev_direct_config_read(dev, PCI_MIN_GNT, 1);
    assigned_dev_direct_config_read(dev, PCI_MAX_LAT, 1);
    memcpy(dev->emulate_config_write, dev->emulate_config_read,
           sizeof(dev->emulate_config_read));

    if (get_real_device(dev, dev->host.seg, dev->host.bus,
                        dev->host.dev, dev->host.func)) {
        error_report("pci-assign: Error: Couldn't get real device (%s)!",
                     dev->dev.qdev.id);
        goto out;
    }

    if (assigned_device_pci_cap_init(pci_dev) < 0) {
        goto out;
    }

    /* intercept MSI-X entry page in the MMIO */
    if (dev->cap.available & ASSIGNED_DEVICE_CAP_MSIX) {
        if (assigned_dev_register_msix_mmio(dev)) {
            goto out;
        }
    }

    /* handle real device's MMIO/PIO BARs */
    if (assigned_dev_register_regions(dev->real_device.regions,
                                      dev->real_device.region_number,
                                      dev))
        goto out;

    /* handle interrupt routing */
    e_intx = dev->dev.config[0x3d] - 1;
    dev->intpin = e_intx;
    dev->run = 0;
    dev->girq = -1;
    dev->h_segnr = dev->host.seg;
    dev->h_busnr = dev->host.bus;
    dev->h_devfn = PCI_DEVFN(dev->host.dev, dev->host.func);

    /* assign device to guest */
    r = assign_device(dev);
    if (r < 0)
        goto out;

    /* assign irq for the device */
    r = assign_irq(dev);
    if (r < 0)
        goto assigned_out;

    assigned_dev_load_option_rom(dev);
    QLIST_INSERT_HEAD(&devs, dev, next);

    add_boot_device_path(dev->bootindex, &pci_dev->qdev, NULL);

    return 0;

assigned_out:
    deassign_device(dev);
out:
    free_assigned_device(dev);
    return -1;
}

static int assigned_exitfn(struct PCIDevice *pci_dev)
{
    AssignedDevice *dev = DO_UPCAST(AssignedDevice, dev, pci_dev);

    QLIST_REMOVE(dev, next);
    deassign_device(dev);
    free_assigned_device(dev);
    return 0;
}

static int parse_hostaddr(DeviceState *dev, Property *prop, const char *str)
{
    PCIHostDevice *ptr = qdev_get_prop_ptr(dev, prop);
    int rc;

    rc = pci_parse_host_devaddr(str, &ptr->seg, &ptr->bus, &ptr->dev, &ptr->func);
    if (rc != 0)
        return -1;
    return 0;
}

static int print_hostaddr(DeviceState *dev, Property *prop, char *dest, size_t len)
{
    PCIHostDevice *ptr = qdev_get_prop_ptr(dev, prop);

    return snprintf(dest, len, "%02x:%02x.%x", ptr->bus, ptr->dev, ptr->func);
}

PropertyInfo qdev_prop_hostaddr = {
    .name  = "pci-hostaddr",
    .type  = -1,
    .size  = sizeof(PCIHostDevice),
    .parse = parse_hostaddr,
    .print = print_hostaddr,
};

static PCIDeviceInfo assign_info = {
    .qdev.name    = "pci-assign",
    .qdev.desc    = "pass through host pci devices to the guest",
    .qdev.size    = sizeof(AssignedDevice),
    .qdev.vmsd    = &vmstate_assigned_device,
    .qdev.reset   = reset_assigned_device,
    .init         = assigned_initfn,
    .exit         = assigned_exitfn,
    .config_read  = assigned_dev_pci_read_config,
    .config_write = assigned_dev_pci_write_config,
    .qdev.props   = (Property[]) {
        DEFINE_PROP("host", AssignedDevice, host, qdev_prop_hostaddr, PCIHostDevice),
        DEFINE_PROP_BIT("iommu", AssignedDevice, features,
                        ASSIGNED_DEVICE_USE_IOMMU_BIT, true),
        DEFINE_PROP_BIT("prefer_msi", AssignedDevice, features,
                        ASSIGNED_DEVICE_PREFER_MSI_BIT, true),
        DEFINE_PROP_INT32("bootindex", AssignedDevice, bootindex, -1),
        DEFINE_PROP_STRING("configfd", AssignedDevice, configfd_name),
        DEFINE_PROP_END_OF_LIST(),
    },
};

static void assign_register_devices(void)
{
    pci_qdev_register(&assign_info);
}

device_init(assign_register_devices)

/*
 * Scan the assigned devices for the devices that have an option ROM, and then
 * load the corresponding ROM data to RAM. If an error occurs while loading an
 * option ROM, we just ignore that option ROM and continue with the next one.
 */
static void assigned_dev_load_option_rom(AssignedDevice *dev)
{
    char name[32], rom_file[64];
    FILE *fp;
    uint8_t val;
    struct stat st;
    void *ptr;

    /* If loading ROM from file, pci handles it */
    if (dev->dev.romfile || !dev->dev.rom_bar)
        return;

    snprintf(rom_file, sizeof(rom_file),
             "/sys/bus/pci/devices/%04x:%02x:%02x.%01x/rom",
             dev->host.seg, dev->host.bus, dev->host.dev, dev->host.func);

    if (stat(rom_file, &st)) {
        return;
    }

    if (access(rom_file, F_OK)) {
        fprintf(stderr, "pci-assign: Insufficient privileges for %s\n",
                rom_file);
        return;
    }

    /* Write "1" to the ROM file to enable it */
    fp = fopen(rom_file, "r+");
    if (fp == NULL) {
        return;
    }
    val = 1;
    if (fwrite(&val, 1, 1, fp) != 1) {
        goto close_rom;
    }
    fseek(fp, 0, SEEK_SET);

    snprintf(name, sizeof(name), "%s.rom", dev->dev.qdev.info->name);
    memory_region_init_ram(&dev->dev.rom, &dev->dev.qdev, name, st.st_size);
    ptr = memory_region_get_ram_ptr(&dev->dev.rom);
    memset(ptr, 0xff, st.st_size);

    if (!fread(ptr, 1, st.st_size, fp)) {
        fprintf(stderr, "pci-assign: Cannot read from host %s\n"
                "\tDevice option ROM contents are probably invalid "
                "(check dmesg).\n\tSkip option ROM probe with rombar=0, "
                "or load from file with romfile=\n", rom_file);
        memory_region_destroy(&dev->dev.rom);
        goto close_rom;
    }

    pci_register_bar(&dev->dev, PCI_ROM_SLOT, 0, &dev->dev.rom);
    dev->dev.has_rom = true;
close_rom:
    /* Write "0" to disable ROM */
    fseek(fp, 0, SEEK_SET);
    val = 0;
    if (!fwrite(&val, 1, 1, fp)) {
        DEBUG("%s\n", "Failed to disable pci-sysfs rom file");
    }
    fclose(fp);
}
