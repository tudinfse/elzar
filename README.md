# Elzar

Triple Modular Redundancy using Intel Advanced Vector Extensions.

ELZAR is a compiler framework that transforms unmodified multithreaded applications
to support triple modular redundancy using Intel AVX extensions for vectorization.
See [our technical report](https://arxiv.org/abs/1604.00500) and
[our DSN'16 publication](http://dsn-2016.sciencesconf.org) for details.

## Docker

Ready-to-use [Docker image](https://hub.docker.com/r/tudinfse/elzar/)

## Full installation

* Refer to Dockerfile for initial setup

* Install benchmarks:

```sh
./install/install_parsec.sh
./install/install_phoenix.sh
```

* Run benchmarks:

```sh
./install/run_parsec.sh
./install/run_phoenix.sh
```

By default, each benchmarks is run 3 times. To set the number of times, set environmental variable NUM_RUNS.