/*
 * Copyright (c) 1994, Sean Eric Fagan
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by Sean Eric Fagan.
 * 4. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $FreeBSD: src/sys/kern/sys_process.c,v 1.51.2.6 2003/01/08 03:06:45 kan Exp $
 */

#include "sys/select.h"
#include "sys/signal.h"
#include "sys/signalvar.h"
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/sysmsg.h>
#include <sys/uio.h>
#include <sys/proc.h>
#include <sys/caps.h>
#include <sys/vnode.h>
#include <sys/ptrace.h>
#include <sys/reg.h>
#include <sys/lock.h>
#include <sys/types.h>
#include <sys/malloc.h>

#include <vm/vm.h>
#include <vm/pmap.h>
#include <vm/vm_map.h>
#include <vm/vm_page.h>

#include <vfs/procfs/procfs.h>

#include <sys/thread2.h>
#include <sys/spinlock2.h>
#include <sys/signal2.h>

/* use the equivalent procfs code */
#if 0
static int
pread (struct proc *procp, unsigned int addr, unsigned int *retval)
{
	int		rv;
	vm_map_t	map, tmap;
	vm_object_t	object;
	vm_map_backing_t ba;
	vm_offset_t	kva = 0;
	int		page_offset;	/* offset into page */
	vm_offset_t	pageno;		/* page number */
	vm_map_entry_t	out_entry;
	vm_prot_t	out_prot;
	int		wflags;
	vm_pindex_t	pindex;
	vm_pindex_t	pcount;

	/* Map page into kernel space */

	map = &procp->p_vmspace->vm_map;

	page_offset = addr - trunc_page(addr);
	pageno = trunc_page(addr);

	tmap = map;
	rv = vm_map_lookup(&tmap, pageno, VM_PROT_READ, &out_entry,
			   &ba, &pindex, &pcount, &out_prot, &wflags);
	if (ba)
		object = ba->object;
	else
		object = NULL;


	if (rv != KERN_SUCCESS)
		return EINVAL;

	vm_map_lookup_done (tmap, out_entry, 0);

	/* Find space in kernel_map for the page we're interested in */
	rv = vm_map_find (kernel_map, object, NULL,
			  IDX_TO_OFF(pindex), &kva, PAGE_SIZE,
			  PAGE_SIZE, FALSE,
			  VM_MAPTYPE_NORMAL, VM_SUBSYS_PROC,
			  VM_PROT_ALL, VM_PROT_ALL, 0);

	if (!rv) {
		vm_object_reference XXX (object);

		/* wire the pages */
		rv = vm_map_kernel_wiring(kernel_map, kva, kva + PAGE_SIZE, 0);
		if (!rv) {
			*retval = 0;
			bcopy ((caddr_t)kva + page_offset,
			       retval, sizeof *retval);
		}
		vm_map_remove (kernel_map, kva, kva + PAGE_SIZE);
	}

	return rv;
}

static int
pwrite (struct proc *procp, unsigned int addr, unsigned int datum)
{
	int		rv;
	vm_map_t	map, tmap;
	vm_object_t	object;
	vm_map_backing_t ba;
	vm_offset_t	kva = 0;
	int		page_offset;	/* offset into page */
	vm_offset_t	pageno;		/* page number */
	vm_map_entry_t	out_entry;
	vm_prot_t	out_prot;
	int		wflags;
	vm_pindex_t	pindex;
	vm_pindex_t	pcount;
	boolean_t	fix_prot = 0;

	/* Map page into kernel space */

	map = &procp->p_vmspace->vm_map;

	page_offset = addr - trunc_page(addr);
	pageno = trunc_page(addr);

	/*
	 * Check the permissions for the area we're interested in.
	 */

	if (vm_map_check_protection (map, pageno, pageno + PAGE_SIZE,
				     VM_PROT_WRITE, FALSE) == FALSE) {
		/*
		 * If the page was not writable, we make it so.
		 * XXX It is possible a page may *not* be read/executable,
		 * if a process changes that!
		 */
		fix_prot = 1;
		/* The page isn't writable, so let's try making it so... */
		if ((rv = vm_map_protect (map, pageno, pageno + PAGE_SIZE,
			VM_PROT_ALL, 0)) != KERN_SUCCESS)
		  return EFAULT;	/* I guess... */
	}

	/*
	 * Now we need to get the page.  out_entry, out_prot, wflags, and
	 * single_use aren't used.  One would think the vm code would be
	 * a *bit* nicer...  We use tmap because vm_map_lookup() can
	 * change the map argument.
	 */

	tmap = map;
	rv = vm_map_lookup(&tmap, pageno, VM_PROT_WRITE, &out_entry,
			   &ba, &pindex, &pcount, &out_prot, &wflags);
	if (ba)
		object = ba->object;
	else
		object = NULL;

	if (rv != KERN_SUCCESS)
		return EINVAL;

	/*
	 * Okay, we've got the page.  Let's release tmap.
	 */
	vm_map_lookup_done (tmap, out_entry, 0);

	/*
	 * Fault the page in...
	 */
	rv = vm_fault(map, pageno, VM_PROT_WRITE|VM_PROT_READ, FALSE);
	if (rv != KERN_SUCCESS)
		return EFAULT;

	/* Find space in kernel_map for the page we're interested in */
	rv = vm_map_find (kernel_map, object, NULL,
			  IDX_TO_OFF(pindex), &kva, PAGE_SIZE,
			  PAGE_SIZE, FALSE,
			  VM_MAPTYPE_NORMAL, VM_SUBSYS_PROC,
			  VM_PROT_ALL, VM_PROT_ALL, 0);
	if (!rv) {
		vm_object_reference XXX (object);

		/* wire the pages */
		rv = vm_map_kernel_wiring(kernel_map, kva, kva + PAGE_SIZE, 0);
		if (!rv) {
		  bcopy (&datum, (caddr_t)kva + page_offset, sizeof datum);
		}
		vm_map_remove (kernel_map, kva, kva + PAGE_SIZE);
	}

	if (fix_prot)
		vm_map_protect (map, pageno, pageno + PAGE_SIZE,
			VM_PROT_READ|VM_PROT_EXECUTE, 0);
	return rv;
}
#endif

static inline int
req_is_valid(int req)
{
	if (PT_REQ_IS_GENERIC(req))
		return 1;

#ifdef PT_LASTMACH
	if (PT_REQ_IS_MACH(req))
		return 1;
#endif

	return 0;
}

/*
 * Permissions check. Returns non-zero on error.
 * Called with PHOLD(p) and lwkt token held.
 * Request id must be valid.
 */
static int
ptrace_check_perms(struct proc *curp, struct proc *p, int req)
{
	struct proc *pp;
	int error;

	if (req == PT_TRACE_ME) {
		/* Always legal. */
		return 0;
	} else if (req == PT_ATTACH) {
		/* Self */
		if (p->p_pid == curp->p_pid) {
			return EINVAL;
		}

		/* Already traced */
		if (p->p_flags & P_TRACED) {
			return EBUSY;
		}

		if (curp->p_flags & P_TRACED) {
			for (pp = curp->p_pptr; pp != NULL; pp = pp->p_pptr) {
				if (pp == p) {
					return EINVAL;
				}
			}
		}

		/* not owned by you, has done setuid (unless you're root) */
		if ((p->p_ucred->cr_ruid != curp->p_ucred->cr_ruid) ||
		     (p->p_flags & P_SUGID)) {
			error = caps_priv_check(curp->p_ucred,
						SYSCAP_RESTRICTEDROOT);
			if (error) {
				return error;
			}
		}

		/* can't trace init when securelevel > 0 */
		if (securelevel > 0 && p->p_pid == 1) {
			return EPERM;
		}

		/* OK */
		return 0;
	} else {
		/* not being traced... */
		if ((p->p_flags & P_TRACED) == 0) {
			return EPERM;
		}

		/* not being traced by YOU */
		if (p->p_pptr != curp) {
			kprintf("p->p_pptr != curp\n");
			return EBUSY;
		}

		if (req != PT_GETNEXTEVENT) {
			/* not currently stopped */
			if (p->p_stat != SSTOP) {
				kprintf("p->p_stat != SSTOP\n");
				return EBUSY;
			}
		}

		/* OK */
		return 0;
	}
}

static struct lwp*
get_signaled_lwp(struct proc *p)
{
	struct lwp *tmplp;
	int signal = 0;

	FOREACH_LWP_IN_PROC(tmplp, p) {
		signal = CURSIG_NOBLOCK(tmplp);
		if (signal)
			return tmplp;
	}

	return NULL;
}

static int
get_signal_lwp(struct lwp *lwp)
{
	int i;
	sigset_t sigset = lwp_sigpend(lwp);
	for (i = 1; i < _SIG_MAXSIG; i++) {
		if (SIGISMEMBER(sigset, i))
			return i;
	}
	return 0;
}

static int
wait_next_event(struct proc *p, struct lwp *lp, void *user_addr,
		struct ptrace_event *event)
{
	struct lwp *tmplp;
	int error = 0;

	if (user_addr == NULL)
		return EINVAL;

	event->status = PT_EV_NONE;

	/* Check if process has exited and turned into zombie */
	if (p->p_stat == SZOMB) {
		event->status = PT_EV_PROC_EXITED;
		return 0;
	}

	tmplp = get_signaled_lwp(p);

	if (tmplp) {
		event->status = PT_EV_SIGNALED;
		event->lwpid = tmplp->lwp_tid;
		event->signal = get_signal_lwp(tmplp);
		kprintf("first: lwpid=%d, signal=%d\n",
			event->lwpid, event->signal);
		atomic_set_int(&p->p_ptrace_events, 0);
		error = copyout(event, user_addr, sizeof(*event));
		return error;
	}

	kprintf("p->p_ptrace_events=%d\n", p->p_ptrace_events);

	if (p->p_ptrace_events == 0) {
		tsleep_interlock(&p->p_ptrace_events, PCATCH);
		cpu_ccfence();
		if (p->p_ptrace_events == 0) {
			atomic_set_int(&p->p_ptrace_events, 0);

			LWPRELE(lp);
			/* XXX: is it needed? */
			lwkt_reltoken(&p->p_token);
			PRELE(p);

			error = tsleep(&p->p_ptrace_events,
				       PCATCH | PINTERLOCKED, "ptnextevent", 0);

			PHOLD(p);
			/* XXX: is it needed? */
			lwkt_gettoken(&p->p_token);
			LWPHOLD(lp);

			if (error)
				return error;
			tmplp = get_signaled_lwp(p);
			if (tmplp) {
				event->status = PT_EV_SIGNALED;
				event->lwpid = tmplp->lwp_tid;
				event->signal = get_signal_lwp(tmplp);
				kprintf("second: lwpid=%d, signal=%d\n",
					event->lwpid, event->signal);
				error = copyout(event, user_addr, sizeof(*event));
				return error;
			}
		}
	}

	return error;
}

/*
 * Performs generic ptrace request.
 * Request id was validated before.
 * Called with PHOLD(p), LWPHOLD(lwp), and lwkt token held.
 * NOTE! User addr points at userspace address.
 */
static int
ptrace_req_generic(int req, struct proc *p, struct lwp *lp, void *user_addr,
		   int data, int *res)
{
	struct proc *curp = curproc;
	struct iovec iov;
	struct uio uio;
	struct lwp *tmplp;
	int *buf;
	int write, tmp, nthreads;
	int error = 0;

	/*
	 * XXX this obfuscation is to reduce stack usage.
	 */
	union {
		struct ptrace_io_desc piod;
		struct ptrace_event event;
	} r;

	write = 0;
	switch (req) {
	case PT_TRACE_ME:
		/* set my trace flag and "owner" so it can read/write me */
		p->p_flags |= P_TRACED;
		p->p_oppid = p->p_pptr->p_pid;
		return 0;

	case PT_ATTACH:
		/* security check done above */
		p->p_flags |= P_TRACED;
		p->p_oppid = p->p_pptr->p_pid;
		proc_reparent(p, curp);
		data = SIGSTOP;
		goto sendsig;	/* in PT_CONTINUE below */


	case PT_KILL:
		data = SIGKILL;
		goto sendsig;	/* in PT_CONTINUE below */

	case PT_STEP:
	case PT_CONTINUE:
	case PT_DETACH:
		/* Zero means do not send any signal */
		if (data < 0 || data >= _SIG_MAXSIG) {
			return EINVAL;
		}

		if (req == PT_STEP) {
			if ((error = ptrace_single_step (lp))) {
				return error;
			}
		}

		if (user_addr != (void *)1) {
			if ((error = ptrace_set_pc (lp, (u_long)user_addr))) {
				return error;
			}
		}

		if (req == PT_DETACH) {
			/* reset process parent */
			if (p->p_oppid != p->p_pptr->p_pid) {
				struct proc *pp;

				pp = pfind(p->p_oppid);
				if (pp) {
					proc_reparent(p, pp);
					PRELE(pp);
				}
			}

			p->p_flags &= ~(P_TRACED | P_WAITED);
			p->p_oppid = 0;

			/* should we send SIGCHLD? */
		}

	sendsig:
		/*
		 * Deliver or queue signal.  If the process is stopped
		 * force it to be SACTIVE again.
		 */
		crit_enter();
		if (p->p_stat == SSTOP) {
			p->p_xstat = data;
			proc_unstop(p, SSTOP);
		} else if (data) {
			ksignal(p, data);
		}
		crit_exit();
		return 0;


	case PT_WRITE_I:
	case PT_WRITE_D:
		write = 1;
		/* fallthrough */
	case PT_READ_I:
	case PT_READ_D:
		/*
		 * NOTE! uio_offset represents the offset in the target
		 * process.  The iov is in the current process (the guy
		 * making the ptrace call) so uio_td must be the current
		 * process (though for a SYSSPACE transfer it doesn't
		 * really matter).
		 */
		tmp = 0;
		/* write = 0 set above */
		iov.iov_base = write ? (caddr_t)&user_addr : (caddr_t)&tmp;
		iov.iov_len = sizeof(int);
		uio.uio_iov = &iov;
		uio.uio_iovcnt = 1;
		uio.uio_offset = (off_t)(uintptr_t)user_addr;
		uio.uio_resid = sizeof(int);
		uio.uio_segflg = UIO_SYSSPACE;
		uio.uio_rw = write ? UIO_WRITE : UIO_READ;
		uio.uio_td = curthread;
		error = procfs_domem(curp, lp, NULL, &uio);
		if (uio.uio_resid != 0) {
			/*
			 * XXX procfs_domem() doesn't currently return ENOSPC,
			 * so I think write() can bogusly return 0.
			 * XXX what happens for short writes?  We don't want
			 * to write partial data.
			 * XXX procfs_domem() returns EPERM for other invalid
			 * addresses.  Convert this to EINVAL.  Does this
			 * clobber returns of EPERM for other reasons?
			 */
			if (error == 0 || error == ENOSPC || error == EPERM)
				error = EINVAL;	/* EOF */
		}
		if (!write)
			*res = tmp;
		return error;

	case PT_IO:
		/*
		 * NOTE! uio_offset represents the offset in the target
		 * process.  The iov is in the current process (the guy
		 * making the ptrace call) so uio_td must be the current
		 * process.
		 */
		error = copyin(user_addr, &r.piod, sizeof(r.piod));
		if (error)
			return error;
//		piod = addr;
		iov.iov_base = r.piod.piod_addr;
		iov.iov_len = r.piod.piod_len;
		uio.uio_iov = &iov;
		uio.uio_iovcnt = 1;
		uio.uio_offset = (off_t)(uintptr_t)r.piod.piod_offs;
		uio.uio_resid = r.piod.piod_len;
		uio.uio_segflg = UIO_USERSPACE;
		uio.uio_td = curthread;
		switch (r.piod.piod_op) {
		case PIOD_READ_D:
		case PIOD_READ_I:
			uio.uio_rw = UIO_READ;
			break;
		case PIOD_WRITE_D:
		case PIOD_WRITE_I:
			uio.uio_rw = UIO_WRITE;
			break;
		default:
			return EINVAL;
		}
		error = procfs_domem(curp, lp, NULL, &uio);
		r.piod.piod_len -= uio.uio_resid;
		if (error == 0)
			copyout(&r.piod, user_addr, sizeof(r.piod));
		return error;

	case PT_GETNUMLWPS:
		*res = p->p_nthreads;
		break;
	case PT_GETLWPLIST:
		if (data <= 0)
			return EINVAL;
		nthreads = MIN(data, p->p_nthreads);
		tmp = 0;
		buf = kmalloc(nthreads * sizeof(lwpid_t), M_TEMP, M_WAITOK);
		FOREACH_LWP_IN_PROC(tmplp, p) {
			if (tmp >= nthreads)
				break;
			buf[tmp] = tmplp->lwp_tid;
			tmp++;
		}
		error = copyout(buf, user_addr, nthreads * sizeof(lwpid_t));
		kfree(buf, M_TEMP);
		if (!error)
			*res = nthreads;
		break;
	case PT_GETNEXTEVENT:
		error = wait_next_event(p, lp, user_addr, &r.event);
		break;
	case PT_SUSPEND:
		
		break;
	case PT_RESUME:
		
		break;
	default:
		return EINVAL;
	}

	return error;
}

/*
 * Process debugging system call.
 *
 * MPALMOSTSAFE
 */
int
sys_ptrace(struct sysmsg *sysmsg, const struct ptrace_args *uap)
{
	int error = 0;

	error = kern_ptrace(uap->req, uap->pid, uap->addr, uap->data,
			    &sysmsg->sysmsg_result);
	return (error);
}

int
kern_ptrace(int req, ptrace_ptid_t ptid, void *user_addr, int data, int *res)
{
	struct proc *curp = curproc;
	struct proc *p;
	struct lwp *lp;
	int error = 0;
	pid_t pid = PTRACE_GET_PID(ptid);
	lwpid_t lwpid = PTRACE_GET_LWPID(ptid);

	kprintf("req=%d, pid=%d, lwpid=%d\n", req, pid, lwpid);
	/*
	 * Validate request id.
	 */
	if (!req_is_valid(req))
		return EINVAL;

	if (req == PT_TRACE_ME) {
		p = curp;
		PHOLD(p);
	} else {
		if ((p = pfind(pid)) == NULL)
			return ESRCH;
	}
	if (!PRISON_CHECK(curp->p_ucred, p->p_ucred)) {
		error = ESRCH;
		goto err_proc;
	}
	if (p->p_flags & P_SYSTEM) {
		error = EINVAL;
		goto err_proc;
	}

	lwkt_gettoken(&p->p_token);
	/* Can't trace a process that's currently exec'ing. */
	if ((p->p_flags & P_INEXEC) != 0) {
		error = EAGAIN;
		goto err_lwkt;
	}

	/*
	 * Permissions check
	 */
	error = ptrace_check_perms(curp, p, req);
	if (error)
		goto err_lwkt;

	/* XXX lwp */
	lp = NULL;
	if (lwpid == 0) {
		lp = FIRST_LWP_IN_PROC(p);
		if (lp != NULL)
			LWPHOLD(lp);
	} else {
		lp = lwpfind(p, lwpid);
	}
	if (lp == NULL) {
		error = EINVAL;
		goto err_lwkt;
	}

#ifdef FIX_SSTEP
	/*
	 * Single step fixup ala procfs
	 */
	FIX_SSTEP(lp);
#endif

	/*
	 * Actually do the requests
	 */

	*res = 0;

	if (PT_REQ_IS_GENERIC(req)) {
		error = ptrace_req_generic(req, p, lp, user_addr, data, res);
	}
#ifdef PT_LASTMACH
	else if (PT_REQ_IS_MACH(req)) {
		error = ptrace_req_mach(req, p, lp, user_addr, data, res);
	}
#endif

	LWPRELE(lp);
err_lwkt:
	lwkt_reltoken(&p->p_token);
err_proc:
	PRELE(p);
	return error;
}

#if 0

/*
 * Process debugging system call.
 *
 * MPALMOSTSAFE
 */
int
sys_ptrace(struct sysmsg *sysmsg, const struct ptrace_args *uap)
{
	struct proc *p = curproc;

	/*
	 * XXX this obfuscation is to reduce stack usage, but the register
	 * structs may be too large to put on the stack anyway.
	 */
	union {
		struct ptrace_io_desc piod;
		struct dbreg dbreg;
		struct fpreg fpreg;
		struct reg reg;
	} r;
	void *addr;
	int error = 0;

	addr = &r;
	switch (uap->req) {
	case PT_GETREGS:
	case PT_GETFPREGS:
#ifdef PT_GETDBREGS
	case PT_GETDBREGS:
#endif
		break;
	case PT_SETREGS:
		error = copyin(uap->addr, &r.reg, sizeof r.reg);
		break;
	case PT_SETFPREGS:
		error = copyin(uap->addr, &r.fpreg, sizeof r.fpreg);
		break;
#ifdef PT_SETDBREGS
	case PT_SETDBREGS:
		error = copyin(uap->addr, &r.dbreg, sizeof r.dbreg);
		break;
#endif
	case PT_IO:
		error = copyin(uap->addr, &r.piod, sizeof r.piod);
		break;
	default:
		addr = uap->addr;
	}
	if (error)
		return (error);

	error = kern_ptrace(p, uap->req, uap->pid, addr, uap->data,
			&sysmsg->sysmsg_result);
	if (error)
		return (error);

	switch (uap->req) {
	case PT_IO:
		(void)copyout(&r.piod, uap->addr, sizeof r.piod);
		break;
	case PT_GETREGS:
		error = copyout(&r.reg, uap->addr, sizeof r.reg);
		break;
	case PT_GETFPREGS:
		error = copyout(&r.fpreg, uap->addr, sizeof r.fpreg);
		break;
#ifdef PT_GETDBREGS
	case PT_GETDBREGS:
		error = copyout(&r.dbreg, uap->addr, sizeof r.dbreg);
		break;
#endif
	}

	return (error);
}

int
kern_ptrace(struct proc *curp, int req, pid_t pid, void *addr,
	    int data, int *res)
{
	struct proc *p, *pp;
	struct lwp *lp;
	struct iovec iov;
	struct uio uio;
	struct ptrace_io_desc *piod;
	int error = 0;
	int write, tmp;
	int t;

	write = 0;
	if (req == PT_TRACE_ME) {
		p = curp;
		PHOLD(p);
	} else {
		if ((p = pfind(pid)) == NULL)
			return ESRCH;
	}
	if (!PRISON_CHECK(curp->p_ucred, p->p_ucred)) {
		PRELE(p);
		return (ESRCH);
	}
	if (p->p_flags & P_SYSTEM) {
		PRELE(p);
		return EINVAL;
	}

	lwkt_gettoken(&p->p_token);
	/* Can't trace a process that's currently exec'ing. */
	if ((p->p_flags & P_INEXEC) != 0) {
		lwkt_reltoken(&p->p_token);
		PRELE(p);
		return EAGAIN;
	}

	/*
	 * Permissions check
	 */
	switch (req) {
	case PT_TRACE_ME:
		/* Always legal. */
		break;

	case PT_ATTACH:
		/* Self */
		if (p->p_pid == curp->p_pid) {
			lwkt_reltoken(&p->p_token);
			PRELE(p);
			return EINVAL;
		}

		/* Already traced */
		if (p->p_flags & P_TRACED) {
			lwkt_reltoken(&p->p_token);
			PRELE(p);
			return EBUSY;
		}

		if (curp->p_flags & P_TRACED)
			for (pp = curp->p_pptr; pp != NULL; pp = pp->p_pptr)
				if (pp == p) {
					lwkt_reltoken(&p->p_token);
					PRELE(p);
					return (EINVAL);
				}

		/* not owned by you, has done setuid (unless you're root) */
		if ((p->p_ucred->cr_ruid != curp->p_ucred->cr_ruid) ||
		     (p->p_flags & P_SUGID)) {
			error = caps_priv_check(curp->p_ucred,
						SYSCAP_RESTRICTEDROOT);
			if (error) {
				lwkt_reltoken(&p->p_token);
				PRELE(p);
				return error;
			}
		}

		/* can't trace init when securelevel > 0 */
		if (securelevel > 0 && p->p_pid == 1) {
			lwkt_reltoken(&p->p_token);
			PRELE(p);
			return EPERM;
		}

		/* OK */
		break;

	case PT_READ_I:
	case PT_READ_D:
	case PT_WRITE_I:
	case PT_WRITE_D:
	case PT_IO:
	case PT_CONTINUE:
	case PT_KILL:
	case PT_STEP:
	case PT_DETACH:
#ifdef PT_GETREGS
	case PT_GETREGS:
#endif
#ifdef PT_SETREGS
	case PT_SETREGS:
#endif
#ifdef PT_GETFPREGS
	case PT_GETFPREGS:
#endif
#ifdef PT_SETFPREGS
	case PT_SETFPREGS:
#endif
#ifdef PT_GETDBREGS
	case PT_GETDBREGS:
#endif
#ifdef PT_SETDBREGS
	case PT_SETDBREGS:
#endif
		/* not being traced... */
		if ((p->p_flags & P_TRACED) == 0) {
			lwkt_reltoken(&p->p_token);
			PRELE(p);
			return EPERM;
		}

		/* not being traced by YOU */
		if (p->p_pptr != curp) {
			lwkt_reltoken(&p->p_token);
			PRELE(p);
			return EBUSY;
		}

		/* not currently stopped */
		if (p->p_stat != SSTOP ||
		    (p->p_flags & P_WAITED) == 0) {
			lwkt_reltoken(&p->p_token);
			PRELE(p);
			return EBUSY;
		}

		/* OK */
		break;

	default:
		lwkt_reltoken(&p->p_token);
		PRELE(p);
		return EINVAL;
	}

	/* XXX lwp */
	lp = FIRST_LWP_IN_PROC(p);
	if (lp == NULL) {
		lwkt_reltoken(&p->p_token);
		PRELE(p);
		return EINVAL;
	}

#ifdef FIX_SSTEP
	/*
	 * Single step fixup ala procfs
	 */
	FIX_SSTEP(lp);
#endif

	/*
	 * Actually do the requests
	 */

	*res = 0;

	switch (req) {
	case PT_TRACE_ME:
		/* set my trace flag and "owner" so it can read/write me */
		p->p_flags |= P_TRACED;
		p->p_oppid = p->p_pptr->p_pid;
		lwkt_reltoken(&p->p_token);
		PRELE(p);
		return 0;

	case PT_ATTACH:
		/* security check done above */
		p->p_flags |= P_TRACED;
		p->p_oppid = p->p_pptr->p_pid;
		proc_reparent(p, curp);
		data = SIGSTOP;
		goto sendsig;	/* in PT_CONTINUE below */

	case PT_STEP:
	case PT_CONTINUE:
	case PT_DETACH:
		/* Zero means do not send any signal */
		if (data < 0 || data >= _SIG_MAXSIG) {
			lwkt_reltoken(&p->p_token);
			PRELE(p);
			return EINVAL;
		}

		LWPHOLD(lp);

		if (req == PT_STEP) {
			if ((error = ptrace_single_step (lp))) {
				LWPRELE(lp);
				lwkt_reltoken(&p->p_token);
				PRELE(p);
				return error;
			}
		}

		if (addr != (void *)1) {
			if ((error = ptrace_set_pc (lp, (u_long)addr))) {
				LWPRELE(lp);
				lwkt_reltoken(&p->p_token);
				PRELE(p);
				return error;
			}
		}
		LWPRELE(lp);

		if (req == PT_DETACH) {
			/* reset process parent */
			if (p->p_oppid != p->p_pptr->p_pid) {
				struct proc *pp;

				pp = pfind(p->p_oppid);
				if (pp) {
					proc_reparent(p, pp);
					PRELE(pp);
				}
			}

			p->p_flags &= ~(P_TRACED | P_WAITED);
			p->p_oppid = 0;

			/* should we send SIGCHLD? */
		}

	sendsig:
		/*
		 * Deliver or queue signal.  If the process is stopped
		 * force it to be SACTIVE again.
		 */
		crit_enter();
		if (p->p_stat == SSTOP) {
			p->p_xstat = data;
			proc_unstop(p, SSTOP);
		} else if (data) {
			ksignal(p, data);
		}
		crit_exit();
		lwkt_reltoken(&p->p_token);
		PRELE(p);
		return 0;

	case PT_WRITE_I:
	case PT_WRITE_D:
		write = 1;
		/* fallthrough */
	case PT_READ_I:
	case PT_READ_D:
		/*
		 * NOTE! uio_offset represents the offset in the target
		 * process.  The iov is in the current process (the guy
		 * making the ptrace call) so uio_td must be the current
		 * process (though for a SYSSPACE transfer it doesn't
		 * really matter).
		 */
		tmp = 0;
		/* write = 0 set above */
		iov.iov_base = write ? (caddr_t)&data : (caddr_t)&tmp;
		iov.iov_len = sizeof(int);
		uio.uio_iov = &iov;
		uio.uio_iovcnt = 1;
		uio.uio_offset = (off_t)(uintptr_t)addr;
		uio.uio_resid = sizeof(int);
		uio.uio_segflg = UIO_SYSSPACE;
		uio.uio_rw = write ? UIO_WRITE : UIO_READ;
		uio.uio_td = curthread;
		error = procfs_domem(curp, lp, NULL, &uio);
		if (uio.uio_resid != 0) {
			/*
			 * XXX procfs_domem() doesn't currently return ENOSPC,
			 * so I think write() can bogusly return 0.
			 * XXX what happens for short writes?  We don't want
			 * to write partial data.
			 * XXX procfs_domem() returns EPERM for other invalid
			 * addresses.  Convert this to EINVAL.  Does this
			 * clobber returns of EPERM for other reasons?
			 */
			if (error == 0 || error == ENOSPC || error == EPERM)
				error = EINVAL;	/* EOF */
		}
		if (!write)
			*res = tmp;
		lwkt_reltoken(&p->p_token);
		PRELE(p);
		return (error);

	case PT_IO:
		/*
		 * NOTE! uio_offset represents the offset in the target
		 * process.  The iov is in the current process (the guy
		 * making the ptrace call) so uio_td must be the current
		 * process.
		 */
		piod = addr;
		iov.iov_base = piod->piod_addr;
		iov.iov_len = piod->piod_len;
		uio.uio_iov = &iov;
		uio.uio_iovcnt = 1;
		uio.uio_offset = (off_t)(uintptr_t)piod->piod_offs;
		uio.uio_resid = piod->piod_len;
		uio.uio_segflg = UIO_USERSPACE;
		uio.uio_td = curthread;
		switch (piod->piod_op) {
		case PIOD_READ_D:
		case PIOD_READ_I:
			uio.uio_rw = UIO_READ;
			break;
		case PIOD_WRITE_D:
		case PIOD_WRITE_I:
			uio.uio_rw = UIO_WRITE;
			break;
		default:
			lwkt_reltoken(&p->p_token);
			PRELE(p);
			return (EINVAL);
		}
		error = procfs_domem(curp, lp, NULL, &uio);
		piod->piod_len -= uio.uio_resid;
		lwkt_reltoken(&p->p_token);
		PRELE(p);
		return (error);

	case PT_KILL:
		data = SIGKILL;
		goto sendsig;	/* in PT_CONTINUE above */

#ifdef PT_SETREGS
	case PT_SETREGS:
		write = 1;
		/* fallthrough */
#endif /* PT_SETREGS */
#ifdef PT_GETREGS
	case PT_GETREGS:
		/* write = 0 above */
#endif /* PT_SETREGS */
#if defined(PT_SETREGS) || defined(PT_GETREGS)
		if (!procfs_validregs(lp)) {
			lwkt_reltoken(&p->p_token);
			PRELE(p);
			return EINVAL;
		} else {
			iov.iov_base = addr;
			iov.iov_len = sizeof(struct reg);
			uio.uio_iov = &iov;
			uio.uio_iovcnt = 1;
			uio.uio_offset = 0;
			uio.uio_resid = sizeof(struct reg);
			uio.uio_segflg = UIO_SYSSPACE;
			uio.uio_rw = write ? UIO_WRITE : UIO_READ;
			uio.uio_td = curthread;
			t = procfs_doregs(curp, lp, NULL, &uio);
			lwkt_reltoken(&p->p_token);
			PRELE(p);
			return t;
		}
#endif /* defined(PT_SETREGS) || defined(PT_GETREGS) */

#ifdef PT_SETFPREGS
	case PT_SETFPREGS:
		write = 1;
		/* fallthrough */
#endif /* PT_SETFPREGS */
#ifdef PT_GETFPREGS
	case PT_GETFPREGS:
		/* write = 0 above */
#endif /* PT_SETFPREGS */
#if defined(PT_SETFPREGS) || defined(PT_GETFPREGS)
		if (!procfs_validfpregs(lp)) {
			lwkt_reltoken(&p->p_token);
			PRELE(p);
			return EINVAL;
		} else {
			iov.iov_base = addr;
			iov.iov_len = sizeof(struct fpreg);
			uio.uio_iov = &iov;
			uio.uio_iovcnt = 1;
			uio.uio_offset = 0;
			uio.uio_resid = sizeof(struct fpreg);
			uio.uio_segflg = UIO_SYSSPACE;
			uio.uio_rw = write ? UIO_WRITE : UIO_READ;
			uio.uio_td = curthread;
			t = procfs_dofpregs(curp, lp, NULL, &uio);
			lwkt_reltoken(&p->p_token);
			PRELE(p);
			return t;
		}
#endif /* defined(PT_SETFPREGS) || defined(PT_GETFPREGS) */

#ifdef PT_SETDBREGS
	case PT_SETDBREGS:
		write = 1;
		/* fallthrough */
#endif /* PT_SETDBREGS */
#ifdef PT_GETDBREGS
	case PT_GETDBREGS:
		/* write = 0 above */
#endif /* PT_SETDBREGS */
#if defined(PT_SETDBREGS) || defined(PT_GETDBREGS)
		if (!procfs_validdbregs(lp)) {
			lwkt_reltoken(&p->p_token);
			PRELE(p);
			return EINVAL;
		} else {
			iov.iov_base = addr;
			iov.iov_len = sizeof(struct dbreg);
			uio.uio_iov = &iov;
			uio.uio_iovcnt = 1;
			uio.uio_offset = 0;
			uio.uio_resid = sizeof(struct dbreg);
			uio.uio_segflg = UIO_SYSSPACE;
			uio.uio_rw = write ? UIO_WRITE : UIO_READ;
			uio.uio_td = curthread;
			t = procfs_dodbregs(curp, lp, NULL, &uio);
			lwkt_reltoken(&p->p_token);
			PRELE(p);
			return t;
		}
#endif /* defined(PT_SETDBREGS) || defined(PT_GETDBREGS) */

	default:
		break;
	}

	lwkt_reltoken(&p->p_token);
	PRELE(p);

	return 0;
}

#endif

int
trace_req(struct proc *p)
{
	return 1;
}

/*
 * stopevent()
 *
 * Stop a process because of a procfs event.  Stay stopped until p->p_step
 * is cleared (cleared by PIOCCONT in procfs).
 *
 * MPSAFE
 */
void
stopevent(struct proc *p, unsigned int event, unsigned int val) 
{
	/*
	 * Set event info.  Recheck p_stops in case we are
	 * racing a close() on procfs.
	 */
	spin_lock(&p->p_spin);
	if ((p->p_stops & event) == 0) {
		spin_unlock(&p->p_spin);
		return;
	}
	p->p_xstat = val;
	p->p_stype = event;
	p->p_step = 1;
	tsleep_interlock(&p->p_step, 0);
	spin_unlock(&p->p_spin);

	/*
	 * Wakeup any PIOCWAITing procs and wait for p_step to
	 * be cleared.
	 */
	for (;;) {
		wakeup(&p->p_stype);
		tsleep(&p->p_step, PINTERLOCKED, "stopevent", 0);
		spin_lock(&p->p_spin);
		if (p->p_step == 0) {
			spin_unlock(&p->p_spin);
			break;
		}
		tsleep_interlock(&p->p_step, 0);
		spin_unlock(&p->p_spin);
	}
}

