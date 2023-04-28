#include "usb_find.h"
#include <stdbool.h>
#include <dirent.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

#define LOG_TAG "RIL"
#include <ril_log.h>

#define USB_DEVICE_PATH "/sys/bus/usb/devices"
#define MAX_PATH 256


#define AIR720SDHG_USB_DEVICE_PRODUCT_ID 0x4e3d
#define AIR72XUX_USB_DEVICE_PRODUCT_ID 0x4e00
#define AIR72XCX_USB_DEVICE_PRODUCT_ID 0xa000
#define AIR780E_USB_DEVICE_PRODUCT_ID 0x0001

#define USBID_LEN 4

char* FindUsbDevice(int device_interface_id)
{
    DIR *pDir;
    int fd;
    char filename[MAX_PATH];
    bool usb_device_found = false;
    struct stat statbuf;
    struct dirent* ent = NULL;
	struct dirent ent_t;
    char dir[MAX_PATH] = USB_DEVICE_PATH;

    unsigned int vendor_id;
    unsigned int product_id;
    char idVendor[USBID_LEN + 1] = {0};
    char idProduct[USBID_LEN + 1] = {0};

    char usb_device_prefix[MAX_PATH] = {0};

    LOGE("FindUsbDevice");

    if ((pDir = opendir(dir)) == NULL)  {
        LOGE("Cannot open directory:%s/", dir);
        return 0;
    }

    LOGD("first stage start!" );

    /*first: find the entity contain the Suitable verdor and product id*/
    while ((ent = readdir(pDir)) != NULL)  {
        sprintf(filename, "%s/%s", dir, ent->d_name);
        lstat(filename, &statbuf);



        if (S_ISLNK(statbuf.st_mode))  {

            LOGD("first stage start found %s!", ent->d_name);

            sprintf(filename, "%s/%s/idVendor", dir, ent->d_name);
            fd = open(filename, O_RDONLY);
            if (fd > 0) {
                read(fd, idVendor, USBID_LEN);
                close(fd);

                sscanf(idVendor, "%x", &vendor_id);

                LOGD("first stage get vendor id %s!", idVendor);
                if(vendor_id != AIR720SDHG_USB_DEVICE_VENDOR_ID && vendor_id != AIR72XUX_USB_DEVICE_VENDOR_ID && vendor_id != AIR72XCX_USB_DEVICE_VENDOR_ID && vendor_id != AIR780E_USB_DEVICE_VENDOR_ID)
                    continue;
            }
            else
            {
                continue;
            }

            sprintf(filename, "%s/%s/idProduct", dir, ent->d_name);
            fd = open(filename, O_RDONLY);
            if (fd > 0) {
                read(fd, idProduct, USBID_LEN);
                close(fd);

                sscanf(idProduct, "%x", &product_id);

                LOGD("first stage get product id %s!", idProduct);

                if(product_id != AIR720SDHG_USB_DEVICE_PRODUCT_ID && product_id != AIR72XUX_USB_DEVICE_PRODUCT_ID && product_id != AIR72XCX_USB_DEVICE_PRODUCT_ID && product_id != AIR780E_USB_DEVICE_PRODUCT_ID)
                    continue;

                LOGD("first stage success %s!", ent->d_name);
                usb_device_found = true;
                break;
            }
            else
            {
                continue;
            }

        }
    }

	if(usb_device_found)
		memcpy(&ent_t,ent,sizeof(ent_t));



    closedir(pDir);

    if(!usb_device_found)
        return NULL;

    /*second: find the entity */
    usb_device_found = false;

    sprintf(usb_device_prefix, "%s:1.%d", ent_t.d_name, device_interface_id);

    pDir = opendir(dir);

    LOGD("usb_device_prefix lj2 %s", usb_device_prefix);

    while ((ent = readdir(pDir)) != NULL)  {

        sprintf(filename, "%s/%s", dir, ent->d_name);
        lstat(filename, &statbuf);

        if (S_ISLNK(statbuf.st_mode))  {

            LOGD("first second start found %s!", ent->d_name);

            if(!strcmp(ent->d_name, usb_device_prefix))
            {
                usb_device_found = true;
                break;
            }
        }
    }
	if(usb_device_found)
			memcpy(&ent_t,ent,sizeof(ent_t));


    closedir(pDir);


    if(!usb_device_found || ent_t.d_name[0] == '\0')
        return NULL;

    /*second: get device name */
    usb_device_found = false;
    strcat(dir, "/");
    strcat(dir, ent_t.d_name);
    // linux3.10.65 ttyxxx in tty folder
    strcat(dir, "/tty");

    LOGD("the file name %s", ent_t.d_name);

    pDir = opendir(dir);

    while ((ent = readdir(pDir)) != NULL)  {

        sprintf(filename, "%s/%s", dir, ent->d_name);

        if(!strncmp(ent->d_name, "ttyUSB", strlen("ttyUSB")) || !strncmp(ent->d_name, "ttyACM", strlen("ttyACM")))
        {
            usb_device_found = true;
            break;
        }
    }
    if(usb_device_found)
			memcpy(&ent_t,ent,sizeof(ent_t));

    closedir(pDir);


    if(usb_device_found)
        return &(ent_t.d_name);
    else
        return NULL;
}
