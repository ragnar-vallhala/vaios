# VAIOS - Realtime Operating System
One can run the OS in qemu-arm as below:
1. Clone repository as:
```
git clone --recurse-submodules https://github.com/ragnar-vallhala/vaios.git
```
2. Build the `docker image`:
```
cd vaios
docker build -t vaios-arm-qemu .
```
Note: Docker is required