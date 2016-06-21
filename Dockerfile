# Dockerfile for LLVM, enhanced by Elzar pass

FROM ubuntu

MAINTAINER Oleksii Oleksenko (alexo_o@ukr.net)

RUN rm /bin/sh && \
    ln -s /bin/bash /bin/sh

# == Basic packages ==
RUN apt-get update && \
    apt-get install -y git \
                       texinfo \
                       vim \
                       libxml2-dev \
                       cmake \
                       python \
                       gcc \
                       build-essential \
                       flex \
                       bison \
                       linux-tools-generic

# get correct perf
RUN list=( /usr/lib/linux-tools/*-generic/perf ) && \
    ln -sf ${list[-1]} /usr/bin/perf

# == LLVM & CLang ==
# prepare environment
ENV LLVM_SOURCE=/root/bin/llvm/llvm/ \
    LLVM_BUILD=/root/bin/llvm/build/ \
    CLANG_SOURCE=/root/bin/llvm/llvm/tools/clang/ \
    GOLD_PLUGIN=/root/bin/binutils/

RUN mkdir -p $LLVM_SOURCE $LLVM_BUILD $GOLD_PLUGIN ${GOLD_PLUGIN}build

# get correct versions of sources
RUN git clone https://github.com/llvm-mirror/llvm $LLVM_SOURCE && \
    git clone --depth 1 git://sourceware.org/git/binutils-gdb.git ${GOLD_PLUGIN}binutils && \
    git clone http://llvm.org/git/compiler-rt.git ${LLVM_SOURCE}projects\compiler-rt && \
    git clone http://llvm.org/git/openmp.git ${LLVM_SOURCE}projects\openmp && \
    git clone https://github.com/llvm-mirror/clang $CLANG_SOURCE

WORKDIR $LLVM_SOURCE
COPY install/llvm/llvm370-01-x86-swift-nocmp.patch ./
COPY install/llvm/llvm370-02-codegen-avxswift-bugfixes.patch ./
RUN git checkout 509fb2c84c5b1cbff85c5963d5a112dd157e91ad && \
    git apply llvm370-01-x86-swift-nocmp.patch && \
    git apply llvm370-02-codegen-avxswift-bugfixes.patch

WORKDIR $CLANG_SOURCE
RUN git checkout e7b486824bfac07b13bb554edab7d62452dab4d8

# build
WORKDIR $LLVM_BUILD
RUN cmake -G "Unix Makefiles" -DCMAKE_BUILD_TYPE="Release" -DLLVM_TARGETS_TO_BUILD="X86" -DCMAKE_INSTALL_PREFIX=${LLVM_BUILD} -DLLVM_BINUTILS_INCDIR=${GOLD_PLUGIN}binutils/include ../llvm && \
    make && \
    make install

# == Gold Linker ==
WORKDIR ${GOLD_PLUGIN}build
RUN ../binutils/configure --enable-gold --enable-plugins --disable-werror
RUN make all-gold && \
    make

RUN cp gold/ld-new /usr/bin/ld && \
    cp binutils/ar /usr/bin/ar && \
    cp binutils/nm-new /usr/bin/nm-new && \
    \
    mkdir -p /usr/lib/bfd-plugins && \
    cp ${LLVM_BUILD}/lib/LLVMgold.so /usr/lib/bfd-plugins

# == ELZAR ==
ENV ELZAR=/root/code/simd-swift/
COPY ./ ${ELZAR}

RUN make -C ${ELZAR}src/simdswift/pass && \
    \
    make -C ${ELZAR}src/benches/util/instanalyzer && \
    make -C ${ELZAR}src/benches/util/renamer


WORKDIR ${ELZAR}src/benches/util/libc
RUN ./makeall.sh

VOLUME /data

WORKDIR /root/code/simd-swift/


# == Interface ==
ENTRYPOINT ["/bin/bash"]