from __future__ import print_function
import argparse
import random
import subprocess
import os
import signal
import time
import hashlib
import shutil

# ---------------------------- LOCAL PATHS ----------------------------------- #
GDB = "~/bin/binutils-gdb/gdb/gdb"
SDE = "~/bin/intel_sde/sde64"

# ---------------------------- CONSTANTS ------------------------------------- #
DUMPINFO = True
MAXTRIES= 3

SORTOUTPUT  = False
ERROROUTPUT = False

# number of fault injection runs
if not os.environ.get('LIMIT'):  # set default value if not set in environment
    LIMIT = 100
else:
    LIMIT = int(os.environ.get('LIMIT'))

TIMEOUT = 3600  # in seconds

DYNTRACE_REGSEP = "|"

LOGDIR    = "logs"
FULLLOG   = "log.log"
GDBSCRIPT = "gdbscript"
SDELOG    = "sdelog"
GDBLOG    = "gdblog"

DEBUGPORT = 10000

# not all GP registers are supported:
#   - we do not inject into rflags, rsp and rip, these are considered control-flow
SUPPORTED_GP_REGS = ["rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp",
                  "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"]

# instructions that must be definitely ignored (control-flow instructions)
IGNORED_INSTS = ["pop", "push", "ret", "call", "cmp"]

# name for Intel RTM in dynamic trace produced by Intel SDE
RTM_TYPE_NAME = "RTM"
# name for common x86 instrs in dynamic trace produced by Intel SDE
BASE_TYPE_NAME = "BASE"

XBEGIN_NAME = "xbegin"
XEND_NAME   = "xtest"


# ---------------------------- GLOBALS --------------------------------------- #

# instr address -> [total number of invocations, output reg, next instr address]
insts = {}

# reference output for this program
ref_output = ""
# file with binary output written by the program; compare using md5
binary_output = ""

# ------------------------------- HELPERS ------------------------------------ #
def run2(args1, args2, timeout = TIMEOUT):
    class Alarm(Exception):
        pass

    def alarm_handler(signum, frame):
        raise Alarm

    if DUMPINFO:
        print(args1)
        print(args2)

    p1 = subprocess.Popen(args1, shell = True
            , stdout = subprocess.PIPE
            , stderr = subprocess.PIPE
            , preexec_fn = os.setsid)

    # wait a bit until first process is loaded
    time.sleep(2)

    p2 = subprocess.Popen(args2, shell = True
            , stdout = subprocess.PIPE
            , stderr = subprocess.PIPE
            , preexec_fn = os.setsid)

    signal.signal(signal.SIGALRM, alarm_handler)
    signal.alarm(timeout)

    try:
        stdout2, stderr2 = p2.communicate()
        stdout1, stderr1 = p1.communicate()
        if timeout != -1:
            signal.alarm(0)
    except Alarm:
        try:
            os.killpg(p2.pid, signal.SIGKILL)
            os.killpg(p1.pid, signal.SIGKILL)
        except OSError:
            pass
        return -1, '', '', -1, '', ''
    return p1.returncode, stdout1, stderr1, p2.returncode, stdout2, stderr2


def initLog(ref_output, dynamic_trace_file, rtm_mode, program, args):
    full_log_file = "%s/%s" % (LOGDIR, FULLLOG)
    with open(full_log_file, "w") as f:
        f.write("----- info -----\n")
        f.write("    program: %s\n" % program)
        f.write("       args: %s\n" % args)
        f.write("\n")
        f.write(" ref output: %s\n" % ref_output)
        f.write("  dyn trace: %s\n" % dynamic_trace_file)
        f.write("   rtm mode: %s\n" % rtm_mode)
        f.write("\n")
        f.write("----- log -----\n")
        f.close()


# ------------------- IDENTIFY INSTRUCTIONS TO INJECT INTO ------------------- #
def identifyInstsInOneThread(dynamic_trace_file, examine_thread_id):
    insts_modified = False
    in_rtm = False
    last_inst_addr = "DUMMY"

    with open(dynamic_trace_file, "r") as f:
        for line in f:
            if line.strip() == "":
                continue

            inst_splitted = line.split()
            assert(len(inst_splitted) >= 5)
            assert(inst_splitted[1] == "INS")

            # dissect parts of line
            thread_id  = inst_splitted[0].replace("TID", "").replace(":", "")
            inst_addr  = inst_splitted[2]
            inst_type  = inst_splitted[3]   # category, e.g, "BASE" and "RTM"
            inst_name  = inst_splitted[4]   # mnemonic, e.g. "xor"

            if int(thread_id) != examine_thread_id:
                # we ignore what other threads do
                continue

            # update last added to insts instruction with its successor
            if last_inst_addr != "DUMMY":
                insts[last_inst_addr][2] = inst_addr
                last_inst_addr = "DUMMY"

            if inst_type == RTM_TYPE_NAME:
                if inst_name == XBEGIN_NAME: in_rtm = True
                if inst_name == XEND_NAME:   in_rtm = False
                continue

            if not in_rtm:
                # instruction is not in RTM-covered portion of code, ignore
                continue

            if inst_name in IGNORED_INSTS:
                continue

            if DYNTRACE_REGSEP in line:
                # --- get GP register
                (_, regs_str) = line.split(DYNTRACE_REGSEP)
                # get output register name
                regs_str = regs_str.split(",")[0]   # leave only first reg
                reg_name = regs_str.split("=")[0].strip()
                # not all regs are supported
                if reg_name not in SUPPORTED_GP_REGS:
                    continue
            elif "SSE" in inst_type:
                # --- get SSE (xmm) register
                if len(inst_splitted) < 6:
                    continue
                # get output register name
                reg_name = inst_splitted[5].split(",")[0]
                # only xmm regs are supported
                if reg_name.startswith("xmmword"):
                    continue
                if not reg_name.startswith("xmm"):
                    continue
            elif "AVX" in inst_type or "AVX2" in inst_type:
                # --- get AVX (ymm) register
                if len(inst_splitted) < 6:
                    continue
                # get output register name
                reg_name = inst_splitted[5].split(",")[0]
                # only ymm regs are supported
                if reg_name.startswith("ymmword"):
                    continue
                if not reg_name.startswith("ymm"):
                    continue
            else:
                # --- all other instructions are ignored
                continue

            if inst_addr not in insts:
                # initialize new instruction
                insts[inst_addr] = [1, reg_name, "DUMMY"]
            else:
                # increment number of invocations for existing instruction
                insts[inst_addr][0] += 1

            insts_modified = True
            last_inst_addr = inst_addr

        f.close()
    return insts_modified


def identifyInsts(dyntrace_file):
    # run identifyInstsInOneThread on all threads in program
    # except TID0 (thread 0 is main thread which does not do real processing)
    examine_thread_id = 1
    while True:
        insts_modified = identifyInstsInOneThread(dyntrace_file, examine_thread_id)
        if not insts_modified:
            break
        examine_thread_id += 1

    assert(len(insts) > 1)
    if DUMPINFO:
        print("[examined %d threads]" % (examine_thread_id-1))
        print("insts = %s" % insts)


# ---------------------- WRITE GDB SCRIPT FOR INJECTION ---------------------- #
def writeScript(scriptfile, instaddr, numinvoc, regname, mask):
    if instaddr == "DUMMY":
        return

    with open(scriptfile, "w") as f:
        f.write("target remote :%d\n" % DEBUGPORT)
        f.write("tb *%s\n" % instaddr)          # set breakpoint on instr address
        f.write("ignore 1 %d\n" % (numinvoc-1)) # ignore this breakpoint x times
        f.write("commands 1\n")

        # inject fault in output register
        if regname.startswith("xmm"):
            f.write("  p $%s.uint128\n" % regname)
            f.write("  set $%s.v2_int64[0] = $%s.v2_int64[0] ^ %d\n" % (regname, regname, mask))
            f.write("  p $%s.uint128\n" % regname)
        elif regname.startswith("ymm"):
            f.write("  p $%s.v2_int128\n" % regname)
            f.write("  set $%s.v4_int64[0] = $%s.v4_int64[0] ^ %d\n" % (regname, regname, mask))
            f.write("  p $%s.v2_int128\n" % regname)
        elif regname.startswith("rflags"):
            f.write("  p $eflags\n")
            f.write("  set $eflags = $eflags ^ 0xC5\n") # flip CF, PF, ZF, and SF
            f.write("  p $eflags\n")
        else:
            f.write("  p $%s\n" % regname)
            f.write("  set $%s = (long long) $%s ^ %d\n" % (regname, regname, mask))
            f.write("  p $%s\n" % regname)

        f.write("  continue\n")
        f.write("end\n")
        f.write("continue\n")
        f.write("p \"Going to detach...\"\n")
        f.write("detach\n")

        f.close()


# --------------------------- INJECT RANDOM FAULT ---------------------------- #
def injectFault(rtmmode, index, program, args):
    for trynum in range(0, MAXTRIES):
        instaddr = random.choice(insts.keys())

        totalinvoc = insts[instaddr][0]
        numinvoc   = random.randint(1, totalinvoc)
        regname    = insts[instaddr][1]
        injectinstaddr = insts[instaddr][2]
        mask       = random.randint(1, 255)

        # restrict only to first 300 invocations, otherwise too slow
        numinvoc   = numinvoc % 300

        scriptfile = "%s/%s_%06d.%s" % (LOGDIR, program, index, GDBSCRIPT)
        writeScript(scriptfile, injectinstaddr, numinvoc, regname, mask)

        sde_run = "%s -rtm-mode %s -debug -debug-port %d -- %s %s" % \
            (SDE, rtmmode, DEBUGPORT, program, args)

        gdb_run = "%s --batch --command=%s --args %s %s" % \
            (GDB, scriptfile, program, args)

        # before we run, remove tmp folder
        shutil.rmtree('tmp', True)

        retcode1, stdout1, stderr1, retcode2, stdout2, stderr2 = run2(sde_run, gdb_run)

        sde_log = "[return code: %d]\n\n---------- stderr ----------\n%s\n\n---------- stdout ----------\n%s" % \
                    (retcode1, stderr1, stdout1)
        gdb_log = "[return code: %d]\n\n---------- stderr ----------\n%s\n\n---------- stdout ----------\n%s" % \
                    (retcode2, stderr2, stdout2)

        res = "DUMMY"
        if retcode1 == -1 or retcode2 == -1:
            # timeout signaled
            res = "HANG"
        elif retcode2 != 0:
            # gdb failed -- this is weird
            res = "GDB"
        elif retcode1 != 0:
            # program failed
            if retcode1 == 2:   res = "SWIFT"
            elif (retcode1 == 255 and
                  stdout1.startswith("E: Unable to create debugger connection")):
                res = "GDB"
            elif retcode1 == 1:
                if "SDE PINTOOL EXITNOW ERROR" in stderr1:    res = "SDE"
                elif "unaligned memory reference" in stderr1: res = "OS"
                else:                                         res = "PROG"
            else:               res = "OS"
        else:
            # both program and gdb exited nicely -- maybe it's a SDC?
            if binary_output != "":
                # binary, calc md5 of binary_output and compare with ref
                prog_output = hashlib.md5(open(binary_output, 'rb').read()).hexdigest()
            else:
                # normal text, remove header, sort if needed and compare with ref
                if ERROROUTPUT:
                    tmplist = stderr1.splitlines(True)
                    if len(tmplist) > 0 and "TSX log collection started" in tmplist[0]:
                        # remove line of TSX info
                        tmplist = tmplist[1:]
                else:
                    tmplist = stdout1.splitlines(True)
                    tmplist = tmplist[3:] # remove 3 lines of SDE info
                if SORTOUTPUT:
                    tmplist.sort()
                prog_output = ''.join(tmplist)

            if prog_output == ref_output: res = "MASKED"
            else:                         res = "SDC"

        if res == "SDE" or res == "GDB":
            # we want to silently ignore SDE & GDB failures and retry FI again
            continue

        # ----- log everything
        sdelogfile = "%s/%s_%06d.%s" % (LOGDIR, program, index, SDELOG)
        gdblogfile = "%s/%s_%06d.%s" % (LOGDIR, program, index, GDBLOG)
        fulllogfile = "%s/%s" % (LOGDIR, FULLLOG)
        with open(sdelogfile, "w") as f:
            f.write(sde_log)
            f.close()
        with open(gdblogfile, "w") as f:
            f.write(gdb_log)
            f.close()
        with open(fulllogfile, "a") as f:
            f.write("%06d   %6s\n" % (index, res))
            f.close()
        # it was a succesfull fault injection, stop trying
        return


# ------------------------------- MAIN FUNCTION ------------------------------ #
rtmmodes = ['full', 'nop']


def get_compand_line_arguments():
    parser = argparse.ArgumentParser(description='GDB Fault Injector')
    parser.add_argument('-p', '--program',
                        required=True,
                        help='Program under test')
    parser.add_argument('-a', '--arguments',
                        default="",
                        help='Program under test')
    parser.add_argument('-d', '--dyntrace',
                        required=True,
                        help='Dynamic trace log obtained via Intel SDE')
    parser.add_argument('-m', '--rtmmode',
                        required=True,
                        choices=rtmmodes,
                        help='RTM mode to pass to Intel SDE')
    parser.add_argument('-r', '--refoutput',
                        required=True,
                        help='Reference output file')

    group = parser.add_mutually_exclusive_group()
    group.add_argument('-b', '--binaryoutput',
                       default="",
                       help='Binary output file; compare with md5')
    group.add_argument('-s', '--sortoutput',
                       action="store_true",
                       help='Sorts output before comparison')

    parser.add_argument('-l', '--logdir',
                        default="",
                        help='Reference output file')
    parser.add_argument('-o', '--debugport',
                        default=10000,
                        help='Debug port')
    parser.add_argument('-e', '--erroroutput',
                        action="store_true",
                        help='Compare stderr outputs, not stdout')
    parser.add_argument('-f', '--injecteflags',
                        action="store_true",
                        help='Inject into EFLAGS register')

    return parser.parse_args()


def main():
    print("Changing ptrace_scope to 0...")
    subprocess.call('echo 0 | sudo tee /proc/sys/kernel/yama/ptrace_scope > /dev/null', shell=True)

    args = get_compand_line_arguments()

    global SORTOUTPUT, ERROROUTPUT, FULLLOG, LOGDIR, SUPPORTED_GP_REGS, DEBUGPORT, binary_output, ref_output

    # set globals to values from command line or keep default values
    SORTOUTPUT = True if args.sortoutput else SORTOUTPUT
    ERROROUTPUT = True if args.erroroutput else ERROROUTPUT
    LOGDIR = args.logdir if args.logdir != "" else LOGDIR
    DEBUGPORT = int(args.debugport) if args.debugport != "" else DEBUGPORT
    binary_output = args.binaryoutput if args.binaryoutput != "" else binary_output

    FULLLOG = os.path.basename(args.program) + '.log'

    if args.injecteflags:
        SUPPORTED_GP_REGS.append('rflags')  # look for rflags in the trace

    if binary_output != "":
        # ref output as binary, calculate md5 sum
        ref_output = hashlib.md5(open(args.refoutput, 'rb').read()).hexdigest()
    else:
        # ref output as normal text, read all lines and sort if needed
        with open(args.refoutput, "r") as f:
            ref_output = f.read()
            if SORTOUTPUT:
                tmplist = ref_output.splitlines(True)
                tmplist.sort()
                ref_output = ''.join(tmplist)
    assert(ref_output != "")

    try:
        os.makedirs("%s/%s" % (LOGDIR, os.path.dirname(args.program)))
    except:
        pass
#        assert(0)

    initLog(args.refoutput, args.dyntrace, args.rtmmode, args.program, args.arguments)
    identifyInsts(args.dyntrace)
    for i in range(0, LIMIT):
        injectFault(args.rtmmode, i, args.program, args.arguments)


if __name__ == "__main__":
    main()
