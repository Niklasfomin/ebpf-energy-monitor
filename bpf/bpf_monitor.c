#define SELECTOR_DIM 2
#define NUM_SLOTS NUM_SOCKETS * SELECTOR_DIM

struct pid_status {
        int pid;
        char comm[16];
        u64 weighted_cycles[NUM_SLOTS];
        u64 time_ns[NUM_SLOTS];
        // set which item of weighted_cycles should be used in bpf
        // in user space, the weighted_cycles is read and initialized
        unsigned int bpf_selector;
        u64 ts[NUM_SLOTS];
};
struct proc_topology {
        u64 ht_id;
        u64 sibling_id;
        u64 core_id;
        u64 processor_id;
        u64 cycles_core;
        u64 cycles_core_updated;
        u64 cycles_core_delta_sibling;
        u64 cycles_thread;
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

#ifdef DEBUG
struct error_code {
        int err;
};

BPF_PERF_OUTPUT(err);
#endif

//BPF_PERF_ARRAY(cpu_cycles, NUM_CPUS);
BPF_PERF_ARRAY(cycles_core, NUM_CPUS);
BPF_PERF_ARRAY(cycles_thread, NUM_CPUS);
BPF_HASH(processors, u64, struct proc_topology);
BPF_HASH(pids, int, struct pid_status);
BPF_HASH(idles, u64, struct pid_status);
BPF_HASH(conf, int, unsigned int);

// Beware: Changing the step in userspace means invalidate the last sample
#define STEP_MIN 1000000000 //2000000000
#define STEP_MAX 4000000000 //2000000000

#define HAPPY_FACTOR 11/20
#define STD_FACTOR 1


static void send_error(struct sched_switch_args *ctx, int err_code) {
#ifdef DEBUG
        struct error_code error;
        error.err = err_code;
        err.perf_submit(ctx, &error, sizeof(error));
#endif
}

int trace_switch(struct sched_switch_args *ctx) {

        int selector_key = 0;
        int old_selector_key = 1;
        int step_key = 2;
        int switch_count_key = 3;

        int array_index = 0;

        unsigned int bpf_selector = 0;
        int ret = 0;
        ret = bpf_probe_read(&bpf_selector, sizeof(bpf_selector), conf.lookup(&selector_key));
        // if selector is not in place correctly, signal debug error and stop tracing routine
        if (ret!= 0 || bpf_selector > 1) {
                send_error(ctx, -1);
                return 0;
        }

        // retrieve general switch count
        unsigned int switch_count = 0;
        ret = 0;
        ret = bpf_probe_read(&switch_count, sizeof(switch_count), conf.lookup(&switch_count_key));

        //retrieve old selector to update switch count correctly
        unsigned int old_bpf_selector = 0;
        ret = 0;
        ret = bpf_probe_read(&old_bpf_selector, sizeof(old_bpf_selector), conf.lookup(&old_selector_key));
        if (ret!= 0 || old_bpf_selector > 1) {
                send_error(ctx, -2);
                return 0;
        } else if(old_bpf_selector != bpf_selector) {
                switch_count = 1;
                conf.update(&old_selector_key, &bpf_selector);
        } else {
                switch_count++;
        }
        conf.update(&switch_count_key, &switch_count);

        // retrieve sampling step
        // Beware: Increasing the step in userspace means that the next sample is invalid
        // Reducing the step in userspace is not an issue, give that it excludes data
        // that is inside an can be discarded
        unsigned int step = 1000000000;
        ret = bpf_probe_read(&step, sizeof(step), conf.lookup(&step_key));
        if (ret!= 0 || step < STEP_MIN || step > STEP_MAX) {
                send_error(ctx, -3);
                return 0;
        }


        // get data about processor and performance counters
        // lookup also the pid of the exiting process
        u64 processor_id = bpf_get_smp_processor_id();
        u64 thread_cycles_sample = cycles_thread.perf_read(processor_id);
        u64 core_cycles_sample = cycles_core.perf_read(processor_id);
        u64 ts = bpf_ktime_get_ns();
        int old_pid = ctx->prev_pid;

        // fetch data about processor executing the thing
        struct proc_topology topology_info;
        ret = bpf_probe_read(&topology_info, sizeof(topology_info), processors.lookup(&processor_id));
        if(ret!= 0 || topology_info.ht_id > NUM_CPUS) {
                send_error(ctx, -4);
                return 0;
        }

        // fetch the status of the exiting pid
        struct pid_status status_old;
        status_old.pid = -1;

        // if the pid is 0, then use the idles perf_hash
        if(old_pid == 0) {
                ret = bpf_probe_read(&status_old, sizeof(status_old), idles.lookup(&(processor_id)));
        } else {
                ret = bpf_probe_read(&status_old, sizeof(status_old), pids.lookup(&(old_pid)));
        }

        if(ret == 0) {
                u64 sibling_id = topology_info.sibling_id;
                struct proc_topology sibling_info;
                ret = bpf_probe_read(&sibling_info, sizeof(sibling_info), processors.lookup(&(sibling_id)));

                if(ret != 0) {
                        // wrong info on topology, do nothing
                        send_error(ctx, 3);
                        return 0;
                }
                u64 old_thread_cycles = thread_cycles_sample;
                u64 old_core_cycles = core_cycles_sample;
                u64 cycles_core_delta_sibling = 0;
                u64 old_time = ts;
                if (topology_info.ts > 0) {
                        old_time = topology_info.ts;
                        old_thread_cycles = topology_info.cycles_thread;
                        old_core_cycles = topology_info.cycles_core;
                        cycles_core_delta_sibling = topology_info.cycles_core_delta_sibling;
                        if (old_pid > 0 && sibling_info.running_pid > 0 && core_cycles_sample > topology_info.cycles_core_updated) {
                                cycles_core_delta_sibling += core_cycles_sample - topology_info.cycles_core_updated;
                        }
                }

                //find the sibling pid status
                int sibling_pid = sibling_info.running_pid;
                struct pid_status sibling_process;
                if(sibling_pid == 0) {
                        //read from idles table
                        ret = bpf_probe_read(&sibling_process, sizeof(sibling_process), idles.lookup(&(sibling_id)));
                } else {
                        //read from pids table
                        ret = bpf_probe_read(&sibling_process, sizeof(sibling_process), pids.lookup(&(sibling_pid)));
                }

                if(ret == 0) {
                        u64 last_ts_pid_in = 0;
                        //trick the compiler with loop unrolling
                        #pragma clang loop unroll(full)
                        for(array_index = 0; array_index<NUM_SLOTS; array_index++) {
                                if(array_index == sibling_process.bpf_selector + SELECTOR_DIM * sibling_info.processor_id) {
                                        last_ts_pid_in = sibling_process.ts[array_index];
                                }
                        }

                        // here just update the selector and reset counter if needed
                        if(sibling_process.bpf_selector != bpf_selector || last_ts_pid_in + step < ts) {
                                sibling_process.bpf_selector = bpf_selector;
                                //trick the compiler with loop unrolling
                                #pragma clang loop unroll(full)
                                for(array_index = 0; array_index<NUM_SLOTS; array_index++) {
                                        if(array_index % SELECTOR_DIM == bpf_selector) {
                                                sibling_process.weighted_cycles[array_index] = 0;
                                                sibling_process.time_ns[array_index] = 0;
                                        }
                                }
                        }

                        //trick the compiler with loop unrolling
                        //update measurements for sibling pid
                        #pragma clang loop unroll(full)
                        for(array_index = 0; array_index<NUM_SLOTS; array_index++) {
                                if(array_index == sibling_process.bpf_selector + SELECTOR_DIM * sibling_info.processor_id) {
                                        sibling_process.time_ns[array_index] += ts - old_time;
                                        sibling_process.ts[array_index] = ts;
                                        if(sibling_pid == 0) {
                                                idles.update(&(sibling_id), &sibling_process);
                                        } else {
                                                pids.update(&(sibling_pid), &sibling_process);
                                        }
                                }
                        }

                        //
                        // Instead of adding stuff directly, given that we don't have the measure of the sibling thread cycles,
                        // We are summing up the information on our side to the core cycles of the sibling
                        //
                        //update sibling process info
                        if(sibling_pid > 0 && old_pid > 0 && core_cycles_sample > sibling_info.cycles_core_updated) {
                                sibling_info.cycles_core_delta_sibling += core_cycles_sample - sibling_info.cycles_core_updated;
                        }
                        sibling_info.cycles_core_updated = core_cycles_sample;
                        sibling_info.ts = ts;
                        processors.update(&sibling_id, &sibling_info);
                }

                u64 last_ts_pid_in = 0;
                //trick the compiler with loop unrolling
                #pragma clang loop unroll(full)
                for(array_index = 0; array_index<NUM_SLOTS; array_index++) {
                        if(array_index == status_old.bpf_selector + SELECTOR_DIM * topology_info.processor_id) {
                                last_ts_pid_in = status_old.ts[array_index];
                        }
                }

                // here just update the selector and reset counter if needed
                if(status_old.bpf_selector != bpf_selector || last_ts_pid_in + step < ts) {
                        status_old.bpf_selector = bpf_selector;
                        //trick the compiler with loop unrolling
                        #pragma clang loop unroll(full)
                        for(array_index = 0; array_index<NUM_SLOTS; array_index++) {
                                if(array_index % SELECTOR_DIM == bpf_selector) {
                                        status_old.weighted_cycles[array_index] = 0;
                                        status_old.time_ns[array_index] = 0;
                                }
                        }
                }

                //trick the compiler with loop unrolling
                // update measurements for our pid
                #pragma clang loop unroll(full)
                for(array_index = 0; array_index<NUM_SLOTS; array_index++) {
                        if(array_index == status_old.bpf_selector + SELECTOR_DIM * topology_info.processor_id) {
                                //discard sample if cycles counter did overflow
                                if (thread_cycles_sample > old_thread_cycles){
                                        u64 cycle1 = thread_cycles_sample - old_thread_cycles;
                                        u64 cycle_overlap = cycles_core_delta_sibling;
                                        u64 cycle_non_overlap = cycle1 > cycles_core_delta_sibling ? cycle1 - cycles_core_delta_sibling : 0;
                                        status_old.weighted_cycles[array_index] += cycle_non_overlap + cycle_overlap*HAPPY_FACTOR;
                                } else {
                                        send_error(ctx, old_pid);
                                }
                                status_old.time_ns[array_index] += ts - old_time;
                                status_old.ts[array_index] = ts;
                                if(old_pid == 0) {
                                        idles.update(&processor_id, &status_old);
                                } else {
                                        pids.update(&old_pid, &status_old);
                                }
                        }
                }
        }

        //
        // handle new scheduled process
        //
        int new_pid = ctx->next_pid;
        struct pid_status status_new;
        if(new_pid == 0) {
                ret = bpf_probe_read(&status_new, sizeof(status_new), idles.lookup(&(processor_id)));
        } else {
                ret = bpf_probe_read(&status_new, sizeof(status_new), pids.lookup(&(new_pid)));
        }
        //If no status for PID, then create one, otherwise update selector
        if(ret) {
                //send_error(ctx, -1 * new_pid);
                bpf_probe_read(&(status_new.comm), sizeof(status_new.comm), ctx->next_comm);
                #pragma clang loop unroll(full)
                for(array_index = 0; array_index<NUM_SLOTS; array_index++) {
                        status_new.ts[array_index] = ts;
                        status_new.weighted_cycles[array_index] = 0;
                        status_new.time_ns[array_index] = 0;
                }
                status_new.pid = new_pid;
                status_new.bpf_selector = bpf_selector;
                if(new_pid == 0) {
                        idles.insert(&processor_id, &status_new);
                } else {
                        pids.insert(&new_pid, &status_new);
                }
        }
        //add info on new running pid into processors table
        topology_info.running_pid = new_pid;
        topology_info.cycles_thread = thread_cycles_sample;
        topology_info.cycles_core = core_cycles_sample;
        topology_info.cycles_core_delta_sibling = 0;
        topology_info.cycles_core_updated = core_cycles_sample;
        topology_info.ts = ts;
        processors.update(&processor_id, &topology_info);
        return 0;

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
        topology_info.cycles_thread = cycles_thread.perf_read(processor_id);
        topology_info.cycles_core = cycles_core.perf_read(processor_id);
        topology_info.cycles_core_delta_sibling = 0;
        topology_info.ts = ts;

        processors.update(&processor_id, &topology_info);

        return 0;
}
