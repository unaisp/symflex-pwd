#ifndef _LINUX_VIRTIO_BLK_H
#define _LINUX_VIRTIO_BLK_H
/* This header is BSD licensed so anyone can use the definitions to implement
 * compatible drivers/servers.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of IBM nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL IBM OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE. */
#include "standard-headers/linux/types.h"
#include "standard-headers/linux/virtio_ids.h"
#include "standard-headers/linux/virtio_config.h"
#include "standard-headers/linux/virtio_types.h"

/* Feature bits */
#define VIRTIO_VSSD_F_SIZE_MAX	1	/* Indicates maximum segment size */
#define VIRTIO_VSSD_F_SEG_MAX	2	/* Indicates maximum # of segments */
#define VIRTIO_VSSD_F_GEOMETRY	4	/* Legacy geometry available  */
#define VIRTIO_VSSD_F_RO		5	/* Disk is read-only */
#define VIRTIO_VSSD_F_BLK_SIZE	6	/* Block size of disk is available*/
#define VIRTIO_VSSD_F_TOPOLOGY	10	/* Topology information is available */
#define VIRTIO_VSSD_F_MQ		12	/* support more than one vq */

/* Legacy feature bits */
#ifndef VIRTIO_BLK_NO_LEGACY
#define VIRTIO_VSSD_F_BARRIER	0	/* Does host support barriers? */
#define VIRTIO_VSSD_F_SCSI	7	/* Supports scsi command passthru */
#define VIRTIO_VSSD_F_FLUSH	9	/* Flush command supported */
#define VIRTIO_VSSD_F_CONFIG_WCE	11	/* Writeback mode available in config */
/* Old (deprecated) name for VIRTIO_VSSD_F_FLUSH. */
#define VIRTIO_VSSD_F_WCE VIRTIO_VSSD_F_FLUSH
#endif /* !VIRTIO_BLK_NO_LEGACY */

#define VIRTIO_VSSD_ID_BYTES	20	/* ID string length */

#define SECTORS_IN_A_GB 2097152

struct virtio_vssd_config {
	/* The capacity (in 512-byte sectors). */
	uint64_t capacity;
	/* The maximum segment size (if VIRTIO_VSSD_F_SIZE_MAX) */
	uint32_t size_max;
	/* The maximum number of segments (if VIRTIO_VSSD_F_SEG_MAX) */
	uint32_t seg_max;
	/* geometry of the device (if VIRTIO_VSSD_F_GEOMETRY) */
	struct virtio_vssd_geometry {
		uint16_t cylinders;
		uint8_t heads;
		uint8_t sectors;
	} geometry;

	/* block size of device (if VIRTIO_VSSD_F_BLK_SIZE) */
	uint32_t blk_size;

	/* the next 4 entries are guarded by VIRTIO_VSSD_F_TOPOLOGY  */
	/* exponent for physical block per logical block. */
	uint8_t physical_block_exp;
	/* alignment offset in logical blocks. */
	uint8_t alignment_offset;
	/* minimum I/O size without performance penalty in logical blocks. */
	uint16_t min_io_size;
	/* optimal sustained I/O size in logical blocks. */
	uint32_t opt_io_size;

	/* writeback mode (if VIRTIO_VSSD_F_CONFIG_WCE) */
	uint8_t wce;
	uint8_t unused;

	/* number of vqs, only available when VIRTIO_VSSD_F_MQ is set */
	uint16_t num_queues;

	/*New Varialble unais*/
	uint64_t current_capacity;
	//uint32_t bitmap[1* SECTORS_IN_A_GB/32];
	//uint32_t bitmap[8192];
	int32_t command;

	// struct vssd_bitmap
	// {
	uint32_t bitmap_offset;
	uint32_t bitmap_reading;
	uint32_t bitmap_value;		//400 Bytes.	3200 Blocks
	uint32_t current_request_id;
	// } bitmap;


} QEMU_PACKED;

/*
 * Command types
 *
 * Usage is a bit tricky as some bits are used as flags and some are not.
 *
 * Rules:
 *   VIRTIO_VSSD_T_OUT may be combined with VIRTIO_VSSD_T_SCSI_CMD or
 *   VIRTIO_VSSD_T_BARRIER.  VIRTIO_VSSD_T_FLUSH is a command of its own
 *   and may not be combined with any of the other flags.
 */

/* These two define direction. */
#define VIRTIO_VSSD_T_IN		0
#define VIRTIO_VSSD_T_OUT	1

#ifndef VIRTIO_VSSD_NO_LEGACY
/* This bit says it's a scsi command, not an actual read or write. */
#define VIRTIO_VSSD_T_SCSI_CMD	2
#endif /* VIRTIO_BLK_NO_LEGACY */

/* Cache flush command */
#define VIRTIO_VSSD_T_FLUSH	4

/* Get device ID command */
#define VIRTIO_VSSD_T_GET_ID    8

#ifndef VIRTIO_VSSD_NO_LEGACY
/* Barrier before this op. */
#define VIRTIO_VSSD_T_BARRIER	0x80000000
#endif /* !VIRTIO_BLK_NO_LEGACY */





#ifndef VIRTIO_VSSD_NO_LEGACY
struct virtio_scsi_inhdr {
	__virtio32 errors;
	__virtio32 data_len;
	__virtio32 sense_len;
	__virtio32 residual;
};
#endif /* !VIRTIO_BLK_NO_LEGACY */

/* And this is the final byte of the write scatter-gather list. */
#define VIRTIO_VSSD_S_OK		0
#define VIRTIO_VSSD_S_IOERR	1
#define VIRTIO_VSSD_S_UNSUPP	2
#endif /* _LINUX_VIRTIO_BLK_H */
