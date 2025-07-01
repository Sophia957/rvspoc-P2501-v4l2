#include <linux/module.h>
// #include <linux/kernel.h>
#include <linux/init.h>

#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <linux/videodev2.h>
#include <media/videobuf2-vmalloc.h>
#include <media/videobuf2-v4l2.h>
#include <linux/platform_device.h>
#include <linux/timer.h>

/* Debug level control */
static int debug = 0;
module_param(debug, int, 0644);
MODULE_PARM_DESC(debug, "Debug level (0=none, 1=errors, 2=warnings, 3=info, 4=debug)");

/* Debug print macros */
#define SG2000X_ERR(fmt, ...) \
    do { if (debug >= 1) printk(KERN_ERR "sg2000x: " fmt, ##__VA_ARGS__); } while (0)

#define SG2000X_WARN(fmt, ...) \
    do { if (debug >= 2) printk(KERN_WARNING "sg2000x: " fmt, ##__VA_ARGS__); } while (0)

#define SG2000X_INFO(fmt, ...) \
    do { if (debug >= 3) printk(KERN_INFO "sg2000x: " fmt, ##__VA_ARGS__); } while (0)

#define SG2000X_DEBUG(fmt, ...) \
    do { if (debug >= 4) printk(KERN_DEBUG "sg2000x: " fmt, ##__VA_ARGS__); } while (0)

#define SG2000X_FUNC_ENTRY() SG2000X_DEBUG("%s()\n", __func__)


#define SG2000x_WIDTH	640
#define SG2000x_HEIGHT	480


static const struct v4l2_pix_format sg2000x_def_pix_format = {
	.width		= SG2000x_WIDTH,
	.height		= SG2000x_HEIGHT,
	.pixelformat	= V4L2_PIX_FMT_YUYV,
	.field		= V4L2_FIELD_NONE,
	.bytesperline	= SG2000x_WIDTH * 2,
	.sizeimage	= SG2000x_WIDTH * SG2000x_HEIGHT * 2,
	.colorspace	= V4L2_COLORSPACE_SRGB,
};

// Define the list of supported formats
static const struct v4l2_fmtdesc supported_formats[] = {
    {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .flags = 0,
        .pixelformat = V4L2_PIX_FMT_YUYV,
        .description = "YUYV 4:2:2",
    },
    {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .flags = V4L2_FMT_FLAG_COMPRESSED,
        .pixelformat = V4L2_PIX_FMT_MJPEG,
        .description = "Motion JPEG",
    },
};

// Total number of supported formats
#define NUM_SUPPORTED_FORMATS (sizeof(supported_formats) / sizeof(supported_formats[0]))


// Device structure definition
struct sg2000x_v4l2_dev {
    struct v4l2_device v4l2_dev;
    struct video_device vdev;
	spinlock_t				slock;
    struct mutex lock;         // Mutex lock
    int streaming;             // Streaming status flag
    struct vb2_queue vb2_queue;      // Video buffer queue
    struct v4l2_pix_format user_format; // Video format
    struct list_head buffer_queue;
    struct timer_list 		timer;

};

struct sg2000x_buffer {
	struct vb2_v4l2_buffer vb;
	struct list_head	list;
};


/**
 * generate_yuyv_test_pattern - Generate a YUYV format test pattern
 * @buffer: Pointer to the buffer to fill with the test pattern
 * @width: Width of the test pattern in pixels
 * @height: Height of the test pattern in pixels
 *
 * This function generates a gradient test pattern in YUYV format.
 * The Y component has a horizontal gradient, U has a vertical gradient,
 * and V has a horizontal gradient.
 *
 * Return: Pointer to the buffer (same as input), or NULL on error.
 */
uint8_t* generate_yuyv_test_pattern(uint8_t *buffer, int width, int height) {
    
    int x, y;
    
    // Calculate the required buffer size (2 bytes per pixel for YUYV)
    // Fill the test pattern - horizontal and vertical gradient
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x += 2) {
            int index = (y * width + x) * 2;
            
            // Y component - horizontal gradient (dark on left, bright on right)
            uint8_t y1 = (uint8_t)(255 * x / width);
            uint8_t y2 = (uint8_t)(255 * (x + 1) / width);
            
            // U component - vertical gradient (green on top, purple on bottom)
            uint8_t u = (uint8_t)(128 + 100 * y / height);
            
            // V component - horizontal gradient (blue on left, red on right)
            uint8_t v = (uint8_t)(128 + 100 * x / width);
            
            // Write YUYV data: Y1 U Y2 V
            buffer[index] = y1;
            buffer[index + 1] = u;
            buffer[index + 2] = y2;
            buffer[index + 3] = v;
        }
    }
    
    return buffer;
}


/**
 * sg2000x_timer_function - Timer handler for generating video frames
 * @t: Pointer to the timer list structure
 *
 * This function is called when the timer expires. It retrieves a buffer from the 
 * queue, fills it with a test pattern, and marks the buffer as done to notify 
 * the user space application.
 */
static void sg2000x_timer_function(struct timer_list *t){
	struct sg2000x_v4l2_dev *sg2000x_dev  = container_of(t, struct sg2000x_v4l2_dev, timer);
    struct sg2000x_buffer *vid_cap_buf = NULL;
	uint8_t *vbuf;
	
	SG2000X_DEBUG("------%s----\n",__func__);
	SG2000X_DEBUG("width = %d,height = %d\n",sg2000x_dev->user_format.width,sg2000x_dev->user_format.height);
	
	if (!list_empty(&sg2000x_dev->buffer_queue)) {
		vid_cap_buf = list_entry(sg2000x_dev->buffer_queue.next, struct sg2000x_buffer , list);
		if(vid_cap_buf->vb.vb2_buf.state != VB2_BUF_STATE_ACTIVE) {
			SG2000X_ERR("buffer no active,error!!!\n");
			return;
		}
		list_del(&vid_cap_buf->list);
	}else {
		SG2000X_INFO("No active queue to serve\n");
        goto out;
	}
    
	// Get the buffer virtual address
	vbuf = vb2_plane_vaddr(&vid_cap_buf->vb.vb2_buf, 0);
	SG2000X_DEBUG("bytesperline=%d\n",sg2000x_dev->user_format.bytesperline);
	
	// Fill the buffer with test data
	memset(vbuf, 0xff, sg2000x_dev->user_format.bytesperline * sg2000x_dev->user_format.height);
    generate_yuyv_test_pattern(vbuf, sg2000x_dev->user_format.width, sg2000x_dev->user_format.height);
	
    // Mark the buffer as done and wake up the user space application
    vb2_buffer_done(&vid_cap_buf->vb.vb2_buf, VB2_BUF_STATE_DONE);
    
out:
    // Update the timer to generate frames at 30fps
    mod_timer(&sg2000x_dev->timer, jiffies + HZ/30);
}


/**
 * sg2000x_v4l2_fop_release - Release handler for the video device
 * @file: File pointer to the device
 *
 * This function is called when the device file is closed. It releases the 
 * video buffer queue if it exists, otherwise falls back to the default release.
 * 
 * Return: 0 on success, or the result of the fallback release
 */
static int sg2000x_v4l2_fop_release(struct file *file)
{
	struct video_device *vdev = video_devdata(file);
	
	if (vdev->queue)
		return vb2_fop_release(file);
	return v4l2_fh_release(file);
}

static const struct v4l2_file_operations sg2000x_v4l2_fops = {
	.owner			= THIS_MODULE,
	.open           = v4l2_fh_open,
	.release        = sg2000x_v4l2_fop_release,
	.poll			= vb2_fop_poll,
	.unlocked_ioctl = video_ioctl2,
	.mmap           = vb2_fop_mmap,
};

/**
 * sg2000x_vb2_queue_setup - Setup the video buffer queue
 * @vq: Pointer to the vb2_queue structure
 * @nbuffers: Pointer to the number of buffers
 * @nplanes: Pointer to the number of planes
 * @sizes: Array of buffer sizes
 * @alloc_devs: Array of allocation devices
 *
 * This function is called by the vb2 core to determine the buffer setup. It sets
 * the number of planes and buffer size based on the current video format.
 * 
 * Return: 0 on success, -EINVAL if buffer size is insufficient
 */
static int sg2000x_vb2_queue_setup(struct vb2_queue *vq,
		       unsigned *nbuffers, unsigned *nplanes,
		       unsigned sizes[], struct device *alloc_devs[]){
	struct sg2000x_v4l2_dev *sg2000x_dev = vb2_get_drv_priv(vq);
    
    int size = sg2000x_dev->user_format.sizeimage;
    SG2000X_DEBUG("-----%s:line=%d nbuffers = %d, size = %d\n",__func__,__LINE__, *nbuffers, size);
    if (vq->num_buffers + *nbuffers < 2)
		*nbuffers = 2 - vq->num_buffers;

	if (*nplanes)
		return sizes[0] < size ? -EINVAL : 0;

	*nplanes = 1;
	sizes[0] = size;
	return 0;
}


/**
 * sg2000x_vb2_buf_prepare - Prepare a video buffer
 * @vb: Pointer to the vb2_buffer structure
 *
 * This function verifies that the buffer size meets the current video format 
 * requirements and sets the payload size. It is called when a buffer is queued 
 * using VIDIOC_QBUF.
 * 
 * Return: 0 on success, -EINVAL if buffer size is insufficient
 */
static int sg2000x_vb2_buf_prepare(struct vb2_buffer *vb)
{
	struct sg2000x_v4l2_dev *sg2000x_dev = vb2_get_drv_priv(vb->vb2_queue);
        // Debug log
    if (!sg2000x_dev) {
        SG2000X_ERR("sg2000 Driver private data is NULL!\n");
        return -EINVAL;
    }

    SG2000X_DEBUG("-----%s:line=%d\n",__func__,__LINE__);
	if (vb2_plane_size(vb, 0) < sg2000x_dev->user_format.sizeimage) {
		SG2000X_INFO("sg2000 plane size too small (%lu < %u)\n",
			vb2_plane_size(vb, 0),
			sg2000x_dev->user_format.sizeimage);
		return -EINVAL;
	}

	vb2_set_plane_payload(vb, 0, sg2000x_dev->user_format.sizeimage);

	return 0;
}


/**
 * sg2000x_vb2_buf_finish - Finish processing a buffer (no-op)
 * @vb: Pointer to the vb2_buffer structure
 *
 * This function is a no-op and exists to satisfy the vb2_ops interface.
 */
static void sg2000x_vb2_buf_finish(struct vb2_buffer *vb) {
	
}


/**
 * sg2000x_vb2_buf_queue - Queue a buffer for processing
 * @vb: Pointer to the vb2_buffer structure
 *
 * This function is called when an application queues a buffer using VIDIOC_QBUF.
 * It adds the buffer to the driver's local queue for processing by the timer.
 */
static void sg2000x_vb2_buf_queue(struct vb2_buffer *vb) {
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct sg2000x_v4l2_dev *sg2000x_dev = vb2_get_drv_priv(vb->vb2_queue);
	struct sg2000x_buffer *buf = container_of(vbuf, struct sg2000x_buffer, vb);

	SG2000X_DEBUG("-----%s:line=%d buf=%px\n",__func__,__LINE__, buf);

	spin_lock(&sg2000x_dev->slock);
	// Add the buffer to the driver's local queue for timer processing
	list_add_tail(&buf->list, &sg2000x_dev->buffer_queue);
	spin_unlock(&sg2000x_dev->slock);
}

/**
 * sg2000x_vb2_start_streaming - Start video streaming
 * @vq: Pointer to the vb2_queue structure
 * @count: Number of buffers to start with
 *
 * This function initializes and starts the timer to generate video frames at 
 * the specified rate (30fps).
 * 
 * Return: 0 on success
 */
static int sg2000x_vb2_start_streaming(struct vb2_queue *vq, unsigned count) {
    struct sg2000x_v4l2_dev *sg2000x_dev = vb2_get_drv_priv(vq);
	SG2000X_INFO("------start timer-----\n");
	timer_setup(&sg2000x_dev->timer, sg2000x_timer_function, 0);
	sg2000x_dev->timer.expires = jiffies + HZ/2;
	add_timer(&sg2000x_dev->timer);
	
	return 0;
}

/**
 * sg2000x_vb2_stop_streaming - Stop video streaming
 * @vq: Pointer to the vb2_queue structure
 *
 * This function stops the timer and releases all active buffers, marking them 
 * as errors to clean up the queue.
 */
static void sg2000x_vb2_stop_streaming(struct vb2_queue *vq) {
	struct sg2000x_v4l2_dev *sg2000x_dev = vb2_get_drv_priv(vq);

	SG2000X_INFO("%s\n", __func__);
	del_timer(&sg2000x_dev->timer);
	
	/* Release all active buffers */
	while (!list_empty(&sg2000x_dev->buffer_queue)) {
		struct sg2000x_buffer *buf;

		buf = list_entry(sg2000x_dev->buffer_queue.next,
				 struct sg2000x_buffer, list);
		list_del(&buf->list);
		vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_ERROR);
		SG2000X_DEBUG("vid_cap buffer %d stop\n",buf->vb.vb2_buf.index);
	}
	
}

const struct vb2_ops sg2000x_vb2_ops = {
	.queue_setup		= sg2000x_vb2_queue_setup,
	.buf_prepare		= sg2000x_vb2_buf_prepare,
	.buf_finish			= sg2000x_vb2_buf_finish,
	.buf_queue			= sg2000x_vb2_buf_queue,
	.start_streaming	= sg2000x_vb2_start_streaming,
	.stop_streaming		= sg2000x_vb2_stop_streaming,
    .wait_prepare		= vb2_ops_wait_prepare,
	.wait_finish		= vb2_ops_wait_finish,
};

/**
 * sg2000x_vidioc_g_fbuf - Get framebuffer information (not implemented)
 * @file: User file pointer
 * @fh: File handle
 * @a: Pointer to v4l2_framebuffer structure
 *
 * This function is a placeholder and always returns 0.
 * 
 * Return: 0
 */
static int sg2000x_vidioc_g_fbuf(struct file *file, void *fh, struct v4l2_framebuffer *a)
{
	return 0;
}

/**
 * sg2000x_vidioc_s_fbuf - Set framebuffer information (not implemented)
 * @file: User file pointer
 * @fh: File handle
 * @a: Pointer to v4l2_framebuffer structure
 *
 * This function is a placeholder and always returns 0.
 * 
 * Return: 0
 */
static int sg2000x_vidioc_s_fbuf(struct file *file, void *fh, const struct v4l2_framebuffer *a)
{
	return 0;
}
/**
 * sg2000x_vidioc_enum_fmt_vid_cap - Enumerate supported video capture formats
 * @file: User file pointer
 * @priv: Private driver data
 * @fdesc: Pointer to v4l2_fmtdesc structure to store format description
 *
 * This function enumerates the supported video capture formats by copying
 * entries from the predefined supported_formats array.
 *
 * Return: 0 on success, negative error code on failure
 */
static int sg2000x_vidioc_enum_fmt_vid_cap(struct file *file, void *priv,
                                           struct v4l2_fmtdesc *fdesc)
{
    SG2000X_INFO("%s\n", __func__);
    // Validate the requested buffer type
    if (fdesc->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
        return -EINVAL;
        
    // Check if the index is within the supported format range
    if (fdesc->index >= NUM_SUPPORTED_FORMATS)
        return -EINVAL;
    

       switch (fdesc->index) {
    case 0:
        fdesc->pixelformat = V4L2_PIX_FMT_YUYV;
        strlcpy((char *)fdesc->description, "YUYV 4:2:2", sizeof(fdesc->description));
        fdesc->flags = 0; // 设置标志
        break;
        
    case 1:
        fdesc->pixelformat = V4L2_PIX_FMT_MJPEG;
        strlcpy((char *)fdesc->description, "Motion-JPEG", sizeof(fdesc->description));
        fdesc->flags = V4L2_FMT_FLAG_COMPRESSED; // 设置标志
        break;
        
    // case 2:
    //     fdesc->pixelformat = V4L2_PIX_FMT_UYVY;
    //     strlcpy((char *)fdesc->description, "UYVY 4:2:2", sizeof(fdesc->description));
    //     break;
        
    default:
        return -EINVAL;
    }
    

    fdesc->flags = 0;
    
    SG2000X_DEBUG("sg2000x: Enumerated format %d: 0x%08X (%s)\n",
           fdesc->index, fdesc->pixelformat, fdesc->description);
    
    return 0;
}

/**
 * sg2000x_vidioc_querycap - Query device capabilities
 * @file: User file pointer
 * @priv: Private driver data
 * @cap: Pointer to v4l2_capability structure to store device capabilities
 *
 * This function fills the device capabilities structure with information
 * about the driver, device, bus, and supported capabilities.
 *
 * Return: 0 on success, negative error code on failure
 */
static int sg2000x_vidioc_querycap(struct file *file, void *priv,
                                    struct v4l2_capability *cap)
{
    struct sg2000x_v4l2_dev *sg2000x_dev_info = video_drvdata(file);
    
    SG2000X_DEBUG("-----%s:line=%d\n", __func__, __LINE__);
    SG2000X_INFO("sg2000x_vidioc_querycap called\n");

    // Set driver, device, and bus information
    strscpy(cap->driver, "sg2000x", sizeof(cap->driver));
    strscpy(cap->card, "SG2000X Camera", sizeof(cap->card));
    snprintf(cap->bus_info, sizeof(cap->bus_info),
             "platform:%s", sg2000x_dev_info->v4l2_dev.name);
    
    // Set device capabilities, including basic and device-specific capabilities
    cap->capabilities = sg2000x_dev_info->vdev.device_caps | V4L2_CAP_DEVICE_CAPS;
    
    return 0;
}
/**
 * sg2000x_vidioc_g_fmt_vid_cap - Get the current video capture format
 * @file: User file pointer
 * @priv: Private data
 * @fmt: Pointer to the v4l2_format structure to store the format information
 *
 * Returns 0 on success, or a negative value on error.
 */
static int sg2000x_vidioc_g_fmt_vid_cap(struct file *file, void *priv,
                                        struct v4l2_format *fmt)
{
    struct sg2000x_v4l2_dev *dev = video_drvdata(file);
    
    // Verify parameter validity
    if (!fmt)
        return -EINVAL;
        
    // Ensure the requested type is video capture
    if (fmt->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
        return -EINVAL;
        
    // Copy the current format settings from the device structure
    memcpy(&fmt->fmt.pix, &dev->user_format, sizeof(struct v4l2_pix_format));
    
    // Optional: Print debug information
    SG2000X_DEBUG("sg2000x: Current format: %4s %dx%d\n",
           (char *)&fmt->fmt.pix.pixelformat,
           fmt->fmt.pix.width,
           fmt->fmt.pix.height);
    
    return 0;
}

/**
 * sg2000x_vidioc_try_fmt_vid_cap - Attempt to set the video capture format (verify and adjust)
 * @file: User file pointer
 * @priv: Private data
 * @fmt: Pointer to the v4l2_format structure containing the requested format
 *        and to return the adjusted format
 *
 * Returns 0 on success, or a negative value on error.
 */
static int sg2000x_vidioc_try_fmt_vid_cap(struct file *file, void *priv,
                                         struct v4l2_format *fmt)
{
    struct v4l2_pix_format *pix = &fmt->fmt.pix;
    int format_supported = 0;
    u32 bytes_per_pixel = 0;
    int i = 0;
    
    // Verify parameter validity
    if (!fmt || fmt->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
        return -EINVAL;
    
    // Check if the pixel format is supported
    for (i = 0; i < NUM_SUPPORTED_FORMATS; i++) {
        if (pix->pixelformat == supported_formats[i].pixelformat) {
            format_supported = 1;
            // Calculate bytes per pixel based on the pixel format
            switch (pix->pixelformat) {
            case V4L2_PIX_FMT_YUYV:
                bytes_per_pixel = 2;  // 2 bytes per pixel for YUYV
                break;
            case V4L2_PIX_FMT_MJPEG:
                bytes_per_pixel = 1;  // 1 byte per pixel for MJPEG (compressed format)
                break;
            default:
                bytes_per_pixel = 0;
                break;
            }
            break;
        }
    }
    
    if (!format_supported) {
        SG2000X_WARN("sg2000x: Unsupported pixel format: 0x%08X\n",
               pix->pixelformat);
        return -EINVAL;
    }
    
    // Calculate and set bytes per line and image size
    pix->bytesperline = pix->width * bytes_per_pixel;
    pix->sizeimage = pix->bytesperline * pix->height;
    
    // Set default color space and other parameters
    if (pix->colorspace == 0)
        pix->colorspace = V4L2_COLORSPACE_SRGB;
    
    // Print debug information
    SG2000X_DEBUG("sg2000x: Tried format: %4s %dx%d -> %dx%d\n",
           (char *)&pix->pixelformat,
           fmt->fmt.pix.width, fmt->fmt.pix.height,
           pix->width, pix->height);
    
    return 0;
}


/**
 * sg2000x_vidioc_s_fmt_vid_cap - Set the video capture format
 * @file: User file pointer
 * @priv: Private data
 * @fmt: Pointer to the v4l2_format structure containing the format to set
 *
 * Returns 0 on success, or a negative value on error.
 */
static int sg2000x_vidioc_s_fmt_vid_cap(struct file *file, void *priv,
                                        struct v4l2_format *fmt)
{
    struct sg2000x_v4l2_dev *dev = video_drvdata(file);
    int ret;
    
    // Verify parameter validity
    if (!fmt || fmt->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
        return -EINVAL;
    
    // First attempt to set the format to verify and get adjusted parameters
    ret = sg2000x_vidioc_try_fmt_vid_cap(file, priv, fmt);
    if (ret < 0) {
        SG2000X_ERR("sg2000x: Failed to try format: %d\n", ret);
        return ret;
    }
    
    // Save the new format to the device structure
    dev->user_format = fmt->fmt.pix;
    
    SG2000X_INFO("sg2000x: Format set to: %4s %dx%d\n",
           (char *)&dev->user_format.pixelformat,
           dev->user_format.width,
           dev->user_format.height);
    
    return 0;
}

static const struct v4l2_ioctl_ops sg2000x_v4l2_ioctl_ops = {
	/**
	 * vidioc_querycap: Query device capabilities
	 * Indicates that it is a camera device.
	 */
	.vidioc_querycap = sg2000x_vidioc_querycap,

	/**
	 * Video format operations:
	 * - Enumerate, get, try, and set camera data formats.
	 */
	.vidioc_enum_fmt_vid_cap 	= sg2000x_vidioc_enum_fmt_vid_cap,
	.vidioc_g_fmt_vid_cap 		= sg2000x_vidioc_g_fmt_vid_cap,
	.vidioc_try_fmt_vid_cap 	= sg2000x_vidioc_try_fmt_vid_cap,
	.vidioc_s_fmt_vid_cap 		= sg2000x_vidioc_s_fmt_vid_cap,
	
	.vidioc_g_fbuf			= sg2000x_vidioc_g_fbuf,
	.vidioc_s_fbuf			= sg2000x_vidioc_s_fbuf,

	/**
	 * Buffer operations:
	 * - Request, query, queue, and dequeue buffers.
	 */
	.vidioc_reqbufs 			= vb2_ioctl_reqbufs,
	.vidioc_querybuf 			= vb2_ioctl_querybuf,
	.vidioc_qbuf 				= vb2_ioctl_qbuf,
	.vidioc_dqbuf 				= vb2_ioctl_dqbuf,

	.vidioc_create_bufs	= vb2_ioctl_create_bufs,

	.vidioc_prepare_buf	= vb2_ioctl_prepare_buf,
	.vidioc_expbuf		= vb2_ioctl_expbuf,
	
	/**
	 * Streaming control:
	 * - Start and stop video streaming.
	 */
	.vidioc_streamon 			= vb2_ioctl_streamon,
	.vidioc_streamoff 			= vb2_ioctl_streamoff,
};


// Driver global variable
static struct sg2000x_v4l2_dev *sg2000x_v4l2;
static const struct video_device sg2000_video_template = {
	.name		= "sg2000x",
	.minor		= -1,
	.fops		= &sg2000x_v4l2_fops,
	.ioctl_ops	= &sg2000x_v4l2_ioctl_ops,
	.release	= video_device_release_empty, /* Check this */

	.device_caps	= V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_READWRITE |
			  V4L2_CAP_STREAMING,
};

/**
 * sg2000x_video_probe - Probe function for the platform driver
 * @pdev: Pointer to the platform device structure
 *
 * This function initializes the V4L2 device, VB2 queue, and registers the video
 * device. It is called when the driver probes the platform device.
 * 
 * Returns 0 on success, or a negative value on error.
 */
static int sg2000x_video_probe(struct platform_device *pdev) {
    int ret;
    struct vb2_queue *q;

    SG2000X_INFO("sg2000 in sg2000x_v4l2_init\n");

    sg2000x_v4l2 = kzalloc(sizeof(struct sg2000x_v4l2_dev), GFP_KERNEL);
    if (!sg2000x_v4l2)
        return -ENOMEM;
    
    spin_lock_init(&sg2000x_v4l2->slock);
    mutex_init(&sg2000x_v4l2->lock);
    sg2000x_v4l2->streaming = 0;
    sg2000x_v4l2->user_format = sg2000x_def_pix_format; 
    
    /* Initialize V4L2 device */
    ret = v4l2_device_register(&pdev->dev, &sg2000x_v4l2->v4l2_dev);
    if (ret < 0) {
        SG2000X_ERR("sg2000 failed to register v4l2_device: %d\n", ret);
        kfree(sg2000x_v4l2);
        return ret;
    }
    SG2000X_INFO("sg2000 finish  v4l2_device_register\n");


    /* Initialize DMA queues */
    INIT_LIST_HEAD(&sg2000x_v4l2->buffer_queue);
	
    /* Initialize vb2_queue */
    q = &sg2000x_v4l2->vb2_queue;
    q->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    q->io_modes = VB2_MMAP | VB2_USERPTR | VB2_DMABUF | VB2_READ;
    q->drv_priv = sg2000x_v4l2;
    q->buf_struct_size = sizeof(struct sg2000x_buffer);
    q->ops = &sg2000x_vb2_ops;
    q->mem_ops = &vb2_vmalloc_memops;
    q->lock = &sg2000x_v4l2->lock;
    q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
    q->dev = sg2000x_v4l2->v4l2_dev.dev;

    ret = vb2_queue_init(q);
    if (ret < 0) {
        SG2000X_ERR("sg2000 failed to initialize vb2 queue: %d\n", ret);
        v4l2_device_unregister(&sg2000x_v4l2->v4l2_dev);
        kfree(sg2000x_v4l2);
        return ret;
    }
    SG2000X_INFO("sg2000 finished  vb2_queue_init\n");
    snprintf(sg2000x_v4l2->vdev.name, sizeof(sg2000x_v4l2->vdev.name),
             "sg2000x_csi_v4l2");
    

    sg2000x_v4l2->vdev = sg2000_video_template;
    sg2000x_v4l2->vdev.v4l2_dev = &sg2000x_v4l2->v4l2_dev;
    sg2000x_v4l2->vdev.queue = &sg2000x_v4l2->vb2_queue;



    video_set_drvdata(&sg2000x_v4l2->vdev, sg2000x_v4l2);
    
    SG2000X_INFO("sg2000 finished  video_set_drvdata\n");
    /* Register the video_device */
    ret = video_register_device(&sg2000x_v4l2->vdev, VFL_TYPE_VIDEO, -1);
    if (ret < 0) {
        SG2000X_ERR("sg2000 Failed to register video device: %d\n", ret);
        v4l2_device_unregister(&sg2000x_v4l2->v4l2_dev);
        kfree(sg2000x_v4l2);
        return ret;
    }

    SG2000X_INFO("sg2000 v4l2 driver initialized\n");
    return 0;
}


/**
 * sg2000x_video_remove - Remove function for the platform driver
 * @pdev: Pointer to the platform device structure
 *
 * This function unregisters the video device, V4L2 device, and frees resources.
 * It is called when the driver is removed.
 * 
 * Returns 0 on success.
 */
static int sg2000x_video_remove(struct platform_device *pdev){

    video_unregister_device(&sg2000x_v4l2->vdev);
    v4l2_device_unregister(&sg2000x_v4l2->v4l2_dev);
    kfree(sg2000x_v4l2);
    
    SG2000X_INFO("sg2000 v4l2 driver unloaded\n");
    return 0;
}

/**
 * sg2000_video_device_release - Device release callback
 * @vdev: Pointer to the device structure
 *
 * This function is called when the device is released. It is currently empty.
 */
void sg2000_video_device_release(struct device *vdev) {

}

static struct platform_device sg2000_video_device = {
	.name			= "sg2000x",
	.dev.release	= sg2000_video_device_release,
};

static struct platform_driver sg2000_video_driver = {
	.probe		= sg2000x_video_probe,
	.remove		= sg2000x_video_remove,
	.driver = {
		.name	= "sg2000x",
	},
};

/**
 * sg2000_video_init - Module initialization function
 *
 * This function registers the platform device and driver.
 * 
 * Returns 0 on success, or a negative value on error.
 */
static int __init sg2000_video_init(void)
{
	int ret;

	ret = platform_device_register(&sg2000_video_device);
	if (ret)
		return ret;

	ret = platform_driver_register(&sg2000_video_driver);
	if (ret)
		platform_device_unregister(&sg2000_video_device);

	return ret;
}

/**
 * sg2000_video_exit - Module exit function
 *
 * This function unregisters the platform driver and device.
 */
static void __exit sg2000_video_exit(void)
{
	platform_driver_unregister(&sg2000_video_driver);
	platform_device_unregister(&sg2000_video_device);
}

module_init(sg2000_video_init);
module_exit(sg2000_video_exit);


MODULE_LICENSE("GPL");
MODULE_AUTHOR("Sophia Qu");
MODULE_DESCRIPTION("SG2000X V4L2 Driver");