/*******************************************************
 Windows HID simplification

 Alan Ott
 Signal 11 Software

 8/22/2009

 Copyright 2009, All Rights Reserved.
 
 This software may be used by anyone for any reason so
 long as this copyright notice remains intact.
********************************************************/

#include <windows.h>
extern "C" {
	#include <setupapi.h>
	#include <hidsdi.h>
}
#include <stdio.h>
#include <stdlib.h>


#include "hidapi.h"

extern "C" {

struct Device {
		int valid;
		HANDLE device_handle;
		void *last_error;
};


#define MAX_DEVICES 64
static Device devices[MAX_DEVICES];
static int devices_initialized = 0;

static void register_error(Device *device, const char *op)
{
	LPTSTR msg=NULL;
	FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER |
		FORMAT_MESSAGE_FROM_SYSTEM |
		FORMAT_MESSAGE_IGNORE_INSERTS,
		NULL,
		GetLastError(),
		MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
		(LPTSTR)&msg, 0/*sz*/,
		NULL);
	
	printf("%s: ", op);
    wprintf(L"%s\n", msg);

	// Store the message off in the Device entry so that 
	// the hid_error() function can pick it up.
	LocalFree(device->last_error);
	device->last_error = msg;
}



int HID_API_EXPORT hid_open(unsigned short vendor_id, unsigned short product_id, wchar_t *serial_number)
{
  	int i;
	int handle = -1;
	Device *dev = NULL;
	BOOL res;

	// Windows objects for interacting with the driver.
	GUID InterfaceClassGuid = {0x4d1e55b2, 0xf16f, 0x11cf, 0x88, 0xcb, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30};
	SP_DEVINFO_DATA devinfo_data;
	SP_DEVICE_INTERFACE_DATA device_interface_data;
	SP_DEVICE_INTERFACE_DETAIL_DATA *device_interface_detail_data = NULL;
	HDEVINFO device_info_set = INVALID_HANDLE_VALUE;

	// Initialize the Windows objects.
	devinfo_data.cbSize = sizeof(SP_DEVINFO_DATA);
	device_interface_data.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

	// Initialize the Device array if it hasn't been done.
	if (!devices_initialized) {
		int i;
		for (i = 0; i < MAX_DEVICES; i++) {
			devices[i].valid = 0;
			devices[i].device_handle = INVALID_HANDLE_VALUE;
			devices[i].last_error = NULL;
		}
		devices_initialized = true;
	}

	// Find an available handle to use;
	for (i = 0; i < MAX_DEVICES; i++) {
		if (!devices[i].valid) {
			devices[i].valid = 1;
			handle = i;
			dev = &devices[i];
			break;
		}
	}

	if (handle < 0) {
		return -1;
	}

	// Get information for all the devices belonging to the HID class.
	device_info_set = SetupDiGetClassDevs(&InterfaceClassGuid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
	
	// Iterate over each device in the HID class, looking for the right one.
	int device_index = 0;
	for (;;) {
		DWORD required_size = 0;
		res = SetupDiEnumDeviceInterfaces(device_info_set,
			NULL,
			&InterfaceClassGuid,
			device_index,
			&device_interface_data);
		
		if (!res) {
			// A return of FALSE from this function means that
			// there are no more devices.
			break;
		}

		// Call with 0-sized detail size, and let the function
		// tell us how long the detail struct needs to be. The
		// size is put in &required_size.
		res = SetupDiGetDeviceInterfaceDetail(device_info_set,
			&device_interface_data,
			NULL,
			0,
			&required_size,
			NULL);

		// Allocate a long enough structure for device_interface_detail_data.
		device_interface_detail_data = (SP_DEVICE_INTERFACE_DETAIL_DATA*) malloc(required_size);
		device_interface_detail_data->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);

		// Get the detailed data for this device. The detail data gives us
		// the device path for this device, which is then passed into
		// CreateFile() to get a handle to the device.
		res = SetupDiGetDeviceInterfaceDetail(device_info_set,
			&device_interface_data,
			device_interface_detail_data,
			required_size,
			NULL,
			NULL);

		if (!res) {
			printf("Unable to call SetupDiGetDeviceInterfaceDetail");
			free(device_interface_detail_data);
			// Continue to the next device.
			goto cont;
		}

		wprintf(L"HandleName: %s\n", device_interface_detail_data->DevicePath);

		// Open a handle to the device
		HANDLE write_handle = CreateFile(device_interface_detail_data->DevicePath,
			GENERIC_WRITE |GENERIC_READ,
			0x0, /*share mode*/
			NULL,
			OPEN_EXISTING,
			0,//FILE_ATTRIBUTE_NORMAL,
			0);

		// We no longer need the detail data. It can be freed
		free(device_interface_detail_data);

		// Check validity of write_handle.
		if (write_handle == INVALID_HANDLE_VALUE) {
			// Unable to open the device.
			register_error(dev, "CreateFile");
			CloseHandle(write_handle);
			goto cont;
		}		


		// Get the Vendor ID and Product ID for this device.
		HIDD_ATTRIBUTES attrib;
		attrib.Size = sizeof(HIDD_ATTRIBUTES);
		HidD_GetAttributes(write_handle, &attrib);
		wprintf(L"Product/Vendor: %x %x\n", attrib.ProductID, attrib.VendorID);

		// Check if this is our device.
		if (attrib.VendorID == vendor_id &&
		    attrib.ProductID == product_id) {

			// Check the serial number (if one was provided)
			if (serial_number) {
				wchar_t ser[256];
				memset(ser, 0, sizeof(ser));

				res = HidD_GetSerialNumberString(write_handle, ser, sizeof(ser));
				if (!res) {
					register_error(dev, "HidD_GetSerialNumberString");
					// Can't get a serial and one was specified, continue.
					goto cont;
				}
				else {
					wprintf(L"Ser: %s\n", ser);
					// Compare the serial number
					if (::wcscmp(ser, serial_number) == 0) {
						// Serial is good. This is our device.
						dev->device_handle = write_handle;
						break;
					}
				}
			}
			else {
				// This is our device. Break out of this loop and return it.
				dev->device_handle = write_handle;
				break;
			}
		}
		else {
			CloseHandle(write_handle);
			goto cont;
		}

		cont:
		device_index++;
	}

	// Close the device information handle.
	CloseHandle(device_info_set);

	// If we have a good handle, return it.
	if (dev->device_handle != INVALID_HANDLE_VALUE) {
		return handle;
	}
	else {
		// Unable to open any devices.
		dev->valid = 0;
		return -1;
	}

}

int HID_API_EXPORT hid_write(int device, const unsigned char *data, size_t length)
{
	Device *dev = NULL;
	DWORD bytes_read;
	BOOL res;

	// Get the handle 
	if (device < 0 || device >= MAX_DEVICES)
		return -1;
	if (devices[device].valid == 0)
		return -1;
	dev = &devices[device];

	res = WriteFile(dev->device_handle, data, length, &bytes_read, NULL);

	if (!res) {
		register_error(dev, "WriteFile");
		return -1;
	}

	return bytes_read;
}


int HID_API_EXPORT hid_read(int device, unsigned char *data, size_t length)
{
	Device *dev = NULL;
	DWORD bytes_read;
	BOOL res;

	// Get the handle 
	if (device < 0 || device >= MAX_DEVICES)
		return -1;
	if (devices[device].valid == 0)
		return -1;
	dev = &devices[device];

	res = ReadFile(dev->device_handle, data, length, &bytes_read, NULL);

	if (!res) {
		register_error(dev, "ReadFile");
		return -1;
	}
	
	return bytes_read;
}

void HID_API_EXPORT hid_close(int device)
{
	Device *dev = NULL;

	// Get the handle 
	if (device < 0 || device >= MAX_DEVICES)
		return;
	if (devices[device].valid == 0)
		return;
	dev = &devices[device];

	CloseHandle(dev->device_handle);
	dev->device_handle = INVALID_HANDLE_VALUE;
	dev->valid = 0;
}

int HID_API_EXPORT_CALL hid_get_manufacturer_string(int device, wchar_t *string, size_t maxlen)
{
	Device *dev = NULL;
	BOOL res;

	// Get the handle 
	if (device < 0 || device >= MAX_DEVICES)
		return -1;
	if (devices[device].valid == 0)
		return -1;
	dev = &devices[device];


	res = HidD_GetManufacturerString(dev->device_handle, string, 2 * maxlen);
	if (!res) {
		register_error(dev, "HidD_GetManufacturerString");
		return -1;
	}

	return 0;
}

int HID_API_EXPORT_CALL hid_get_product_string(int device, wchar_t *string, size_t maxlen)
{
	Device *dev = NULL;
	BOOL res;

	// Get the handle 
	if (device < 0 || device >= MAX_DEVICES)
		return -1;
	if (devices[device].valid == 0)
		return -1;
	dev = &devices[device];


	res = HidD_GetProductString(dev->device_handle, string, 2 * maxlen);
	if (!res) {
		register_error(dev, "HidD_GetProductString");
		return -1;
	}

	return 0;
}

int HID_API_EXPORT_CALL hid_get_serial_number_string(int device, wchar_t *string, size_t maxlen)
{
	Device *dev = NULL;
	BOOL res;

	// Get the handle 
	if (device < 0 || device >= MAX_DEVICES)
		return -1;
	if (devices[device].valid == 0)
		return -1;
	dev = &devices[device];


	res = HidD_GetSerialNumberString(dev->device_handle, string, 2 * maxlen);
	if (!res) {
		register_error(dev, "HidD_GetSerialNumberString");
		return -1;
	}

	return 0;
}

int HID_API_EXPORT_CALL hid_get_indexed_string(int device, int string_index, wchar_t *string, size_t maxlen)
{
	Device *dev = NULL;
	BOOL res;

	// Get the handle 
	if (device < 0 || device >= MAX_DEVICES)
		return -1;
	if (devices[device].valid == 0)
		return -1;
	dev = &devices[device];


	res = HidD_GetIndexedString(dev->device_handle, string_index, string, 2 * maxlen);
	if (!res) {
		register_error(dev, "HidD_GetIndexedString");
		return -1;
	}

	return 0;
}


HID_API_EXPORT const wchar_t * HID_API_CALL  hid_error(int device)
{
	Device *dev = NULL;

	// Get the handle 
	if (device < 0 || device >= MAX_DEVICES)
		return NULL;
	if (devices[device].valid == 0)
		return NULL;
	dev = &devices[device];

	return (wchar_t*) dev->last_error;
}


//#define PICPGM
//#define S11
#define P32
#ifdef S11 
  unsigned short VendorID = 0xa0a0;
	unsigned short ProductID = 0x0001;
#endif

#ifdef P32
  unsigned short VendorID = 0x04d8;
	unsigned short ProductID = 0x3f;
#endif


#ifdef PICPGM
  unsigned short VendorID = 0x04d8;
  unsigned short ProductID = 0x0033;
#endif


#if 0
int __cdecl main(int argc, char* argv[])
{
	int res;
	unsigned char buf[65];

	UNREFERENCED_PARAMETER(argc);
	UNREFERENCED_PARAMETER(argv);

	// Set up the command buffer.
	memset(buf,0x00,sizeof(buf));
	buf[0] = 0;
	buf[1] = 0x81;
	

	// Open the device.
	int handle = open(VendorID, ProductID, L"12345");
	if (handle < 0)
		printf("unable to open device\n");


	// Toggle LED (cmd 0x80)
	buf[1] = 0x80;
	res = write(handle, buf, 65);
	if (res < 0)
		printf("Unable to write()\n");

	// Request state (cmd 0x81)
	buf[1] = 0x81;
	write(handle, buf, 65);
	if (res < 0)
		printf("Unable to write() (2)\n");

	// Read requested state
	read(handle, buf, 65);
	if (res < 0)
		printf("Unable to read()\n");

	// Print out the returned buffer.
	for (int i = 0; i < 4; i++)
		printf("buf[%d]: %d\n", i, buf[i]);

	return 0;
}
#endif

} // extern "C"
