/*
 * DiagSaver application
 *
 * Marvell Diag log saver on Linux - 1.0.0.0
 *
 * Copyright (C) 1995-2014 Marvell.Co.,LTD
 */

#include <stdio.h>  
#include <stdlib.h> 
#include <string.h>   
#include <unistd.h>   
#include <fcntl.h>   
#include <errno.h>   
#include <termios.h> 
#include <time.h>  
#include <signal.h>
#include <sys/time.h> 
#include <sys/stat.h>
#include <usb_find.h>
#include<pthread.h>

#define LOG_TAG "RIL_L_DS"
#include <ril_log.h>



#define	_FILE_SIZE		200  //2MB

char filename[256] = {0};
FILE *fp = NULL;
unsigned int g_uiFileIndex = 0x0;

struct CSDLFileHeader
{
	unsigned int    dwHeaderVersion;//0x0
	unsigned int    dwDataFormat;//0x1
	unsigned int    dwAPVersion;
	unsigned int    dwCPVersion;
	unsigned int    dwSequenceNum;//文件序号，从0开始递增
	unsigned int    dwTime;//Total seconds from 1970.1.1 0:0:0
	unsigned int    dwCheckSum;//0x0
};


int openport(char *pszDeviceName)   
 {
	 int fd = open( pszDeviceName, O_RDWR|O_SYNC|O_NOCTTY|O_NDELAY ); 
	 if (-1 == fd) 
	 {    
		  perror("Can't Open Diag device!");
		  return -1;  
	 } 
	 else 
	 {
		 ////tLOGD (_T("\nOpened Diag DeviceName : %s\n"),pszDeviceName);
	  	return fd;
	 }
 }   
    
int setport(int fd, int baud,int databits,int stopbits,int parity)
{
	 int baudrate;
	 struct   termios   newtio;   
	 switch(baud)
	 {
		 case 300:
			  baudrate=B300;
			  break;
		 case 600:
			  baudrate=B600;
			  break;
		 case 1200:
			  baudrate=B1200;
			  break;
		 case 2400:
			  baudrate=B2400;
			  break;
		 case 4800:
			  baudrate=B4800;
			  break;
		 case 9600:
			  baudrate=B9600;
			  break;
		 case 19200:
			  baudrate=B19200;
			  break;
		 case 38400:
			  baudrate=B38400;
			  break;
		 default :
			  baudrate=B9600;  
			  break;
	 }
	 tcgetattr(fd,&newtio);     
	 bzero(&newtio,sizeof(newtio));   
	   //setting   c_cflag 
	 newtio.c_cflag   &=~CSIZE;     
	 switch (databits) 
	 {   
		 case 7:  
		  	newtio.c_cflag |= CS7; 
		  	break;
		 case 8:     
			  newtio.c_cflag |= CS8; 
			  break;   
		 default:    
			  newtio.c_cflag |= CS8;
			  break;    
	 }
	 switch (parity) //设置校验
	 {   
		 case 'n':
		 case 'N':    
			  newtio.c_cflag &= ~PARENB;   /* Clear parity enable */
			  newtio.c_iflag &= ~INPCK;     /* Enable parity checking */ 
			  break;  
		 case 'o':   
		 case 'O':     
			  newtio.c_cflag |= (PARODD | PARENB);
			  newtio.c_iflag |= INPCK;             /* Disnable parity checking */ 
			  break;  
		 case 'e':  
		 case 'E':   
			newtio.c_cflag |= PARENB;     /* Enable parity */    
			newtio.c_cflag &= ~PARODD;      
			newtio.c_iflag |= INPCK;       /* Disnable parity checking */
			break;
		 case 'S': 
		 case 's':  /*as no parity*/   
			newtio.c_cflag &= ~PARENB;
			newtio.c_cflag &= ~CSTOPB;
			break;  
		 default:   
			  newtio.c_cflag &= ~PARENB;   /* Clear parity enable */
			  newtio.c_iflag &= ~INPCK;     /* Enable parity checking */ 
			  break;   
	 } 
	 switch (stopbits)//设置停止位
	 {   
		 case 1:    
			  newtio.c_cflag &= ~CSTOPB;  //1
			  break;  
		 case 2:    
			  newtio.c_cflag |= CSTOPB;  //2
			  break;
		 default:  
			  newtio.c_cflag &= ~CSTOPB;  
			  break;  
	 } 
	 newtio.c_cc[VTIME] = 0;    
	 newtio.c_cc[VMIN] = 0; 
	 newtio.c_cflag   |=   (CLOCAL|CREAD);
	 //newtio.c_oflag|=OPOST; 
	 newtio.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG); //Input, RawData mode
	 newtio.c_oflag &= ~OPOST; //Output
	 newtio.c_iflag   &=~(IXON|IXOFF|IXANY);  
	 cfsetispeed(&newtio,baudrate);   
	 cfsetospeed(&newtio,baudrate);   
	 tcflush(fd,   TCIFLUSH); 
	 if (tcsetattr(fd,TCSANOW,&newtio) != 0)   
	 { 
		  perror("SetupSerial 3");  
		  return -1;  
	 }  
	 return 0;
}

int writeport(int fd, unsigned char *buf,int len)
{
	int i;
	write(fd,buf,len);
	LOGD("Send CMD to UE: ");
	for(i=0; i<16; i++)
		LOGD("0x%2X ", buf[i]);
	LOGD("\n");
return 0 ;
}

void clearport(int fd) 
{
 	tcflush(fd,TCIOFLUSH);
}

int readport(int fd, unsigned char *buf, int maxwaittime)
{
	 int len=0;
	 int rc;
	 int i;
	 
	 struct timeval tv;
	 fd_set readfd;
	 tv.tv_sec=maxwaittime/1000;    //SECOND
	 tv.tv_usec=maxwaittime%1000*1000;  //USECOND
	 FD_ZERO(&readfd);
	 FD_SET(fd,&readfd);
	 rc=select(fd+1,&readfd,NULL,NULL,&tv);
	 if(rc>0)
	 {
		rc=read(fd,buf,4096);
		LOGD("recv:%d\n",rc);
		return rc;
	 }
	 else
	 {
		return -1;
	 }
}

int savelog(unsigned char *buf, int len)
{
	int i;
	time_t ltime;
	struct tm *currtime;
	char openmode[4] = {0};
	struct stat fileinfo;

	struct CSDLFileHeader SDLHeader;
	char *pHeader = (char *)&SDLHeader;

	// init the SDL file header
	SDLHeader.dwHeaderVersion = 0x0;
	SDLHeader.dwDataFormat = 0x1;//0x1
	SDLHeader.dwAPVersion = 0x0;
	SDLHeader.dwCPVersion = 0x0;
	SDLHeader.dwTime = 0x0;//Total seconds from 1970.1.1 0:0:0
	SDLHeader.dwCheckSum = 0x0;//0x0

	if ( 0 == strlen(filename)  )
	{
		time(&ltime);

		SDLHeader.dwTime = (long)ltime;
		SDLHeader.dwSequenceNum = g_uiFileIndex;//文件序号，从0开始递增

		currtime = localtime(&ltime);
		sprintf(filename, "/system/%d_%d_%d_%d_%d_%d.sdl", (currtime->tm_year+1900), (currtime->tm_mon+1), currtime->tm_mday, currtime->tm_hour, currtime->tm_min, currtime->tm_sec);
		sprintf(openmode, "wb");
		fp = fopen(filename, openmode);
		// Write the file header.
		for(i=0; i<28; i++)
			fputc(pHeader[i], fp);
	}
	else
	{
		stat(filename, &fileinfo);
		if( fileinfo.st_size > _FILE_SIZE*1024*1024-len )
		{
			fclose(fp);
			g_uiFileIndex++;	// Increse the file index
			time(&ltime);

			SDLHeader.dwTime = (long)ltime;
			SDLHeader.dwSequenceNum = g_uiFileIndex;//文件序号，从0开始递增

			currtime = localtime(&ltime);
			sprintf(filename, "/system/%d_%d_%d_%d_%d_%d.sdl", currtime->tm_year+1900, currtime->tm_mon+1, currtime->tm_mday, currtime->tm_hour, currtime->tm_min, currtime->tm_sec);
			sprintf(openmode, "wb");
			fp = fopen(filename, openmode);
		}
		else
		{
			sprintf(openmode, "ab");
		}
	}
	
	// Write the data to current file.
	for(i=0; i<len; i++)
		fputc(buf[i], fp);

return 0;
	
}

void ctrl_c_process(int signo)
{
	fclose(fp);
	LOGD("\nExiting DiagSaver...\n");
	exit(1);
}


void* diagsaver_main(void* param)
{   
	 int   fd,rc,i,ret,readlen;  
	 readlen = 0;
	 unsigned char rbuf[8192] = {0}; 
	 unsigned char ACATReady[16]={0x10, 0x00, 0x00, 0x00, 0x00, 0x04, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};	// ACAT Ready command
	 unsigned char GetAPDBVer[16]={0x10, 0x00, 0x00, 0x00, 0x80, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};	// Get AP DB Ver command
	 unsigned char GetCPDBVer[16]={0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};	// Get CP DB Ver command
	char dev[256];	 
	//char *dev ="/dev/ttyACM1";    //Set the ZTE Diag device path
    int last_write = -1234;
    
	// Capture the Ctrl+C
	struct sigaction act;
	act.sa_handler = ctrl_c_process;
	act.sa_flags = 0;

#if 0

	if( sigaction(SIGINT, &act, NULL) < 0 )
	{
		LOGD("Install Signal Action Error: %s \n", strerror(errno));
		return ;
	}


	LOGD("Please enter the device path to capture the log. [Example: /dev/ttyACM1]:\n");
	scanf("%s", dev);
#endif

	sprintf(dev, "/dev/%s",FindUsbDevice(AIRM2M_USB_DEVICE_DIAG_INTERFACE_ID));
	//Open Diag device
	 fd  =  openport(dev);
	 
	 if(fd>0)
	 {	
		LOGD("Opened the device %s\n", dev);
		if ( strstr( dev, "MDiagUSB") == NULL )
		{
			ret=setport(fd,115200,8,1,'n');  //Set serial port
			if(ret<0)
			{
			  LOGD("Can't Set Serial Port!\n");
			  return NULL;
			}
			else
			{
				LOGD("Set Serial Port Done.\n");
			}
		}
	 }
	 else
	 {
		  LOGD("Can't Open Serial Port!\n");
		  return NULL;
	 }
	 
	//Send the ACAT Ready to UE
	 writeport(fd,ACATReady,16);
	 usleep(200);
	 writeport(fd,GetAPDBVer,16);
	 usleep(300);
	 writeport(fd,GetCPDBVer,16);
	 
	 LOGD("Sent CMD, receiving data. Press Ctrl+C to exit.\n");

     while(1)
	 {
		rc=readport(fd,rbuf,500);   //Read data, timeout is 500 ms
		if(rc>0)
		{		   
			//for(i=0;i<rc;i++)
			//	LOGD("%02x ",rbuf[i]);
			//LOGD("\n");
			savelog(rbuf, rc);
		}
		else
		{
			//writeport(fd,wbuf,16);
		   	LOGD("recv none, data length: %d Bytes\n", rc);

            if(rc < 0)
            {
			    fflush(fp);
                
                /*避免不停的创建文件*/
                if(last_write > 0)
                {
                    filename[0] = '\0';
                }

                
                close(fd);   

                while(1)
                {
                    fd = openport(dev);
                    
                    if(fd > 0)
                    {
                        setport(fd,115200,8,1,'n');  //Set serial port
                        break;
                    }
                    usleep(300000);
                }
            }
            
            last_write = rc;
 		}
	 }   
	 close(fd);   
}   

pthread_t s_tid_diagsaver;



void start_diagsaver(void)
{
    int ret;
    int fd = -1;
    int opt;
    pthread_attr_t attr;
    
    LOGI("start_diagsaver");
  
    pthread_attr_init (&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    ret = pthread_create(&s_tid_diagsaver, &attr, diagsaver_main, NULL);

}

