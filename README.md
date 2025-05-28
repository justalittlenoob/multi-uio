# A multi uio driver based on IVSHMEM

## Build driver
`make`

## Install driver
`sudo modprobe uio`\
`sudo insmod ./multi_uio.ko`

## Uninstall driver
`sudo rmmod multi_uio.ko`

## Details
`docs/design.md`