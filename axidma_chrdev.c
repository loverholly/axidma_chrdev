#include <linux/module.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <linux/io.h>
#include <linux/of_platform.h>
#include <linux/atomic.h>

#define DRV_NAME   "axidma"
#define AXIDMA_BUF_LEN (32 * 1024 * 1024)

enum dma_dir {
	DMA_TO_DEV   = 0,   /* MM2S */
	DMA_FROM_DEV = 1,    /* S2MM */
	DMA_MAX_DEV = 2,
};

struct axidma_dev {
	struct dma_chan     *tx_chan;   /* MM2S */
	struct dma_chan     *rx_chan;   /* S2MM */
	dma_cookie_t         dma_cookie[DMA_MAX_DEV];
	dma_addr_t           dma_phy_addr;
	void                *cpu_addr;
	wait_queue_head_t    wait[DMA_MAX_DEV];
	atomic_t             done[DMA_MAX_DEV];
	atomic_t             open_count;
	struct cdev          cdev;
	dev_t                devt;
	struct class        *cls;
};

/* ---------- open ---------- */
static int axidma_open(struct inode *i, struct file *f)
{
	struct axidma_dev *d = container_of(i->i_cdev, struct axidma_dev, cdev);

	if (atomic_inc_return(&d->open_count) != 1) {
		atomic_dec(&d->open_count);
		return -EBUSY;
	}

	f->private_data = d;
	return 0;
}

static void mm2s_complete(void *arg)
{
	struct axidma_dev *d = arg;

	atomic_set(&d->done[DMA_TO_DEV], 1);
	wake_up_interruptible(&d->wait[DMA_TO_DEV]);
}

static void s2mm_complete(void *arg)
{
	struct axidma_dev *d = arg;

	atomic_set(&d->done[DMA_FROM_DEV], 1);
	wake_up_interruptible(&d->wait[DMA_FROM_DEV]);
}

/* ---------- MM2S send ---------- */
static ssize_t axidma_write(struct file *f, const char __user *ubuf,
                            size_t len, loff_t *off)
{
	struct axidma_dev *d = f->private_data;
	struct dma_async_tx_descriptor *desc;

	/* not support async mode */
	if (f->f_flags & O_NONBLOCK)
		return -EAGAIN;

	if (!access_ok(ubuf, len))
		return -EFAULT;

	if (len == 0)
		return -EFAULT;

	if (d->tx_chan == NULL)
		return -EFAULT;

	if (len > AXIDMA_BUF_LEN)
		len = AXIDMA_BUF_LEN;
	if (copy_from_user(d->cpu_addr, ubuf, len))
		return -EFAULT;

	atomic_set(&d->done[DMA_TO_DEV], 0);
	desc = dmaengine_prep_slave_single(d->tx_chan, d->dma_phy_addr,
	                                   len, DMA_MEM_TO_DEV,
	                                   DMA_PREP_INTERRUPT);
	if (!desc)
		return -EIO;

	desc->callback = mm2s_complete;
	desc->callback_param = d;
	d->dma_cookie[DMA_TO_DEV] = dmaengine_submit(desc);
	dma_async_issue_pending(d->tx_chan);
	if (wait_event_interruptible(d->wait[DMA_TO_DEV], atomic_read(&d->done[DMA_TO_DEV])) < 0) {
		dmaengine_terminate_sync(d->tx_chan);
		return -ERESTARTSYS;
	}

	return len;
}

/* ---------- S2MM receive ---------- */
static ssize_t axidma_read(struct file *f, char __user *ubuf,
                           size_t len, loff_t *off)
{
	struct axidma_dev *d = f->private_data;
	struct dma_async_tx_descriptor *desc;

	/* not support async mode */
	if (f->f_flags & O_NONBLOCK)
		return -EAGAIN;

	if (!access_ok(ubuf, len))
		return -EFAULT;

	if (len == 0)
		return -EFAULT;

	if (d->rx_chan == NULL)
		return -EIO;

	if (len > AXIDMA_BUF_LEN)
		len = AXIDMA_BUF_LEN;

	atomic_set(&d->done[DMA_FROM_DEV], 0);
	desc = dmaengine_prep_slave_single(d->rx_chan, d->dma_phy_addr,
	                                   len, DMA_DEV_TO_MEM,
	                                   DMA_PREP_INTERRUPT);
	if (!desc)
		return -EIO;

	desc->callback = s2mm_complete;
	desc->callback_param = d;
	d->dma_cookie[DMA_FROM_DEV] = dmaengine_submit(desc);
	dma_async_issue_pending(d->rx_chan);
	if (wait_event_interruptible(d->wait[DMA_FROM_DEV], atomic_read(&d->done[DMA_FROM_DEV])) < 0) {
		dmaengine_terminate_sync(d->rx_chan);
		return -ERESTARTSYS;
	}

	if (copy_to_user(ubuf, d->cpu_addr, len))
		return -EFAULT;

	return len;
}

static long axidma_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	int ret = -ENOTTY;

	return ret;
}

static int axidma_release(struct inode *inode, struct file *f)
{
	struct axidma_dev *d = f->private_data;

	if (!IS_ERR(d->tx_chan))
		dmaengine_terminate_all(d->tx_chan);
	if (!IS_ERR(d->rx_chan))
		dmaengine_terminate_all(d->rx_chan);

	atomic_dec(&d->open_count);

	return 0;
}

static const struct file_operations axidma_fops = {
	.owner = THIS_MODULE,
	.write = axidma_write,
	.read  = axidma_read,
	.open  = axidma_open,
	.release = axidma_release,
	.unlocked_ioctl = axidma_ioctl,
};

static int axidma_probe(struct platform_device *pdev)
{
	int ret = -ENODEV;
	struct axidma_dev *d;
	struct dma_slave_config cfg;

	d = devm_kzalloc(&pdev->dev, sizeof(*d), GFP_KERNEL);
	if (!d) {
		dev_err(&pdev->dev, "chr dev alloc failed!\n");
		return -ENOMEM;
	}

	d->tx_chan = dma_request_chan(&pdev->dev, "axidma0");
	d->rx_chan = dma_request_chan(&pdev->dev, "axidma1");
	if (IS_ERR(d->tx_chan) && IS_ERR(d->rx_chan)) {
		dev_err(&pdev->dev, "request dma chan failed!\n");
		return -ENODEV;
	}

	if (!IS_ERR(d->tx_chan)) {
		memset(&cfg, 0, sizeof(cfg));
		cfg.direction = DMA_MEM_TO_DEV;
		cfg.dst_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
		cfg.dst_maxburst = 256;
		dmaengine_slave_config(d->tx_chan, &cfg);
	}

	if (!IS_ERR(d->rx_chan)) {
		memset(&cfg, 0, sizeof(cfg));
		cfg.direction = DMA_DEV_TO_MEM;
		cfg.src_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
		cfg.src_maxburst = 256;
		dmaengine_slave_config(d->rx_chan, &cfg);
	}

	d->cpu_addr = dma_alloc_coherent(&pdev->dev, AXIDMA_BUF_LEN,
	                                 &d->dma_phy_addr, GFP_KERNEL);
	if (!d->cpu_addr) {
		dev_err(&pdev->dev, "alloc dma buf failed!\n");
		ret = -ENODEV;
		goto err_dma;
	}

	init_waitqueue_head(&d->wait[DMA_TO_DEV]);
	init_waitqueue_head(&d->wait[DMA_FROM_DEV]);

	ret = alloc_chrdev_region(&d->devt, 0, 1, DRV_NAME);
	if (ret) {
		dev_err(&pdev->dev, "alloc chr dev failed!\n");
		goto err_dma;
	}
	cdev_init(&d->cdev, &axidma_fops);
	ret = cdev_add(&d->cdev, d->devt, 1);
	if (ret) {
		dev_err(&pdev->dev, "chr dev add failed!\n");
		goto err_device;
	}

	d->cls = class_create(THIS_MODULE, DRV_NAME);
	if (IS_ERR(d->cls)) {
		dev_err(&pdev->dev, "chr dev class register failed!\n");
		ret = IS_ERR(d->cls);
		goto err_class;
	}

	device_create(d->cls, NULL, d->devt, NULL, DRV_NAME);
	platform_set_drvdata(pdev, d);
	return 0;

err_class:
	if (!IS_ERR(d->cls)) {
		device_destroy(d->cls, d->devt);
		class_destroy(d->cls);
	}

err_device:
	cdev_del(&d->cdev);
	unregister_chrdev_region(d->devt, 1);
	if (d->cpu_addr)
		dma_free_coherent(&pdev->dev, AXIDMA_BUF_LEN, d->cpu_addr, d->dma_phy_addr);

err_dma:
	if (!IS_ERR(d->tx_chan))
		dma_release_channel(d->tx_chan);
	if (!IS_ERR(d->rx_chan))
		dma_release_channel(d->rx_chan);

	return ret;
}

static int axidma_remove(struct platform_device *pdev)
{
	struct axidma_dev *d = platform_get_drvdata(pdev);

	device_destroy(d->cls, d->devt);
	class_destroy(d->cls);
	cdev_del(&d->cdev);
	unregister_chrdev_region(d->devt, 1);

	if (!IS_ERR(d->tx_chan))
		dma_release_channel(d->tx_chan);
	if (!IS_ERR(d->rx_chan))
		dma_release_channel(d->rx_chan);
	if (d->cpu_addr)
		dma_free_coherent(&pdev->dev, AXIDMA_BUF_LEN, d->cpu_addr, d->dma_phy_addr);

	return 0;
}

static const struct of_device_id axidma_match[] = {
	{ .compatible = "axidma-chrdev-1.0" },
	{},
};
MODULE_DEVICE_TABLE(of, axidma_match);

static struct platform_driver axidma_drv = {
	.probe  = axidma_probe,
	.remove = axidma_remove,
	.driver = {
		.name           = DRV_NAME,
		.of_match_table = of_match_ptr(axidma_match),
	},
};
module_platform_driver(axidma_drv);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("AXI-DMA character driver");
MODULE_AUTHOR("johenleem");
MODULE_VERSION("1.0");
MODULE_ALIAS("platform:" DRV_NAME);
