struct pid_status {
        int pid;
        char comm[16];
        u64 weighted_cycles[2];
        u64 time_ns[2];
        // set which item of weighted_cycles should be used in bpf
        // in user space, the weighted_cycles is read and initialized
        unsigned int bpf_selector;
        u64 ts;
};
struct proc_topology {
        u64 ht_id;
        u64 sibling_id;
        u64 core_id;
        u64 processor_id;
        u64 cycles;
        u64 ts;
        int running_pid;
};
struct sched_switch_args {
        __u64 pad; // regs after 4.x?
        char prev_comm[16];
        int prev_pid;
        int prev_prio;
        long long prev_state;
        char next_comm[16];
        int next_pid;
        int next_prio;
};
struct sched_process_exec_args {
        __u64 pad; // regs after 4.x?
        char filename[4];
        int pid;
        int old_pid;
};
struct sched_process_fork_args {
        __u64 pad; // regs after 4.x?
        char parent_comm[16];
        int parent_pid;
        char child_comm[16];
        int child_pid;
};
struct sched_process_exit_args {
        __u64 pad; // regs after 4.x?
        char comm[16];
        int pid;
        int prio;
};

//#define DEBUG

#ifdef DEBUG
struct error_code {
        int err;
};

BPF_PERF_OUTPUT(err);
#endif

BPF_PERF_ARRAY(cpu_cycles, NUM_CPUS);
BPF_HASH(processors, u64, struct proc_topology);
BPF_HASH(pids, int, struct pid_status);
BPF_HASH(conf, int, unsigned int);

#define STEP 2000000000
#define HAPPY_FACTOR 5
#define STD_FACTOR 1


static void send_error(struct sched_switch_args *ctx, int err_code) {
#ifdef DEBUG
        struct error_code error;
        error.err = err_code;
        err.perf_submit(ctx, &error, sizeof(error));
#endif
}

int trace_switch(struct sched_switch_args *ctx) {
        int conf_key = 0;
        unsigned int bpf_selector = 0;
        bpf_probe_read(&bpf_selector, sizeof(bpf_selector), conf.lookup(&conf_key));
        // if selector is not in place correctly, signal debug error and stop
        // tracing routine
        if (bpf_selector > 1) {
                send_error(ctx, 0);
                return 0;
        }

        // get data about processor and performance counters
        // lookup also the pid of the exiting process
        u64 processor_id = bpf_get_smp_processor_id();
        u64 cycles = cpu_cycles.perf_read(processor_id);
        u64 ts = bpf_ktime_get_ns();
        int old_pid = ctx->prev_pid;

        // fetch the status of the exiting pid
        struct pid_status status_old;
        bpf_probe_read(&status_old, sizeof(status_old), pids.lookup(&(old_pid)));

        // fetch data about processor executing the thing
        struct proc_topology topology_info;
        bpf_probe_read(&topology_info, sizeof(topology_info), processors.lookup(&processor_id));

        if(topology_info.ht_id > NUM_CPUS) {
                send_error(ctx, 1);
                return 0;
        }

        if(status_old.pid == old_pid) {
                //find the entry related to processor_id and its sibling
                u64 sibling_id = 0;
                bpf_probe_read(&sibling_id, sizeof(sibling_id), &topology_info.sibling_id);
                struct proc_topology *sibling_info = processors.lookup(&(sibling_id));
                if(!sibling_info) {
                        // wrong info on topology, do nothing
                        send_error(ctx, 2);
                        return 0;
                }
                u64 old_cycles = (sibling_info->ts > topology_info.ts) ? sibling_info->cycles : topology_info.cycles;
                u64 old_time = (sibling_info->ts > topology_info.ts) ? sibling_info->ts : topology_info.ts;

                //discard sample if cycles counter did overflow
                if (cycles < old_cycles) {
                        // go to entering process
                        goto handle_entering_pid;
                }

                u64 weight_factor = STD_FACTOR;
                //find the sibling pid status
                if(sibling_info->running_pid > 0) {
                        int sibling_pid = 0;
                        bpf_probe_read(&sibling_pid, sizeof(sibling_pid), &sibling_info->running_pid);
                        struct pid_status sibling_process;// = pids.lookup(&(sibling_pid));
                        bpf_probe_read(&sibling_process, sizeof(sibling_process), pids.lookup(&(sibling_pid)));
                        if(sibling_process.pid == sibling_pid && sibling_process.pid > 0) {
                                weight_factor = HAPPY_FACTOR;
                                if(sibling_process.bpf_selector == 0) {
                                        sibling_process.weighted_cycles[0] += (cycles - old_cycles) + (cycles - old_cycles)/weight_factor;
                                        sibling_process.time_ns[0] += ts - old_time;
                                        sibling_process.ts = ts;
                                        pids.update(&(sibling_pid), &sibling_process);
                                } else if (sibling_process.bpf_selector == 1) {
                                        sibling_process.weighted_cycles[1] += (cycles - old_cycles) + (cycles - old_cycles)/weight_factor;
                                        sibling_process.time_ns[1] += ts - old_time;
                                        sibling_process.ts = ts;
                                        pids.update(&(sibling_pid), &sibling_process);
                                } else {
                                        //selector corrupted, do nothing
                                        send_error(ctx, 3);
                                        return 0;
                                }

                        } else {
                                // outdated info on pid table, do nothing
                                send_error(ctx, 4);
                                return 0;
                        }
                }
                //increment counters on our pid
                if(status_old.bpf_selector == 0) {
                        status_old.weighted_cycles[0] += (cycles - old_cycles) + (cycles - old_cycles)/weight_factor;
                        status_old.time_ns[0] += ts - old_time;
                        status_old.ts = ts;
                        pids.update(&old_pid, &status_old);
                } else if (status_old.bpf_selector == 1) {
                        status_old.weighted_cycles[1] += (cycles - old_cycles) + (cycles - old_cycles)/weight_factor;
                        status_old.time_ns[1] += ts - old_time;
                        status_old.ts = ts;
                        pids.update(&old_pid, &status_old);
                } else {
                        // selector corrupted, do nothing
                        send_error(ctx, 5);
                        return 0;
                }

        }
        //no info on old status, let another enter sched build it

        // handle new scheduled process
entering_pid: old_pid = 0;
        int new_pid = ctx->next_pid;
        struct pid_status status_new;// = pids.lookup(&(new_pid));
        bpf_probe_read(&status_new, sizeof(status_new), pids.lookup(&(new_pid)));

        //If no status for PID, then create one, otherwise update selector
        if(status_new.pid == new_pid) {
                // here just update the selector and reset counter if needed
                if(status_new.bpf_selector != bpf_selector || status_new.ts + STEP < ts) {
                        status_new.bpf_selector = bpf_selector;
                        if(bpf_selector) {
                                status_new.weighted_cycles[1] = 0;
                                status_new.time_ns[1] = 0;
                        } else if (!bpf_selector) {
                                status_new.weighted_cycles[0] = 0;
                                status_new.time_ns[0] = 0;
                        } else {
                                // selector corrupted, do nothing
                                return 0;
                        }
                        pids.update(&new_pid, &status_new);
                }
        } else {
                bpf_probe_read(&(status_new.comm), sizeof(status_new.comm), ctx->next_comm);
                status_new.pid = new_pid;
                status_new.ts = ts;
                status_new.weighted_cycles[0] = 0;
                status_new.weighted_cycles[1] = 0;
                status_new.time_ns[0] = 0;
                status_new.time_ns[1] = 0;
                status_new.bpf_selector = bpf_selector;
                pids.insert(&new_pid, &status_new);
        }

        //add info on new running pid into processors table
        topology_info.running_pid = new_pid;
        topology_info.cycles = cycles;
        topology_info.ts = ts;
        processors.update(&processor_id, &topology_info);
        return 0;

handle_entering_pid: send_error(ctx, 6);
        goto entering_pid;
}

int trace_exit(struct sched_process_exit_args *ctx) {

        char comm[16];
        bpf_probe_read(&(comm), sizeof(comm), ctx->comm);
        int pid = ctx->pid;
        u64 ts = bpf_ktime_get_ns();
        u64 processor_id = bpf_get_smp_processor_id();

        //remove the pid from the table if there
        pids.delete(&pid);

        struct proc_topology topology_info;
        bpf_probe_read(&topology_info, sizeof(topology_info), processors.lookup(&processor_id));

        topology_info.running_pid = 0;
        topology_info.cycles = cpu_cycles.perf_read(processor_id);
        topology_info.ts = ts;

        processors.update(&processor_id, &topology_info);

        return 0;
}
