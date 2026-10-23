#include "../core/kernel.h"

/* ============================================================
 * signal.c — POSIX-style signals for ring-3 processes (v5.8.11)
 * ============================================================
 * Model: a signal is POSTED (do_kill / keyboard Ctrl-C) by setting a bit in the
 * target's `sig_pending`, and DELIVERED at return-to-ring-3 by `signal_dispatch`,
 * which the syscall entry path (isr_stubs.asm) calls with the process's saved
 * register frame. Delivery either runs the default action (terminate, or ignore
 * for SIGCHLD/SIGCONT), drops the signal (SIG_IGN), or diverts the process into a
 * user handler by rewriting the frame (RIP=handler, RDI=signo) and pushing a
 * return trampoline onto the user stack; the handler returns through that
 * trampoline into SYS_SIGRETURN, which restores the interrupted context.
 *
 * Because delivery happens on the syscall return path, a blocked syscall must be
 * signal-aware: do_kill wakes a PROC_BLOCKED target so its wait returns (EINTR)
 * and delivery runs. Pure compute loops that never syscall won't see a signal
 * until their next timer-driven syscall return — IRQ-path delivery is future work. */

/* get_current_process / find_process / wake_waiters are declared in kernel.h. */

/* The foreground process for terminal-generated signals (Ctrl-C). cmd_exec sets
 * this to the pid it kwait()s on; 0 = none. */
uint32_t g_foreground_pid = 0;

/* SIGKILL and SIGSTOP can never be caught or ignored. */
static int uncatchable(int sig) { return sig == SIGKILL || sig == SIGSTOP; }

/* Signals whose SIG_DFL action is "ignore" instead of "terminate". */
static int default_is_ignore(int sig) { return sig == SIGCHLD || sig == SIGCONT; }

/* Post `sig` to process p: set the pending bit, and if p is parked in a blocking
 * syscall (waitpid / pipe / stdin), wake it so the syscall returns and delivery
 * runs on the way out. Safe from IRQ context (just field writes). */
void signal_raise(process_t* p, int sig) {
    if (!p || sig <= 0 || sig >= NSIG) return;
    /* SIGCONT resumes a job-control-stopped process: flip it back to RUN so the
     * scheduler runs it again (it's parked in signal_dispatch waiting for exactly
     * this). SIGCONT's default action is "ignore", so no handler runs on delivery. */
    if (sig == SIGCONT && p->state == PROC_STOPPED) {
        p->stop_sig = 0;
        p->state = PROC_RUN;
    }
    p->sig_pending |= (1u << sig);
    if (p->state == PROC_BLOCKED) {          /* unblock so its wait returns EINTR */
        p->waiting_for = 0;
        p->wake_tick = 0;
        p->state = PROC_RUN;
    }
}

/* The signal that would deliver next: the lowest-numbered PENDING signal not blocked by the
 * MASK, or 0 if none. ONE definition for signal_pending() (any deliverable?) and
 * signal_dispatch() (which one?), so the "is a signal up" test and the "which signal" pick
 * can never disagree. Lowest-number-first is the delivery order; SIGKILL/SIGSTOP can't be
 * masked (see sigmask_apply), so a pending one is always returned here. */
static int signal_next_deliverable(uint32_t pending, uint32_t mask) {
    uint32_t d = pending & ~mask;
    for (int s = 1; s < NSIG; s++)
        if (d & (1u << s)) return s;
    return 0;
}

/* 1 if p has a pending, unblocked signal ready to deliver. Used by the blocking
 * syscalls to decide whether to bail out with EINTR. */
int signal_pending(process_t* p) {
    return p && signal_next_deliverable(p->sig_pending, p->sig_mask) != 0;
}

/* If the next deliverable signal has the default STOP action, PARK the process here
 * until SIGCONT resumes it, and return 1 — so the interrupted blocking syscall can
 * RESTART its wait rather than returning -EINTR (stop/continue is then transparent to
 * the operation, e.g. a `sleep` keeps sleeping after fg/bg). Returns 0 if no stop is
 * pending. Called with interrupts off from a blocking syscall's wait loop; the caller
 * already saves/restores user_cr3/user_rsp around that loop. Parks like do_waitpid
 * (blocked_in_kernel -> resume on the kernel CR3, sti/hlt until PROC_RUN). */
int signal_check_stop(process_t* p) {
    if (!p) return 0;
    int sig = signal_next_deliverable(p->sig_pending, p->sig_mask);
    if (sig != SIGTSTP && sig != SIGSTOP) return 0;
    if (sig == SIGTSTP && p->sig_handlers[sig] != SIG_DFL) return 0;
    p->sig_pending &= ~(1u << sig);
    p->stop_sig = (uint32_t)sig;
    p->stopped_reported = 0;
    p->state = PROC_STOPPED;
    wake_waiters(p);                         /* parent's waitpid(WUNTRACED) reports the stop */
    p->blocked_in_kernel = 1;
    while (p->state == PROC_STOPPED) __asm__ volatile("sti; hlt");
    __asm__ volatile("cli");
    p->blocked_in_kernel = 0;
    p->sig_pending &= ~(1u << SIGCONT);      /* consume the SIGCONT that resumed us (default: ignore),
                                              * else the caller's next signal_pending() bails -EINTR */
    return 1;                                /* resumed (SIGCONT): caller restarts its wait */
}

/* SYS_KILL: post `sig` to `pid`. sig 0 is the existence probe (returns 0 if the
 * process exists, -1 otherwise) and sends nothing. */
int do_kill(int pid, int sig) {
    if (sig < 0 || sig >= NSIG) return -1;
    process_t* target = find_process((uint32_t)pid);
    if (!target) return -1;                  /* ESRCH */
    if (sig == 0) return 0;                   /* existence check only */
    signal_raise(target, sig);
    return 0;
}

/* SYS_SIGNAL: set the disposition of `sig` (SIG_DFL / SIG_IGN / a handler VA) and
 * record the ring-3 sigreturn trampoline. Returns the previous disposition, or -1
 * for an invalid or uncatchable signal. */
long do_signal(int sig, uint64_t handler, uint64_t trampoline) {
    if (sig <= 0 || sig >= NSIG || uncatchable(sig)) return -1;
    process_t* p = get_current_process();
    if (!p) return -1;
    long prev = (long)p->sig_handlers[sig];
    p->sig_handlers[sig] = handler;
    if (trampoline) p->sig_trampoline = trampoline;
    if (handler == SIG_IGN) p->sig_pending &= ~(1u << sig);   /* discard pending */
    return prev;
}

/* Terminate the current process from an uncaught default-terminate signal. Mirrors
 * SYS_EXIT: become a zombie, wake a parent blocked in waitpid, then yield forever
 * (the scheduler switches away and reap_zombies frees us — we can't free our own
 * stack). exit_code uses the shell convention 128+signo so waitpid can report that
 * the child was killed by a signal. Never returns. */
static void signal_terminate(process_t* p, int sig) {
    p->exit_code = 128 + sig;
    p->state = PROC_ZOMBIE;
    wake_waiters(p);
    __asm__ volatile("sti");
    for (;;) __asm__ volatile("hlt");
}

/* Deliver at most one pending, unblocked signal to the process about to return to
 * ring 3. `frame` is its saved register frame on the kernel stack (isr_stubs.asm
 * SAVE_REGS order): frame[0..14] = GPRs r15..rax, frame[15] = RFLAGS, frame[16] =
 * RIP; the user RSP lives in the `user_rsp` global. Called on the kernel CR3 with
 * user_cr3 still pointing at the caller's address space (copy_to_user works). */
void signal_dispatch(uint64_t* frame) {
    process_t* p = get_current_process();
    if (!p || !p->page_directory) return;

    int sig = signal_next_deliverable(p->sig_pending, p->sig_mask);   /* lowest-numbered, unblocked */
    if (!sig) return;

    uint64_t disp = uncatchable(sig) ? SIG_DFL : p->sig_handlers[sig];

    if (disp == SIG_IGN) { p->sig_pending &= ~(1u << sig); return; }
    if (disp == SIG_DFL) {
        p->sig_pending &= ~(1u << sig);
        if (default_is_ignore(sig)) return;
        /* Job-control stop (SIGTSTP/SIGSTOP): PARK here instead of terminating, so the
         * process never returns to ring 3 while stopped. We mark PROC_STOPPED, wake a
         * parent blocked in waitpid(WUNTRACED) to report the stop, then sti/hlt until
         * SIGCONT flips us back to PROC_RUN (the scheduler skips non-RUN states). This
         * is the do_waitpid mid-syscall discipline: blocked_in_kernel resumes us on the
         * kernel CR3, and user_cr3/user_rsp are saved/restored across the park because
         * other processes' syscalls clobber those globals while we sleep. */
        if (sig == SIGTSTP || sig == SIGSTOP) {
            uint64_t s_cr3 = user_cr3, s_rsp = user_rsp;
            p->stop_sig = (uint32_t)sig;
            p->stopped_reported = 0;
            p->state = PROC_STOPPED;
            wake_waiters(p);                 /* parent's WUNTRACED waitpid reports the stop */
            p->blocked_in_kernel = 1;        /* resume on the kernel CR3 after SIGCONT */
            while (p->state == PROC_STOPPED) __asm__ volatile("sti; hlt");
            __asm__ volatile("cli");
            p->blocked_in_kernel = 0;
            p->sig_pending &= ~(1u << SIGCONT);  /* consume the SIGCONT that resumed us */
            user_cr3 = s_cr3;                /* restore OUR globals for the asm return path */
            user_rsp = s_rsp;
            return;                          /* resumed (SIGCONT): finish the syscall return */
        }
        signal_terminate(p, sig);            /* never returns */
        return;
    }

    /* User handler. Serialize handlers: a single saved-context slot means we can't
     * nest, so if one is already running, leave this signal pending — it delivers
     * after the current handler's sigreturn (which clears sig_active). */
    if (p->sig_active) return;
    p->sig_pending &= ~(1u << sig);          /* commit to delivering it */

    /* Save the interrupted ring-3 context for SYS_SIGRETURN. */
    for (int i = 0; i < 15; i++) p->sig_saved[i] = frame[i];   /* GPRs r15..rax */
    p->sig_saved[15] = frame[15];            /* RFLAGS */
    p->sig_saved[16] = frame[16];            /* RIP    */
    p->sig_saved[17] = user_rsp;             /* user RSP */

    /* Build the handler's ring-3 stack: a small slack gap below RSP (user code is
     * -mno-red-zone, but stay clear anyway), 16-byte aligned, then push the
     * trampoline return address so RSP%16 == 8 at handler entry (SysV: as if
     * reached by a `call`). The handler `ret`s into the trampoline -> SYS_SIGRETURN. */
    uint64_t sp = user_rsp;
    sp -= 128;
    sp &= ~15ULL;
    sp -= 8;
    uint64_t tramp = p->sig_trampoline;
    if (copy_to_user(sp, &tramp, 8) != 0) {  /* bad user stack -> can't deliver */
        signal_terminate(p, sig);            /* never returns */
        return;
    }

    frame[9]  = (uint64_t)sig;               /* RDI = signo (SysV arg1) */
    frame[16] = disp;                        /* RIP = handler entry */
    user_rsp  = sp;                          /* handler runs on the adjusted stack */
    p->sig_active |= (1u << sig);
    p->sig_mask   |= (1u << sig);            /* block this signal inside its handler */
}

/* Deliver a SYNCHRONOUS CPU-exception signal (SIGSEGV/SIGFPE/SIGILL from the #PF/#DE/
 * #UD/... fault handler) to the current process's handler, if it installed one — the
 * same divert-into-handler-via-trampoline mechanism as signal_dispatch, but operating
 * on the isr_common EXCEPTION frame rather than the syscall frame. `f` is the exception
 * frame base (isr_stubs.asm passes RSP): f[0..14] = GPRs r15..rax, f[17] = RIP, f[19] =
 * RFLAGS, f[20] = the user RSP the CPU pushed on the ring3->ring0 transition.
 *
 * Returns 1 if it diverted the process into its handler (isr_handler then just returns;
 * isr_common iretq's into the handler). Returns 0 for the DEFAULT action — no handler
 * (SIG_DFL), SIG_IGN (ignoring a synchronous fault would just re-fault forever), a fault
 * taken inside a running handler (can't nest), or an unwritable user stack — the caller
 * then terminates the process as before. The saved context is the FAULTING instruction,
 * so a handler that returns normally re-executes it (and re-faults): real handlers exit()
 * or longjmp() out. */
int signal_deliver_fault(uint64_t* f, int sig) {
    process_t* p = get_current_process();
    if (!p || !p->page_directory) return 0;
    if (sig <= 0 || sig >= NSIG || uncatchable(sig)) return 0;

    uint64_t disp = p->sig_handlers[sig];
    if (disp == SIG_DFL || disp == SIG_IGN) return 0;   /* default action -> terminate */
    if (p->sig_active) return 0;                         /* fault inside a handler -> terminate */

    /* Save the interrupted ring-3 context for SYS_SIGRETURN (sig_saved layout: GPRs
     * r15..rax, then RFLAGS, RIP, user RSP — same as signal_dispatch). */
    for (int i = 0; i < 15; i++) p->sig_saved[i] = f[i];
    p->sig_saved[15] = f[19];                           /* RFLAGS */
    p->sig_saved[16] = f[17];                           /* RIP (faulting instruction) */
    p->sig_saved[17] = f[20];                           /* user RSP */

    /* Build the handler's ring-3 stack (128 B slack, 16-align, push trampoline so
     * RSP%16==8 at entry). copy_to_user translates through user_cr3, which the
     * exception path doesn't set — point it at the faulting process's pd first (its
     * pages are identity-mapped, so the write lands correctly). */
    uint64_t sp = f[20];
    sp -= 128; sp &= ~15ULL; sp -= 8;
    uint64_t tramp = p->sig_trampoline;
    user_cr3 = (uint64_t)p->page_directory;
    if (!tramp || copy_to_user(sp, &tramp, 8) != 0) return 0;   /* can't deliver -> terminate */

    f[9]  = (uint64_t)sig;                              /* RDI = signo (SysV arg1) */
    f[17] = disp;                                       /* RIP = handler entry */
    f[20] = sp;                                         /* user RSP = adjusted stack */
    p->sig_active |= (1u << sig);
    p->sig_mask   |= (1u << sig);                       /* block this signal in its handler */
    return 1;
}

/* SYS_SIGRETURN: the handler returned through the trampoline. Restore the context
 * saved by signal_dispatch into THIS syscall's frame + user_rsp and unblock the
 * handled signal, so the syscall return path iretq's back to where we interrupted. */
void do_sigreturn(void) {
    process_t* p = get_current_process();
    if (!p || !p->sig_active) return;        /* ignore a spurious sigreturn */
    uint64_t* frame = (uint64_t*)syscall_frame_ptr;
    if (!frame) return;
    for (int i = 0; i < 15; i++) frame[i] = p->sig_saved[i];   /* GPRs */
    frame[15] = p->sig_saved[15];            /* RFLAGS */
    frame[16] = p->sig_saved[16];            /* RIP    */
    user_rsp  = p->sig_saved[17];            /* user RSP */
    p->sig_mask  &= ~p->sig_active;          /* unblock the handled signal */
    p->sig_active = 0;
}

/* SYS_SIGPROCMASK: read and/or change the blocked-signal mask. sigsetjmp/
 * siglongjmp use this to save and restore the mask across a non-local jump out
 * of a handler: on restore, a signal being UNBLOCKED also has its sig_active bit
 * cleared (we're abandoning that handler's stack frame), so the SAME signal can
 * be caught again — that's what makes repeated fault recovery work. `how` is
 * SIG_BLOCK(0) / SIG_UNBLOCK(1) / SIG_SETMASK(2); oldset_ptr (if non-zero)
 * receives the previous mask. Returns 0, or -1. */
/* Pure core of sigprocmask: apply `how` (SIG_BLOCK/UNBLOCK/SETMASK) to `old` using `set`,
 * then force SIGKILL/SIGSTOP to stay UNBLOCKED — they can never be masked, or a process
 * could block SIGKILL and make itself unkillable. Returns 0 and writes *out on success, or
 * -1 for an unknown `how` (POSIX EINVAL). Pulled out so the `signal` KAT can pin the mask
 * math AND that uncatchable-can't-be-blocked invariant, both previously untested. */
static int sigmask_apply(int how, uint32_t old, uint32_t set, uint32_t* out) {
    uint32_t nm;
    if (how == 0)      nm = old | set;                /* SIG_BLOCK   */
    else if (how == 1) nm = old & ~set;               /* SIG_UNBLOCK */
    else if (how == 2) nm = set;                      /* SIG_SETMASK */
    else return -1;                                   /* EINVAL: unknown how */
    nm &= ~((1u << SIGKILL) | (1u << SIGSTOP));        /* SIGKILL/SIGSTOP are never blockable */
    *out = nm;
    return 0;
}

long do_sigprocmask(int how, uint64_t set, uint64_t oldset_ptr) {
    process_t* p = get_current_process();
    if (!p) return -1;
    uint32_t nm;
    if (sigmask_apply(how, p->sig_mask, (uint32_t)set, &nm) != 0) return -1;  /* EINVAL, no change */
    if (oldset_ptr) { uint64_t o = p->sig_mask; if (copy_to_user(oldset_ptr, &o, 8) != 0) return -1; }
    p->sig_mask = nm;
    p->sig_active &= nm;   /* a signal we just unblocked is no longer "in its handler" */
    return 0;
}

/* SYS_ALARM: schedule SIGALRM to the calling process after `seconds` seconds (0
 * cancels a pending alarm). Returns the whole seconds left on a previous alarm, or
 * 0. irq_scheduler_tick posts SIGALRM once tick_count passes the deadline; with no
 * handler installed SIGALRM's default action terminates the process (like Unix). */
unsigned int do_alarm(unsigned int seconds) {
    extern volatile uint32_t tick_count;
    process_t* p = get_current_process();
    if (!p) return 0;
    uint32_t now = tick_count;
    unsigned int remaining = 0;
    if (p->alarm_tick != 0) {
        int32_t left = (int32_t)(p->alarm_tick - now);
        if (left > 0) remaining = ((unsigned int)left + 999) / 1000;   /* round up to seconds */
    }
    if (seconds == 0) {
        p->alarm_tick = 0;                     /* cancel */
    } else {
        uint32_t d = now + seconds * 1000;     /* 1000 Hz -> ticks */
        p->alarm_tick = d ? d : 1;             /* avoid 0 (== "no alarm") on a rare wrap */
    }
    return remaining;
}

/* Keyboard Ctrl-C -> post SIGINT to the foreground process. */
void signal_send_foreground(int sig) {
    if (!g_foreground_pid) return;
    process_t* p = find_process(g_foreground_pid);
    if (p) signal_raise(p, sig);
}

/* KAT for the signal-posting core (this subsystem had no self-test). Uses a LOCAL
 * process_t, so it never touches the scheduler or the process table: signal_raise
 * and signal_pending only read/write the struct's own fields. Pins raise validation
 * (range reject + the SIGCONT-resumes-a-stopped-process rule), the highest-signal
 * shift boundary (NSIG-1 = 31, which must not overflow the uint32 masks), and the
 * pending-vs-mask deliverability test. */
int signal_selftest(void) {
    process_t sp = {0};
    sp.state = PROC_RUN;

    signal_raise(&sp, SIGINT);                       /* valid: sets exactly that bit */
    if (sp.sig_pending != (1u << SIGINT)) return 1;
    signal_raise(&sp, 0);                            /* invalid (<= 0): ignored */
    signal_raise(&sp, NSIG);                         /* out of range (>= NSIG): ignored */
    if (sp.sig_pending != (1u << SIGINT)) return 2;
    signal_raise(&sp, NSIG - 1);                     /* highest valid (31): bit 31, no UB */
    if (!(sp.sig_pending & (1u << (NSIG - 1)))) return 3;

    sp.sig_pending = (1u << SIGINT); sp.sig_mask = 0;
    if (!signal_pending(&sp)) return 4;              /* unblocked -> deliverable */
    sp.sig_mask = (1u << SIGINT);
    if (signal_pending(&sp)) return 5;               /* blocked -> not deliverable */

    sp.state = PROC_STOPPED; sp.sig_pending = 0; sp.sig_mask = 0;
    signal_raise(&sp, SIGCONT);                      /* resumes a job-control-stopped process */
    if (sp.state != PROC_RUN) return 6;
    if (!(sp.sig_pending & (1u << SIGCONT))) return 7;

    /* do_sigprocmask's mask core (sigmask_apply): the block/unblock/setmask math, the
     * EINVAL on an unknown `how`, and — critically — that SIGKILL/SIGSTOP can NEVER be
     * blocked (else a process could mask SIGKILL and become unkillable). Untested before. */
    uint32_t m;
    if (sigmask_apply(0, 0, (1u << SIGINT), &m) != 0 || m != (1u << SIGINT)) return 8;   /* BLOCK */
    if (sigmask_apply(1, (1u << SIGINT) | (1u << SIGTERM), (1u << SIGINT), &m) != 0
        || m != (1u << SIGTERM)) return 9;                                                /* UNBLOCK */
    if (sigmask_apply(2, (1u << SIGINT), (1u << SIGUSR1), &m) != 0 || m != (1u << SIGUSR1)) return 10; /* SETMASK replaces */
    if (sigmask_apply(3, 0, 0, &m)  != -1) return 11;   /* unknown how -> EINVAL */
    if (sigmask_apply(-1, 0, 0, &m) != -1) return 12;   /* unknown how -> EINVAL */
    /* the uncatchable-can't-be-blocked invariant, whatever `how`/`set` ask for */
    if (sigmask_apply(2, 0, 0xFFFFFFFFu, &m) != 0 || (m & (1u << SIGKILL)) || (m & (1u << SIGSTOP))) return 13;
    if (sigmask_apply(0, 0, (1u << SIGKILL) | (1u << SIGSTOP), &m) != 0 || m != 0) return 14;

    /* signal_next_deliverable: the lowest-numbered pending, unblocked signal (0 if none) —
     * the delivery-ORDER decision at the heart of signal_dispatch (which signal runs when
     * several are pending, and that a masked one is skipped for the next). Untested before. */
    if (signal_next_deliverable(0, 0) != 0) return 15;                                          /* nothing pending */
    if (signal_next_deliverable(1u << SIGINT, 0) != SIGINT) return 16;                           /* one pending */
    if (signal_next_deliverable((1u << SIGTERM) | (1u << SIGINT), 0) != SIGINT) return 17;       /* lowest number wins (SIGINT < SIGTERM) */
    if (signal_next_deliverable(1u << SIGINT, 1u << SIGINT) != 0) return 18;                     /* pending but masked -> none */
    if (signal_next_deliverable((1u << SIGINT) | (1u << SIGUSR1), 1u << SIGINT) != SIGUSR1) return 19; /* masked one skipped -> next delivers */

    return 0;
}
