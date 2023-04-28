
#include <inttypes.h>
#include <stdint.h>
#include <stdbool.h>
#include <malloc.h>

typedef uint8_t UINT8;
typedef uint8_t INT8;
typedef uint8_t Int8;
typedef uint16_t UINT16;
typedef uint16_t INT16;
typedef uint16_t Int16;
typedef uint8_t Boolean;
typedef uint16_t SimEfId;

#define COMBINE_TWO_BYTES(high,low)     ((((UINT16)high) << 8) | (low))


typedef enum SimEfStatusTag
{
    SIM_EFS_OK,                 /* The data was decoded successfully */
    SIM_EFS_INVALID,            /* The data does not contain EF status */
    SIM_EFS_UNKNOWN_STRUCTURE,  /* The structure is not T, LF or C */
    SIM_EFS_UNEXPECTED_TYPE,    /* Type EF was expected but not returned */
    SIM_EFS_INVALID_STAT_LENGTH /* The status data has an invalid length */
}
SimEfStatus;

typedef enum SimAccessConditionTag
{
    SIM_ACCESS_ALWAYS,
    SIM_ACCESS_CHV1,
    SIM_ACCESS_CHV2,
    SIM_ACCESS_RFU,
    SIM_ACCESS_ADMIN_4,
    SIM_ACCESS_ADMIN_5,
    SIM_ACCESS_ADMIN_6,
    SIM_ACCESS_ADMIN_7,
    SIM_ACCESS_ADMIN_8,
    SIM_ACCESS_ADMIN_9,
    SIM_ACCESS_ADMIN_A,
    SIM_ACCESS_ADMIN_B,
    SIM_ACCESS_ADMIN_C,
    SIM_ACCESS_ADMIN_D,
    SIM_ACCESS_ADMIN_E,
    SIM_ACCESS_NEVER
}
SimAccessCondition;

typedef enum SimEfStructureTag
{
    SIM_EFSTRUCT_T              =   0,
    SIM_EFSTRUCT_LF             =   1,
    SIM_EFSTRUCT_C              =   3,
    SIM_EFSTRUCT_UNKNOWN        =   0xff
}
SimEfStructure;

typedef enum SimFileTypeTag
{
    SIM_FILETYPE_MF             =   1,
    SIM_FILETYPE_DF             =   2,
    SIM_FILETYPE_EF             =   4,
    SIM_FILETYPE_UNKNOWN        =   0xff
}
SimFileType;

typedef struct SimAccessDataTag
{
    SimAccessCondition    update;
    SimAccessCondition    readSeek;
    SimAccessCondition    increase;
    SimAccessCondition    invalidate;
    SimAccessCondition    rehabilitate;
}
SimAccessData;


typedef struct SimEfStatusDataTag
{
    SimEfStatus             status;
    SimEfId                 fileId;

    Boolean                 fileIsValidated;

    Int16                   fileSize;
    Int8                    recordLength;
    Int8                    numberOfRecords;
    SimEfStructure          efStructure;

    SimAccessData           accessData;

    SimFileType             fileType;
    Boolean                 increaseAllowed;
    Boolean                  readWriteWhenInvalid;
}
SimEfStatusData;



char vgIntToHexChar(UINT8 i)
{
    if(i <= 9)
        return (char)(i + '0');
    else if(10 <= i && i <= 15)
        return (char)(i - 10 + 'A');

    return -1;
}


UINT8 vgHexCharToInt(char ch)
{
    if('0' <= ch && ch <= '9')
        return((UINT8)ch - 48);
    else if('a' <= ch && ch <= 'f')
        return((UINT8)ch - 87);
    else if('A' <= ch && ch <= 'F')
        return((UINT8)ch - 55);

    return -1;
}


void vgHexDataToString(char* buf, char* data, UINT16 length)
{
    int i = 0,j = 0;
    for(i = 0;i < length;i++)
    {
        buf[j++] = vgIntToHexChar((data[i] >> 4)&0x0f);
        buf[j++] = vgIntToHexChar(data[i]&0x0f);
    }
}


void vgStringToHexData(char* buf, UINT16 length, char* data)
{
    int i = 0,j = 0;
    
    while(j < length)
    {
        data[i] = vgHexCharToInt(buf[j]) << 4 | vgHexCharToInt(buf[j+1]);
        i ++;
        j += 2;
    }

}





INT8 UsimDecodeDataLength (INT8 *dataLgth,  INT8 *data)
{
    INT8    lengthSize = 0;

    if ( (*data) <= 0x7F)
    {
        *dataLgth = *data;
        lengthSize = 1;
    }
    else
    {
        *dataLgth = 0;
        if ( (*data) == 0x81)
        {
            if ( *(data+1) >= 0x80 )
            {
                *dataLgth = *(data+1);
            }

            lengthSize = 2;
        }
    }

    // returns num of bytes the length is stored on (zero, if data in error) 
    return (lengthSize);

}


/*
================================================================================
Function   : UsimDecEfFileDescriptor
--------------------------------------------------------------------------------

  Scope      : This function decodes the File descriptor TVL data object (TS 102 221 section 11.1.1.4.3) inside the FCP template 

  Parameters :
   INT8                   *offset          :- Offset in respData                          
   UINT8                *respData        :- A pointer to a structure which holds the raw data from the SIM       
  CFW_UsimEfStatus* pSimEfStatus: EF file status stucture 
  Return     :
  This function returns SimUiccDecodeStatus (indicates whether the decoding was successful)  
================================================================================
*/
void UsimDecEfFileDescriptor (UINT8  *offset,  UINT8 *respData, SimEfStatusData* pSimEfStatus)
{
    INT8 fileDescLength;
    INT8 *ptr = (INT8*)&respData[*offset];

    // structure of file
    fileDescLength = ptr[1];


    switch(ptr[2] & 0x07)

    {
    case 1:
        pSimEfStatus->efStructure = SIM_EFSTRUCT_T;
        break;

    case 2:
        pSimEfStatus->efStructure = SIM_EFSTRUCT_LF;
        break;

    case 6:
        pSimEfStatus->efStructure = SIM_EFSTRUCT_C;
        break;
        
    }

    switch((ptr[2] & 0x38) >> 3)
    {
    case 0:
    case 1:
        pSimEfStatus->fileType = SIM_FILETYPE_EF;
        break;

    case 7:
        pSimEfStatus->fileType = SIM_FILETYPE_DF;
        break;
    }

    if (fileDescLength == 5)
    {
       pSimEfStatus->recordLength = COMBINE_TWO_BYTES(ptr[4], ptr[5]) ; 
       pSimEfStatus->numberOfRecords = ptr[6];
    }
    else if(fileDescLength == 2)
    {
       pSimEfStatus->recordLength = 0;  
       pSimEfStatus->numberOfRecords = 0;
    }
    else
    {
        pSimEfStatus->recordLength = 0;  
        pSimEfStatus->numberOfRecords = 0;
    }

    *offset += (2 + ptr[1]);

}


INT16 UsimDecFileSize (UINT8  *offset,  UINT8 *respData)
{
    INT16 length;
    UINT8 *ptr = &respData[*offset];    // pointer to TLV object 

    if (ptr[1] == 2)   //If the file is an EF, length should always equal 2
    {
        length = (INT16)(COMBINE_TWO_BYTES(ptr[2], ptr[3]));
    }
    else
    {
        length = 0;
    }

    *offset += (2 + ptr[1]);

    return (length);
}


void UsimDecEfFileId(UINT8  *offset,  UINT8 *respData, SimEfStatusData* pSimEfStatus)
{
    INT8 fileDescLength;
    INT8 *ptr = (INT8*)&respData[*offset];
 
    // structure of file
   fileDescLength = ptr[1];

   if (fileDescLength == 2)
   {
       pSimEfStatus->fileId =COMBINE_TWO_BYTES(ptr[2], ptr[3]) ; 
 
   }
   else
   {
      pSimEfStatus->fileId = 0;
   }

      *offset += (2 + ptr[1]);

}



void UsimDecodeEfStatusData (char *dataBlock, SimEfStatusData* efStatus)
{
    INT8 fcpLength;                   /* can be one or two bytes */
    UINT8 offset = 0;
    INT8 fcpOffset;
    INT8 TLVLength, TLVOffset;
	
    // Decode Fcp template  (see TS 102 221 section 11.1.1.3.1)
    if ( dataBlock[offset++] == 0x62)          // Fcp template tag 
    {

        fcpOffset = UsimDecodeDataLength (&fcpLength, (INT8 *)&dataBlock[offset]);
        
        // fcp offset can be 1 or 2
        if (fcpOffset != 0)
        {
            offset += fcpOffset;

            while ( offset <= (fcpLength + fcpOffset))
            {
                switch (dataBlock[offset])
                {
                // file descriptor   -mandatory
                case  0x82:
                    UsimDecEfFileDescriptor (&offset, (UINT8*)dataBlock, efStatus);
                    break;
               case  0x83:
					UsimDecEfFileId (&offset, (UINT8*)dataBlock, efStatus);	
				    break;
                /*
                //   File identifier   -mandatory
                case  UICC_FILE_ID_TAG:
                   decStatus = sim_UiccDecEfId (&offset, RespData);
                   break;

                //      Proprietary info   (TS 102 221 section 11.1.1.4.6)  optional for ef
                case  UICC_PROPRIETARY_INFO_TAG:
                decStatus = sim_UiccDecProprietaryInfo (&offset, RespData);
                break;

                //      Life Cycle status Integer - mandatory
                case  UICC_LIFE_CYCLE_STATUS_TAG:
                decStatus = sim_UiccDecLifeCycleStatus (&offset, RespData);
                break;

                //   security attributes, compact format (TS 102 221 section 11.1.1.4.7.1)
                case  UICC_COMPACT_TAG:
                 pSimCtx->CurrentEFStatus.format = USIM_COMPACT_FORMAT;
                sim_UiccDecEfCompactFormat (&offset, RespData);
                break;

                //      security attributes - (TS 102 221 section 11.1.1.4.7) - mandatory
                // Note: if the ME is not able to decode the security attributes,because they are malformed for example, it should ignore them
                case  UICC_EXPANDED_TAG:
                pSimCtx->CurrentEFStatus.format = USIM_EXPANDED_FORMAT;
                sim_UiccDecEfExpandedFormat (&offset, RespData);
                break;

                //     security attributes - (TS 102 221 section 11.1.1.4.7) - mandatory
                case  UICC_REF_TO_EXPANDED_TAG:
                pSimCtx->CurrentEFStatus.format = USIM_REF_TO_EXPANDED_FORMAT;
                // Note: if the ME is not able to decode the security attributes, because they are malformed for example, it should ignore them
                sim_UiccDecRefToExpandedFormat (&offset, RespData, &pSimCtx->CurrentEFStatus.secAttributes.refToExpandedFormat);
                break;
                */

                case 0x8A:
                {
                    int len;

                    offset++;
                    len = dataBlock[offset];

                    offset++;
                    
                    if(len == 1) 
                    {
                        efStatus->fileIsValidated = ((dataBlock[offset] & 0xFD) != 0x04);
                    }
                    else 
                    {
                        efStatus->fileIsValidated = true;
                    }

                    offset += len;
                    break;
                }
                    

#if 0
				case  0x8c:
				case  0xab:
				case  0x8b:
					// security attributes,  (TS 102 221 section 11.1.1.4.7)
					Usim_DecDirSecurityAttributes (&offset, RespData);
					break;
#endif
                //    Decode File size object (TS 102 221 section 11.1.1.4.1) - Mandatory
                case 0x80:
                    efStatus->fileSize = UsimDecFileSize (&offset, (UINT8*)dataBlock);
                    break;
                /*					
                //      Decode Total File size object and Short File Identifier (TS 102 221 section 11.1.1.4.2/8) - Optional
                case  UICC_TOTAL_FILE_SIZE_TAG:
                 sim_UiccDecTotalEfFileSize (&offset, RespData);
                 break;

                //    Short File Identifier
                case  UICC_SFI_TAG:
                 decStatus = sim_UiccDecSfi (&offset, RespData);
                 break;
                */					 
                default:
                    // Just ignore TLV object we don't know
                    offset++;
                    TLVOffset = UsimDecodeDataLength (&TLVLength, (INT8 *)&dataBlock[offset]);  // length of tvl object can be coded on1 or 2 bytes 
                    offset += TLVOffset + TLVLength;
                    break;
                }

            }
        }
    }
}

void To2gSimStatus(SimEfStatusData* status, char* simStatusData)
{
	/*RFU*/
	simStatusData[0] = simStatusData[1]= 0;

	simStatusData[2] = (status->fileSize >> 8);
	simStatusData[3] = (status->fileSize & 0xff);

	simStatusData[4] = (status->fileId >> 8);
	simStatusData[5] = (status->fileId & 0xff);

	simStatusData[6] = (status->fileType);

	simStatusData[7] = 0;

	/*Access conditions*/
	simStatusData[8] = 0;
	simStatusData[9] = 0;
	simStatusData[10] = 0;

	/*File status */
	simStatusData[11] = 0;

	/*Length of the following data*/
	simStatusData[12]= 2;

	simStatusData[13] = (status->efStructure);

	if (status->efStructure == SIM_EFSTRUCT_LF ||
        status->efStructure == SIM_EFSTRUCT_C)
	{
		simStatusData[14] = status->recordLength;
	}
	else
	{
		simStatusData[14] = 0;
	}
}


void convertUSimFcp(char* fcp, int len, char* out)
{
    SimEfStatusData efStatus;
    
    char *data = (char *)malloc(len / 2 + 1);
    char out_temp[15];

    vgStringToHexData(fcp, len, data);

    UsimDecodeEfStatusData(data, &efStatus);
    To2gSimStatus(&efStatus, out_temp);

    vgHexDataToString(out, out_temp,  15);
    
    free(data);
    out[30]= '\0';
}