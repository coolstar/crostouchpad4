#if !defined(_CYAPA_COMMON_H_)
#define _CYAPA_COMMON_H_

//
//These are the device attributes returned by vmulti in response
// to IOCTL_HID_GET_DEVICE_ATTRIBUTES.
//

#define CYAPA_PID              0xBACC
#define CYAPA_VID              0x00FF
#define CYAPA_VERSION          0x0001

//
// These are the report ids
//

#define REPORTID_MTOUCH         0x01
#define REPORTID_FEATURE        0x02
#define REPORTID_MOUSE			0x03
#define REPORTID_PTPHQA			0x04

//
// Multitouch specific report information
//

#define MULTI_CONFIDENCE_BIT   1
#define MULTI_TIPSWITCH_BIT    2

#define MULTI_MIN_COORDINATE   0x0000
#define MULTI_MAX_COORDINATE   0x7FFF

#define MULTI_MAX_COUNT        5

#pragma pack(1)
typedef struct
{

	BYTE	  Status;

	BYTE	  ContactID;

	USHORT    XValue;

	USHORT    YValue;

	USHORT	  Pressure;
}
TOUCH, *PTOUCH;

typedef struct _CYAPA_MULTITOUCH_REPORT
{

	BYTE      ReportID;

	TOUCH      Touch[5];

	USHORT	  ScanTime;

	BYTE	  ContactCount;

	BYTE	  IsDepressed;
} CyapaMultiTouchReport;
#pragma pack()

//
// Feature report infomation
//

#define DEVICE_MODE_MOUSE        0x00
#define DEVICE_MODE_SINGLE_INPUT 0x01
#define DEVICE_MODE_MULTI_INPUT  0x02

#pragma pack(1)
typedef struct _CYAPA_FEATURE_REPORT
{

	BYTE      ReportID;

	BYTE      DeviceMode;

	BYTE      DeviceIdentifier;

} CyapaFeatureReport;

typedef struct _CYAPA_MAXCOUNT_REPORT
{

	BYTE         ReportID;

	BYTE		 MaximumCount;

	BYTE		 PadType;

} CyapaMaxCountReport;
#pragma pack()

#endif
