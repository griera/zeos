/*
 * sys.c - Syscalls implementation
 */

#include <devices.h>
#include <utils.h>
#include <io.h>
#include <mm.h>
#include <mm_address.h>
#include <sched.h>
#include <errno.h>

#define LECTURA 0
#define ESCRIPTURA 1

extern unsigned int zeos_ticks;

int new_pid()
{
    return next_free_pid++;
}

int check_fd(int fd, int permissions)
{
    if (fd != 1) return -EBADF;
    if (permissions != ESCRIPTURA) return -EACCES;
    return 0;
}

int sys_ni_syscall()
{
    update_stats(current(), RUSER_TO_RSYS);
    update_stats(current(), RSYS_TO_RUSER);
    return -ENOSYS;
}

int sys_getpid()
{
    update_stats(current(), RUSER_TO_RSYS);
    update_stats(current(), RSYS_TO_RUSER);
    return current()->PID;
}

int ret_from_fork() {
    return 0;
}

int sys_fork()
{
    update_stats(current(), RUSER_TO_RSYS);

    int PID = -1;
    unsigned int i;

    /* Returns error if there isn't any available task in the free queue */
    if (list_empty(&freequeue)) return -EAGAIN;

    /* Needed variables related to child and father processes */
    struct list_head *free_pcb = list_first(&freequeue);
    union task_union *child = (union task_union*)list_head_to_task_struct(free_pcb);
    union task_union *parent = (union task_union *)current();
    struct task_struct *pcb_child = &(child->task);
    struct task_struct *pcb_parent = &(parent->task);

    list_del(free_pcb);

    /* Inherits system code+data from father to child */
    copy_data(parent, child, sizeof(union task_union));

    /* Reserve free frames (physical memory) to allocate child's user data */
    allocate_DIR(pcb_child);
    page_table_entry* pagt_child = get_PT(pcb_child);
    page_table_entry* pagt_parent = get_PT(pcb_parent);

    /* Array of free frames that will be reserved for child's user data allocation */
    int resv_frames[NUM_PAG_DATA];

    for (i = 0; i < NUM_PAG_DATA; i++) {
        /* If there is no enough free frames, those reserved thus far must be freed */
        if ((resv_frames[i] = alloc_frame()) == -1) {
            while (i >= 0) free_frame(resv_frames[i--]);
            update_stats(current(), RSYS_TO_RUSER);
            return -ENOMEM;
        }
    }

    /* Inherit user code. Since it's shared among child and father, only it's needed
     * to update child's table page in order to map the correpond entries to frames
     * which allocates father's user code.
     */
    for (i = PAG_LOG_INIT_CODE_P0; i < PAG_LOG_INIT_DATA_P0; i++) {
        set_ss_pag(pagt_child, i, get_frame(pagt_parent, i));
    }

    unsigned int stride = PAGE_SIZE * NUM_PAG_DATA;
    for (i = 0; i < NUM_PAG_DATA; i++) {
        /* Associates a logical page from child to physical reserved frame */
        set_ss_pag(pagt_child, PAG_LOG_INIT_DATA_P0+i, resv_frames[i]);

        /* Inherits one page of user data */
        unsigned int logic_addr = (i + PAG_LOG_INIT_DATA_P0) * PAGE_SIZE;
        set_ss_pag(pagt_parent, i + PAG_LOG_INIT_DATA_P0 + NUM_PAG_DATA, resv_frames[i]);
        copy_data((void *)(logic_addr), (void *)(logic_addr + stride), PAGE_SIZE);
        del_ss_pag(pagt_parent, i + PAG_LOG_INIT_DATA_P0 + NUM_PAG_DATA);
    }

    /* Flushes entire TLB */
    set_cr3(get_DIR(pcb_parent));

    /* Updates child's PCB (Only the ones that the process does not inherit) */
    PID = new_pid();
    pcb_child->PID = PID;
    init_stats(pcb_child);

    /* Prepares the return of the new process, it must return a 0 value
     * and his kernel_esp must point to the top of the stack
     */
    unsigned int ebp;
    __asm__ __volatile__(
        "mov %%ebp,%0\n"
        :"=g"(ebp)
    );

    unsigned int stack_stride = (ebp - (unsigned int)parent)/sizeof(unsigned long);
    /* Dummy value for ebp (new process) */
    child->stack[stack_stride-1] = 0;
    child->stack[stack_stride] = (unsigned long)&ret_from_fork;
    child->task.kernel_esp = &child->stack[stack_stride-1];

    /* Adds child process to ready queue and returns its PID from parent */
    list_add_tail(&(pcb_child->list), &readyqueue);

    update_stats(current(), RSYS_TO_RUSER);

    return PID;
}

void sys_exit()
{
    update_stats(current(), RUSER_TO_RSYS);
    current()->state = ST_FREE;
    free_user_pages(current());
    update_current_state_rr(&freequeue);
    sched_next_rr();
}

int sys_get_stats(int pid, struct stats *st)
{
    update_stats(current(), RUSER_TO_RSYS);

    /* Check user parameters */
    if (pid < 0) {
        update_stats(current(), RSYS_TO_RUSER);
        return -EINVAL;
    }

    /* Checks if st pointer points to a valid user space address */
    if (!access_ok(VERIFY_WRITE, st, sizeof(struct stats))) {
        update_stats(current(), RSYS_TO_RUSER);
        return -EFAULT;
    }

    struct task_struct *desired_pcb = NULL;

    /* Checks if the pid corresponds to the current process or the process
     * associated to PID = pid exists and it's alive
     */
    if (pid == current()->PID) desired_pcb = current();
    else {
        int i = 0, found = 0;
        while (i < NR_TASKS && !found) {
            if (task[i].task.PID == pid && task[i].task.state != ST_FREE) {
                found = 1;
                desired_pcb = &task[i];
            }
            i++;
        }
    }

    /* Comment this section for future improvements on ZeOS
    else {
        struct list_head *pt_list;
        list_for_each(pt_list, &readyqueue) {
            struct task_struct *pcb = list_head_to_task_struct(pt_list);
            if ((pcb->PID == pid) && (pcb->state == ST_READY)) {
                desired_pcb = pcb;
                break;
            }
        }
    }
    */

    if (desired_pcb == NULL) {
        update_stats(current(), RSYS_TO_RUSER);
        return -ESRCH;
    }

    if (copy_to_user(&(desired_pcb->statistics), st, sizeof(struct stats)) < 0) {
        update_stats(current(), RSYS_TO_RUSER);
        return -EPERM;
    }

    update_stats(current(), RSYS_TO_RUSER);
    return 0;
}

int sys_write(int fd, char *buffer, int size)
{
    update_stats(current(), RUSER_TO_RSYS);

    /* Check user parameters */
    int err = check_fd(fd, ESCRIPTURA);
    if (err < 0) {
        update_stats(current(), RSYS_TO_RUSER);
        return err;
    }
    if (buffer == NULL) {
        update_stats(current(), RSYS_TO_RUSER);
        return -EFAULT;
    }
    if (size < 0) {
        update_stats(current(), RSYS_TO_RUSER);
        return -EINVAL;
    }

    /* Checks if buffer pointer points to a valid user space address */
    if (!access_ok(VERIFY_WRITE, buffer, size)) {
        update_stats(current(), RSYS_TO_RUSER);
        return -EFAULT;
    }

    char sys_buffer[size];
    copy_from_user(buffer, sys_buffer, size);

    /* Call the requested service routine and return the result */
    err = sys_write_console(sys_buffer, size);
    update_stats(current(), RSYS_TO_RUSER);
    return err;

}

int sys_gettime()
{
    update_stats(current(), RUSER_TO_RSYS);
    update_stats(current(), RSYS_TO_RUSER);
    return zeos_ticks;
}

