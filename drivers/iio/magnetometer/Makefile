#
# Makefile for industrial I/O Magnetometer sensor drivers
#
include $(srctree)/drivers/misc/mediatek/Makefile.custom

obj-$(CONFIG_AK8975)	+= ak8975.o
obj-$(CONFIG_HID_SENSOR_MAGNETOMETER_3D) += hid-sensor-magn-3d.o

obj-$(CONFIG_IIO_ST_MAGN_3AXIS) += st_magn.o
st_magn-y := st_magn_core.o
st_magn-$(CONFIG_IIO_BUFFER) += st_magn_buffer.o

obj-$(CONFIG_IIO_ST_MAGN_I2C_3AXIS) += st_magn_i2c.o
# obj-$(CONFIG_IIO_ST_MAGN_SPI_3AXIS) += st_magn_spi.o
obj-$(CONFIG_MZ_MAGNETOMETER) += mz_mag.o
