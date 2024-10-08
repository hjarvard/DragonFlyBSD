/*
 * Copyright (c) 1982, 1986, 1989, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	@(#)file.h	8.3 (Berkeley) 1/9/95
 * $FreeBSD: src/sys/sys/file.h,v 1.22.2.7 2002/11/21 23:39:24 sam Exp $
 */

#ifndef _SYS_FILE_H_
#define	_SYS_FILE_H_

#ifndef _SYS_TYPES_H_
#include <sys/types.h>
#endif
#ifndef _SYS_FCNTL_H_
#include <sys/fcntl.h>
#endif
#ifndef _SYS_UNISTD_H_
#include <sys/unistd.h>
#endif

#if defined(_KERNEL) || defined(_KERNEL_STRUCTURES)

#ifndef _SYS_EVENT_H_
#include <sys/event.h>
#endif
#ifndef _SYS_QUEUE_H_
#include <sys/queue.h>
#endif
#ifndef _SYS_SPINLOCK_H_
#include <sys/spinlock.h>
#endif
#ifndef _SYS_NAMECACHE_H_
#include <sys/namecache.h>
#endif
#ifndef _SYS__UIO_H_
#include <sys/_uio.h>
#endif

struct stat;
struct proc;
struct thread;
struct uio;
struct knote;
struct file;
struct ucred;
struct vnode;
struct lwkt_port;
struct namecache;
struct sysmsg;

struct	fileops {
	int (*fo_read)	(struct file *fp, struct uio *uio,
			 struct ucred *cred, int flags);
	int (*fo_write)	(struct file *fp, struct uio *uio,
			 struct ucred *cred, int flags);
	int (*fo_ioctl)	(struct file *fp, u_long com, caddr_t data,
			 struct ucred *cred, struct sysmsg *msg);
	int (*fo_kqfilter)(struct file *fp, struct knote *kn);
	int (*fo_stat)	(struct file *fp, struct stat *sb,
			 struct ucred *cred);
	int (*fo_close)	(struct file *fp);
	int (*fo_shutdown)(struct file *fp, int how);
	int (*fo_seek)	(struct file *fp, off_t offset, int whence, off_t *res);
};

/*
 * Kernel descriptor table - One entry for each open kernel vnode and socket.
 *
 * (A) - (filehead_spin) - descriptor subsystems only (kern/kern_descrip.c)
 * (U) - (unp_spin) 	 - uipc subsystems only (kern/uipc_usrreq.c)
 * (ro)- these fields may be read without holding a spinlock as long as you
 *       have (or know) that the reference to the fp is going to stay put.
 * ?   - remaining fields have to be spinlocked
 */
struct file {
	LIST_ENTRY(file) f_list;/* (A) list of active files */
	short	f_FILLER3;
	short	f_type;		/* (ro) descriptor type */
	u_int	f_flag;		/* see fcntl.h */
	struct	ucred *f_cred;	/* (ro) creds associated with descriptor */
	struct  fileops *f_ops;	/* (ro) operations vector */
	int	f_seqcount;	/*
				 * count of sequential accesses -- cleared
				 * by most seek operations.
				 */
	off_t	f_nextoff;	/*
				 * offset of next expected read or write
				 */
	off_t	f_offset;
	void   *f_data;		/* vnode, pipe, socket, or kqueue */
	void   *f_data1;	/* devfs per-file data */
	int	f_count;	/* reference count */
	int	f_msgcount;	/* (U) reference count from message queue */
	struct nchandle f_nchandle; /* namecache reference */
	struct spinlock f_spin;	/* NOT USED */
	struct klist 	f_klist;/* knotes attached to fp/kq */
	void   *private_data;	/* Linux (drm) per-file data */
};

#define	DTYPE_VNODE	1	/* file */
#define	DTYPE_SOCKET	2	/* communications endpoint */
#define	DTYPE_PIPE	3	/* pipe */
#define	DTYPE_FIFO	4	/* fifo (named pipe) */
#define	DTYPE_KQUEUE	5	/* event queue */
#define DTYPE_CRYPTO	6	/* crypto */
#define DTYPE_MQUEUE	7	/* message queue */
#define DTYPE_DMABUF	8	/* DRM DMA buffer */

LIST_HEAD(filelist, file);

#endif

#ifdef _KERNEL

void fhold(struct file *);
int fdrop(struct file *);
int checkfdclosed(thread_t, struct filedesc *, int, struct file *, int);
int fp_open(const char *, int, int, struct file **);
int fp_vpopen(struct vnode *, int, struct file **);
int fp_pread(struct file *, void *, size_t, off_t, ssize_t *, enum uio_seg);
int fp_pwrite(struct file *, void *, size_t, off_t, ssize_t *, enum uio_seg);
int fp_read(struct file *, void *, size_t, ssize_t *, int, enum uio_seg);
int fp_write(struct file *, void *, size_t, ssize_t *, enum uio_seg);
int fp_stat(struct file *, struct stat *);
int fp_mmap(void *, size_t, int, int, struct file *, off_t, void **);

int nofo_shutdown(struct file *, int);

int fp_close(struct file *);
int fp_shutdown(struct file *, int);

extern struct fileops vnode_fileops;
extern struct fileops specvnode_fileops;
extern struct fileops badfileops;
extern int maxfiles;		/* kernel limit on number of open files */
extern int maxfilesrootres;	/* descriptors reserved for root use */
extern int minfilesperproc;	/* minimum (safety) open files per proc */
extern int maxfilesperproc;	/* per process limit on number of open files */
extern int maxfilesperuser;	/* per user limit on number of open files */

/* Commonly used fileops */
int badfo_readwrite(struct file *fp, struct uio *uio,
		    struct ucred *cred, int flags);
int badfo_ioctl(struct file *fp, u_long com, caddr_t data,
		struct ucred *cred, struct sysmsg *msg);
int badfo_kqfilter(struct file *fp, struct knote *kn);
int badfo_stat(struct file *fp, struct stat *sb, struct ucred *cred);
int badfo_close(struct file *fp);
int badfo_shutdown(struct file *fp, int how);
int badfo_seek(struct file *fp, off_t offset, int whence, off_t *res);

#endif /* _KERNEL */

#endif /* !SYS_FILE_H */
