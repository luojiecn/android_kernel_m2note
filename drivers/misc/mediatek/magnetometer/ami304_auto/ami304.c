/* drivers/i2c/chips/ami304.c - AMI304 compass driver
 *
 * Copyright (C) 2009 AMIT Technology Inc.
 * Author: Kyle Chen <sw-support@amit-inc.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/interrupt.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/irq.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <asm/atomic.h>
#include <linux/delay.h>
#include <linux/input.h>
#include <linux/workqueue.h>
#include <linux/kobject.h>
#include <linux/platform_device.h>
#include <linux/earlysuspend.h>

#include <linux/hwmsensor.h>
#include <linux/hwmsen_dev.h>
#include <linux/sensors_io.h>


#include <cust_mag.h>
#include "ami304.h"
#include <linux/hwmsen_helper.h>

#include <mach/mt_typedefs.h>
#include <mach/mt_gpio.h>
#include <mach/mt_pm_ldo.h>


/*-------------------------MT6516&MT6573 define-------------------------------*/

#define POWER_NONE_MACRO MT65XX_POWER_NONE


/*----------------------------------------------------------------------------*/
#define I2C_DRIVERID_AMI304 304
//#define DEBUG 1
#define AMI304_DEV_NAME         "ami304"
#define DRIVER_VERSION          "1.0.6.11"
/*----------------------------------------------------------------------------*/
#define AMI304_AXIS_X            0
#define AMI304_AXIS_Y            1
#define AMI304_AXIS_Z            2
#define AMI304_AXES_NUM          3
/*----------------------------------------------------------------------------*/
#define MSE_TAG                  "MSENSOR"
#define MSE_FUN(f)               printk(KERN_INFO MSE_TAG" %s\r\n", __func__)
#define MSE_ERR(fmt, args...)    printk(KERN_ERR MSE_TAG" %s %d : \r\n"fmt, __func__, __LINE__, ##args)
#define MSE_LOG(fmt, args...)    printk(KERN_INFO MSE_TAG fmt, ##args)
#define MSE_VER(fmt, args...)   ((void)0)



static DECLARE_WAIT_QUEUE_HEAD(data_ready_wq);
static DECLARE_WAIT_QUEUE_HEAD(open_wq);

static atomic_t open_flag = ATOMIC_INIT(0);
static atomic_t m_flag = ATOMIC_INIT(0);
static atomic_t o_flag = ATOMIC_INIT(0);

/*----------------------------------------------------------------------------*/
static struct i2c_client *ami304_i2c_client = NULL;
/*----------------------------------------------------------------------------*/
/*----------------------------------------------------------------------------*/
static const struct i2c_device_id ami304_i2c_id[] = {{AMI304_DEV_NAME,0},{}};
/*the adapter id will be available in customization*/
static unsigned short ami304_force[] = {0x00, AMI304_I2C_ADDRESS, I2C_CLIENT_END, I2C_CLIENT_END};
static const unsigned short *const ami304_forces[] = { ami304_force, NULL };
static struct i2c_client_address_data ami304_addr_data = { .forces = ami304_forces,};
/*----------------------------------------------------------------------------*/
static int ami304_i2c_probe(struct i2c_client *client, const struct i2c_device_id *id); 
static int ami304_i2c_remove(struct i2c_client *client);
static int ami304_i2c_detect(struct i2c_client *client, int kind, struct i2c_board_info *info);

static int  ami304_local_init(void);
static int  ami304_remove(void);

static int ami304_init_flag =0; // 0<==>OK -1 <==> fail

//static struct platform_driver ami_sensor_driver;

/*----------------------------------------------------------------------------*/
typedef enum {
    AMI_TRC_DEBUG  = 0x01,
} AMI_TRC;
/*----------------------------------------------------------------------------*/
struct _ami302_data {
    rwlock_t lock;
    int mode;
    int rate;
    volatile int updated;
} ami304_data;
/*----------------------------------------------------------------------------*/
struct _ami304mid_data {
    rwlock_t datalock;
    rwlock_t ctrllock;    
    int controldata[10];
    unsigned int debug;
    int yaw;
    int roll;
    int pitch;
    int nmx;
    int nmy;
    int nmz;
    int nax;
    int nay;
    int naz;
    int mag_status;
} ami304mid_data;
/*----------------------------------------------------------------------------*/
struct ami304_i2c_data {
    struct i2c_client *client;
    struct mag_hw *hw;
    struct hwmsen_convert   cvt;
    atomic_t layout;   
    atomic_t trace;
#if defined(CONFIG_HAS_EARLYSUSPEND)    
    struct early_suspend    early_drv;
#endif 
};

static struct sensor_init_info ami304_init_info = {
		.name = "ami304",
		.init = ami304_local_init,
		.uninit = ami304_remove,
	
};

/*----------------------------------------------------------------------------*/
static struct i2c_driver ami304_i2c_driver = {
    .driver = {
        .owner = THIS_MODULE, 
        .name  = AMI304_DEV_NAME,
    },
	.probe      = ami304_i2c_probe,
	.remove     = ami304_i2c_remove,
	.detect     = ami304_i2c_detect,
#if !defined(CONFIG_HAS_EARLYSUSPEND)
	.suspend    = ami304_suspend,
	.resume     = ami304_resume,
#endif 
	.id_table = ami304_i2c_id,
	.address_data = &ami304_addr_data,
};
/*----------------------------------------------------------------------------*/
static atomic_t dev_open_count;
/*----------------------------------------------------------------------------*/
static void ami304_power(struct mag_hw *hw, unsigned int on) 
{
	static unsigned int power_on = 0;

	if(hw->power_id != POWER_NONE_MACRO)
	{        
		MSE_LOG("power %s\n", on ? "on" : "off");
		if(power_on == on)
		{
			MSE_LOG("ignore power control: %d\n", on);
		}
		else if(on)
		{
			if(!hwPowerOn(hw->power_id, hw->power_vol, "AMI304")) 
			{
				MSE_ERR("power on fails!!\n");
			}
		}
		else
		{
			if(!hwPowerDown(hw->power_id, "AMI304")) 
			{
				MSE_ERR("power off fail!!\n");
			}
		}
	}
	power_on = on;
}
/*----------------------------------------------------------------------------*/

static int ami304_GetOpenStatus(void)
{
	wait_event_interruptible(open_wq, (atomic_read(&open_flag) != 0));
	return atomic_read(&open_flag);
}


static int ami304_gpio_config(void)
{
    //because we donot use EINT ,to support low power
    // config to GPIO input mode + PD    
    //set   GPIO_MSE_EINT_PIN
    mt_set_gpio_mode(GPIO_MSE_EINT_PIN, GPIO_MSE_EINT_PIN_M_GPIO);
    mt_set_gpio_dir(GPIO_MSE_EINT_PIN, GPIO_DIR_IN);
	mt_set_gpio_pull_enable(GPIO_MSE_EINT_PIN, GPIO_PULL_ENABLE);
	mt_set_gpio_pull_select(GPIO_MSE_EINT_PIN, GPIO_PULL_DOWN);
	return 0;
}

static int AMI304_Chipset_Init(int mode)
{
	u8 databuf[10];
	u8 ctrl1, ctrl2, ctrl3;
	int err;

	ami304_gpio_config();
	ami304_i2c_client->addr = (ami304_i2c_client->addr & I2C_MASK_FLAG )|(I2C_ENEXT_FLAG);

	if((err = hwmsen_read_byte(ami304_i2c_client, AMI304_REG_CTRL1, &ctrl1)))
	{
		MSE_ERR("read CTRL1 fail: %d\n", err);
		return err;
	}
	
	if((err = hwmsen_read_byte(ami304_i2c_client, AMI304_REG_CTRL2, &ctrl2)))
	{
		MSE_ERR("read CTRL2 fail: %d\n", err);
		return err;        
	}
	
	if((err = hwmsen_read_byte(ami304_i2c_client, AMI304_REG_CTRL3, &ctrl3)))
	{
		MSE_ERR("read CTRL3 fail: %d\n", err);
		return err;
	}     

	databuf[0] = AMI304_REG_CTRL1;
	
	if(mode==AMI304_FORCE_MODE)
	{
		databuf[1] = ctrl1 | AMI304_CTRL1_PC1 | AMI304_CTRL1_FS1_FORCE;
		write_lock(&ami304_data.lock);
		ami304_data.mode = AMI304_FORCE_MODE;
		write_unlock(&ami304_data.lock);
	}
	else    
	{
		databuf[1] = ctrl1 | AMI304_CTRL1_PC1 | AMI304_CTRL1_FS1_NORMAL | AMI304_CTRL1_ODR1;
		write_lock(&ami304_data.lock);
		ami304_data.mode = AMI304_NORMAL_MODE;
		write_unlock(&ami304_data.lock);
	}
	
	i2c_master_send(ami304_i2c_client, databuf, 2);        

	//databuf[0] = AMI304_REG_CTRL2;
	//databuf[1] = ctrl2 | AMI304_CTRL2_DREN;
	//i2c_master_send(ami304_i2c_client, databuf, 2);        

	databuf[0] = AMI304_REG_CTRL3;
	databuf[1] = ctrl3 | AMI304_CTRL3_B0_LO_CLR;
	i2c_master_send(ami304_i2c_client, databuf, 2);
	
	return 0;
}
/*----------------------------------------------------------------------------*/
static int AMI304_SetMode(int newmode)
{
	int mode = 0;

	read_lock(&ami304_data.lock);
	mode = ami304_data.mode;
	read_unlock(&ami304_data.lock);        

	if(mode == newmode)
	{
		return 0;    
	}
	
	return AMI304_Chipset_Init(newmode);
}
/*----------------------------------------------------------------------------*/
static int AMI304_ReadChipInfo(char *buf, int bufsize)
{
	if((!buf)||(bufsize<=30))
	{
		return -1;
	}
	if(!ami304_i2c_client)
	{
		*buf = 0;
		return -2;
	}

	sprintf(buf, "AMI304 Chip");
	return 0;
}
/*----------------------------------------------------------------------------*/
static int AMI304_ReadSensorData(char *buf, int bufsize)
{
	struct ami304_i2c_data *data = i2c_get_clientdata(ami304_i2c_client);
	char cmd;
	int mode = 0;    
	unsigned char databuf[10];
	short output[3];
	int mag[AMI304_AXES_NUM];

	if((!buf)||(bufsize<=80))
	{
		return -1;
	}	
	if(NULL == ami304_i2c_client)
	{
		*buf = 0;
		return -2;
	}

	read_lock(&ami304_data.lock);    
	mode = ami304_data.mode;
	read_unlock(&ami304_data.lock);        

	databuf[0] = AMI304_REG_CTRL3;
	databuf[1] = AMI304_CTRL3_FORCE_BIT;
	i2c_master_send(ami304_i2c_client, databuf, 2);    
	// We can read all measured data in once
	cmd = AMI304_REG_DATAXH;
	i2c_master_send(ami304_i2c_client, &cmd, 1);    
	i2c_master_recv(ami304_i2c_client, &(databuf[0]), 6);

	output[0] = ((int) databuf[1]) << 8 | ((int) databuf[0]);
	output[1] = ((int) databuf[3]) << 8 | ((int) databuf[2]);
	output[2] = ((int) databuf[5]) << 8 | ((int) databuf[4]);

	mag[data->cvt.map[AMI304_AXIS_X]] = data->cvt.sign[AMI304_AXIS_X]*output[AMI304_AXIS_X];
	mag[data->cvt.map[AMI304_AXIS_Y]] = data->cvt.sign[AMI304_AXIS_Y]*output[AMI304_AXIS_Y];
	mag[data->cvt.map[AMI304_AXIS_Z]] = data->cvt.sign[AMI304_AXIS_Z]*output[AMI304_AXIS_Z];

	sprintf(buf, "%04x %04x %04x", mag[AMI304_AXIS_X], mag[AMI304_AXIS_Y], mag[AMI304_AXIS_Z]);

	return 0;
}
/*----------------------------------------------------------------------------*/
static int AMI304_ReadPostureData(char *buf, int bufsize)
{
	if((!buf)||(bufsize<=80))
	{
		return -1;
	}
	
	read_lock(&ami304mid_data.datalock);
	sprintf(buf, "%d %d %d %d", ami304mid_data.yaw, ami304mid_data.pitch,
		ami304mid_data.roll, ami304mid_data.mag_status);
	read_unlock(&ami304mid_data.datalock);
	return 0;
}
/*----------------------------------------------------------------------------*/
static int AMI304_ReadCaliData(char *buf, int bufsize)
{
	if((!buf)||(bufsize<=80))
	{
		return -1;
	}
	
	read_lock(&ami304mid_data.datalock);
	sprintf(buf, "%d %d %d %d %d %d %d", ami304mid_data.nmx, ami304mid_data.nmy, 
		ami304mid_data.nmz,ami304mid_data.nax,ami304mid_data.nay,ami304mid_data.naz,ami304mid_data.mag_status);
	read_unlock(&ami304mid_data.datalock);
	return 0;
}
/*----------------------------------------------------------------------------*/
static int AMI304_ReadMiddleControl(char *buf, int bufsize)
{
	if ((!buf)||(bufsize<=80))
	{
		return -1;
	}
	
	read_lock(&ami304mid_data.ctrllock);
	sprintf(buf, "%d %d %d %d %d %d %d %d %d %d",ami304mid_data.controldata[0],	ami304mid_data.controldata[1], 
		ami304mid_data.controldata[2],ami304mid_data.controldata[3],ami304mid_data.controldata[4],
		ami304mid_data.controldata[5], ami304mid_data.controldata[6], ami304mid_data.controldata[7],
		ami304mid_data.controldata[8], ami304mid_data.controldata[9]);
	read_unlock(&ami304mid_data.ctrllock);
	return 0;
}
/*----------------------------------------------------------------------------*/
static ssize_t show_daemon_name(struct device_driver *ddri, char *buf)
{
	char strbuf[AMI304_BUFSIZE];
	sprintf(strbuf, "ami304d");
	return sprintf(buf, "%s", strbuf);		
}

static ssize_t show_chipinfo_value(struct device_driver *ddri, char *buf)
{
	char strbuf[AMI304_BUFSIZE];
	AMI304_ReadChipInfo(strbuf, AMI304_BUFSIZE);
	return sprintf(buf, "%s\n", strbuf);        
}
/*----------------------------------------------------------------------------*/
static ssize_t show_sensordata_value(struct device_driver *ddri, char *buf)
{
	char strbuf[AMI304_BUFSIZE];
	AMI304_ReadSensorData(strbuf, AMI304_BUFSIZE);
	return sprintf(buf, "%s\n", strbuf);
}
/*----------------------------------------------------------------------------*/
static ssize_t show_posturedata_value(struct device_driver *ddri, char *buf)
{
	char strbuf[AMI304_BUFSIZE];
	AMI304_ReadPostureData(strbuf, AMI304_BUFSIZE);
	return sprintf(buf, "%s\n", strbuf);            
}
/*----------------------------------------------------------------------------*/
static ssize_t show_calidata_value(struct device_driver *ddri, char *buf)
{
	char strbuf[AMI304_BUFSIZE];
	AMI304_ReadCaliData(strbuf, AMI304_BUFSIZE);
	return sprintf(buf, "%s\n", strbuf);            
}
/*----------------------------------------------------------------------------*/
static ssize_t show_midcontrol_value(struct device_driver *ddri, char *buf)
{
	char strbuf[AMI304_BUFSIZE];
	AMI304_ReadMiddleControl(strbuf, AMI304_BUFSIZE);
	return sprintf(buf, "%s\n", strbuf);            
}
/*----------------------------------------------------------------------------*/
static ssize_t store_midcontrol_value(struct device_driver *ddri, const char *buf, size_t count)
{   
	int p[10];
	if(10 == sscanf(buf, "%d %d %d %d %d %d %d %d %d %d",&p[0], &p[1], &p[2], &p[3], &p[4], 
		&p[5], &p[6], &p[7], &p[8], &p[9]))
	{
		write_lock(&ami304mid_data.ctrllock);
		memcpy(&ami304mid_data.controldata[0], &p, sizeof(int)*10);    
		write_unlock(&ami304mid_data.ctrllock);        
	}
	else
	{
		MSE_ERR("invalid format\n");     
	}
	return sizeof(int)*10;            
}
/*----------------------------------------------------------------------------*/
static ssize_t show_middebug_value(struct device_driver *ddri, char *buf)
{
	ssize_t len;
	read_lock(&ami304mid_data.ctrllock);
	len = sprintf(buf, "0x%08X\n", ami304mid_data.debug);
	read_unlock(&ami304mid_data.ctrllock);

	return len;            
}
/*----------------------------------------------------------------------------*/
static ssize_t store_middebug_value(struct device_driver *ddri, const char *buf, size_t count)
{   
	int debug;
	if(1 == sscanf(buf, "0x%x", &debug))
	{
		write_lock(&ami304mid_data.ctrllock);
		ami304mid_data.debug = debug;
		write_unlock(&ami304mid_data.ctrllock);        
	}
	else
	{
		MSE_ERR("invalid format\n");     
	}
	return count;            
}
/*----------------------------------------------------------------------------*/
static ssize_t show_mode_value(struct device_driver *ddri, char *buf)
{
	int mode=0;
	read_lock(&ami304_data.lock);
	mode = ami304_data.mode;
	read_unlock(&ami304_data.lock);        
	return sprintf(buf, "%d\n", mode);            
}
/*----------------------------------------------------------------------------*/
static ssize_t store_mode_value(struct device_driver *ddri, const char *buf, size_t count)
{
	int mode = 0;
	sscanf(buf, "%d", &mode);    
	AMI304_SetMode(mode);
	return count;            
}
/*----------------------------------------------------------------------------*/
static ssize_t show_layout_value(struct device_driver *ddri, char *buf)
{
	struct i2c_client *client = ami304_i2c_client;  
	struct ami304_i2c_data *data = i2c_get_clientdata(client);

	return sprintf(buf, "(%d, %d)\n[%+2d %+2d %+2d]\n[%+2d %+2d %+2d]\n",
		data->hw->direction,atomic_read(&data->layout),	data->cvt.sign[0], data->cvt.sign[1],
		data->cvt.sign[2],data->cvt.map[0], data->cvt.map[1], data->cvt.map[2]);            
}
/*----------------------------------------------------------------------------*/
static ssize_t store_layout_value(struct device_driver *ddri, const char *buf, size_t count)
{
	struct i2c_client *client = ami304_i2c_client;  
	struct ami304_i2c_data *data = i2c_get_clientdata(client);
	int layout = 0;

	if(1 == sscanf(buf, "%d", &layout))
	{
		atomic_set(&data->layout, layout);
		if(!hwmsen_get_convert(layout, &data->cvt))
		{
			MSE_ERR("HWMSEN_GET_CONVERT function error!\r\n");
		}
		else if(!hwmsen_get_convert(data->hw->direction, &data->cvt))
		{
			MSE_ERR("invalid layout: %d, restore to %d\n", layout, data->hw->direction);
		}
		else
		{
			MSE_ERR("invalid layout: (%d, %d)\n", layout, data->hw->direction);
			hwmsen_get_convert(0, &data->cvt);
		}
	}
	else
	{
		MSE_ERR("invalid format = '%s'\n", buf);
	}
	
	return count;            
}
/*----------------------------------------------------------------------------*/
static ssize_t show_status_value(struct device_driver *ddri, char *buf)
{
	struct i2c_client *client = ami304_i2c_client;  
	struct ami304_i2c_data *data = i2c_get_clientdata(client);
	ssize_t len = 0;

	if(data->hw)
	{
		len += snprintf(buf+len, PAGE_SIZE-len, "CUST: %d %d (%d %d)\n", 
			data->hw->i2c_num, data->hw->direction, data->hw->power_id, data->hw->power_vol);
	}
	else
	{
		len += snprintf(buf+len, PAGE_SIZE-len, "CUST: NULL\n");
	}
	
	len += snprintf(buf+len, PAGE_SIZE-len, "OPEN: %d\n", atomic_read(&dev_open_count));
	return len;
}
/*----------------------------------------------------------------------------*/
static ssize_t show_trace_value(struct device_driver *ddri, char *buf)
{
	ssize_t res;
	struct ami304_i2c_data *obj = i2c_get_clientdata(ami304_i2c_client);
	if(NULL == obj)
	{
		MSE_ERR("ami304_i2c_data is null!!\n");
		return 0;
	}	
	
	res = snprintf(buf, PAGE_SIZE, "0x%04X\n", atomic_read(&obj->trace));     
	return res;    
}
/*----------------------------------------------------------------------------*/
static ssize_t store_trace_value(struct device_driver *ddri, const char *buf, size_t count)
{
	struct ami304_i2c_data *obj = i2c_get_clientdata(ami304_i2c_client);
	int trace;
	if(NULL == obj)
	{
		MSE_ERR("ami304_i2c_data is null!!\n");
		return 0;
	}
	
	if(1 == sscanf(buf, "0x%x", &trace))
	{
		atomic_set(&obj->trace, trace);
	}
	else 
	{
		MSE_ERR("invalid content: '%s', length = %d\n", buf, count);
	}
	
	return count;    
}


/*----------------------------------------------------------------------------*/
static DRIVER_ATTR(daemon,      S_IRUGO, show_daemon_name, NULL);
static DRIVER_ATTR(chipinfo,    S_IRUGO, show_chipinfo_value, NULL);
static DRIVER_ATTR(sensordata,  S_IRUGO, show_sensordata_value, NULL);
static DRIVER_ATTR(posturedata, S_IRUGO, show_posturedata_value, NULL);
static DRIVER_ATTR(calidata,    S_IRUGO, show_calidata_value, NULL);
static DRIVER_ATTR(midcontrol,  S_IRUGO | S_IWUSR, show_midcontrol_value, store_midcontrol_value );
static DRIVER_ATTR(middebug,    S_IRUGO | S_IWUSR, show_middebug_value, store_middebug_value );
static DRIVER_ATTR(mode,        S_IRUGO | S_IWUSR, show_mode_value, store_mode_value );
static DRIVER_ATTR(layout,      S_IRUGO | S_IWUSR, show_layout_value, store_layout_value );
static DRIVER_ATTR(status,      S_IRUGO, show_status_value, NULL);
static DRIVER_ATTR(trace,       S_IRUGO | S_IWUSR, show_trace_value, store_trace_value );
/*----------------------------------------------------------------------------*/
static struct driver_attribute *ami304_attr_list[] = {
    &driver_attr_daemon,
	&driver_attr_chipinfo,
	&driver_attr_sensordata,
	&driver_attr_posturedata,
	&driver_attr_calidata,
	&driver_attr_midcontrol,
	&driver_attr_middebug,
	&driver_attr_mode,
	&driver_attr_layout,
	&driver_attr_status,
	&driver_attr_trace,
};
/*----------------------------------------------------------------------------*/
static int ami304_create_attr(struct device_driver *driver) 
{
	int idx, err = 0;
	int num = (int)(sizeof(ami304_attr_list)/sizeof(ami304_attr_list[0]));
	if (driver == NULL)
	{
		return -EINVAL;
	}

	for(idx = 0; idx < num; idx++)
	{
		if((err = driver_create_file(driver, ami304_attr_list[idx])))
		{            
			MSE_ERR("driver_create_file (%s) = %d\n", ami304_attr_list[idx]->attr.name, err);
			break;
		}
	}    
	return err;
}
/*----------------------------------------------------------------------------*/
static int ami304_delete_attr(struct device_driver *driver)
{
	int idx ,err = 0;
	int num = (int)(sizeof(ami304_attr_list)/sizeof(ami304_attr_list[0]));

	if(driver == NULL)
	{
		return -EINVAL;
	}
	

	for(idx = 0; idx < num; idx++)
	{
		driver_remove_file(driver, ami304_attr_list[idx]);
	}
	

	return err;
}


/*----------------------------------------------------------------------------*/
static int ami304_open(struct inode *inode, struct file *file)
{    
	struct ami304_i2c_data *obj = i2c_get_clientdata(ami304_i2c_client);    
	int ret = -1;
	atomic_inc(&dev_open_count);
	
	if(atomic_read(&obj->trace) & AMI_TRC_DEBUG)
	{
		MSE_LOG("Open device node:ami304\n");
	}
	ret = nonseekable_open(inode, file);
	
	return ret;
}
/*----------------------------------------------------------------------------*/
static int ami304_release(struct inode *inode, struct file *file)
{
	struct ami304_i2c_data *obj = i2c_get_clientdata(ami304_i2c_client);
	atomic_dec(&dev_open_count);
	if(atomic_read(&obj->trace) & AMI_TRC_DEBUG)
	{
		MSE_LOG("Release device node:ami304\n");
	}	
	return 0;
}
/*----------------------------------------------------------------------------*/
static int ami304_ioctl(struct inode *inode, struct file *file, unsigned int cmd,unsigned long arg)
{
    void __user *argp = (void __user *)arg;
	int valuebuf[4];
	int calidata[7];
	int controlbuf[10];
	char strbuf[AMI304_BUFSIZE];
	void __user *data;
	int retval=0;
	int mode=0;
	hwm_sensor_data* osensor_data;
	uint32_t enable;
	char buff[512];	
	int status; 				/* for OPEN/CLOSE_STATUS */
	short sensor_status;		/* for Orientation and Msensor status */
//	MSE_FUN(f);

	switch (cmd)
	{
		case MSENSOR_IOCTL_INIT:
			read_lock(&ami304_data.lock);
			mode = ami304_data.mode;
			read_unlock(&ami304_data.lock);
			AMI304_Chipset_Init(mode);         
			break;

		case MSENSOR_IOCTL_SET_POSTURE:
			data = (void __user *) arg;
			if(data == NULL)
			{
				MSE_ERR("IO parameter pointer is NULL!\r\n");
				break;
			}
			   
			if(copy_from_user(&valuebuf, data, sizeof(valuebuf)))
			{
				retval = -EFAULT;
				goto err_out;
			}
			
			write_lock(&ami304mid_data.datalock);
			ami304mid_data.yaw   = valuebuf[0];
			ami304mid_data.pitch = valuebuf[1];
			ami304mid_data.roll  = valuebuf[2];
			ami304mid_data.mag_status = valuebuf[3];
			write_unlock(&ami304mid_data.datalock);    
			break;

		case ECOMPASS_IOC_GET_OFLAG:
			sensor_status = atomic_read(&o_flag);
			if(copy_to_user(argp, &sensor_status, sizeof(sensor_status)))
			{
				MSE_ERR("copy_to_user failed.");
				return -EFAULT;
			}
			break;

		case ECOMPASS_IOC_GET_MFLAG:
			sensor_status = atomic_read(&m_flag);
			if(copy_to_user(argp, &sensor_status, sizeof(sensor_status)))
			{
				MSE_ERR("copy_to_user failed.");
				return -EFAULT;
			}
			break;
			
		case ECOMPASS_IOC_GET_OPEN_STATUS:
			status = ami304_GetOpenStatus();			
			if(copy_to_user(argp, &status, sizeof(status)))
			{
				MSE_LOG("copy_to_user failed.");
				return -EFAULT;
			}
			break;

		case MSENSOR_IOCTL_SET_CALIDATA:
			data = (void __user *) arg;
			if (data == NULL)
			{
				MSE_ERR("IO parameter pointer is NULL!\r\n");
				break;
			}
			if(copy_from_user(&calidata, data, sizeof(calidata)))
			{
				retval = -EFAULT;
				goto err_out;
			}
			
			write_lock(&ami304mid_data.datalock);            
			ami304mid_data.nmx = calidata[0];
			ami304mid_data.nmy = calidata[1];
			ami304mid_data.nmz = calidata[2];
			ami304mid_data.nax = calidata[3];
			ami304mid_data.nay = calidata[4];
			ami304mid_data.naz = calidata[5];
			ami304mid_data.mag_status = calidata[6];
			write_unlock(&ami304mid_data.datalock);    
			break;                                

		case MSENSOR_IOCTL_READ_CHIPINFO:
			data = (void __user *) arg;
			if(data == NULL)
			{
				MSE_ERR("IO parameter pointer is NULL!\r\n");
				break;
			}
			
			AMI304_ReadChipInfo(strbuf, AMI304_BUFSIZE);
			if(copy_to_user(data, strbuf, strlen(strbuf)+1))
			{
				retval = -EFAULT;
				goto err_out;
			}                
			break;

		case MSENSOR_IOCTL_SENSOR_ENABLE:
			
			data = (void __user *) arg;
			if (data == NULL)
			{
				MSE_ERR("IO parameter pointer is NULL!\r\n");
				break;
			}
			if(copy_from_user(&enable, data, sizeof(enable)))
			{
				MSE_ERR("copy_from_user failed.");
				return -EFAULT;
			}
			else
			{
			    printk( "MSENSOR_IOCTL_SENSOR_ENABLE enable=%d!\r\n",enable);
				read_lock(&ami304mid_data.ctrllock);
				if(enable == 1)
				{
					ami304mid_data.controldata[7] |= SENSOR_ORIENTATION;
					atomic_set(&o_flag, 1);
					atomic_set(&open_flag, 1);
				}
				else
				{
					ami304mid_data.controldata[7] &= ~SENSOR_ORIENTATION;
					atomic_set(&o_flag, 0);
					if(atomic_read(&m_flag) == 0)
					{
						atomic_set(&open_flag, 0);
					}		
				}
				wake_up(&open_wq);
				read_unlock(&ami304mid_data.ctrllock);
				
			}
			
			break;
			
		case MSENSOR_IOCTL_READ_SENSORDATA:
			data = (void __user *) arg;
			if(data == NULL)
			{
				MSE_ERR("IO parameter pointer is NULL!\r\n");
				break;    
			}
			AMI304_ReadSensorData(strbuf, AMI304_BUFSIZE);
			if(copy_to_user(data, strbuf, strlen(strbuf)+1))
			{
				retval = -EFAULT;
				goto err_out;
			}                
			break;

		case MSENSOR_IOCTL_READ_FACTORY_SENSORDATA:
			
			data = (void __user *) arg;
			if (data == NULL)
			{
				MSE_ERR("IO parameter pointer is NULL!\r\n");
				break;
			}
			
			osensor_data = (hwm_sensor_data *)buff;

			read_lock(&ami304mid_data.datalock);
			osensor_data->values[0] = ami304mid_data.yaw;
			osensor_data->values[1] = ami304mid_data.pitch;
			osensor_data->values[2] = ami304mid_data.roll;
			//status = ami304mid_data.mag_status;
			read_unlock(&ami304mid_data.datalock); 
						
			osensor_data->value_divide = ORIENTATION_ACCURACY_RATE;	

			switch (ami304mid_data.mag_status)
		    {
		            case 1: case 2:
		                osensor_data->status = SENSOR_STATUS_ACCURACY_HIGH;
		                break;
		            case 3:
		                osensor_data->status = SENSOR_STATUS_ACCURACY_MEDIUM;
		                break;
		            case 4:
		                osensor_data->status = SENSOR_STATUS_ACCURACY_LOW;
		                break;
		            default:        
		                osensor_data->status = SENSOR_STATUS_UNRELIABLE;
		                break;    
		    }
     
			
            sprintf(buff, "%x %x %x %x %x", osensor_data->values[0], osensor_data->values[1],
				osensor_data->values[2],osensor_data->status,osensor_data->value_divide);
			if(copy_to_user(data, buff, strlen(buff)+1))
			{
				return -EFAULT;
			} 
			
			break;                

		case MSENSOR_IOCTL_READ_POSTUREDATA:
			data = (void __user *) arg;
			if(data == NULL)
			{
				MSE_ERR("IO parameter pointer is NULL!\r\n");
				break;
			}
			
			AMI304_ReadPostureData(strbuf, AMI304_BUFSIZE);
			if(copy_to_user(data, strbuf, strlen(strbuf)+1))
			{
				retval = -EFAULT;
				goto err_out;
			}                
			break;            

		case MSENSOR_IOCTL_READ_CALIDATA:
			data = (void __user *) arg;
			if(data == NULL)
			{
				break;    
			}
			AMI304_ReadCaliData(strbuf, AMI304_BUFSIZE);
			if(copy_to_user(data, strbuf, strlen(strbuf)+1))
			{
				retval = -EFAULT;
				goto err_out;
			}                
			break;

		case MSENSOR_IOCTL_READ_CONTROL:
			read_lock(&ami304mid_data.ctrllock);
			memcpy(controlbuf, &ami304mid_data.controldata[0], sizeof(controlbuf));
			read_unlock(&ami304mid_data.ctrllock);            
			data = (void __user *) arg;
			if(data == NULL)
			{
				break;
			}
			if(copy_to_user(data, controlbuf, sizeof(controlbuf)))
			{
				retval = -EFAULT;
				goto err_out;
			}                                
			break;

		case MSENSOR_IOCTL_SET_CONTROL:
			data = (void __user *) arg;
			if(data == NULL)
			{
				break;
			}
			if(copy_from_user(controlbuf, data, sizeof(controlbuf)))
			{
				retval = -EFAULT;
				goto err_out;
			}    
			write_lock(&ami304mid_data.ctrllock);
			memcpy(&ami304mid_data.controldata[0], controlbuf, sizeof(controlbuf));
			write_unlock(&ami304mid_data.ctrllock);        
			break;

		case MSENSOR_IOCTL_SET_MODE:
			data = (void __user *) arg;
			if(data == NULL)
			{
				break;
			}
			if(copy_from_user(&mode, data, sizeof(mode)))
			{
				retval = -EFAULT;
				goto err_out;
			}
			
			AMI304_SetMode(mode);                
			break;
		    
		default:
			MSE_ERR("%s not supported = 0x%04x", __func__, cmd);
			retval = -ENOIOCTLCMD;
			break;
		}

	err_out:
	return retval;    
}
/*----------------------------------------------------------------------------*/
static struct file_operations ami304_fops = {
	.owner = THIS_MODULE,
	.open = ami304_open,
	.release = ami304_release,
	.ioctl = ami304_ioctl,
};
/*----------------------------------------------------------------------------*/
static struct miscdevice ami304_device = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "msensor",
    .fops = &ami304_fops,
};
/*----------------------------------------------------------------------------*/
int ami304_operate(void* self, uint32_t command, void* buff_in, int size_in,
		void* buff_out, int size_out, int* actualout)
{
	int err = 0;
	int value, sample_delay, status;
	hwm_sensor_data* msensor_data;
	
//	MSE_FUN(f);
	switch (command)
	{
		case SENSOR_DELAY:
			if((buff_in == NULL) || (size_in < sizeof(int)))
			{
				MSE_ERR("Set delay parameter error!\n");
				err = -EINVAL;
			}
			else
			{
				value = *(int *)buff_in;
				if(value <= 20)
				{
					sample_delay = 20;
				}
				
				
				ami304mid_data.controldata[0] = sample_delay;  // Loop Delay
			}	
			break;

		case SENSOR_ENABLE:
			if((buff_in == NULL) || (size_in < sizeof(int)))
			{
				MSE_ERR("Enable sensor parameter error!\n");
				err = -EINVAL;
			}
			else
			{
				value = *(int *)buff_in;
				read_lock(&ami304mid_data.ctrllock);
				if(value == 1)
				{
					ami304mid_data.controldata[7] |= SENSOR_MAGNETIC;
					atomic_set(&m_flag, 1);
					atomic_set(&open_flag, 1);
				}
				else
				{
					ami304mid_data.controldata[7] &= ~SENSOR_MAGNETIC;
					atomic_set(&m_flag, 0);
					if(atomic_read(&o_flag) == 0)
					{
						atomic_set(&open_flag, 0);
					}	
				}
				wake_up(&open_wq);
				read_unlock(&ami304mid_data.ctrllock);
				// TODO: turn device into standby or normal mode
			}
			break;

		case SENSOR_GET_DATA:
			if((buff_out == NULL) || (size_out< sizeof(hwm_sensor_data)))
			{
				MSE_ERR("get sensor data parameter error!\n");
				err = -EINVAL;
			}
			else
			{
				msensor_data = (hwm_sensor_data *)buff_out;
				read_lock(&ami304mid_data.datalock);
				msensor_data->values[0] = ami304mid_data.nmx;
				msensor_data->values[1] = ami304mid_data.nmy;
				msensor_data->values[2] = ami304mid_data.nmz;
				status = ami304mid_data.mag_status;
				read_unlock(&ami304mid_data.datalock); 
				
				msensor_data->values[0] = msensor_data->values[0] * CONVERT_M;
				msensor_data->values[1] = msensor_data->values[1] * CONVERT_M;
				msensor_data->values[2] = msensor_data->values[2] * CONVERT_M;
				msensor_data->value_divide = 1000;

				switch (status)
		        {
		            case 1: case 2:
		                msensor_data->status = SENSOR_STATUS_ACCURACY_HIGH;
		                break;
		            case 3:
		                msensor_data->status = SENSOR_STATUS_ACCURACY_MEDIUM;
		                break;
		            case 4:
		                msensor_data->status = SENSOR_STATUS_ACCURACY_LOW;
		                break;
		            default:        
		                msensor_data->status = SENSOR_STATUS_UNRELIABLE;
		                break;    
		        }
				
			}
			break;
		default:
			MSE_ERR("msensor operate function no this parameter %d!\n", command);
			err = -1;
			break;
	}
	
	return err;
}

/*----------------------------------------------------------------------------*/
int ami304_orientation_operate(void* self, uint32_t command, void* buff_in, int size_in,
		void* buff_out, int size_out, int* actualout)
{
	int err = 0;
	int value, sample_delay, status=0;
	hwm_sensor_data* osensor_data=NULL;
	
	//MSE_FUN(f);
	switch (command)
	{
		case SENSOR_DELAY:
			if((buff_in == NULL) || (size_in < sizeof(int)))
			{
				MSE_ERR("Set delay parameter error!\n");
				err = -EINVAL;
			}
			else
			{
				value = *(int *)buff_in;
				if(value <= 20)
				{
					sample_delay = 20;
				}
				
				
				ami304mid_data.controldata[0] = sample_delay;  // Loop Delay
			}	
			break;

		case SENSOR_ENABLE:
			if((buff_in == NULL) || (size_in < sizeof(int)))
			{
				MSE_ERR("Enable sensor parameter error!\n");
				err = -EINVAL;
			}
			else
			{
				value = *(int *)buff_in;
				read_lock(&ami304mid_data.ctrllock);
				if(value == 1)
				{
					ami304mid_data.controldata[7] |= SENSOR_ORIENTATION;
					atomic_set(&o_flag, 1);
					atomic_set(&open_flag, 1);
				}
				else
				{
					ami304mid_data.controldata[7] &= ~SENSOR_ORIENTATION;
					atomic_set(&o_flag, 0);
					if(atomic_read(&m_flag) == 0)
					{
						atomic_set(&open_flag, 0);
					}
				}
				wake_up(&open_wq);
				read_unlock(&ami304mid_data.ctrllock);
				// Do nothing
			}
			break;

		case SENSOR_GET_DATA:
			if((buff_out == NULL) || (size_out< sizeof(hwm_sensor_data)))
			{
				MSE_ERR("get sensor data parameter error!\n");
				err = -EINVAL;
			}
			else
			{
				osensor_data = (hwm_sensor_data *)buff_out;
				read_lock(&ami304mid_data.datalock);
				osensor_data->values[0] = ami304mid_data.yaw;
				osensor_data->values[1] = ami304mid_data.pitch;
				osensor_data->values[2] = ami304mid_data.roll;
				status = ami304mid_data.mag_status;
				read_unlock(&ami304mid_data.datalock); 
				
				
				osensor_data->value_divide = ORIENTATION_ACCURACY_RATE;				
			}

			switch (status)
	        {
	            case 1: case 2:
	                osensor_data->status = SENSOR_STATUS_ACCURACY_HIGH;
	                break;
	            case 3:
	                osensor_data->status = SENSOR_STATUS_ACCURACY_MEDIUM;
	                break;
	            case 4:
	                osensor_data->status = SENSOR_STATUS_ACCURACY_LOW;
	                break;
	            default:        
	                osensor_data->status = SENSOR_STATUS_UNRELIABLE;
	                break;    
	        }
			break;
		default:
			MSE_ERR("gsensor operate function no this parameter %d!\n", command);
			err = -1;
			break;
	}
	
	return err;
}

/*----------------------------------------------------------------------------*/
#ifndef	CONFIG_HAS_EARLYSUSPEND
/*----------------------------------------------------------------------------*/
static int ami304_suspend(struct i2c_client *client, pm_message_t msg) 
{
	int err;
	struct ami304_i2c_data *obj = i2c_get_clientdata(client)
	MSE_FUN();    

	if(msg.event == PM_EVENT_SUSPEND)
	{   
		if(err = hwmsen_write_byte(client, AMI304_REG_POWER_CTL, 0x00))
		{
			MSE_ERR("write power control fail!!\n");
			return err;
		}
		
		ami304_power(obj->hw, 0);
	}
	return 0;
}
/*----------------------------------------------------------------------------*/
static int ami304_resume(struct i2c_client *client)
{
	int err;
	struct ami304_i2c_data *obj = i2c_get_clientdata(client)
	MSE_FUN();

	ami304_power(obj->hw, 1);
	if(err = AMI304_Init(AMI304_FORCE_MODE))
	{
		MSE_ERR("initialize client fail!!\n");
		return err;        
	}

	return 0;
}
/*----------------------------------------------------------------------------*/
#else /*CONFIG_HAS_EARLY_SUSPEND is defined*/
/*----------------------------------------------------------------------------*/
static void ami304_early_suspend(struct early_suspend *h) 
{
	struct ami304_i2c_data *obj = container_of(h, struct ami304_i2c_data, early_drv);   
	int err;
	MSE_FUN();    

	if(NULL == obj)
	{
		MSE_ERR("null pointer!!\n");
		return;
	}

	if((err = hwmsen_write_byte(obj->client, AMI304_REG_CTRL1, 0x00)))
	{
		MSE_ERR("write power control fail!!\n");
		return;
	}        
}
/*----------------------------------------------------------------------------*/
static void ami304_late_resume(struct early_suspend *h)
{
	struct ami304_i2c_data *obj = container_of(h, struct ami304_i2c_data, early_drv);         
	int err;
	MSE_FUN();

	if(NULL == obj)
	{
		MSE_ERR("null pointer!!\n");
		return;
	}

	ami304_power(obj->hw, 1);
	if((err = AMI304_Chipset_Init(AMI304_FORCE_MODE)))
	{
		MSE_ERR("initialize client fail!!\n");
		return;        
	}    
}
/*----------------------------------------------------------------------------*/
#endif /*CONFIG_HAS_EARLYSUSPEND*/
/*----------------------------------------------------------------------------*/
static int ami304_i2c_detect(struct i2c_client *client, int kind, struct i2c_board_info *info) 
{    
	strcpy(info->type, AMI304_DEV_NAME);
	return 0;
}

/*----------------------------------------------------------------------------*/
static int ami304_i2c_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	struct i2c_client *new_client;
	struct ami304_i2c_data *data;
	int err = 0;
	struct hwmsen_object sobj_m, sobj_o;

	if (!(data = kmalloc(sizeof(struct ami304_i2c_data), GFP_KERNEL)))
	{
		err = -ENOMEM;
		goto exit;
	}
	memset(data, 0, sizeof(struct ami304_i2c_data));

	data->hw = ami304_get_cust_mag_hw();
	if((err = hwmsen_get_convert(data->hw->direction, &data->cvt)))
	{
		MSE_ERR("invalid direction: %d\n", data->hw->direction);
		goto exit;
	}
	
	atomic_set(&data->layout, data->hw->direction);
	atomic_set(&data->trace, 0);

	init_waitqueue_head(&data_ready_wq);
	init_waitqueue_head(&open_wq);

	data->client = client;
	new_client = data->client;
	i2c_set_clientdata(new_client, data);
	
	ami304_i2c_client = new_client;	

	if((err = AMI304_Chipset_Init(AMI304_FORCE_MODE)))
	{
		goto exit_init_failed;
	}

	/* Register sysfs attribute */
	if((err = ami304_create_attr(&(ami304_init_info.platform_diver_addr->driver))))
	{
		MSE_ERR("create attribute err = %d\n", err);
		goto exit_sysfs_create_group_failed;
	}

	
	if((err = misc_register(&ami304_device)))
	{
		MSE_ERR("ami304_device register failed\n");
		goto exit_misc_device_register_failed;	}    

	sobj_m.self = data;
    sobj_m.polling = 1;
    sobj_m.sensor_operate = ami304_operate;
	if((err = hwmsen_attach(ID_MAGNETIC, &sobj_m)))
	{
		MSE_ERR("attach fail = %d\n", err);
		goto exit_kfree;
	}
	
	sobj_o.self = data;
    sobj_o.polling = 1;
    sobj_o.sensor_operate = ami304_orientation_operate;
	if((err = hwmsen_attach(ID_ORIENTATION, &sobj_o)))
	{
		MSE_ERR("attach fail = %d\n", err);
		goto exit_kfree;
	}
	
#if CONFIG_HAS_EARLYSUSPEND
	data->early_drv.level    = EARLY_SUSPEND_LEVEL_STOP_DRAWING - 2,
	data->early_drv.suspend  = ami304_early_suspend,
	data->early_drv.resume   = ami304_late_resume,    
	register_early_suspend(&data->early_drv);
#endif

	MSE_LOG("%s: OK\n", __func__);
    ami304_init_flag = 0;
	return 0;

	exit_sysfs_create_group_failed:   
	exit_init_failed:
	//i2c_detach_client(new_client);
	exit_misc_device_register_failed:
	exit_kfree:
	kfree(data);
	exit:
	MSE_ERR("%s: err = %d\n", __func__, err);
	ami304_init_flag = -1;
	return err;
}
/*----------------------------------------------------------------------------*/
static int ami304_i2c_remove(struct i2c_client *client)
{
	int err;	
	
	if((err = ami304_delete_attr(&(ami304_init_info.platform_diver_addr->driver))))
	{
		MSE_ERR("ami304_delete_attr fail: %d\n", err);
	}
	
	ami304_i2c_client = NULL;
	i2c_unregister_device(client);
	kfree(i2c_get_clientdata(client));	
	misc_deregister(&ami304_device);    
	return 0;
}
/*----------------------------------------------------------------------------*/
static int ami304_local_init(void) 
{
	struct mag_hw *hw = ami304_get_cust_mag_hw();

	ami304_power(hw, 1);    
	rwlock_init(&ami304mid_data.ctrllock);
	rwlock_init(&ami304mid_data.datalock);
	rwlock_init(&ami304_data.lock);
	memset(&ami304mid_data.controldata[0], 0, sizeof(int)*10);    
	ami304mid_data.controldata[0] =    20;  // Loop Delay
	ami304mid_data.controldata[1] =     0;  // Run   
	ami304mid_data.controldata[2] =     0;  // Disable Start-AccCali
	ami304mid_data.controldata[3] =     1;  // Enable Start-Cali
	ami304mid_data.controldata[4] =   350;  // MW-Timout
	ami304mid_data.controldata[5] =    10;  // MW-IIRStrength_M
	ami304mid_data.controldata[6] =    10;  // MW-IIRStrength_G   
	ami304mid_data.controldata[7] =     0;  // Active Sensors
	ami304mid_data.controldata[8] =     0;  // Wait for define
	ami304mid_data.controldata[9] =     0;  // Wait for define   
	atomic_set(&dev_open_count, 0);
	ami304_force[0] = hw->i2c_num;

	if(i2c_add_driver(&ami304_i2c_driver))
	{
		MSE_ERR("add driver error\n");
		return -1;
	} 
	if(-1 == ami304_init_flag)
	{
	   return -1;
	}
	return 0;
}

/*----------------------------------------------------------------------------*/
static int ami304_remove(void)
{
	struct mag_hw *hw = ami304_get_cust_mag_hw();

	MSE_FUN();    
	ami304_power(hw, 0);    
	atomic_set(&dev_open_count, 0);  
	i2c_del_driver(&ami304_i2c_driver);
	return 0;
}
/*----------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------*/

static int __init ami304_init(void)
{
	MSE_FUN();
	hwmsen_msensor_add(&ami304_init_info);
	return 0;    
}
/*----------------------------------------------------------------------------*/
static void __exit ami304_exit(void)
{
	MSE_FUN();
}
/*----------------------------------------------------------------------------*/
module_init(ami304_init);
module_exit(ami304_exit);
/*----------------------------------------------------------------------------*/
MODULE_AUTHOR("Kyle K.Y. Chen");
MODULE_DESCRIPTION("AMI304 MI-Sensor driver without DRDY");
MODULE_LICENSE("GPL");
MODULE_VERSION(DRIVER_VERSION);
