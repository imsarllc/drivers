#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/iio/buffer.h>
#include <linux/iio/buffer-dma.h>
#include <linux/iio/buffer-dmaengine.h>

#include <linux/dma-mapping.h>
#include <linux/dmaengine.h>

#include <linux/platform_device.h>
#include <linux/module.h>

static int test_read_raw(struct iio_dev *indio_dev, const struct iio_chan_spec *chan, int *val,
			 int *val2, long info)
{
	pr_err("Called read raw\n");
	switch (info) {
	case IIO_CHAN_INFO_RAW:
		*val = 100;
		*val2 = 8;
		// Returns 100/8
		return IIO_VAL_FRACTIONAL;
	case IIO_CHAN_INFO_PEAK:
		*val = 1000;
		*val2 = 6;
		// Returns 1000/64 = 15.625
		return IIO_VAL_FRACTIONAL_LOG2;
	case IIO_CHAN_INFO_OFFSET:
		*val = 10;
		*val2 = 1234;
		// Returns 10.001234
		return IIO_VAL_INT_PLUS_MICRO;
	case IIO_CHAN_INFO_SAMP_FREQ:
		*val = 150000000;
		*val2 = 2;
		return IIO_VAL_FRACTIONAL;
	case IIO_CHAN_INFO_SCALE:
		*val = 1;
		*val2 = 0;
		return IIO_VAL_INT;
	default:
		return -EINVAL;
	}
}

static int test_write_raw(struct iio_dev *indio_dev, struct iio_chan_spec const *chan, int val,
			  int val2, long info)
{
	pr_err("Called write raw\n");
	return -EINVAL;
}

static const struct iio_info test_info = {
	.driver_module = THIS_MODULE,
	.read_raw = test_read_raw,
	.write_raw = test_write_raw,
};

static const struct iio_chan_spec test_channels[] = {
	{ .type = IIO_VOLTAGE,
	  .info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),
	  .info_mask_shared_by_all = BIT(IIO_CHAN_INFO_SAMP_FREQ),
	  .info_mask_separate = BIT(IIO_CHAN_INFO_RAW)|BIT(IIO_CHAN_INFO_PEAK)|BIT(IIO_CHAN_INFO_OFFSET),
	  //.info_mask_separate = BIT(IIO_CHAN_INFO_SAMP_FREQ),
	  .extend_name = "sample",
	  .address = 0,
	  .indexed = 1,
	  .channel = 0,
	  .scan_index = 0,
	  .scan_type = {
		      .sign = 's',
		      .realbits = 12,
		      .storagebits = 16,
		      .shift = 4,
		      .endianness = IIO_LE,
	  }
},
};

#include "linux/iio/buffer-dma.h"
struct dmaengine_buffer {
	struct iio_dma_buffer_queue queue;

	struct dma_chan *chan;
	struct list_head active;

	size_t align;
	u32 max_size;
};

static void my_iio_dmaengine_buffer_block_done(void *data)
{
	struct iio_dma_buffer_block *block = data;
	unsigned long flags;

	spin_lock_irqsave(&block->queue->list_lock, flags);
	list_del(&block->head);
	spin_unlock_irqrestore(&block->queue->list_lock, flags);
	iio_dma_buffer_block_done(block);
}

static struct dmaengine_buffer *iio_buffer_to_dmaengine_buffer(struct iio_buffer *buffer)
{
	return container_of(buffer, struct dmaengine_buffer, queue.buffer);
}

int my_iio_dmaengine_buffer_submit_block(struct iio_dma_buffer_queue *queue,
					 struct iio_dma_buffer_block *block, int direction)
{
	struct dmaengine_buffer *dmaengine_buffer;
	struct dma_async_tx_descriptor *desc;
	dma_cookie_t cookie;
	pr_err("Entering submit block 1\n");
	dmaengine_buffer = iio_buffer_to_dmaengine_buffer(&block->queue->buffer);

	if (direction == DMA_DEV_TO_MEM)
		block->block.bytes_used = block->block.size;
	block->block.bytes_used = min(block->block.bytes_used, dmaengine_buffer->max_size);
	block->block.bytes_used = rounddown(block->block.bytes_used, dmaengine_buffer->align);
	if (block->block.bytes_used == 0) {
		iio_dma_buffer_block_done(block);
		return 0;
	}
	pr_err("Entering submit block 2\n");

	// if (block->block.flags & IIO_BUFFER_BLOCK_FLAG_CYCLIC) {
	// 	desc = dmaengine_prep_dma_cyclic(dmaengine_buffer->chan, block->phys_addr,
	// 					 block->block.bytes_used, block->block.bytes_used,
	// 					 direction, 0);
	// 	if (!desc)
	// 		return -ENOMEM;
	// }
	// else
	{
		desc = dmaengine_prep_slave_single(dmaengine_buffer->chan, block->phys_addr,
						   block->block.bytes_used, direction,
						   DMA_PREP_INTERRUPT);
		if (!desc)
			return -ENOMEM;

		desc->callback = my_iio_dmaengine_buffer_block_done;
		desc->callback_param = block;
	}
	pr_err("Entering submit block 3\n");

	spin_lock_irq(&dmaengine_buffer->queue.list_lock);
	list_add_tail(&block->head, &dmaengine_buffer->active);
	spin_unlock_irq(&dmaengine_buffer->queue.list_lock);

	cookie = dmaengine_submit(desc);
	if (dma_submit_error(cookie))
		return dma_submit_error(cookie);
	pr_err("Calling async issue pending\n");
	dma_async_issue_pending(dmaengine_buffer->chan);

	return 0;
}

static int hw_submit_block(struct iio_dma_buffer_queue *queue, struct iio_dma_buffer_block *block)
{
	pr_err("Called submit_block\n");
	block->block.bytes_used = block->block.size;
	//pr_err("Virtual Address of block = %p\n", &queue->buffer);
	return my_iio_dmaengine_buffer_submit_block(queue, block, DMA_DEV_TO_MEM);
	// return 0;
}

static const struct iio_dma_buffer_ops dma_buffer_ops = {
	.submit = hw_submit_block,
	.abort = iio_dmaengine_buffer_abort,
};

struct private_state {
	struct iio_dev *indio_dev;
	int foo;
};

static void callback(void *data)
{
	pr_err("DMA finished.  Callback executed\n");
}

static int manual_dma(struct platform_device *pdev)
{
	struct dma_chan *chan;
	struct dma_async_tx_descriptor *desc;

	dma_cookie_t cookie;
	// struct iio_dma_buffer_block *block;
	void *vaddr;
	dma_addr_t phys_addr;
	__u32 bytes_used = 4096;
	struct scatterlist sg;
	sg_init_table(&sg, 1);

	vaddr = dma_alloc_coherent(&pdev->dev, PAGE_ALIGN(4096), &phys_addr, GFP_KERNEL);

	pr_err("DMA addr v=%p, p=%p\n", vaddr, (void *)phys_addr);

	chan = dma_request_slave_channel_reason(&pdev->dev, "rx");
	if (IS_ERR(chan)) {
		return PTR_ERR(chan);
	}

	// dma_map_sg(dev, sg, 1, r);
	// _single allocates the sg list and calls device_prep_slave_sg
	sg_dma_address(&sg) = phys_addr;
	sg_dma_len(&sg) = 4096;
	desc = dmaengine_prep_slave_sg(chan, &sg, 1, DMA_DEV_TO_MEM, DMA_PREP_INTERRUPT);
	// desc = dmaengine_prep_slave_single(chan, phys_addr, bytes_used, DMA_DEV_TO_MEM,
	// 				   DMA_PREP_INTERRUPT);
	pr_err("Finished calling dma prep");
	// if (!desc)
	// 	return -ENOMEM;
	// pr_err("Done with prep dma\n");
	// desc->callback = callback;
	// desc->callback_param = &cookie;

	// cookie = dmaengine_submit(desc);
	// if (dma_submit_error(cookie))
	// 	return dma_submit_error(cookie);

	// dma_async_issue_pending(chan);

	return 0;
}

static int test_probe(struct platform_device *pdev)
{
	struct iio_buffer *buffer;
	struct iio_dev *indio_dev;
	struct private_state *st;

	indio_dev = devm_iio_device_alloc(&pdev->dev, sizeof(*st));
	if (!indio_dev)
		return -ENOMEM;

	st = iio_priv(indio_dev);
	st->indio_dev = indio_dev;
	st->foo = 1;
	platform_set_drvdata(pdev, st);

	indio_dev->dev.parent = &pdev->dev;
	indio_dev->name = pdev->dev.of_node->name;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = test_channels;
	indio_dev->num_channels = ARRAY_SIZE(test_channels);
	indio_dev->info = &test_info;

	buffer = iio_dmaengine_buffer_alloc(&pdev->dev, "rx", &dma_buffer_ops, indio_dev);
	if (IS_ERR(buffer))
		return PTR_ERR(buffer);
	indio_dev->modes |= INDIO_BUFFER_HARDWARE;
	iio_device_attach_buffer(indio_dev, buffer);
	pr_err("Address of block = %p\n", buffer);

	devm_iio_device_register(&pdev->dev, indio_dev);

	// 	if (iio_get_debugfs_dentry(indio_dev))
	// 	debugfs_create_file("pseudorandom_err_check", 0644,
	// 				iio_get_debugfs_dentry(indio_dev),
	// 				indio_dev, &axiadc_debugfs_pncheck_fops);

	return 0;
}

static int test_remove(struct platform_device *pdev)
{
	struct private_state *st;
	st = platform_get_drvdata(pdev);
	devm_iio_device_unregister(&pdev->dev, st->indio_dev);
	//Freeing the device detaches the buffer.
	devm_iio_device_free(&pdev->dev, st->indio_dev);
	iio_dmaengine_buffer_free(st->indio_dev->buffer);
	return 0;
}

static const struct of_device_id test_match[] = {
	{ .compatible = "imsar,iiotest" },
	{ /* end of list */ },
};
MODULE_DEVICE_TABLE(of, test_match);

static struct platform_driver test_driver = {
	// .probe = manual_dma,
	.probe = test_probe,
	.remove = test_remove,
	.driver = {
		.name = "iio_test_dev",
		.owner = THIS_MODULE,
		.of_match_table = test_match,
	},
};
module_platform_driver(test_driver);

MODULE_LICENSE("GPL");
