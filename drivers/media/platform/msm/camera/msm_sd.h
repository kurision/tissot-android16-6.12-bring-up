/* Copyright (c) 2012-2016, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef _MSM_SD_H
/* The donor's camera stack flag: this tree has only the legacy stack. */

/* The donor's camera stack flag: this tree has only the legacy stack. */

#define _MSM_SD_H

#include <media/v4l2-subdev.h>
#include <linux/gpio/consumer.h>
#include <linux/of.h>
#include <media/msmb_camera-legacy.h>

/*
 * The donor's camera stack flag: the donor carried two stacks for the
 * same silicon and chose between them per board.  This tree has only
 * the legacy one, so the flag is always set.
 */
extern bool camera_legacy_enable;

struct work_struct;
void camera_queue_late_init(struct work_struct *work);

/*
 * The donor's camera stack flag: the donor carried two stacks for the
 * same silicon and chose between them per board.  This tree has only
 * the legacy one, so the flag is always set.
 */

/* NOTE: this header file should ONLY be included by subdev drivers */

struct msm_sd_close_ioctl {
	unsigned int session;
	unsigned int stream;
};

#define MSM_SD_CLOSE_STREAM \
	_IOWR('V', BASE_VIDIOC_PRIVATE + 26, struct msm_sd_close_ioctl)

#define MSM_SD_CLOSE_SESSION \
	_IOWR('V', BASE_VIDIOC_PRIVATE + 27, struct msm_sd_close_ioctl)

#define MSM_SD_CLOSE_SESSION_AND_STREAM \
	_IOWR('V', BASE_VIDIOC_PRIVATE + 28, struct msm_sd_close_ioctl)

#define MSM_SD_SHUTDOWN \
	_IOWR('V', BASE_VIDIOC_PRIVATE + 29, struct msm_sd_close_ioctl)

#define MSM_SD_NOTIFY_FREEZE \
	_IOWR('V', BASE_VIDIOC_PRIVATE + 30, struct msm_sd_close_ioctl)

#define MSM_SD_UNNOTIFY_FREEZE \
	_IOWR('V', BASE_VIDIOC_PRIVATE + 31, struct msm_sd_close_ioctl)
/*
 * This is used to install Sequence in legacy_msm_sd_register.
 * During msm_close, proper close sequence will be triggered.
 * For example:
 *
 * close_sequence = 0x00100001 (ISP)
 * close_sequence = 0x00100002 (ISP)
 * close_sequence = 0x00100003 (ISP)
 * close_sequence = 0x00200001 (sensor)
 * close_sequence = 0x00200002 (sensor)
 * close_sequence = 0x00200003 (sensor)
 */
#define MSM_SD_CLOSE_1ST_CATEGORY  0x00010000
#define MSM_SD_CLOSE_2ND_CATEGORY  0x00020000
#define MSM_SD_CLOSE_3RD_CATEGORY  0x00030000
#define MSM_SD_CLOSE_4TH_CATEGORY  0x00040000

struct msm_sd_subdev {
	struct v4l2_subdev sd;
	int close_seq;
	struct list_head list;
};

struct msm_sd_req_sd {
	char *name;
	struct v4l2_subdev *subdev;
};

struct msm_sd_req_vb2_q {
	struct vb2_v4l2_buffer * (*get_buf)(int session_id, unsigned int stream_id);
	struct vb2_queue * (*get_vb2_queue)(int session_id,
		unsigned int stream_id);
	struct vb2_v4l2_buffer * (*get_buf_by_idx)(int session_id,
		unsigned int stream_id, uint32_t index);
	int (*put_buf)(struct vb2_v4l2_buffer *vb2_buf, int session_id,
		unsigned int stream_id);
	int (*buf_done)(struct vb2_v4l2_buffer *vb2_buf, int session_id,
		unsigned int stream_id, uint32_t sequence, struct __kernel_v4l2_timeval *ts,
		uint32_t reserved);
	int (*flush_buf)(int session_id, unsigned int stream_id);
};

#define MSM_SD_NOTIFY_GET_SD 0x00000001
#define MSM_SD_NOTIFY_PUT_SD 0x00000002
#define MSM_SD_NOTIFY_REQ_CB 0x00000003

#define MSM_CAM_GET_IOCTL_ARG_PTR(ptr, \
	ioctl_ptr, len) memcpy(ptr, ioctl_ptr, len)

int legacy_msm_sd_register(struct msm_sd_subdev *msm_subdev);
int legacy_msm_sd_unregister(struct msm_sd_subdev *sd);
struct v4l2_subdev *msm_sd_get_subdev(struct v4l2_subdev *sd,
	const char *get_name);
void msm_sd_put_subdev(struct v4l2_subdev *sd, struct v4l2_subdev *put);
void legacy_msm_cam_copy_v4l2_subdev_fops(struct v4l2_file_operations *d1);


/*
 * The legacy of_gpio helpers of the kernel are gone in 6.12.  The camera
 * counts GPIO specifiers using each provider's #gpio-cells, then reads
 * the GPIO numbers through the descriptor API.
 */
static inline int msm_camera_gpio_count(struct device *dev,
	struct device_node *np)
{
	int count = of_count_phandle_with_args(np, "gpios", "#gpio-cells");

	return count > 0 ? count : 0;
}

static inline int msm_camera_gpio_number(struct device *dev,
	struct device_node *np, int index)
{
	struct gpio_desc *desc;
	int gpio;

	desc = devm_fwnode_gpiod_get_index(dev, of_fwnode_handle(np), NULL,
		index, GPIOD_ASIS, "camera");
	if (IS_ERR(desc))
		return PTR_ERR(desc);

	gpio = desc_to_gpio(desc);
	devm_gpiod_put(dev, desc);

	return gpio;
}
#endif /*_MSM_SD_H */
