/* SPDX-License-Identifier: MIT */
/*
 * Description: Helpers for NVMe uring passthrough commands
 */
#ifndef LIBURING_NVME_H
#define LIBURING_NVME_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/ioctl.h>
#include <linux/nvme_ioctl.h>

/*
 * If the uapi headers installed on the system lacks nvme uring command
 * support, use the local version to prevent compilation issues.
 */
#ifndef CONFIG_HAVE_NVME_URING
struct nvme_uring_cmd {
	__u8	opcode;
	__u8	flags;
	__u16	rsvd1;
	__u32	nsid;
	__u32	cdw2;
	__u32	cdw3;
	__u64	metadata;
	__u64	addr;
	__u32	metadata_len;
	__u32	data_len;
	__u32	cdw10;
	__u32	cdw11;
	__u32	cdw12;
	__u32	cdw13;
	__u32	cdw14;
	__u32	cdw15;
	__u32	timeout_ms;
	__u32   rsvd2;
};

#define NVME_URING_CMD_IO	_IOWR('N', 0x80, struct nvme_uring_cmd)
#define NVME_URING_CMD_IO_VEC	_IOWR('N', 0x81, struct nvme_uring_cmd)
#endif /* CONFIG_HAVE_NVME_URING */

#define NVME_DEFAULT_IOCTL_TIMEOUT 0
#define NVME_IDENTIFY_DATA_SIZE 4096
#define NVME_IDENTIFY_CSI_SHIFT 24
#define NVME_IDENTIFY_CNS_NS 0
#define NVME_CSI_NVM 0

enum nvme_admin_opcode {
	nvme_admin_identify		= 0x06,
};

enum nvme_io_opcode {
	nvme_cmd_write			= 0x01,
	nvme_cmd_read			= 0x02,
};

static int nsid;
static __u32 lba_shift;

struct nvme_lbaf {
	__le16			ms;
	__u8			ds;
	__u8			rp;
};

struct nvme_id_ns {
	__le64			nsze;
	__le64			ncap;
	__le64			nuse;
	__u8			nsfeat;
	__u8			nlbaf;
	__u8			flbas;
	__u8			mc;
	__u8			dpc;
	__u8			dps;
	__u8			nmic;
	__u8			rescap;
	__u8			fpi;
	__u8			dlfeat;
	__le16			nawun;
	__le16			nawupf;
	__le16			nacwu;
	__le16			nabsn;
	__le16			nabo;
	__le16			nabspf;
	__le16			noiob;
	__u8			nvmcap[16];
	__le16			npwg;
	__le16			npwa;
	__le16			npdg;
	__le16			npda;
	__le16			nows;
	__le16			mssrl;
	__le32			mcl;
	__u8			msrc;
	__u8			rsvd81[11];
	__le32			anagrpid;
	__u8			rsvd96[3];
	__u8			nsattr;
	__le16			nvmsetid;
	__le16			endgid;
	__u8			nguid[16];
	__u8			eui64[8];
	struct nvme_lbaf	lbaf[16];
	__u8			rsvd192[192];
	__u8			vs[3712];
};

static inline int ilog2(uint32_t i)
{
	int log = -1;

	while (i) {
		i >>= 1;
		log++;
	}
	return log;
}

__attribute__((__unused__))
static int nvme_get_info(const char *file)
{
	struct nvme_id_ns ns;
	int fd, err;
	__u32 lba_size;

	fd = open(file, O_RDONLY);
	if (fd < 0) {
		perror("file open");
		return -errno;
	}

	nsid = ioctl(fd, NVME_IOCTL_ID);
	if (nsid < 0) {
		close(fd);
		return -errno;
	}

	struct nvme_passthru_cmd cmd = {
		.opcode         = nvme_admin_identify,
		.nsid           = nsid,
		.addr           = (__u64)(uintptr_t)&ns,
		.data_len       = NVME_IDENTIFY_DATA_SIZE,
		.cdw10          = NVME_IDENTIFY_CNS_NS,
		.cdw11          = NVME_CSI_NVM << NVME_IDENTIFY_CSI_SHIFT,
		.timeout_ms     = NVME_DEFAULT_IOCTL_TIMEOUT,
	};

	err = ioctl(fd, NVME_IOCTL_ADMIN_CMD, &cmd);
	if (err) {
		close(fd);
		return err;
	}

	lba_size = 1 << ns.lbaf[(ns.flbas & 0x0f)].ds;
	lba_shift = ilog2(lba_size);

	close(fd);
	return 0;
}

#ifdef __cplusplus
}
#endif

#endif
