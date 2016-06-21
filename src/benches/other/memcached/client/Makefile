all: bench_client.exe

bench_client.exe: bench_client.c
	gcc -std=gnu99 -O2 -o bench_client.exe bench_client.c -lmemcached -L/usr/lib -L/usr/local/lib -pthread 

clean:
	rm -f bench_client.exe
