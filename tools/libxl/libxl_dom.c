/*
 * Copyright (C) 2009      Citrix Ltd.
 * Author Vincent Hanquez <vincent.hanquez@eu.citrix.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; version 2.1 only. with the special
 * exception on linking described in file LICENSE.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 */

#include "libxl_osdeps.h"

#include <stdio.h>
#include <assert.h>
#include <glob.h>
#include <inttypes.h>
#include <string.h>
#include <sys/time.h> /* for struct timeval */
#include <unistd.h> /* for sleep(2) */

#include <xenctrl.h>
#include <xc_dom.h>
#include <xenguest.h>
#include <fcntl.h>

#include "libxl.h"
#include "libxl_internal.h"

int is_hvm(struct libxl_ctx *ctx, uint32_t domid)
{
    xc_domaininfo_t info;
    int ret;

    ret = xc_domain_getinfolist(ctx->xch, domid, 1, &info);
    if (ret != 1)
        return -1;
    if (info.domain != domid)
        return -1;
    return !!(info.flags & XEN_DOMINF_hvm_guest);
}

int get_shutdown_reason(struct libxl_ctx *ctx, uint32_t domid)
{
    xc_domaininfo_t info;
    int ret;

    ret = xc_domain_getinfolist(ctx->xch, domid, 1, &info);
    if (ret != 1)
        return -1;
    if (info.domain != domid)
        return -1;
    if (!(info.flags & XEN_DOMINF_shutdown))
        return -1;
    return dominfo_get_shutdown_reason(&info);
}

int build_pre(struct libxl_ctx *ctx, uint32_t domid,
              libxl_domain_build_info *info, libxl_domain_build_state *state)
{
    xc_domain_max_vcpus(ctx->xch, domid, info->max_vcpus);
    xc_domain_setmaxmem(ctx->xch, domid, info->target_memkb + LIBXL_MAXMEM_CONSTANT);
    xc_domain_set_memmap_limit(ctx->xch, domid, 
            (info->hvm) ? info->max_memkb : 
            (info->max_memkb + info->u.pv.slack_memkb));
    xc_domain_set_tsc_info(ctx->xch, domid, info->tsc_mode, 0, 0, 0);

    if (info->hvm) {
        unsigned long shadow;
        shadow = (info->shadow_memkb + 1023) / 1024;
        xc_shadow_control(ctx->xch, domid, XEN_DOMCTL_SHADOW_OP_SET_ALLOCATION, NULL, 0, &shadow, 0, NULL);
    }

    state->store_port = xc_evtchn_alloc_unbound(ctx->xch, domid, 0);
    state->console_port = xc_evtchn_alloc_unbound(ctx->xch, domid, 0);
    return 0;
}

int build_post(struct libxl_ctx *ctx, uint32_t domid,
               libxl_domain_build_info *info, libxl_domain_build_state *state,
               char **vms_ents, char **local_ents)
{
    char *dom_path, *vm_path;
    xs_transaction_t t;
    char **ents;
    int i;

#if defined(__i386__) || defined(__x86_64__)
    xc_cpuid_apply_policy(ctx->xch, domid);
#endif

    ents = libxl_calloc(ctx, 12 + (info->max_vcpus * 2) + 2, sizeof(char *));
    ents[0] = "memory/static-max";
    ents[1] = libxl_sprintf(ctx, "%d", info->max_memkb);
    ents[2] = "memory/target";
    ents[3] = libxl_sprintf(ctx, "%d", info->target_memkb);
    ents[4] = "memory/videoram";
    ents[5] = libxl_sprintf(ctx, "%d", info->video_memkb);
    ents[6] = "domid";
    ents[7] = libxl_sprintf(ctx, "%d", domid);
    ents[8] = "store/port";
    ents[9] = libxl_sprintf(ctx, "%"PRIu32, state->store_port);
    ents[10] = "store/ring-ref";
    ents[11] = libxl_sprintf(ctx, "%lu", state->store_mfn);
    for (i = 0; i < info->max_vcpus; i++) {
        ents[12+(i*2)]   = libxl_sprintf(ctx, "cpu/%d/availability", i);
        ents[12+(i*2)+1] = (i && info->cur_vcpus && !(info->cur_vcpus & (1 << i)))
                            ? "offline" : "online";
    }

    dom_path = libxl_xs_get_dompath(ctx, domid);
    if (!dom_path)
        return ERROR_FAIL;

    vm_path = xs_read(ctx->xsh, XBT_NULL, libxl_sprintf(ctx, "%s/vm", dom_path), NULL);
retry_transaction:
    t = xs_transaction_start(ctx->xsh);

    libxl_xs_writev(ctx, t, dom_path, ents);
    libxl_xs_writev(ctx, t, dom_path, local_ents);
    libxl_xs_writev(ctx, t, vm_path, vms_ents);

    if (!xs_transaction_end(ctx->xsh, t, 0))
        if (errno == EAGAIN)
            goto retry_transaction;
    xs_introduce_domain(ctx->xsh, domid, state->store_mfn, state->store_port);
    free(vm_path);
    libxl_free(ctx, ents);
    libxl_free(ctx, dom_path);
    return 0;
}

int build_pv(struct libxl_ctx *ctx, uint32_t domid,
             libxl_domain_build_info *info, libxl_domain_build_state *state)
{
    struct xc_dom_image *dom;
    int ret;
    int flags = 0;

    dom = xc_dom_allocate(info->u.pv.cmdline, info->u.pv.features);
    if (!dom) {
        XL_LOG_ERRNO(ctx, XL_LOG_ERROR, "xc_dom_allocate failed");
        return -1;
    }
    ret = xc_dom_linux_build(ctx->xch, dom, domid, info->target_memkb / 1024,
                                  info->kernel, info->u.pv.ramdisk, flags,
                                  state->store_port, &state->store_mfn,
                                  state->console_port, &state->console_mfn);
    if (ret != 0) {
        xc_dom_release(dom);
        XL_LOG_ERRNOVAL(ctx, XL_LOG_ERROR, ret, "xc_dom_linux_build failed");
        return -2;
    }
    xc_dom_release(dom);
    return 0;
}

int build_hvm(struct libxl_ctx *ctx, uint32_t domid,
              libxl_domain_build_info *info, libxl_domain_build_state *state)
{
    int ret;

    ret = xc_hvm_build_target_mem(
        ctx->xch,
        domid,
        (info->max_memkb - info->video_memkb) / 1024,
        (info->target_memkb - info->video_memkb) / 1024,
        libxl_abs_path(ctx, (char *)info->kernel,
                       libxl_xenfirmwaredir_path()));
    if (ret) {
        XL_LOG_ERRNOVAL(ctx, XL_LOG_ERROR, ret, "hvm building failed");
        return ERROR_FAIL;
    }
    ret = hvm_build_set_params(ctx->xch, domid, info, state->store_port,
                               &state->store_mfn);
    if (ret) {
        XL_LOG_ERRNOVAL(ctx, XL_LOG_ERROR, ret, "hvm build set params failed");
        return ERROR_FAIL;
    }
    return 0;
}

int restore_common(struct libxl_ctx *ctx, uint32_t domid,
                   libxl_domain_build_info *info, libxl_domain_build_state *state,
                   int fd)
{
    /* read signature */
    return xc_domain_restore(ctx->xch, fd, domid,
                             state->store_port, &state->store_mfn,
                             state->console_port, &state->console_mfn,
                             info->hvm, info->u.hvm.pae, 0);
}

struct suspendinfo {
    struct libxl_ctx *ctx;
    int xce; /* event channel handle */
    int suspend_eventchn;
    int domid;
    int hvm;
    unsigned int flags;
};

static void core_suspend_switch_qemu_logdirty(int domid, unsigned int enable)
{
    struct xs_handle *xsh;
    char path[64];

    snprintf(path, sizeof(path), "/local/domain/0/device-model/%u/logdirty/cmd", domid);

    xsh = xs_daemon_open();

    if (enable)
        xs_write(xsh, XBT_NULL, path, "enable", strlen("enable"));
    else
        xs_write(xsh, XBT_NULL, path, "disable", strlen("disable"));

    xs_daemon_close(xsh);
}

static int core_suspend_callback(void *data)
{
    struct suspendinfo *si = data;
    unsigned long s_state = 0;
    int ret;
    char *path, *state = "suspend";
    int watchdog = 60;

    if (si->hvm)
        xc_get_hvm_param(si->ctx->xch, si->domid, HVM_PARAM_ACPI_S_STATE, &s_state);
    if ((s_state == 0) && (si->suspend_eventchn >= 0)) {
        ret = xc_evtchn_notify(si->xce, si->suspend_eventchn);
        if (ret < 0) {
            XL_LOG(si->ctx, XL_LOG_ERROR, "xc_evtchn_notify failed ret=%d", ret);
            return 0;
        }
        ret = xc_await_suspend(si->xce, si->suspend_eventchn);
        if (ret < 0) {
            XL_LOG(si->ctx, XL_LOG_ERROR, "xc_await_suspend failed ret=%d", ret);
            return 0;
        }
        return 1;
    }
    path = libxl_sprintf(si->ctx, "%s/control/shutdown", libxl_xs_get_dompath(si->ctx, si->domid));
    libxl_xs_write(si->ctx, XBT_NULL, path, "suspend", strlen("suspend"));
    if (si->hvm) {
        unsigned long hvm_pvdrv, hvm_s_state;
        xc_get_hvm_param(si->ctx->xch, si->domid, HVM_PARAM_CALLBACK_IRQ, &hvm_pvdrv);
        xc_get_hvm_param(si->ctx->xch, si->domid, HVM_PARAM_ACPI_S_STATE, &hvm_s_state);
        if (!hvm_pvdrv || hvm_s_state) {
            XL_LOG(si->ctx, XL_LOG_DEBUG, "Calling xc_domain_shutdown on the domain");
            xc_domain_shutdown(si->ctx->xch, si->domid, SHUTDOWN_suspend);
        }
    }
    XL_LOG(si->ctx, XL_LOG_DEBUG, "wait for the guest to suspend");
    while (!strcmp(state, "suspend") && watchdog > 0) {
        xc_domaininfo_t info;

        usleep(100000);
        ret = xc_domain_getinfolist(si->ctx->xch, si->domid, 1, &info);
        if (ret == 1 && info.domain == si->domid && info.flags & XEN_DOMINF_shutdown) {
            int shutdown_reason;

            shutdown_reason = (info.flags >> XEN_DOMINF_shutdownshift) & XEN_DOMINF_shutdownmask;
            if (shutdown_reason == SHUTDOWN_suspend)
                return 1;
        }
        state = libxl_xs_read(si->ctx, XBT_NULL, path);
        watchdog--;
    }
    if (!strcmp(state, "suspend")) {
        XL_LOG(si->ctx, XL_LOG_ERROR, "guest didn't suspend in time");
        libxl_xs_write(si->ctx, XBT_NULL, path, "", 1);
    }
    return 1;
}

int core_suspend(struct libxl_ctx *ctx, uint32_t domid, int fd,
		int hvm, int live, int debug)
{
    int flags;
    int port;
    struct save_callbacks callbacks;
    struct suspendinfo si;

    flags = (live) ? XCFLAGS_LIVE : 0
          | (debug) ? XCFLAGS_DEBUG : 0
          | (hvm) ? XCFLAGS_HVM : 0;

    si.domid = domid;
    si.flags = flags;
    si.hvm = hvm;
    si.ctx = ctx;
    si.suspend_eventchn = -1;

    si.xce = xc_evtchn_open();
    if (si.xce < 0)
        return -1;

    if (si.xce > 0) {
        port = xs_suspend_evtchn_port(si.domid);

        if (port < 0) {
            XL_LOG(ctx, XL_LOG_WARNING, "Failed to get the suspend evtchn port");
        } else {
            si.suspend_eventchn = xc_suspend_evtchn_init(si.ctx->xch, si.xce, si.domid, port);

            if (si.suspend_eventchn < 0)
                XL_LOG(ctx, XL_LOG_WARNING, "Suspend event channel initialization failed");
        }
    }

    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.suspend = core_suspend_callback;
    callbacks.data = &si;

    xc_domain_save(ctx->xch, fd, domid, 0, 0, flags,
                   &callbacks, hvm,
                   &core_suspend_switch_qemu_logdirty);

    if (si.suspend_eventchn > 0)
        xc_suspend_evtchn_release(si.xce, si.suspend_eventchn);
    if (si.xce > 0)
        xc_evtchn_close(si.xce);

    return 0;
}

int save_device_model(struct libxl_ctx *ctx, uint32_t domid, int fd)
{
    int fd2, c;
    char buf[1024];
    char *filename = libxl_sprintf(ctx, "/var/lib/xen/qemu-save.%d", domid);

    XL_LOG(ctx, XL_LOG_DEBUG, "Saving device model state to %s", filename);
    libxl_xs_write(ctx, XBT_NULL, libxl_sprintf(ctx, "/local/domain/0/device-model/%d/command", domid), "save", strlen("save"));
    libxl_wait_for_device_model(ctx, domid, "paused", NULL, NULL);

    c = libxl_write_exactly(ctx, fd, QEMU_SIGNATURE, strlen(QEMU_SIGNATURE),
                            "saved-state file", "qemu signature");
    if (c)
        return c;
    fd2 = open(filename, O_RDONLY);
    while ((c = read(fd2, buf, sizeof(buf))) != 0) {
        if (c < 0) {
            if (errno == EINTR)
                continue;
            return errno;
        }
        c = libxl_write_exactly(
            ctx, fd, buf, c, "saved-state file", "qemu state");
        if (c)
            return c;
    }
    close(fd2);
    unlink(filename);
    return 0;
}

char *libxl_uuid2string(struct libxl_ctx *ctx, uint8_t uuid[16]) {
    char *s = string_of_uuid(ctx, uuid);
    if (!s) XL_LOG(ctx, XL_LOG_ERROR, "cannot allocate for uuid");
    return s;
}

static const char *userdata_path(struct libxl_ctx *ctx, uint32_t domid,
                                      const char *userdata_userid,
                                      const char *wh) {
    char *path, *uuid_string;
    struct libxl_dominfo info;
    int rc;

    rc = libxl_domain_info(ctx, &info, domid);
    if (rc) {
        XL_LOG_ERRNO(ctx, XL_LOG_ERROR, "unable to find domain info"
                     " for domain %"PRIu32, domid);
        return 0;
    }
    uuid_string = string_of_uuid(ctx, info.uuid);

    path = libxl_sprintf(ctx, "/var/lib/xen/"
                         "userdata-%s.%s.%s",
                         wh, uuid_string, userdata_userid);
    if (!path)
        XL_LOG_ERRNO(ctx, XL_LOG_ERROR, "unable to allocate for"
                     " userdata path");
    return path;
}

static int userdata_delete(struct libxl_ctx *ctx, const char *path) {
    int r;
    r = unlink(path);
    if (r) {
        XL_LOG_ERRNO(ctx, XL_LOG_ERROR, "remove failed for %s", path);
        return errno;
    }
    return 0;
}

void libxl__userdata_destroyall(struct libxl_ctx *ctx, uint32_t domid) {
    const char *pattern;
    glob_t gl;
    int r, i;

    pattern = userdata_path(ctx, domid, "*", "?");
    if (!pattern) return;

    gl.gl_pathc = 0;
    gl.gl_pathv = 0;
    gl.gl_offs = 0;
    r = glob(pattern, GLOB_ERR|GLOB_NOSORT|GLOB_MARK, 0, &gl);
    if (r == GLOB_NOMATCH) return;
    if (r) XL_LOG_ERRNO(ctx, XL_LOG_ERROR, "glob failed for %s", pattern);

    for (i=0; i<gl.gl_pathc; i++) {
        userdata_delete(ctx, gl.gl_pathv[i]);
    }
    globfree(&gl);
}

int libxl_userdata_store(struct libxl_ctx *ctx, uint32_t domid,
                              const char *userdata_userid,
                              const uint8_t *data, int datalen) {
    const char *filename;
    const char *newfilename;
    int e;
    int fd = -1;
    FILE *f = 0;
    size_t rs;

    filename = userdata_path(ctx, domid, userdata_userid, "d");
    if (!filename) return ENOMEM;

    if (!datalen)
        return userdata_delete(ctx, filename);

    newfilename = userdata_path(ctx, domid, userdata_userid, "n");
    if (!newfilename) return ENOMEM;

    fd= open(newfilename, O_RDWR|O_CREAT|O_TRUNC, 0600);
    if (fd<0) goto xe;

    f= fdopen(fd, "wb");
    if (!f) goto xe;
    fd = -1;

    rs = fwrite(data, 1, datalen, f);
    if (rs != datalen) { assert(ferror(f)); goto xe; }

    if (fclose(f)) goto xe;
    f = 0;

    if (rename(newfilename,filename)) goto xe;

    return 0;

 xe:
    e = errno;
    if (f) fclose(f);
    if (fd>=0) close(fd);

    XL_LOG_ERRNO(ctx, XL_LOG_ERROR, "cannot write %s for %s",
                 newfilename, filename);
    return e;
}

int libxl_userdata_retrieve(struct libxl_ctx *ctx, uint32_t domid,
                                 const char *userdata_userid,
                                 uint8_t **data_r, int *datalen_r) {
    const char *filename;
    int e;
    int datalen = 0;
    void *data = 0;

    filename = userdata_path(ctx, domid, userdata_userid, "d");
    if (!filename) return ENOMEM;

    e = libxl_read_file_contents(ctx, filename, data_r ? &data : 0, &datalen);

    if (!e && !datalen) {
        XL_LOG(ctx, XL_LOG_ERROR, "userdata file %s is empty", filename);
        if (data_r) assert(!*data_r);
        return EPROTO;
    }

    if (data_r) *data_r = data;
    if (datalen_r) *datalen_r = datalen;
    return 0;
}
