/*
 * Virtio Block Device
 *
 * Copyright IBM, Corp. 2007
 *
 * Authors:
 *  Anthony Liguori   <aliguori@us.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu-common.h"
#include "qemu/iov.h"
#include "qemu/error-report.h"
#include "trace.h"
#include "hw/block/block.h"
#include "sysemu/block-backend.h"
#include "sysemu/blockdev.h"
#include "hw/virtio/virtio-blk.h"
#include "dataplane/virtio-blk.h"
#include "block/scsi.h"
#ifdef __linux__
# include <scsi/sg.h>
#endif
#include "hw/virtio/virtio-bus.h"
#include "hw/virtio/virtio-access.h"


#include "qemu/timer.h"

static void virtio_blk_init_request(VirtIOBlock *s, VirtQueue *vq,
                                    VirtIOBlockReq *req)
{
    req->dev = s;
    req->vq = vq;
    req->qiov.size = 0;
    req->in_len = 0;
    req->next = NULL;
    req->mr_next = NULL;
}

static void virtio_blk_free_request(VirtIOBlockReq *req)
{
    if (req) {
        g_free(req);
    }
}

static void virtio_blk_req_complete(VirtIOBlockReq *req, unsigned char status)
{
    VirtIOBlock *s = req->dev;
    VirtIODevice *vdev = VIRTIO_DEVICE(s);

    trace_virtio_blk_req_complete(req, status);

    stb_p(&req->in->status, status);
    virtqueue_push(req->vq, &req->elem, req->in_len);
    if (s->dataplane_started && !s->dataplane_disabled) {
        virtio_blk_data_plane_notify(s->dataplane, req->vq);
    } else {
        virtio_notify(vdev, req->vq);
    }
}

static int virtio_blk_handle_rw_error(VirtIOBlockReq *req, int error,
    bool is_read)
{
    BlockErrorAction action = blk_get_error_action(req->dev->blk,
                                                   is_read, error);
    VirtIOBlock *s = req->dev;

    if (action == BLOCK_ERROR_ACTION_STOP) {
        /* Break the link as the next request is going to be parsed from the
         * ring again. Otherwise we may end up doing a double completion! */
        req->mr_next = NULL;
        req->next = s->rq;
        s->rq = req;
    } else if (action == BLOCK_ERROR_ACTION_REPORT) {
        virtio_blk_req_complete(req, VIRTIO_BLK_S_IOERR);
        block_acct_failed(blk_get_stats(s->blk), &req->acct);
        virtio_blk_free_request(req);
    }

    blk_error_action(s->blk, action, is_read, error);
    return action != BLOCK_ERROR_ACTION_IGNORE;
}

static void virtio_blk_rw_complete(void *opaque, int ret)
{
    VirtIOBlockReq *next = opaque;
    VirtIOBlock *s = next->dev;

    aio_context_acquire(blk_get_aio_context(s->conf.conf.blk));
    while (next) {
        VirtIOBlockReq *req = next;
        next = req->mr_next;
        trace_virtio_blk_rw_complete(req, ret);

        if (req->qiov.nalloc != -1) {
            /* If nalloc is != 1 req->qiov is a local copy of the original
             * external iovec. It was allocated in submit_merged_requests
             * to be able to merge requests. */
            qemu_iovec_destroy(&req->qiov);
        }

        if (ret) {
            int p = virtio_ldl_p(VIRTIO_DEVICE(req->dev), &req->out.type);
            bool is_read = !(p & VIRTIO_BLK_T_OUT);
            /* Note that memory may be dirtied on read failure.  If the
             * virtio request is not completed here, as is the case for
             * BLOCK_ERROR_ACTION_STOP, the memory may not be copied
             * correctly during live migration.  While this is ugly,
             * it is acceptable because the device is free to write to
             * the memory until the request is completed (which will
             * happen on the other side of the migration).
             */
            if (virtio_blk_handle_rw_error(req, -ret, is_read)) {
                continue;
            }
        }

        virtio_blk_req_complete(req, VIRTIO_BLK_S_OK);
        block_acct_done(blk_get_stats(req->dev->blk), &req->acct);
        virtio_blk_free_request(req);
    }
    aio_context_release(blk_get_aio_context(s->conf.conf.blk));
}

static void virtio_blk_flush_complete(void *opaque, int ret)
{
    VirtIOBlockReq *req = opaque;
    VirtIOBlock *s = req->dev;

    aio_context_acquire(blk_get_aio_context(s->conf.conf.blk));
    if (ret) {
        if (virtio_blk_handle_rw_error(req, -ret, 0)) {
            goto out;
        }
    }

    virtio_blk_req_complete(req, VIRTIO_BLK_S_OK);
    block_acct_done(blk_get_stats(req->dev->blk), &req->acct);
    virtio_blk_free_request(req);

out:
    aio_context_release(blk_get_aio_context(s->conf.conf.blk));
}

#ifdef __linux__

typedef struct {
    VirtIOBlockReq *req;
    struct sg_io_hdr hdr;
} VirtIOBlockIoctlReq;

static void virtio_blk_ioctl_complete(void *opaque, int status)
{
    VirtIOBlockIoctlReq *ioctl_req = opaque;
    VirtIOBlockReq *req = ioctl_req->req;
    VirtIOBlock *s = req->dev;
    VirtIODevice *vdev = VIRTIO_DEVICE(s);
    struct virtio_scsi_inhdr *scsi;
    struct sg_io_hdr *hdr;

    scsi = (void *)req->elem.in_sg[req->elem.in_num - 2].iov_base;

    if (status) {
        status = VIRTIO_BLK_S_UNSUPP;
        virtio_stl_p(vdev, &scsi->errors, 255);
        goto out;
    }

    hdr = &ioctl_req->hdr;
    /*
     * From SCSI-Generic-HOWTO: "Some lower level drivers (e.g. ide-scsi)
     * clear the masked_status field [hence status gets cleared too, see
     * block/scsi_ioctl.c] even when a CHECK_CONDITION or COMMAND_TERMINATED
     * status has occurred.  However they do set DRIVER_SENSE in driver_status
     * field. Also a (sb_len_wr > 0) indicates there is a sense buffer.
     */
    if (hdr->status == 0 && hdr->sb_len_wr > 0) {
        hdr->status = CHECK_CONDITION;
    }

    virtio_stl_p(vdev, &scsi->errors,
                 hdr->status | (hdr->msg_status << 8) |
                 (hdr->host_status << 16) | (hdr->driver_status << 24));
    virtio_stl_p(vdev, &scsi->residual, hdr->resid);
    virtio_stl_p(vdev, &scsi->sense_len, hdr->sb_len_wr);
    virtio_stl_p(vdev, &scsi->data_len, hdr->dxfer_len);

out:
    aio_context_acquire(blk_get_aio_context(s->conf.conf.blk));
    virtio_blk_req_complete(req, status);
    virtio_blk_free_request(req);
    aio_context_release(blk_get_aio_context(s->conf.conf.blk));
    g_free(ioctl_req);
}

#endif

static VirtIOBlockReq *virtio_blk_get_request(VirtIOBlock *s, VirtQueue *vq)
{
    VirtIOBlockReq *req = virtqueue_pop(vq, sizeof(VirtIOBlockReq));

    if (req) {
        virtio_blk_init_request(s, vq, req);
    }
    return req;
}

static int virtio_blk_handle_scsi_req(VirtIOBlockReq *req)
{
    int status = VIRTIO_BLK_S_OK;
    struct virtio_scsi_inhdr *scsi = NULL;
    VirtIODevice *vdev = VIRTIO_DEVICE(req->dev);
    VirtQueueElement *elem = &req->elem;
    VirtIOBlock *blk = req->dev;

#ifdef __linux__
    int i;
    VirtIOBlockIoctlReq *ioctl_req;
    BlockAIOCB *acb;
#endif

    /*
     * We require at least one output segment each for the virtio_blk_outhdr
     * and the SCSI command block.
     *
     * We also at least require the virtio_blk_inhdr, the virtio_scsi_inhdr
     * and the sense buffer pointer in the input segments.
     */
    if (elem->out_num < 2 || elem->in_num < 3) {
        status = VIRTIO_BLK_S_IOERR;
        goto fail;
    }

    /*
     * The scsi inhdr is placed in the second-to-last input segment, just
     * before the regular inhdr.
     */
    scsi = (void *)elem->in_sg[elem->in_num - 2].iov_base;

    if (!blk->conf.scsi) {
        status = VIRTIO_BLK_S_UNSUPP;
        goto fail;
    }

    /*
     * No support for bidirection commands yet.
     */
    if (elem->out_num > 2 && elem->in_num > 3) {
        status = VIRTIO_BLK_S_UNSUPP;
        goto fail;
    }

#ifdef __linux__
    ioctl_req = g_new0(VirtIOBlockIoctlReq, 1);
    ioctl_req->req = req;
    ioctl_req->hdr.interface_id = 'S';
    ioctl_req->hdr.cmd_len = elem->out_sg[1].iov_len;
    ioctl_req->hdr.cmdp = elem->out_sg[1].iov_base;
    ioctl_req->hdr.dxfer_len = 0;

    if (elem->out_num > 2) {
        /*
         * If there are more than the minimally required 2 output segments
         * there is write payload starting from the third iovec.
         */
        ioctl_req->hdr.dxfer_direction = SG_DXFER_TO_DEV;
        ioctl_req->hdr.iovec_count = elem->out_num - 2;

        for (i = 0; i < ioctl_req->hdr.iovec_count; i++) {
            ioctl_req->hdr.dxfer_len += elem->out_sg[i + 2].iov_len;
        }

        ioctl_req->hdr.dxferp = elem->out_sg + 2;

    } else if (elem->in_num > 3) {
        /*
         * If we have more than 3 input segments the guest wants to actually
         * read data.
         */
        ioctl_req->hdr.dxfer_direction = SG_DXFER_FROM_DEV;
        ioctl_req->hdr.iovec_count = elem->in_num - 3;
        for (i = 0; i < ioctl_req->hdr.iovec_count; i++) {
            ioctl_req->hdr.dxfer_len += elem->in_sg[i].iov_len;
        }

        ioctl_req->hdr.dxferp = elem->in_sg;
    } else {
        /*
         * Some SCSI commands don't actually transfer any data.
         */
        ioctl_req->hdr.dxfer_direction = SG_DXFER_NONE;
    }

    ioctl_req->hdr.sbp = elem->in_sg[elem->in_num - 3].iov_base;
    ioctl_req->hdr.mx_sb_len = elem->in_sg[elem->in_num - 3].iov_len;

    acb = blk_aio_ioctl(blk->blk, SG_IO, &ioctl_req->hdr,
                        virtio_blk_ioctl_complete, ioctl_req);
    if (!acb) {
        g_free(ioctl_req);
        status = VIRTIO_BLK_S_UNSUPP;
        goto fail;
    }
    return -EINPROGRESS;
#else
    abort();
#endif

fail:
    /* Just put anything nonzero so that the ioctl fails in the guest.  */
    if (scsi) {
        virtio_stl_p(vdev, &scsi->errors, 255);
    }
    return status;
}

static void virtio_blk_handle_scsi(VirtIOBlockReq *req)
{
    int status;

    status = virtio_blk_handle_scsi_req(req);
    if (status != -EINPROGRESS) {
        virtio_blk_req_complete(req, status);
        virtio_blk_free_request(req);
    }
}

static inline void submit_requests(BlockBackend *blk, MultiReqBuffer *mrb,
                                   int start, int num_reqs, int niov)
{
    QEMUIOVector *qiov = &mrb->reqs[start]->qiov;
    int64_t sector_num = mrb->reqs[start]->sector_num;
    bool is_write = mrb->is_write;

    if (num_reqs > 1) {
        int i;
        struct iovec *tmp_iov = qiov->iov;
        int tmp_niov = qiov->niov;

        /* mrb->reqs[start]->qiov was initialized from external so we can't
         * modify it here. We need to initialize it locally and then add the
         * external iovecs. */
        qemu_iovec_init(qiov, niov);

        for (i = 0; i < tmp_niov; i++) {
            qemu_iovec_add(qiov, tmp_iov[i].iov_base, tmp_iov[i].iov_len);
        }

        for (i = start + 1; i < start + num_reqs; i++) {
            qemu_iovec_concat(qiov, &mrb->reqs[i]->qiov, 0,
                              mrb->reqs[i]->qiov.size);
            mrb->reqs[i - 1]->mr_next = mrb->reqs[i];
        }

        trace_virtio_blk_submit_multireq(mrb, start, num_reqs,
                                         sector_num << BDRV_SECTOR_BITS,
                                         qiov->size, is_write);
        block_acct_merge_done(blk_get_stats(blk),
                              is_write ? BLOCK_ACCT_WRITE : BLOCK_ACCT_READ,
                              num_reqs - 1);
    }

    if (is_write) {

        /*printf("\n\tvirt-blk:submit_requests: write: sector=%ld [%ld]. call blk_aio_pwritev. req = %x \n", 
            sector_num,
            sector_num << BDRV_SECTOR_BITS,
            mrb->reqs[start]);

        printf("\virt-blk:QEMUIOVector. iov=%x, niov=%d, nalloc=%d, size=%d \n",
            qiov->iov,
            qiov->niov,
            qiov->nalloc,
            qiov->size);*/

        blk_aio_pwritev(blk, sector_num << BDRV_SECTOR_BITS, qiov, 0,
                        virtio_blk_rw_complete, mrb->reqs[start]);
    } else {
        blk_aio_preadv(blk, sector_num << BDRV_SECTOR_BITS, qiov, 0,
                       virtio_blk_rw_complete, mrb->reqs[start]);
    }
}

static int multireq_compare(const void *a, const void *b)
{
    const VirtIOBlockReq *req1 = *(VirtIOBlockReq **)a,
                         *req2 = *(VirtIOBlockReq **)b;

    /*
     * Note that we can't simply subtract sector_num1 from sector_num2
     * here as that could overflow the return value.
     */
    if (req1->sector_num > req2->sector_num) {
        return 1;
    } else if (req1->sector_num < req2->sector_num) {
        return -1;
    } else {
        return 0;
    }
}

static void virtio_blk_submit_multireq(BlockBackend *blk, MultiReqBuffer *mrb)
{

   // printf("virtio: virtio_blk_submit_multireq called \n");

    int i = 0, start = 0, num_reqs = 0, niov = 0, nb_sectors = 0;
    uint32_t max_transfer;
    int64_t sector_num = 0;

    if (mrb->num_reqs == 1) {
        submit_requests(blk, mrb, 0, 1, -1);
        mrb->num_reqs = 0;
        return;
    }

    max_transfer = blk_get_max_transfer(mrb->reqs[0]->dev->blk);

    qsort(mrb->reqs, mrb->num_reqs, sizeof(*mrb->reqs),
          &multireq_compare);

    for (i = 0; i < mrb->num_reqs; i++) {
        VirtIOBlockReq *req = mrb->reqs[i];
        if (num_reqs > 0) {
            /*
             * NOTE: We cannot merge the requests in below situations:
             * 1. requests are not sequential
             * 2. merge would exceed maximum number of IOVs
             * 3. merge would exceed maximum transfer length of backend device
             */
            if (sector_num + nb_sectors != req->sector_num ||
                niov > blk_get_max_iov(blk) - req->qiov.niov ||
                req->qiov.size > max_transfer ||
                nb_sectors > (max_transfer -
                              req->qiov.size) / BDRV_SECTOR_SIZE) {
                submit_requests(blk, mrb, start, num_reqs, niov);
                num_reqs = 0;
            }
        }

        if (num_reqs == 0) {
            sector_num = req->sector_num;
            nb_sectors = niov = 0;
            start = i;
        }

        nb_sectors += req->qiov.size / BDRV_SECTOR_SIZE;
        niov += req->qiov.niov;
        num_reqs++;
    }

    submit_requests(blk, mrb, start, num_reqs, niov);
    mrb->num_reqs = 0;
}

static void virtio_blk_handle_flush(VirtIOBlockReq *req, MultiReqBuffer *mrb)
{
    block_acct_start(blk_get_stats(req->dev->blk), &req->acct, 0,
                     BLOCK_ACCT_FLUSH);

    /*
     * Make sure all outstanding writes are posted to the backing device.
     */
    if (mrb->is_write && mrb->num_reqs > 0) {
        virtio_blk_submit_multireq(req->dev->blk, mrb);
    }
    blk_aio_flush(req->dev->blk, virtio_blk_flush_complete, req);
}

static bool virtio_blk_sect_range_ok(VirtIOBlock *dev,
                                     uint64_t sector, size_t size)
{
    uint64_t nb_sectors = size >> BDRV_SECTOR_BITS;
    uint64_t total_sectors;

    if (nb_sectors > BDRV_REQUEST_MAX_SECTORS) {
        return false;
    }
    if (sector & dev->sector_mask) {
        return false;
    }
    if (size % dev->conf.conf.logical_block_size) {
        return false;
    }
    blk_get_geometry(dev->blk, &total_sectors);
    if (sector > total_sectors || nb_sectors > total_sectors - sector) {
        return false;
    }
    return true;
}

static int virtio_blk_handle_request(VirtIOBlockReq *req, MultiReqBuffer *mrb)
{
    uint32_t type;
    struct iovec *in_iov = req->elem.in_sg;
    struct iovec *iov = req->elem.out_sg;
    unsigned in_num = req->elem.in_num;
    unsigned out_num = req->elem.out_num;
    VirtIOBlock *s = req->dev;
    VirtIODevice *vdev = VIRTIO_DEVICE(s);

    //clock_t start, end;
    //double cpu_time_used; 

    if (req->elem.out_num < 1 || req->elem.in_num < 1) {
        virtio_error(vdev, "virtio-blk missing headers");
        return -1;
    }

    if (unlikely(iov_to_buf(iov, out_num, 0, &req->out,
                            sizeof(req->out)) != sizeof(req->out))) {
        virtio_error(vdev, "virtio-blk request outhdr too short");
        return -1;
    }

    iov_discard_front(&iov, &out_num, sizeof(req->out));

    if (in_iov[in_num - 1].iov_len < sizeof(struct virtio_blk_inhdr)) {
        virtio_error(vdev, "virtio-blk request inhdr too short");
        return -1;
    }

    /* We always touch the last byte, so just see how big in_iov is.  */
    req->in_len = iov_size(in_iov, in_num);
    req->in = (void *)in_iov[in_num - 1].iov_base
              + in_iov[in_num - 1].iov_len
              - sizeof(struct virtio_blk_inhdr);
    iov_discard_back(in_iov, &in_num, sizeof(struct virtio_blk_inhdr));

    type = virtio_ldl_p(VIRTIO_DEVICE(req->dev), &req->out.type);

    /* VIRTIO_BLK_T_OUT defines the command direction. VIRTIO_BLK_T_BARRIER
     * is an optional flag. Although a guest should not send this flag if
     * not negotiated we ignored it in the past. So keep ignoring it. */
    switch (type & ~(VIRTIO_BLK_T_OUT | VIRTIO_BLK_T_BARRIER)) {
    case VIRTIO_BLK_T_IN:
    {
        bool is_write = type & VIRTIO_BLK_T_OUT;
        req->sector_num = virtio_ldq_p(VIRTIO_DEVICE(req->dev),
                                       &req->out.sector);

        if (is_write) 
        {
            //printf("virtio: Received Write request.................................\n");
            //start = clock(); 

            qemu_iovec_init_external(&req->qiov, iov, out_num);
            trace_virtio_blk_handle_write(req, req->sector_num,
                                          req->qiov.size / BDRV_SECTOR_SIZE);

        } 
        else 
        {   
            //printf("virtio: Received Read request\n"); 
            //start = clock();

            qemu_iovec_init_external(&req->qiov, in_iov, in_num);
            trace_virtio_blk_handle_read(req, req->sector_num,
                                         req->qiov.size / BDRV_SECTOR_SIZE);
        }

        if (!virtio_blk_sect_range_ok(req->dev, req->sector_num, req->qiov.size)) 
        {
            virtio_blk_req_complete(req, VIRTIO_BLK_S_IOERR);
            block_acct_invalid(blk_get_stats(req->dev->blk), is_write ? BLOCK_ACCT_WRITE : BLOCK_ACCT_READ);
            virtio_blk_free_request(req);
            return 0;
        }

        block_acct_start(blk_get_stats(req->dev->blk),
                         &req->acct, req->qiov.size,
                         is_write ? BLOCK_ACCT_WRITE : BLOCK_ACCT_READ);

        /* merge would exceed maximum number of requests or IO direction
         * changes */
        //printf("virtio: num_reqs=%d, VIRTIO_BLK_MAX_MERGE_REQS=32, is_write=%d, mrb->is_write=%d, req->dev->conf.request_merging=%d \n",
        //     mrb->num_reqs, is_write, mrb->is_write, req->dev->conf.request_merging);

        if (mrb->num_reqs > 0 && (mrb->num_reqs == VIRTIO_BLK_MAX_MERGE_REQS || is_write != mrb->is_write || !req->dev->conf.request_merging)) 
        {
           // printf("virtio: Calling submit_multireq from handle_request \n");
            virtio_blk_submit_multireq(req->dev->blk, mrb);
        }

        // end = clock();
        // cpu_time_used = ((double) (end - start)) / CLOCKS_PER_SEC;
        // printf("virtio: %s Completed......\n", is_write ? "Write" : "Read");  
        // printf("virtio: %s took %f seconds to execute--------------->>>> \n", is_write ? "Write" : "Read", cpu_time_used);

        assert(mrb->num_reqs < VIRTIO_BLK_MAX_MERGE_REQS);
        mrb->reqs[mrb->num_reqs++] = req;
        mrb->is_write = is_write;
        break;
    }
    case VIRTIO_BLK_T_FLUSH:
        virtio_blk_handle_flush(req, mrb);
        break;
    case VIRTIO_BLK_T_SCSI_CMD:
        virtio_blk_handle_scsi(req);
        break;
    case VIRTIO_BLK_T_GET_ID:
    {
        VirtIOBlock *s = req->dev;

        /*
         * NB: per existing s/n string convention the string is
         * terminated by '\0' only when shorter than buffer.
         */
        const char *serial = s->conf.serial ? s->conf.serial : "";
        size_t size = MIN(strlen(serial) + 1,
                          MIN(iov_size(in_iov, in_num),
                              VIRTIO_BLK_ID_BYTES));
        iov_from_buf(in_iov, in_num, 0, serial, size);
        virtio_blk_req_complete(req, VIRTIO_BLK_S_OK);
        virtio_blk_free_request(req);
        break;
    }
    default:
        virtio_blk_req_complete(req, VIRTIO_BLK_S_UNSUPP);
        virtio_blk_free_request(req);
    }
    return 0;
}

bool virtio_blk_handle_vq(VirtIOBlock *s, VirtQueue *vq)
{
    VirtIOBlockReq *req;
    MultiReqBuffer mrb = {};
    bool progress = false;

    aio_context_acquire(blk_get_aio_context(s->blk));
    blk_io_plug(s->blk);

    do {
        virtio_queue_set_notification(vq, 0);

        while ((req = virtio_blk_get_request(s, vq))) {
            progress = true;
            if (virtio_blk_handle_request(req, &mrb)) 
            {
                virtqueue_detach_element(req->vq, &req->elem, 0);
                virtio_blk_free_request(req);
                break;
            }
         
        }

        virtio_queue_set_notification(vq, 1);
    } while (!virtio_queue_empty(vq));


    if (mrb.num_reqs) 
    {
       // printf("virtio: Calling multi req from handle_vq \n");
        virtio_blk_submit_multireq(s->blk, &mrb);
    }

    blk_io_unplug(s->blk);
    aio_context_release(blk_get_aio_context(s->blk));
    return progress;
}

static void virtio_blk_handle_output_do(VirtIOBlock *s, VirtQueue *vq)
{
    virtio_blk_handle_vq(s, vq);
}

static void virtio_blk_handle_output(VirtIODevice *vdev, VirtQueue *vq)
{
    VirtIOBlock *s = (VirtIOBlock *)vdev;

    if (s->dataplane) {
        /* Some guests kick before setting VIRTIO_CONFIG_S_DRIVER_OK so start
         * dataplane here instead of waiting for .set_status().
         */
        virtio_device_start_ioeventfd(vdev);
        if (!s->dataplane_disabled) {
            return;
        }
    }
    virtio_blk_handle_output_do(s, vq);
}

static void virtio_blk_dma_restart_bh(void *opaque)
{
    VirtIOBlock *s = opaque;
    VirtIOBlockReq *req = s->rq;
    MultiReqBuffer mrb = {};

    qemu_bh_delete(s->bh);
    s->bh = NULL;

    s->rq = NULL;

    aio_context_acquire(blk_get_aio_context(s->conf.conf.blk));
    while (req) {
        VirtIOBlockReq *next = req->next;
        if (virtio_blk_handle_request(req, &mrb)) {
            /* Device is now broken and won't do any processing until it gets
             * reset. Already queued requests will be lost: let's purge them.
             */
            while (req) {
                next = req->next;
                virtqueue_detach_element(req->vq, &req->elem, 0);
                virtio_blk_free_request(req);
                req = next;
            }
            break;
        }
        req = next;
    }

    if (mrb.num_reqs) {
        virtio_blk_submit_multireq(s->blk, &mrb);
    }
    aio_context_release(blk_get_aio_context(s->conf.conf.blk));
}

static void virtio_blk_dma_restart_cb(void *opaque, int running,
                                      RunState state)
{
    VirtIOBlock *s = opaque;

    if (!running) {
        return;
    }

    if (!s->bh) {
        s->bh = aio_bh_new(blk_get_aio_context(s->conf.conf.blk),
                           virtio_blk_dma_restart_bh, s);
        qemu_bh_schedule(s->bh);
    }
}

static void virtio_blk_reset(VirtIODevice *vdev)
{
    VirtIOBlock *s = VIRTIO_BLK(vdev);
    AioContext *ctx;
    VirtIOBlockReq *req;

    ctx = blk_get_aio_context(s->blk);
    aio_context_acquire(ctx);
    blk_drain(s->blk);

    /* We drop queued requests after blk_drain() because blk_drain() itself can
     * produce them. */
    while (s->rq) {
        req = s->rq;
        s->rq = req->next;
        virtqueue_detach_element(req->vq, &req->elem, 0);
        virtio_blk_free_request(req);
    }

    aio_context_release(ctx);

    assert(!s->dataplane_started);
    blk_set_enable_write_cache(s->blk, s->original_wce);
}

/* coalesce internal state, copy to pci i/o region 0
 */
static void virtio_blk_update_config(VirtIODevice *vdev, uint8_t *config)
{
    //printf("virtio-blk : virtio_blk_update_config \n");
    VirtIOBlock *s = VIRTIO_BLK(vdev);
    BlockConf *conf = &s->conf.conf;
    struct virtio_blk_config blkcfg;
    uint64_t capacity;
    int blk_size = conf->logical_block_size;

    blk_get_geometry(s->blk, &capacity);
    memset(&blkcfg, 0, sizeof(blkcfg));
    virtio_stq_p(vdev, &blkcfg.capacity, capacity);
    virtio_stl_p(vdev, &blkcfg.seg_max, 128 - 2);
    virtio_stw_p(vdev, &blkcfg.geometry.cylinders, conf->cyls);
    virtio_stl_p(vdev, &blkcfg.blk_size, blk_size);
    virtio_stw_p(vdev, &blkcfg.min_io_size, conf->min_io_size / blk_size);
    virtio_stw_p(vdev, &blkcfg.opt_io_size, conf->opt_io_size / blk_size);
    blkcfg.geometry.heads = conf->heads;
    /*
     * We must ensure that the block device capacity is a multiple of
     * the logical block size. If that is not the case, let's use
     * sector_mask to adopt the geometry to have a correct picture.
     * For those devices where the capacity is ok for the given geometry
     * we don't touch the sector value of the geometry, since some devices
     * (like s390 dasd) need a specific value. Here the capacity is already
     * cyls*heads*secs*blk_size and the sector value is not block size
     * divided by 512 - instead it is the amount of blk_size blocks
     * per track (cylinder).
     */
    if (blk_getlength(s->blk) /  conf->heads / conf->secs % blk_size) {
        blkcfg.geometry.sectors = conf->secs & ~s->sector_mask;
    } else {
        blkcfg.geometry.sectors = conf->secs;
    }
    blkcfg.size_max = 0;
    blkcfg.physical_block_exp = get_physical_block_exp(conf);
    blkcfg.alignment_offset = 0;
    blkcfg.wce = blk_enable_write_cache(s->blk);
    virtio_stw_p(vdev, &blkcfg.num_queues, s->conf.num_queues);
    memcpy(config, &blkcfg, sizeof(struct virtio_blk_config));
}

static void virtio_blk_set_config(VirtIODevice *vdev, const uint8_t *config)
{
    VirtIOBlock *s = VIRTIO_BLK(vdev);
    struct virtio_blk_config blkcfg;

    memcpy(&blkcfg, config, sizeof(blkcfg));

    aio_context_acquire(blk_get_aio_context(s->blk));
    blk_set_enable_write_cache(s->blk, blkcfg.wce != 0);
    aio_context_release(blk_get_aio_context(s->blk));
}

static uint64_t virtio_blk_get_features(VirtIODevice *vdev, uint64_t features,
                                        Error **errp)
{
    VirtIOBlock *s = VIRTIO_BLK(vdev);

    virtio_add_feature(&features, VIRTIO_BLK_F_SEG_MAX);
    virtio_add_feature(&features, VIRTIO_BLK_F_GEOMETRY);
    virtio_add_feature(&features, VIRTIO_BLK_F_TOPOLOGY);
    virtio_add_feature(&features, VIRTIO_BLK_F_BLK_SIZE);
    if (virtio_has_feature(features, VIRTIO_F_VERSION_1)) {
        if (s->conf.scsi) {
            error_setg(errp, "Please set scsi=off for virtio-blk devices in order to use virtio 1.0");
            return 0;
        }
    } else {
        virtio_clear_feature(&features, VIRTIO_F_ANY_LAYOUT);
        virtio_add_feature(&features, VIRTIO_BLK_F_SCSI);
    }

    if (s->conf.config_wce) {
        virtio_add_feature(&features, VIRTIO_BLK_F_CONFIG_WCE);
    }
    if (blk_enable_write_cache(s->blk)) {
        virtio_add_feature(&features, VIRTIO_BLK_F_WCE);
    }
    if (blk_is_read_only(s->blk)) {
        virtio_add_feature(&features, VIRTIO_BLK_F_RO);
    }
    if (s->conf.num_queues > 1) {
        virtio_add_feature(&features, VIRTIO_BLK_F_MQ);
    }

    return features;
}

static void virtio_blk_set_status(VirtIODevice *vdev, uint8_t status)
{
    VirtIOBlock *s = VIRTIO_BLK(vdev);

    if (!(status & (VIRTIO_CONFIG_S_DRIVER | VIRTIO_CONFIG_S_DRIVER_OK))) {
        assert(!s->dataplane_started);
    }

    if (!(status & VIRTIO_CONFIG_S_DRIVER_OK)) {
        return;
    }

    /* A guest that supports VIRTIO_BLK_F_CONFIG_WCE must be able to send
     * cache flushes.  Thus, the "auto writethrough" behavior is never
     * necessary for guests that support the VIRTIO_BLK_F_CONFIG_WCE feature.
     * Leaving it enabled would break the following sequence:
     *
     *     Guest started with "-drive cache=writethrough"
     *     Guest sets status to 0
     *     Guest sets DRIVER bit in status field
     *     Guest reads host features (WCE=0, CONFIG_WCE=1)
     *     Guest writes guest features (WCE=0, CONFIG_WCE=1)
     *     Guest writes 1 to the WCE configuration field (writeback mode)
     *     Guest sets DRIVER_OK bit in status field
     *
     * s->blk would erroneously be placed in writethrough mode.
     */
    if (!virtio_vdev_has_feature(vdev, VIRTIO_BLK_F_CONFIG_WCE)) {
        aio_context_acquire(blk_get_aio_context(s->blk));
        blk_set_enable_write_cache(s->blk,
                                   virtio_vdev_has_feature(vdev,
                                                           VIRTIO_BLK_F_WCE));
        aio_context_release(blk_get_aio_context(s->blk));
    }
}

static void virtio_blk_save_device(VirtIODevice *vdev, QEMUFile *f)
{
    VirtIOBlock *s = VIRTIO_BLK(vdev);
    VirtIOBlockReq *req = s->rq;

    while (req) {
        qemu_put_sbyte(f, 1);

        if (s->conf.num_queues > 1) {
            qemu_put_be32(f, virtio_get_queue_index(req->vq));
        }

        qemu_put_virtqueue_element(f, &req->elem);
        req = req->next;
    }
    qemu_put_sbyte(f, 0);
}

static int virtio_blk_load_device(VirtIODevice *vdev, QEMUFile *f,
                                  int version_id)
{
    VirtIOBlock *s = VIRTIO_BLK(vdev);

    while (qemu_get_sbyte(f)) {
        unsigned nvqs = s->conf.num_queues;
        unsigned vq_idx = 0;
        VirtIOBlockReq *req;

        if (nvqs > 1) {
            vq_idx = qemu_get_be32(f);

            if (vq_idx >= nvqs) {
                error_report("Invalid virtqueue index in request list: %#x",
                             vq_idx);
                return -EINVAL;
            }
        }

        req = qemu_get_virtqueue_element(vdev, f, sizeof(VirtIOBlockReq));
        virtio_blk_init_request(s, virtio_get_queue(vdev, vq_idx), req);
        req->next = s->rq;
        s->rq = req;
    }

    return 0;
}

static void virtio_blk_resize(void *opaque)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(opaque);

    virtio_notify_config(vdev);
}

static const BlockDevOps virtio_block_ops = {
    .resize_cb = virtio_blk_resize,
};

static void virtio_blk_device_realize(DeviceState *dev, Error **errp)
{
    printf("\n\n\nvirtio: 'virtio_blk_device_realize' function called \n");   

    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    VirtIOBlock *s = VIRTIO_BLK(dev);
    VirtIOBlkConf *conf = &s->conf;
    Error *err = NULL;
    unsigned i;

    if (!conf->conf.blk) {
        error_setg(errp, "drive property not set");
        return;
    }
    if (!blk_is_inserted(conf->conf.blk)) {
        error_setg(errp, "Device needs media, but drive is empty");
        return;
    }
    if (!conf->num_queues) {
        error_setg(errp, "num-queues property must be larger than 0");
        return;
    }

    blkconf_serial(&conf->conf, &conf->serial);
    blkconf_apply_backend_options(&conf->conf,
                                  blk_is_read_only(conf->conf.blk), true,
                                  &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }


    s->original_wce = blk_enable_write_cache(conf->conf.blk);
    blkconf_geometry(&conf->conf, NULL, 65535, 255, 255, &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }
    blkconf_blocksizes(&conf->conf);
    printf("virtio-vssd: Logical block size=%d, Physicalblock size=%d \n",
        conf->conf.logical_block_size, conf->conf.physical_block_size);

    
    virtio_init(vdev, "virtio-blk", VIRTIO_ID_BLOCK,

                sizeof(struct virtio_blk_config));

    s->blk = conf->conf.blk;
    s->rq = NULL;
    s->sector_mask = (s->conf.conf.logical_block_size / BDRV_SECTOR_SIZE) - 1;

    printf("virtio: Adding %d queues to virtio \n", conf->num_queues);
    for (i = 0; i < conf->num_queues; i++) {
        virtio_add_queue(vdev, 128, virtio_blk_handle_output);
    }
    virtio_blk_data_plane_create(vdev, conf, &s->dataplane, &err);
    if (err != NULL) {
        error_propagate(errp, err);
        virtio_cleanup(vdev);
        return;
    }

    s->change = qemu_add_vm_change_state_handler(virtio_blk_dma_restart_cb, s);
    blk_set_dev_ops(s->blk, &virtio_block_ops, s);
    blk_set_guest_block_size(s->blk, s->conf.conf.logical_block_size);

    blk_iostatus_enable(s->blk);
}

static void virtio_blk_device_unrealize(DeviceState *dev, Error **errp)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    VirtIOBlock *s = VIRTIO_BLK(dev);

    virtio_blk_data_plane_destroy(s->dataplane);
    s->dataplane = NULL;
    qemu_del_vm_change_state_handler(s->change);
    blockdev_mark_auto_del(s->blk);
    virtio_cleanup(vdev);
}

static void virtio_blk_instance_init(Object *obj)
{
    VirtIOBlock *s = VIRTIO_BLK(obj);

    object_property_add_link(obj, "iothread", TYPE_IOTHREAD,
                             (Object **)&s->conf.iothread,
                             qdev_prop_allow_set_link_before_realize,
                             OBJ_PROP_LINK_UNREF_ON_RELEASE, NULL);
    device_add_bootindex_property(obj, &s->conf.conf.bootindex,
                                  "bootindex", "/disk@0,0",
                                  DEVICE(obj), NULL);
}

static const VMStateDescription vmstate_virtio_blk = {
    .name = "virtio-blk",
    .minimum_version_id = 2,
    .version_id = 2,
    .fields = (VMStateField[]) {
        VMSTATE_VIRTIO_DEVICE,
        VMSTATE_END_OF_LIST()
    },
};

static Property virtio_blk_properties[] = {
    DEFINE_BLOCK_PROPERTIES(VirtIOBlock, conf.conf),
    DEFINE_BLOCK_ERROR_PROPERTIES(VirtIOBlock, conf.conf),
    DEFINE_BLOCK_CHS_PROPERTIES(VirtIOBlock, conf.conf),
    DEFINE_PROP_STRING("serial", VirtIOBlock, conf.serial),
    DEFINE_PROP_BIT("config-wce", VirtIOBlock, conf.config_wce, 0, true),
#ifdef __linux__
    DEFINE_PROP_BIT("scsi", VirtIOBlock, conf.scsi, 0, false),
#endif
    DEFINE_PROP_BIT("request-merging", VirtIOBlock, conf.request_merging, 0,
                    true),
    DEFINE_PROP_UINT16("num-queues", VirtIOBlock, conf.num_queues, 1),
    DEFINE_PROP_END_OF_LIST(),
};

static void virtio_blk_class_init(ObjectClass *klass, void *data)
{
    printf("virtio: 'virtio_blk_class_init' function called \n");   

    DeviceClass *dc = DEVICE_CLASS(klass);
    VirtioDeviceClass *vdc = VIRTIO_DEVICE_CLASS(klass);

    dc->props = virtio_blk_properties;
    dc->vmsd = &vmstate_virtio_blk;
    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);
    vdc->realize = virtio_blk_device_realize;
    vdc->unrealize = virtio_blk_device_unrealize;
    vdc->get_config = virtio_blk_update_config;
    vdc->set_config = virtio_blk_set_config;
    vdc->get_features = virtio_blk_get_features;
    vdc->set_status = virtio_blk_set_status;
    vdc->reset = virtio_blk_reset;
    vdc->save = virtio_blk_save_device;
    vdc->load = virtio_blk_load_device;
    vdc->start_ioeventfd = virtio_blk_data_plane_start;
    vdc->stop_ioeventfd = virtio_blk_data_plane_stop;
}

static const TypeInfo virtio_blk_info = {
    .name = TYPE_VIRTIO_BLK,
    .parent = TYPE_VIRTIO_DEVICE,
    .instance_size = sizeof(VirtIOBlock),
    .instance_init = virtio_blk_instance_init,
    .class_init = virtio_blk_class_init,
};

static void virtio_register_types(void)
{
    printf("Registration \n");   

    type_register_static(&virtio_blk_info);
    printf("\tvirtio-blk device registered. \n");

}

type_init(virtio_register_types)
