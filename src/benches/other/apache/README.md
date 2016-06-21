# 1. Download and build Apache Web Server #

**NOTE!** One needs LLVM + Clang 3.7.0 and LLVM Gold plugin + Gold linker
installed in the system.

```
# simd-enabled version
mkdir -p ~/bin/httpd/native
cd ~/bin/httpd/native
git clone https://github.com/apache/httpd.git
cd httpd/srclib/
git clone https://github.com/apache/apr.git

cd ../
./buildconf
CC="${HOME}/bin/llvm/build/bin/clang" CFLAGS="-O3 -msse4.2 -mavx2 -mrtm -flto -funroll-loops -fno-builtin -U__OPTIMIZE__ -D__NO_CTYPE=1" LDFLAGS="-flto -Wl,-plugin-opt=save-temps" ./configure --with-included-apr --with-mpm=worker --enable-mods-static=most --prefix="${HOME}/bin/apache"
make -j16
make install

# simd-disabled version
mkdir -p ~/bin/httpd/native_nosse
cd ~/bin/httpd/native_nosse
git clone https://github.com/apache/httpd.git
cd httpd/srclib/
git clone https://github.com/apache/apr.git

cd ../
./buildconf
CC="${HOME}/bin/llvm/build/bin/clang" CFLAGS="-O3 -mno-avx -msse -fno-slp-vectorize -fno-vectorize -mrtm -flto -funroll-loops -fno-builtin -U__OPTIMIZE__ -D__NO_CTYPE=1" LDFLAGS="-flto -Wl,-plugin-opt=save-temps" ./configure --with-included-apr --with-mpm=worker --enable-mods-static=most --prefix="${HOME}/bin/apache"
make -j16
```

This installs Apache httpd web server into `${HOME}/bin/apache`

# 2. Change httpd configuration #

Open `${HOME}/bin/apache/conf/httpd.conf` and change some lines:

```
Listen 8079             # change the port from 80 to some user-land

<IfModule worker.c>     # add in the very end
ServerLimit          1
ThreadLimit          1
StartServers         1
MaxClients         500  # we don't really care
MinSpareThreads      1
MaxSpareThreads      1
ThreadsPerChild      1
MaxRequestsPerChild  0
</IfModule>
```

# 3. Run and test #

```
cd ~/bin/apache/bin/
./httpd
ps auxf | grep 'httpd'
```

Also one can check the web-browser, e.g.: http://141.76.44.184:8079/

Also one can test with ab (Apache Benchmark):
`ab -k -c 1 -n 20000 http://141.76.44.184:8079/`

# 4. Harden #

```
bash collect.sh     # copy httpd.opt.bc into obj/
make ACTION=native  # whatever action we need...
```
