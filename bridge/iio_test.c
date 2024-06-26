#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/iio/buffer.h>
#include <linux/iio/buffer-dma.h>
#include <linux/iio/buffer-dmaengine.h>

#include <linux/dma-mapping.h>
#include <linux/dmaengine.h>

#include <linux/platform_device.h>
#include <linux/module.h>

#define ADI_KERNEL

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

#ifdef ADI_KERNEL
static int hw_submit_block(struct iio_dma_buffer_queue *queue, struct iio_dma_buffer_block *block)
{
	pr_err("Called submit_block\n");

	block->block.bytes_used = block->block.size;
	return iio_dmaengine_buffer_submit_block(queue, block, DMA_DEV_TO_MEM);
}

static const struct iio_dma_buffer_ops dma_buffer_ops = {
	.submit = hw_submit_block,
	.abort = iio_dmaengine_buffer_abort,
};
#endif

struct private_state {
	struct iio_dev *indio_dev;
	int foo;
};

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

#ifdef ADI_KERNEL
	buffer = iio_dmaengine_buffer_alloc(&pdev->dev, "rx", &dma_buffer_ops, indio_dev);
#else
	buffer = iio_dmaengine_buffer_alloc(&pdev->dev, "rx");
#endif
	if (IS_ERR(buffer))
		return PTR_ERR(buffer);
	indio_dev->modes |= INDIO_BUFFER_HARDWARE;
	iio_device_attach_buffer(indio_dev, buffer);
	pr_err("Address of block = %p\n", buffer);

	devm_iio_device_register(&pdev->dev, indio_dev);
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
