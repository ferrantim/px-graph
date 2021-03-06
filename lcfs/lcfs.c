#include "includes.h"

static struct gfs *gfs;
extern struct fuse_lowlevel_ops lc_ll_oper;

#define LC_SIZEOF_MOUNTARGS 1024

/* Return global file system */
struct gfs *
getfs() {
    return gfs;
}

/* Display usage */
static void
usage(char *prog) {
    printf("usage: %s <device> <mnt> <mnt2> [-d] [-f]\n", prog);
}

/* Data passed to the duplicate thread */
struct fuseData {

    /* Fuse session */
    struct fuse_session *fd_se;

#ifndef FUSE3
    /* Fuse channel */
    struct fuse_chan *fd_ch;
#endif

    /* Mount point */
    char *fd_mountpoint;

    /* Global file system */
    struct gfs *fd_gfs;

    /* Run in foreground or not */
    int fd_foreground;

    /* Set if running as a thread */
    bool fd_thread;
};

/* Number of mount points device mounted */
#define LC_MAX_MOUNTS   2
static struct fuseData fd[LC_MAX_MOUNTS];

/* Serve file system requests */
static void *
lc_serve(void *data) {
    struct fuseData *fd = (struct fuseData *)data;
    struct gfs *gfs = fd->fd_gfs;
    pthread_t flusher;
    int err;

    if (!fd->fd_thread) {
        if (fuse_set_signal_handlers(fd->fd_se) == -1) {
            printf("Error setting signal handlers\n");
            err = EPERM;
            goto out;
        }
        err = pthread_create(&flusher, NULL, lc_flusher, NULL);
        if (err) {
            printf("Flusher thread could not be created, err %d\n", err);
            goto out;
        }
    }
#ifdef FUSE3
    fuse_session_mount(fd->fd_se, fd->fd_mountpoint);
#else
    fuse_session_add_chan(fd->fd_se, fd->fd_ch);
#endif
    fuse_daemonize(fd->fd_foreground);
    err = fuse_session_loop_mt(fd->fd_se
#ifdef FUSE3
    /* XXX Experiment with clone fd argument */
                               , 0);
#else
                               );
    fuse_session_remove_chan(fd->fd_ch);
#endif

out:
    if (!fd->fd_thread) {
        fuse_remove_signal_handlers(gfs->gfs_se);
        if (!err) {
            pthread_cancel(flusher);
            pthread_join(flusher, NULL);
        }
    } else {
        fuse_session_exit(gfs->gfs_se);
    }
#ifdef FUSE3
    fuse_session_unmount(fd->fd_se);
#endif
    fuse_session_destroy(fd->fd_se);
#ifndef FUSE3
    fuse_unmount(fd->fd_mountpoint, fd->fd_ch);
#endif
    lc_free(NULL, fd->fd_mountpoint, 0, LC_MEMTYPE_GFS);
    return NULL;
}

/* Mount a device at the specified mount point */
static int
lc_fuseMount(struct gfs *gfs, char **arg, char *device, int argc,
             bool thread) {
    struct fuse_args args = FUSE_ARGS_INIT(argc, arg);
    struct fuse_session *se;
    int id = thread ? 0 : 1;
    char *mountpoint = NULL;
    int err, foreground;
    pthread_t dup;
#ifdef FUSE3
    struct fuse_cmdline_opts opts;

    err = fuse_parse_cmdline(&args, &opts);
    if (err == -1) {
        fuse_cmdline_help();
        //fuse_lowlevel_help();
        err = EINVAL;
        goto out;
    }
    mountpoint = opts.mountpoint;
    foreground = opts.foreground;
    if (opts.show_help) {
        fuse_cmdline_help();
        //fuse_lowlevel_help();
        goto out;
    }
    if (opts.show_version) {
        printf("FUSE library version %s\n", fuse_pkgversion());
        fuse_lowlevel_version();
        goto out;
    }
    se = fuse_session_new
#else
    struct fuse_chan *ch = NULL;

    err = fuse_parse_cmdline(&args, &mountpoint, NULL, &foreground);
    if (err == -1) {
        err = EINVAL;
        goto out;
    }
    ch = fuse_mount(mountpoint, &args);
    if (!ch) {
        err = EINVAL;
        goto out;
    }
    fd[id].fd_ch = ch;
    se = fuse_lowlevel_new
#endif
                          (&args, &lc_ll_oper, sizeof(lc_ll_oper),
                           id ? gfs : NULL);
    if (!se) {
        err = EINVAL;
        goto out;
    }
    if (id == 1) {
        gfs->gfs_se = se;
#ifndef FUSE3
        gfs->gfs_ch = ch;
#endif
    }
    fd[id].fd_gfs = gfs;
    fd[id].fd_se = se;
    fd[id].fd_thread = thread;
    fd[id].fd_foreground = foreground;
    fd[id].fd_mountpoint = mountpoint;
    if (thread) {
        err = pthread_create(&dup, NULL, lc_serve, &fd[id]);
        if (!err) {
            printf("%s mounted at %s\n", device, mountpoint);
        }
    } else {
        printf("%s mounted at %s\n", device, mountpoint);
        lc_serve(&fd[id]);
    }
    mountpoint = NULL;

out:
    if (mountpoint) {
        lc_free(NULL, mountpoint, 0, LC_MEMTYPE_GFS);
    }
    fuse_opt_free_args(&args);
    return err;
}

/* Mount the specified device and start serving requests */
int
main(int argc, char *argv[]) {
    char *arg[argc + 1];
    int i, err = -1;
    struct stat st;

#ifdef FUSE3
    if (argc < 4) {
#else
    if ((argc < 4) || (argc > 6)) {
#endif
        usage(argv[0]);
        exit(EINVAL);
    }

    if (!strcmp(argv[2], argv[3])) {
        printf("Specify different mount points\n");
        usage(argv[0]);
        exit(EINVAL);
    }

    /* Make sure mount points exist */
    if (stat(argv[2], &st) || stat(argv[3], &st)) {
        perror("stat");
        usage(argv[0]);
        exit(errno);
    }

    /* XXX Block signals around lc_mount/lc_unmount calls */
    err = lc_mount(argv[1], &gfs);
    if (err) {
        printf("Mounting %s failed, err %d\n", argv[1], err);
        exit(err);
    }

    /* Setup arguments for fuse mount */
    arg[0] = argv[0];
    arg[1] = argv[2];
    arg[2] = "-o";
    arg[3] = lc_malloc(NULL, LC_SIZEOF_MOUNTARGS, LC_MEMTYPE_GFS);
    sprintf(arg[3], "allow_other,auto_unmount,noatime,subtype=lcfs,fsname=%s,"
#ifndef FUSE3
            "nonempty,atomic_o_trunc,big_writes,"
            "splice_move,splice_read,splice_write,"
#endif
            "default_permissions", argv[1]);
    for (i = 4; i < argc; i++) {
        arg[i] = argv[i];
    }

    /* Mount the device at given mount points */
    err = lc_fuseMount(gfs, arg, argv[1], argc, true);
    if (!err) {
        arg[1] = argv[3];
        lc_fuseMount(gfs, arg, argv[1], argc, false);
    }
    lc_free(NULL, arg[3], LC_SIZEOF_MOUNTARGS, LC_MEMTYPE_GFS);
    lc_free(NULL, gfs, sizeof(struct gfs), LC_MEMTYPE_GFS);
    printf("%s unmounted\n", argv[1]);
    lc_displayGlobalMemStats();
    return err ? 1 : 0;
}
