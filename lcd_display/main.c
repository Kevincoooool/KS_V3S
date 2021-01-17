#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <asm/types.h>
#include <linux/videodev2.h>
#include <linux/fb.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <math.h>
#include <wchar.h>
#include <time.h>
#include <stdbool.h>

#define CAM_WIDTH 240
#define CAM_HEIGHT 240

static char *dev_video;
static char *dev_fb0;

static char *yuv_buffer;
static char *rgb_buffer;
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;
#define YUVToRGB(Y)                                                            \
	((u16)((((u8)(Y) >> 3) << 11) | (((u8)(Y) >> 2) << 5) | ((u8)(Y) >> 3)))
struct v4l2_buffer video_buffer;
/*全局变量*/
int lcd_fd;
int video_fd;
unsigned char *lcd_mem_p = NULL; //保存LCD屏映射到进程空间的首地址
struct fb_var_screeninfo vinfo;
struct fb_fix_screeninfo finfo;

char *video_buff_buff[4]; /*保存摄像头缓冲区的地址*/
int video_height = 0;
int video_width = 0;
unsigned char *lcd_display_buff; //LCD显存空间
unsigned char *lcd_display_buff2; //LCD显存空间
static void errno_exit(const char *s)
{
	fprintf(stderr, "%s error %d, %s\n", s, errno, strerror(errno));
	exit(EXIT_FAILURE);
}

static int xioctl(int fh, int request, void *arg)
{
	int r;

	do {
		r = ioctl(fh, request, arg);
	} while (-1 == r && EINTR == errno);

	return r;
}

static int video_init(void)
{
	struct v4l2_capability cap;
	ioctl(video_fd, VIDIOC_QUERYCAP, &cap);

	struct v4l2_fmtdesc dis_fmtdesc;
	dis_fmtdesc.index = 0;
	dis_fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	// printf("-----------------------支持格式---------------------\n");
	// while (ioctl(video_fd, VIDIOC_ENUM_FMT, &dis_fmtdesc) != -1) {
	// 	printf("\t%d.%s\n", dis_fmtdesc.index + 1,
	// 	       dis_fmtdesc.description);
	// 	dis_fmtdesc.index++;
	// }
	struct v4l2_format video_format;
	video_format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	video_format.fmt.pix.width = CAM_WIDTH;
	video_format.fmt.pix.height = CAM_HEIGHT;
	video_format.fmt.pix.pixelformat =
		V4L2_PIX_FMT_YUYV; //使用JPEG格式帧，用于静态图像采集

	ioctl(video_fd, VIDIOC_S_FMT, &video_format);

	printf("当前摄像头支持的分辨率:%dx%d\n", video_format.fmt.pix.width,
	       video_format.fmt.pix.height);
	if (video_format.fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV) {
		printf("当前摄像头不支持YUYV格式输出.\n");
		video_height = video_format.fmt.pix.height;
		video_width = video_format.fmt.pix.width;
		//return -3;
	} else {
		video_height = video_format.fmt.pix.height;
		video_width = video_format.fmt.pix.width;
		printf("当前摄像头支持YUYV格式输出.width %d height %d\n",
		       video_height, video_height);
	}
	/*3. 申请缓冲区*/
	struct v4l2_requestbuffers video_requestbuffers;
	memset(&video_requestbuffers, 0, sizeof(struct v4l2_requestbuffers));
	video_requestbuffers.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	video_requestbuffers.count = 4;
	video_requestbuffers.memory = V4L2_MEMORY_MMAP;
	if (ioctl(video_fd, VIDIOC_REQBUFS, &video_requestbuffers))
		return -4;
	printf("成功申请的缓冲区数量:%d\n", video_requestbuffers.count);
	/*4. 得到每个缓冲区的地址: 将申请的缓冲区映射到进程空间*/
	struct v4l2_buffer video_buffer;
	memset(&video_buffer, 0, sizeof(struct v4l2_buffer));
	int i;
	for (i = 0; i < video_requestbuffers.count; i++) {
		video_buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		video_buffer.index = i;
		video_buffer.memory = V4L2_MEMORY_MMAP;
		if (ioctl(video_fd, VIDIOC_QUERYBUF, &video_buffer))
			return -5;
		/*映射缓冲区的地址到进程空间*/
		video_buff_buff[i] =
			mmap(NULL, video_buffer.length, PROT_READ | PROT_WRITE,
			     MAP_SHARED, video_fd, video_buffer.m.offset);
		printf("第%d个缓冲区地址:%#X\n", i, video_buff_buff[i]);
	}
	/*5. 将缓冲区放入到采集队列*/
	memset(&video_buffer, 0, sizeof(struct v4l2_buffer));
	for (i = 0; i < video_requestbuffers.count; i++) {
		video_buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		video_buffer.index = i;
		video_buffer.memory = V4L2_MEMORY_MMAP;
		if (ioctl(video_fd, VIDIOC_QBUF, &video_buffer)) {
			printf("VIDIOC_QBUF error\n");
			return -6;
		}
	}
	printf("启动摄像头采集\n");
	/*6. 启动摄像头采集*/
	int opt_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (ioctl(video_fd, VIDIOC_STREAMON, &opt_type)) {
		printf("VIDIOC_STREAMON error\n");
		return -7;
	}

	return 0;
}
int lcd_init(void)
{
	/*2. 获取可变参数*/
	if (ioctl(lcd_fd, FBIOGET_VSCREENINFO, &vinfo))
		return -2;
	printf("屏幕X:%d   屏幕Y:%d  像素位数:%d\n", vinfo.xres, vinfo.yres,
	       vinfo.bits_per_pixel);
	//分配显存空间,完成图像显示
	lcd_display_buff =
		malloc(vinfo.xres * vinfo.yres * vinfo.bits_per_pixel / 8);
	/*3. 获取固定参数*/
	if (ioctl(lcd_fd, FBIOGET_FSCREENINFO, &finfo))
		return -3;
	printf("smem_len=%d Byte,line_length=%d Byte\n", finfo.smem_len,
	       finfo.line_length);

	/*4. 映射LCD屏物理地址到进程空间*/
	lcd_mem_p = (unsigned char *)mmap(0, finfo.smem_len,
					  PROT_READ | PROT_WRITE, MAP_SHARED,
					  lcd_fd, 0); //从文件的那个地方开始映射
	memset(lcd_mem_p, 0xFFFFFFFF, finfo.smem_len);
	printf("映射LCD屏物理地址到进程空间\n");
	return 0;
}

static void close_device(void)
{
	if (-1 == close(video_fd))
		errno_exit("close");
	video_fd = -1;

	if (-1 == close(lcd_fd))
		errno_exit("close");
	lcd_fd = -1;
}

static void open_device(void)
{
	video_fd = open(dev_video, O_RDWR /* required */ | O_NONBLOCK, 0);
	if (-1 == video_fd) {
		fprintf(stderr, "Cannot open '%s': %d, %s\n", dev_video, errno,
			strerror(errno));
		exit(EXIT_FAILURE);
	}

	lcd_fd = open(dev_fb0, O_RDWR, 0);
	if (-1 == lcd_fd) {
		fprintf(stderr, "Cannot open '%s': %d, %s\n", dev_fb0, errno,
			strerror(errno));
		exit(EXIT_FAILURE);
	}
}
/*
将YUV格式数据转为RGB
*/
void yuv_to_rgb(unsigned char *yuv_buffer, unsigned char *rgb_buffer,
		int iWidth, int iHeight)
{
	int x;
	int z = 0;
	unsigned char *ptr = rgb_buffer;
	unsigned char *yuyv = yuv_buffer;
	for (x = 0; x < iWidth * iHeight; x++) {
		int r, g, b;
		int y, u, v;
		if (!z)
			y = yuyv[0] << 8;
		else
			y = yuyv[2] << 8;
		u = yuyv[1] - 128;
		v = yuyv[3] - 128;
		r = (y + (359 * v)) >> 8;
		g = (y - (88 * u) - (183 * v)) >> 8;
		b = (y + (454 * u)) >> 8;
		*(ptr++) = (b > 255) ? 255 : ((b < 0) ? 0 : b);
		*(ptr++) = (g > 255) ? 255 : ((g < 0) ? 0 : g);
		*(ptr++) = (r > 255) ? 255 : ((r < 0) ? 0 : r);
		if (z++) {
			z = 0;
			yuyv += 4;
		}
	}
}

void rgb24_to_rgb565(char *rgb24, char *rgb16)
{
	int i = 0, j = 0;
	for (i = 0; i < 240 * 240 * 3; i += 3) {
		rgb16[j] = rgb24[i] >> 3; // B
		rgb16[j] |= ((rgb24[i + 1] & 0x1C) << 3); // G
		rgb16[j + 1] = rgb24[i + 2] & 0xF8; // R
		rgb16[j + 1] |= (rgb24[i + 1] >> 5); // G

		j += 2;
	}
}
int main(int argc, char **argv)
{
	dev_video = "/dev/video0";
	dev_fb0 = "/dev/fb0";
	open_device();
	video_init();
	lcd_init();
	/*3. 读取摄像头的数据*/
	struct pollfd video_fds;
	video_fds.events = POLLIN;
	video_fds.fd = video_fd;

	memset(&video_buffer, 0, sizeof(struct v4l2_buffer));
	rgb_buffer = malloc(CAM_WIDTH * CAM_HEIGHT * 3);
	yuv_buffer = malloc(CAM_WIDTH * CAM_HEIGHT * 3);
	unsigned char *rgb_p;
	int w, h, i, j;
	unsigned char r, g, b;
	unsigned int c;
	while (1) {
		/*等待摄像头采集数据*/
		poll(&video_fds, 1, -1);
		/*得到缓冲区的编号*/
		video_buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		video_buffer.memory = V4L2_MEMORY_MMAP;
		ioctl(video_fd, VIDIOC_DQBUF, &video_buffer);
		printf("当前采集OK的缓冲区编号:%d,地址:%#X num:%d\n",
		       video_buffer.index, video_buff_buff[video_buffer.index],
		       strlen(video_buff_buff[video_buffer.index]));
		/*对缓冲区数据进行处理*/
		yuv_to_rgb(video_buff_buff[video_buffer.index], yuv_buffer,
		video_height, video_width);
		rgb24_to_rgb565(yuv_buffer, rgb_buffer);
		printf("显示屏进行显示\n");
		//显示屏进行显示: 将显存空间的数据拷贝到LCD屏进行显示
		memcpy(lcd_mem_p, rgb_buffer,
		       vinfo.xres * vinfo.yres * vinfo.bits_per_pixel / 8);
		/*将缓冲区放入采集队列*/
		ioctl(video_fd, VIDIOC_QBUF, &video_buffer);
		printf("将缓冲区放入采集队列\n");
	}
	/*4. 关闭视频设备*/
	close(video_fd);

	return 0;
}
