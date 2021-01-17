CROSS_COMPILE  = /home/kevin/v3s/opt/arm-linux-gcc/gcc-4.6.4/bin/
CC    = $(CROSS_COMPILE)arm-none-linux-gnueabi-gcc

#  CROSS_COMPILE = 
# CC=$(CROSS_COMPILE)gcc

main: main.c
	$(CC)  -o main main.c -static
clean:
	rm main
