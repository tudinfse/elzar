#!/usr/bin/env python
from __future__ import print_function

import os
from argparse import ArgumentParser
from subprocess import check_call

ELZAR = '/root/code/simd-swift/'


# command line arguments
def get_arguments():
    parser = ArgumentParser(description='')
    subparsers = parser.add_subparsers(help='sub-command help', dest='subparser_name')

    # parser for installing benchmarks
    parser_install = subparsers.add_parser('install', help='download and install all benchmarks')

    # parser for running performance tests
    parser_perf = subparsers.add_parser('performance_tests', help='Run all performance tests')
    parser_perf.add_argument('--num_runs', type=str, default='3')

    # parser for the simple compilation
    parser_compile = subparsers.add_parser('compile', help='Simple compilation')
    parser_compile.add_argument('--test', action="store_true", default=False)

    args = parser.parse_args()
    return args


def main():
    args = get_arguments()
    if args.subparser_name == 'install':
        print('Installing:')
        check_call(ELZAR + 'install/install_phoenix.sh')
        check_call(ELZAR + 'install/install_parsec.sh')
    elif args.subparser_name == 'performance_tests':
        print('Running performance tests')
        os.environ['NUM_RUNS'] = args.num_runs
        check_call(ELZAR + 'install/run_phoenix.sh')
        check_call(ELZAR + 'install/run_parsec.sh')
    elif args.subparser_name == 'compile':
        print('Compiling:')
        raise NotImplementedError("Compilation is not yet implemented.")


if __name__ == '__main__':
    main()
