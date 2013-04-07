/*
 * QEMU System Emulator
 *
 * Copyright (c) 2003-2008 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu-common.h"
#include "qemu/timer.h"
#include "slirp/slirp.h"
#include "qemu/main-loop.h"
#include "block/aio.h"

#ifndef _WIN32

#include "qemu/compatfd.h"

/* If we have signalfd, we mask out the signals we want to handle and then
 * use signalfd to listen for them.  We rely on whatever the current signal
 * handler is to dispatch the signals when we receive them.
 */
static void sigfd_handler(void *opaque)
{
    int fd = (intptr_t)opaque;
    struct qemu_signalfd_siginfo info;
    struct sigaction action;
    ssize_t len;
    int result;

    while (1) {
        do {
            len = read(fd, &info, sizeof(info));
        } while (len == -1 && errno == EINTR);

        if (len == -1 && errno == EAGAIN) {
            break;
        }

        if (len != sizeof(info)) {
            printf("read from sigfd returned %zd: %m\n", len);
            return;
        }

        result = sigaction(info.ssi_signo, NULL, &action);
        if (result != 0) {
            printf("sigaction() returned: %i", result);
            return;
        }
        if ((action.sa_flags & SA_SIGINFO) && action.sa_sigaction) {
            action.sa_sigaction(info.ssi_signo,
                                (siginfo_t *)&info, NULL);
        } else if (action.sa_handler) {
            action.sa_handler(info.ssi_signo);
        }
    }
}

static int qemu_signal_init(void)
{
    int sigfd;
    sigset_t set;

    /*
     * SIG_IPI must be blocked in the main thread and must not be caught
     * by sigwait() in the signal thread. Otherwise, the cpu thread will
     * not catch it reliably.
     */
    sigemptyset(&set);
    sigaddset(&set, SIG_IPI);
    sigaddset(&set, SIGIO);
    sigaddset(&set, SIGALRM);
    sigaddset(&set, SIGBUS);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    sigdelset(&set, SIG_IPI);
    sigfd = qemu_signalfd(&set);
    if (sigfd == -1) {
        fprintf(stderr, "failed to create signalfd\n");
        return -errno;
    }

    fcntl_setfl(sigfd, O_NONBLOCK);

    qemu_set_fd_handler2(sigfd, NULL, sigfd_handler, NULL,
                         (void *)(intptr_t)sigfd);

    return 0;
}

#else /* _WIN32 */

static int qemu_signal_init(void)
{
    return 0;
}
#endif

static AioContext *qemu_aio_context;

void qemu_notify_event(void)
{
    if (!qemu_aio_context) {
        return;
    }
    aio_notify(qemu_aio_context);
}

int qemu_init_main_loop(void)
{
    int ret;
    GSource *src;

    init_clocks();
    if (init_timer_alarm() < 0) {
        fprintf(stderr, "could not initialize alarm timer\n");
        exit(1);
    }

    ret = qemu_signal_init();
    if (ret) {
        return ret;
    }

    qemu_aio_context = aio_context_new();
    src = aio_get_g_source(qemu_aio_context);
    g_source_attach(src, NULL);
    g_source_unref(src);
    return 0;
}

static fd_set rfds, wfds, xfds;
static int nfds;
static GPollFD poll_fds[1024 * 2]; /* this is probably overkill */
static int n_poll_fds;
static int max_priority;

#ifndef _WIN32
static void glib_select_fill(int *max_fd, fd_set *rfds, fd_set *wfds,
                             fd_set *xfds, uint32_t *cur_timeout)
{
    GMainContext *context = g_main_context_default();
    int i;
    int timeout = 0;

    g_main_context_prepare(context, &max_priority);

    n_poll_fds = g_main_context_query(context, max_priority, &timeout,
                                      poll_fds, ARRAY_SIZE(poll_fds));
    g_assert(n_poll_fds <= ARRAY_SIZE(poll_fds));

    for (i = 0; i < n_poll_fds; i++) {
        GPollFD *p = &poll_fds[i];

        if ((p->events & G_IO_IN)) {
            FD_SET(p->fd, rfds);
            *max_fd = MAX(*max_fd, p->fd);
        }
        if ((p->events & G_IO_OUT)) {
            FD_SET(p->fd, wfds);
            *max_fd = MAX(*max_fd, p->fd);
        }
        if ((p->events & G_IO_ERR)) {
            FD_SET(p->fd, xfds);
            *max_fd = MAX(*max_fd, p->fd);
        }
    }

    if (timeout >= 0 && timeout < *cur_timeout) {
        *cur_timeout = timeout;
    }
}

static void glib_select_poll(fd_set *rfds, fd_set *wfds, fd_set *xfds,
                             bool err)
{
    GMainContext *context = g_main_context_default();

    if (!err) {
        int i;

        for (i = 0; i < n_poll_fds; i++) {
            GPollFD *p = &poll_fds[i];

            if ((p->events & G_IO_IN) && FD_ISSET(p->fd, rfds)) {
                p->revents |= G_IO_IN;
            }
            if ((p->events & G_IO_OUT) && FD_ISSET(p->fd, wfds)) {
                p->revents |= G_IO_OUT;
            }
            if ((p->events & G_IO_ERR) && FD_ISSET(p->fd, xfds)) {
                p->revents |= G_IO_ERR;
            }
        }
    }

    if (g_main_context_check(context, max_priority, poll_fds, n_poll_fds)) {
        g_main_context_dispatch(context);
    }
}

static int os_host_main_loop_wait(uint32_t timeout)
{
    struct timeval tv, *tvarg = NULL;
    int ret;

    glib_select_fill(&nfds, &rfds, &wfds, &xfds, &timeout);

    if (timeout < UINT32_MAX) {
        tvarg = &tv;
        tv.tv_sec = timeout / 1000;
        tv.tv_usec = (timeout % 1000) * 1000;
    }

    if (timeout > 0) {
        qemu_mutex_unlock_iothread();
    }

    ret = select(nfds + 1, &rfds, &wfds, &xfds, tvarg);

    if (timeout > 0) {
        qemu_mutex_lock_iothread();
    }

    glib_select_poll(&rfds, &wfds, &xfds, (ret < 0));
    return ret;
}
#else
/***********************************************************/
/* Polling handling */

typedef struct PollingEntry {
    PollingFunc *func;
    void *opaque;
    struct PollingEntry *next;
} PollingEntry;

static PollingEntry *first_polling_entry;

int qemu_add_polling_cb(PollingFunc *func, void *opaque)
{
    PollingEntry **ppe, *pe;
    pe = g_malloc0(sizeof(PollingEntry));
    pe->func = func;
    pe->opaque = opaque;
    for(ppe = &first_polling_entry; *ppe != NULL; ppe = &(*ppe)->next);
    *ppe = pe;
    return 0;
}

void qemu_del_polling_cb(PollingFunc *func, void *opaque)
{
    PollingEntry **ppe, *pe;
    for(ppe = &first_polling_entry; *ppe != NULL; ppe = &(*ppe)->next) {
        pe = *ppe;
        if (pe->func == func && pe->opaque == opaque) {
            *ppe = pe->next;
            g_free(pe);
            break;
        }
    }
}

/***********************************************************/
/* Wait objects support */
typedef struct WaitObjects {
    int num;
    int revents[MAXIMUM_WAIT_OBJECTS + 1];
    HANDLE events[MAXIMUM_WAIT_OBJECTS + 1];
    WaitObjectFunc *func[MAXIMUM_WAIT_OBJECTS + 1];
    void *opaque[MAXIMUM_WAIT_OBJECTS + 1];
} WaitObjects;

static WaitObjects wait_objects = {0};

int qemu_add_wait_object(HANDLE handle, WaitObjectFunc *func, void *opaque)
{
    WaitObjects *w = &wait_objects;
    if (w->num >= MAXIMUM_WAIT_OBJECTS) {
        return -1;
    }
    w->events[w->num] = handle;
    w->func[w->num] = func;
    w->opaque[w->num] = opaque;
    w->revents[w->num] = 0;
    w->num++;
    return 0;
}

void qemu_del_wait_object(HANDLE handle, WaitObjectFunc *func, void *opaque)
{
    int i, found;
    WaitObjects *w = &wait_objects;

    found = 0;
    for (i = 0; i < w->num; i++) {
        if (w->events[i] == handle) {
            found = 1;
        }
        if (found) {
            w->events[i] = w->events[i + 1];
            w->func[i] = w->func[i + 1];
            w->opaque[i] = w->opaque[i + 1];
            w->revents[i] = w->revents[i + 1];
        }
    }
    if (found) {
        w->num--;
    }
}

void qemu_fd_register(int fd)
{
    WSAEventSelect(fd, event_notifier_get_handle(&qemu_aio_context->notifier),
                   FD_READ | FD_ACCEPT | FD_CLOSE |
                   FD_CONNECT | FD_WRITE | FD_OOB);
}

static int os_host_main_loop_wait(uint32_t timeout)
{
    GMainContext *context = g_main_context_default();
    int select_ret, g_poll_ret, ret, i;
    PollingEntry *pe;
    WaitObjects *w = &wait_objects;
    gint poll_timeout;
    static struct timeval tv0;

    /* XXX: need to suppress polling by better using win32 events */
    ret = 0;
    for (pe = first_polling_entry; pe != NULL; pe = pe->next) {
        ret |= pe->func(pe->opaque);
    }
    if (ret != 0) {
        return ret;
    }

    g_main_context_prepare(context, &max_priority);
    n_poll_fds = g_main_context_query(context, max_priority, &poll_timeout,
                                      poll_fds, ARRAY_SIZE(poll_fds));
    g_assert(n_poll_fds <= ARRAY_SIZE(poll_fds));

    for (i = 0; i < w->num; i++) {
        poll_fds[n_poll_fds + i].fd = (DWORD_PTR)w->events[i];
        poll_fds[n_poll_fds + i].events = G_IO_IN;
    }

    if (poll_timeout < 0 || timeout < poll_timeout) {
        poll_timeout = timeout;
    }

    qemu_mutex_unlock_iothread();
    g_poll_ret = g_poll(poll_fds, n_poll_fds + w->num, poll_timeout);
    qemu_mutex_lock_iothread();
    if (g_poll_ret > 0) {
        for (i = 0; i < w->num; i++) {
            w->revents[i] = poll_fds[n_poll_fds + i].revents;
        }
        for (i = 0; i < w->num; i++) {
            if (w->revents[i] && w->func[i]) {
                w->func[i](w->opaque[i]);
            }
        }
    }

    if (g_main_context_check(context, max_priority, poll_fds, n_poll_fds)) {
        g_main_context_dispatch(context);
    }

    /* Call select after g_poll to avoid a useless iteration and therefore
     * improve socket latency.
     */

    if (nfds >= 0) {
        select_ret = select(nfds + 1, &rfds, &wfds, &xfds, &tv0);
        if (select_ret != 0) {
            timeout = 0;
        }
    }

    return select_ret || g_poll_ret;
}
#endif

int main_loop_wait(int nonblocking)
{
    int ret;
    uint32_t timeout = UINT32_MAX;

    if (nonblocking) {
        timeout = 0;
    }

    /* poll any events */
    /* XXX: separate device handlers from system ones */
    nfds = -1;
    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    FD_ZERO(&xfds);

#ifdef CONFIG_SLIRP
    slirp_update_timeout(&timeout);
    slirp_select_fill(&nfds, &rfds, &wfds, &xfds);
#endif
    qemu_iohandler_fill(&nfds, &rfds, &wfds, &xfds);
    ret = os_host_main_loop_wait(timeout);
    qemu_iohandler_poll(&rfds, &wfds, &xfds, ret);
#ifdef CONFIG_SLIRP
    slirp_select_poll(&rfds, &wfds, &xfds, (ret < 0));
#endif

    qemu_run_all_timers();

    return ret;
}

/* Functions to operate on the main QEMU AioContext.  */

QEMUBH *qemu_bh_new(QEMUBHFunc *cb, void *opaque)
{
    return aio_bh_new(qemu_aio_context, cb, opaque);
}

bool qemu_aio_wait(void)
{
    return aio_poll(qemu_aio_context, true);
}

#ifdef CONFIG_POSIX
void qemu_aio_set_fd_handler(int fd,
                             IOHandler *io_read,
                             IOHandler *io_write,
                             AioFlushHandler *io_flush,
                             void *opaque)
{
    aio_set_fd_handler(qemu_aio_context, fd, io_read, io_write, io_flush,
                       opaque);
}
#endif

void qemu_aio_set_event_notifier(EventNotifier *notifier,
                                 EventNotifierHandler *io_read,
                                 AioFlushEventNotifierHandler *io_flush)
{
    aio_set_event_notifier(qemu_aio_context, notifier, io_read, io_flush);
}
