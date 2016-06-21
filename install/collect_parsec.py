#!/usr/bin/env python

fres = open('/data/parsec_raw.txt', 'w')
fres.write('bench type threads cycles instructions avxinstructions time\n')


def collect(filename, suffix):
    with open(filename, 'r') as f:
        prog_type   = 'DUMMY'
        benchmark   = 'DUMMY'
        num_threads = 0

        cycles      = 0
        instructions= 0
        avxinstructions = 0

        time        = 0.0

        isPerf = False

        lines = f.readlines()
        for l in lines:
            if l.startswith('--- Running '):
                benchmark   = l.split('--- Running ')[1].split(' ')[0]
                num_threads = int(l.split('--- Running ')[1].split(' ')[1])
                prog_type   = l.split('--- Running ')[1].split(' ')[2]
                continue

            if "Performance counter stats for" in l:
                isPerf = True
                continue

            if isPerf and "cycles" in l:
                cycles = int(l.split()[0].replace('.', ''))
                continue
            if isPerf and "instructions" in l:
                instructions = int(l.split()[0].replace('.', ''))
                continue
            if isPerf and "r7C6" in l:
                avxinstructions = int(l.split()[0].replace('.', ''))
                continue

            if isPerf and "seconds time elapsed" in l:
                time = float(l.split()[0].replace(',', '.'))
                isPerf = False

                benchmark += suffix
                fres.write('%s %s %d %d %d %d %f\n' %
                    (benchmark, prog_type, num_threads, cycles, instructions, avxinstructions, time))
                continue


collect('/data/parsec.log', '')
