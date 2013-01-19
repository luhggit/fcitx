/***************************************************************************
 *   Copyright (C) 2013~2013 by Yichao Yu                                  *
 *   yyc1992@gmail.com                                                     *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.              *
 ***************************************************************************/

#include <unistd.h>
#include <errno.h>

#include <fcitx/module.h>
#include <fcitx-utils/log.h>
#include "wayland-internal.h"
#include "wayland-global.h"
#include "epoll-utils.h"

static void* FxWaylandCreate(FcitxInstance *instance);
static void FxWaylandSetFD(void *self);
static void FxWaylandProcessEvent(void *self);
static void FxWaylandDestroy(void *self);
DECLARE_ADDFUNCTIONS(Wayland)

FCITX_DEFINE_PLUGIN(fcitx_wayland, module, FcitxModule) = {
    .Create = FxWaylandCreate,
    .SetFD = FxWaylandSetFD,
    .ProcessEvent = FxWaylandProcessEvent,
    .Destroy = FxWaylandDestroy,
    .ReloadConfig = NULL
};

static void
FxWaylandExit(FcitxWayland *wl)
{
    fx_epoll_del_task(wl->epoll_fd, &wl->dpy_task);
    // TODO
}

static void
FxWaylandScheduleFlush(FcitxWayland *wl)
{
    if (wl->scheduled_flush)
        return;
    wl->scheduled_flush = true;
    fx_epoll_mod_task(wl->epoll_fd, &wl->dpy_task,
                      EPOLLIN | EPOLLOUT | EPOLLERR | EPOLLHUP);
}

static void
FxWaylandDisplayTaskHandler(FcitxWaylandTask *task, uint32_t events)
{
    FcitxWayland *wl = fcitx_container_of(task, FcitxWayland, dpy_task);
    int ret;
    if (events & EPOLLERR || events & EPOLLHUP) {
        FxWaylandExit(wl);
        return;
    }
    if (events & EPOLLIN) {
        ret = wl_display_dispatch(wl->dpy);
        if (ret == -1) {
            FxWaylandExit(wl);
            return;
        }
    }
    if (events & EPOLLOUT) {
        ret = wl_display_flush(wl->dpy);
        if (ret == 0) {
            wl->scheduled_flush = false;
            fx_epoll_mod_task(wl->epoll_fd, &wl->dpy_task,
                              EPOLLIN | EPOLLERR | EPOLLHUP);
        } else if (ret == -1 && errno != EAGAIN) {
            FxWaylandExit(wl);
            return;
        }
    }
}

void
FcitxWaylandLogFunc(const char *fmt, va_list ap)
{
    /* all of the log's (at least on the client side) are errors. */
    FcitxLogFuncV(FCITX_ERROR, __FILE__, __LINE__, fmt, ap);
}

static void*
FxWaylandCreate(FcitxInstance *instance)
{
    FcitxWayland *wl = fcitx_utils_new(FcitxWayland);
    wl_log_set_handler_client(FcitxWaylandLogFunc);

    wl->owner = instance;
    wl->dpy = wl_display_connect(NULL);
    if (!wl->dpy)
        goto free;

    wl->epoll_fd = fx_epoll_create_cloexec();
    if (fcitx_unlikely(wl->epoll_fd < 0))
        goto disconnect;

    wl->dpy_task.fd = wl_display_get_fd(wl->dpy);
    wl->dpy_task.handler = FxWaylandDisplayTaskHandler;
    if (fcitx_unlikely(fx_epoll_add_task(wl->epoll_fd, &wl->dpy_task,
                                         EPOLLIN | EPOLLERR | EPOLLHUP) == -1))
        goto close_epoll;

    wl->registry = wl_display_get_registry(wl->dpy);
    if (fcitx_unlikely(!wl->registry))
        goto close_epoll;
    if (fcitx_unlikely(!FxWaylandGlobalInit(wl)))
        goto destroy_registry;

    FxWaylandScheduleFlush(wl);
    FcitxWaylandAddFunctions(instance);
    return wl;
destroy_registry:
    wl_registry_destroy(wl->registry);
close_epoll:
    close(wl->epoll_fd);
disconnect:
    wl_display_disconnect(wl->dpy);
free:
    free(wl);
    return NULL;
}

static void
FxWaylandDestroy(void *self)
{
    FcitxWayland *wl = (FcitxWayland*)self;
    wl_registry_destroy(wl->registry);
    close(wl->epoll_fd);
    wl_display_disconnect(wl->dpy);
    free(self);
}

static void
FxWaylandSetFD(void *self)
{
    FcitxWayland *wl = (FcitxWayland*)self;
    int fd = wl->epoll_fd;
    FcitxInstance *instance = wl->owner;
    FD_SET(fd, FcitxInstanceGetReadFDSet(instance));

    if (FcitxInstanceGetMaxFD(instance) < fd) {
        FcitxInstanceSetMaxFD(instance, fd);
    }
}

static void
FxWaylandProcessEvent(void *self)
{
    FcitxWayland *wl = (FcitxWayland*)self;
    fx_epoll_dispatch(wl->epoll_fd);
    int ret = wl_display_flush(wl->dpy);
    if (ret != 0) {
        FxWaylandScheduleFlush(wl);
    }
}

#include "fcitx-wayland-addfunctions.h"
