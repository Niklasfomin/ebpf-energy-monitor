from bcc import BPF
from bcc import PerfType
from bcc import PerfHWConfig
import multiprocessing
import ctypes as ct
import os
from proc_topology import BpfProcTopology
from proc_topology import ProcTopology
from process_info import BpfPidStatus
from process_info import SocketProcessItem
from process_info import ProcessInfo
from sample_controller import SampleController

class BpfSample:

    def __init__(self, max_ts, total_time, sched_switch_count, timeslice, \
        pid_dict):
        self.max_ts = max_ts
        self.total_execution_time = total_time
        self.sched_switch_count = sched_switch_count
        self.timeslice = timeslice
        self.pid_dict = pid_dict

    def get_total_execution_time(self):
        return self.total_execution_time

    def get_sched_switch_count(self):
        return self.sched_switch_count

    def get_timeslice(self):
        return self.timeslice

    def get_pid_dict(self):
        return self.pid_dict

    def __str__(self):
        str_representation = ""

        for key, value in sorted(self.pid_dict.iteritems()):
            str_representation = str_representation + str(value) + "\n"

        str_representation = str_representation + self.get_log_line()

        return str_representation

    def get_log_line(self):
        str_representation = "proc time: " \
            + str(self.total_execution_time) + " sched switch count " \
            + str(self.sched_switch_count) + " timeslice " \
            + str(self.timeslice) + "\n"

        return str_representation


class BpfCollector:

    def __init__(self, topology):
        self.topology = topology
        self.bpf_program = BPF(src_file="bpf/bpf_monitor.c", \
            cflags=["-DNUM_CPUS=%d" % multiprocessing.cpu_count(), \
            "-DNUM_SOCKETS=%d" % len(self.topology.get_sockets())])

        self.processors = self.bpf_program.get_table("processors")
        self.pids = self.bpf_program.get_table("pids")
        self.idles = self.bpf_program.get_table("idles")
        self.bpf_config = self.bpf_program.get_table("conf")
        self.selector = 0
        self.SELECTOR_DIM = 2
        self.timeslice = 1000000000

        self.bpf_program["cpu_cycles"].open_perf_event(PerfType.HARDWARE, \
            PerfHWConfig.CPU_CYCLES)

    def start_capture(self, timeslice):
        for key, value in self.topology.get_new_bpf_topology().iteritems():
            self.processors[ct.c_ulonglong(key)] = value

        self.timeslice = timeslice
        self.bpf_config[ct.c_int(0)] = ct.c_uint(self.selector)     # current selector
        self.bpf_config[ct.c_int(1)] = ct.c_uint(self.selector)     # old selector
        self.bpf_config[ct.c_int(2)] = ct.c_uint(self.timeslice)    # timeslice
        self.bpf_config[ct.c_int(3)] = ct.c_uint(0)                 # switch count

        self.bpf_program.attach_tracepoint(tp="sched:sched_switch", \
            fn_name="trace_switch")
        self.bpf_program.attach_tracepoint(tp="sched:sched_process_exit", \
            fn_name="trace_exit")

    def stop_capture(self):
        self.bpf_program.detach_tracepoint(tp="sched:sched_switch")
        self.bpf_program.detach_tracepoint(tp="sched:sched_process_exit")

    def get_new_sample(self, sample_controller, rapl_monitor):
        sample = self._get_new_sample(rapl_monitor)
        sample_controller.compute_sleep_time(sample.get_sched_switch_count())
        self.timeslice = sample_controller.get_timeslice()
        self.bpf_config[ct.c_int(2)] = ct.c_uint(self.timeslice)    # timeslice

        return sample

    def _get_new_sample(self, rapl_monitor):

        total_execution_time = 0.0
        sched_switch_count = self.bpf_config[ct.c_int(3)].value
        tsmax = 0
        total_weighted_cycles = 0
        read_selector = 0
        total_slots_length = len(self.topology.get_sockets())*self.SELECTOR_DIM

        if self.selector == 0:
            self.selector = 1
            read_selector = 0
        else:
            self.selector = 0
            read_selector = 1

        # get new sample from rapl right before changing selector
        rapl_diff = rapl_monitor.get_sample()
        self.bpf_config[ct.c_int(0)] = ct.c_uint(self.selector)

        pid_dict = {}

        for key, data in self.pids.items():
            for multisocket_selector in \
                range(read_selector, total_slots_length, self.SELECTOR_DIM):
                # Compute the number of total weighted cycles
                total_weighted_cycles = total_weighted_cycles \
                    + data.weighted_cycles[multisocket_selector]
                # search max timestamp of the sample
                if data.ts[multisocket_selector] > tsmax:
                    tsmax = data.ts[multisocket_selector]
        for key, data in self.idles.items():
            for multisocket_selector in \
                range(read_selector, total_slots_length, self.SELECTOR_DIM):
                # Compute the number of total weighted cycles
                total_weighted_cycles = total_weighted_cycles \
                    + data.weighted_cycles[multisocket_selector]
                # search max timestamp of the sample
                if data.ts[multisocket_selector] > tsmax:
                    tsmax = data.ts[multisocket_selector]

        active_power= [rapl_diff[skt].power()*1000
            for skt in self.topology.get_sockets()]
        total_active_power = sum(active_power)
        print(total_active_power)

        for key, data in self.pids.items():

            proc_info = ProcessInfo(len(self.topology.get_sockets()))
            proc_info.set_pid(data.pid)
            proc_info.set_comm(data.comm)
            add_proc = False
            proc_info.set_power(self._get_pid_power(data, total_weighted_cycles, total_active_power))

            for multisocket_selector in \
                range(read_selector, total_slots_length, self.SELECTOR_DIM):

                if data.ts[multisocket_selector] + self.timeslice > tsmax:
                    total_execution_time = total_execution_time \
                        + float(data.time_ns[multisocket_selector])/1000000

                    socket_info = SocketProcessItem()
                    socket_info.set_weighted_cycles(\
                        data.weighted_cycles[multisocket_selector])
                    socket_info.set_time_ns(data.time_ns[multisocket_selector])
                    socket_info.set_ts(data.ts[multisocket_selector])
                    proc_info.set_socket_data(\
                        multisocket_selector/self.SELECTOR_DIM, socket_info)

                    add_proc = True
            if add_proc:
                pid_dict[data.pid] = proc_info

        for key, data in self.idles.items():

            proc_info = ProcessInfo(len(self.topology.get_sockets()))
            proc_info.set_pid(data.pid)
            proc_info.set_comm(data.comm)
            add_proc = False
            proc_info.set_power(self._get_idle_power(data, total_weighted_cycles, total_active_power))

            for multisocket_selector in \
                range(read_selector, total_slots_length, self.SELECTOR_DIM):

                if data.ts[multisocket_selector] + self.timeslice > tsmax:
                    total_execution_time = total_execution_time \
                        + float(data.time_ns[multisocket_selector])/1000000

                    socket_info = SocketProcessItem()
                    socket_info.set_weighted_cycles(\
                        data.weighted_cycles[multisocket_selector])
                    socket_info.set_time_ns(data.time_ns[multisocket_selector])
                    socket_info.set_ts(data.ts[multisocket_selector])
                    proc_info.set_socket_data(\
                        multisocket_selector/self.SELECTOR_DIM, socket_info)

                    add_proc = True
            if add_proc:
                pid_dict[-1 * (1 + int(key.value))] = proc_info

        return BpfSample(tsmax, total_execution_time, sched_switch_count, \
            self.timeslice, pid_dict)


    def _get_total_weighted_cycles(self, pids, idles):
        weighted_socket_cycles_pids = sum([sum(pid.weighted_cycles)
            for key, pid in pids.items()])
        weighted_socket_cycles_idles = sum([sum(idle.weighted_cycles)
            for key, idle in idles.items()])
        weighted_socket_cycles = weighted_socket_cycles_idles + weighted_socket_cycles_pids
        return weighted_socket_cycles

    def _get_pid_power(self, pid, total_cycles, active_power):
        pid_power = (active_power *
            (float(sum(pid.weighted_cycles)) / float(total_cycles)))
        cyc = sum(pid.weighted_cycles)
        print("Pid power: ", pid_power, "Pid cycles:", cyc)

        return pid_power

    def _get_idle_power(self, pid, total_cycles, active_power):
        idle_power = (active_power *
            (float(sum(pid.weighted_cycles)) / float(total_cycles)))
        cyc = sum(pid.weighted_cycles)
        print("Idle power: ", idle_power, "Pid cycles:", cyc)

        return idle_power
