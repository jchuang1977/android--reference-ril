/* //device/system/reference-ril/reference-ril.c
**
** Copyright 2006, The Android Open Source Project
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

#include <telephony/ril.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <pthread.h>
#include <alloca.h>
#include "atchannel.h"
#include "at_tok.h"
#include "misc.h"
#include <getopt.h>
#include <sys/socket.h>
#include <cutils/sockets.h>
#include <termios.h>
#include <sys/statfs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cutils/properties.h>
#include <pppd.h>
#include <dirent.h>
#include <stdbool.h>

#include <usb_find.h>

#define LOG_TAG "RIL_L"
#include <ril_log.h>

#define MAX_AT_RESPONSE 0x1000
#define MAX_PATH 256

#define CONFIG_DEFAULT_PDP 1
#define ALLOW_CLOSE_PPP 1


const int EF_ICCID  = 0x2fe2;
const int COMMAND_READ_BINARY = 0xb0;
static int Include_GSM = -1;
static int LteInserve = 0;
static int GsmInserve = 0;
/* pathname returned from RIL_REQUEST_SETUP_DATA_CALL / RIL_REQUEST_SETUP_DEFAULT_PDP */
#define PPP_TTY_PATH "/dev/ppp"

//#define RIL_SET_PREFERRED_NETWORK_SUPPORTED

#ifdef USE_TI_COMMANDS

// Enable a workaround
// 1) Make incoming call, do not answer
// 2) Hangup remote end
// Expected: call should disappear from CLCC line
// Actual: Call shows as "ACTIVE" before disappearing
#define WORKAROUND_ERRONEOUS_ANSWER 1

// Some varients of the TI stack do not support the +CGEV unsolicited
// response. However, they seem to send an unsolicited +CME ERROR: 150
#define WORKAROUND_FAKE_CGEV 1
#endif

typedef enum {
    SIM_ABSENT = 0,
    SIM_NOT_READY = 1,
    SIM_READY = 2, /* SIM_READY means the radio state is RADIO_STATE_SIM_READY */
    SIM_PIN = 3,
    SIM_PUK = 4,
    SIM_NETWORK_PERSONALIZATION = 5,
    #if RIL_VERSION >11
	    RUIM_ABSENT = 6,
	    RUIM_NOT_READY = 7,
	    RUIM_READY = 8,
	    RUIM_PIN = 9,
	    RUIM_PUK = 10,
	    RUIM_NETWORK_PERSONALIZATION = 11,
	    ISIM_ABSENT = 12,
	    ISIM_NOT_READY = 13,
	    ISIM_READY = 14,
	    ISIM_PIN = 15,
	    ISIM_PUK = 16,
	    ISIM_NETWORK_PERSONALIZATION = 17,
    #endif
} SIM_Status;

#if RIL_VERSION>4
#define MDM_GSM         0x01
#define MDM_WCDMA       0x02
#define MDM_CDMA        0x04
#define MDM_EVDO        0x08
#define MDM_LTE         0x10
#endif


static void onRequest (int request, void *data, size_t datalen, RIL_Token t);
static RIL_RadioState currentState();
static int onSupports (int requestCode);
static void onCancel (RIL_Token t);
static const char *getVersion();
static int isRadioOn();
static SIM_Status getSIMStatus();

#if (RIL_VERSION<=4)
static int getCardStatus(RIL_CardStatus **pp_card_status);
static void freeCardStatus(RIL_CardStatus *p_card_status);
#else
static int getCardStatus(RIL_CardStatus_v5 **pp_card_status);

static int getCardStatusV12(RIL_CardStatus_v6 **pp_card_status);

static void freeCardStatus(RIL_CardStatus_v5 *p_card_status);
static int getCardStatusV6(RIL_CardStatus_v6 **pp_card_status);
static void freeCardStatusV6(RIL_CardStatus_v6 *p_card_status);
#endif

static void onDataCallListChanged(void *param);

extern void start_diagsaver(void);

extern const char * requestToString(int request);

/*** Static Variables ***/
static const RIL_RadioFunctions s_callbacks = {
    RIL_VERSION,
    onRequest,
    currentState,
    onSupports,
    onCancel,
    getVersion
};

#ifdef RIL_SHLIB
static const struct RIL_Env *s_rilenv;

#define RIL_onRequestComplete(t, e, response, responselen) s_rilenv->OnRequestComplete(t,e, response, responselen)
#define RIL_onUnsolicitedResponse(a,b,c) s_rilenv->OnUnsolicitedResponse(a,b,c)
#define RIL_requestTimedCallback(a,b,c) s_rilenv->RequestTimedCallback(a,b,c)
#endif

static RIL_RadioState sState = RADIO_STATE_UNAVAILABLE;

static pthread_mutex_t s_state_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_state_cond = PTHREAD_COND_INITIALIZER;

static int s_port = -1;
static char         s_device_path[MAX_PATH];
static int          s_device_socket = 0;

/* trigger change to this with s_state_cond */
static int s_closed = 0;

static int sFD;     /* file desc of AT channel */
static char sATBuffer[MAX_AT_RESPONSE+1];
static char *sATBufferCur = NULL;

static const struct timeval TIMEVAL_SIMPOLL = {1,0};
static const struct timeval TIMEVAL_CALLSTATEPOLL = {0,500000};
static const struct timeval TIMEVAL_0 = {0,0};
#ifdef AIRM2M_REPORT_SIGNAL_STRENGTH
	static const struct timeval TIMEVAL_20 = {20,0};
	int poll_signal_started = 0;
#endif
static int onRequestCount = 0;
static int onRequestRegCount = 0;

static bool diag_need_save = 0;

#ifdef WORKAROUND_ERRONEOUS_ANSWER
// Max number of times we'll try to repoll when we think
// we have a AT+CLCC race condition
#define REPOLL_CALLS_COUNT_MAX 4
#define BAND_SET  0

// Line index that was incoming or waiting at last poll, or -1 for none
static int s_incomingOrWaitingLine = -1;
// Number of times we've asked for a repoll of AT+CLCC
static int s_repollCallsCount = 0;
// Should we expect a call to be answered in the next CLCC?
static int s_expectAnswer = 0;
#endif /* WORKAROUND_ERRONEOUS_ANSWER */


static int bSetupDataCallCompelete = 0;
static int nSetupDataCallFailTimes = 0;
static int s_default_pdp = 1;
static char sKeepLocalip[PROPERTY_VALUE_MAX]={0};



static void pollSIMState (void *param);
static void setRadioState(RIL_RadioState newState);





static int clccStateToRILState(int state, RIL_CallState *p_state)

{
    switch(state) {
        case 0: *p_state = RIL_CALL_ACTIVE;   return 0;
        case 1: *p_state = RIL_CALL_HOLDING;  return 0;
        case 2: *p_state = RIL_CALL_DIALING;  return 0;
        case 3: *p_state = RIL_CALL_ALERTING; return 0;
        case 4: *p_state = RIL_CALL_INCOMING; return 0;
        case 5: *p_state = RIL_CALL_WAITING;  return 0;
        default: return -1;
    }
}

/**
 * Note: directly modified line and has *p_call point directly into
 * modified line
 */
static int callFromCLCCLine(char *line, RIL_Call *p_call)
{
        //+CLCC: 1,0,2,0,0,\"+18005551212\",145
        //     index,isMT,state,mode,isMpty(,number,TOA)?

    int err;
    int state;
    int mode;

    err = at_tok_start(&line);
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &(p_call->index));
    if (err < 0) goto error;

    err = at_tok_nextbool(&line, &(p_call->isMT));
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &state);
    if (err < 0) goto error;

    err = clccStateToRILState(state, &(p_call->state));
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &mode);
    if (err < 0) goto error;

    p_call->isVoice = (mode == 0);

    err = at_tok_nextbool(&line, &(p_call->isMpty));
    if (err < 0) goto error;

    if (at_tok_hasmore(&line)) {
        err = at_tok_nextstr(&line, &(p_call->number));

        /* tolerate null here */
        if (err < 0) return 0;

        // Some lame implementations return strings
        // like "NOT AVAILABLE" in the CLCC line
        if (p_call->number != NULL
            && 0 == strspn(p_call->number, "+0123456789")
        ) {
            p_call->number = NULL;
        }

        err = at_tok_nextint(&line, &p_call->toa);
        if (err < 0) goto error;
    }

    p_call->uusInfo = NULL;

    return 0;

error:
    LOGE("invalid CLCC line\n");
    return -1;
}

static bool netwrokDataReady(void)
{
    const char*cmd = "AT+CEREG?";
    const char* prefix = "+CEREG:";
    int err;
    ATResponse * p_response;
    char * line;
    int REG_status;

    err = at_send_command_singleline(cmd, prefix, &p_response);

    if (err != 0) goto error;

    line = p_response->p_intermediates->line;

    err = at_tok_start(&line);
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &REG_status);
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &REG_status);
    if (err < 0) goto error;


	LOGD("netwrokDataReady %d  \n",REG_status);

    return((REG_status == 1 || REG_status == 5	));

error:
    return false;

}

/** do post-AT+CFUN=1 initialization */
static void onRadioPowerOn()
{
#ifdef USE_TI_COMMANDS
    /*  Must be after CFUN=1 */
    /*  TI specific -- notifications for CPHS things such */
    /*  as CPHS message waiting indicator */

    at_send_command("AT%CPHS=1", NULL);

    /*  TI specific -- enable NITZ unsol notifs */
    at_send_command("AT%CTZV=1", NULL);
#endif

    pollSIMState(NULL);
}

/** do post- SIM ready initialization */
static void onSIMReady()
{
    at_send_command_singleline("AT+CSMS=1", "+CSMS:", NULL);
    /*
     * Always send SMS messages directly to the TE
     *
     * mode = 1 // discard when link is reserved (link should never be
     *             reserved)
     * mt = 2   // most messages routed to TE
     * bm = 2   // new cell BM's routed to TE
     * ds = 1   // Status reports routed to TE
     * bfr = 1  // flush buffer
     */
    at_send_command("AT+CNMI=1,2,2,1,1", NULL);
}

char MVersion[50];

static void requestBaseBandVersion(void *data, size_t datalen, RIL_Token t)
{
    int err;
    ATResponse *atResponse = NULL;
    char *line;
    char* bandVersion;
    err = at_send_command_singleline("AT+CGMR", "+CGMR: ", &atResponse);

    if(err != 0){
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return;
    }

    line = atResponse->p_intermediates->line;

    at_tok_start(&line);
    at_tok_nextstr(&line, &bandVersion);
	strcpy(MVersion,bandVersion);
    RIL_onRequestComplete(t, RIL_E_SUCCESS, bandVersion, sizeof(char *));

    at_response_free(atResponse);
}


static void requestRadioPower(void *data, size_t datalen, RIL_Token t)
{
    int onOff;

    int err;
    ATResponse *p_response = NULL;

    assert (datalen >= sizeof(int *));
    onOff = ((int *)data)[0];

    LOGD("requestRadioPower onOff %d, sState %d \n", onOff, sState);

#if 1
    if (onOff == 0 && sState != RADIO_STATE_OFF) {
        err = at_send_command("AT+CFUN=4", &p_response);
       if (err < 0 || p_response->success == 0) goto error;
        setRadioState(RADIO_STATE_OFF);
    } else if (onOff > 0 && sState == RADIO_STATE_OFF) {
        err = at_send_command("AT+CFUN=1", &p_response);
        if (err < 0|| p_response->success == 0) {
            // Some stacks return an error when there is no SIM,
            // but they really turn the RF portion on
            // So, if we get an error, let's check to see if it
            // turned on anyway

            if (isRadioOn() != 1) {
                goto error;
            }
        }
		#if RIL_VERSION <12
	        setRadioState(RADIO_STATE_SIM_NOT_READY);
		#else
			setRadioState(RADIO_STATE_ON);
		#endif
    }
#endif

    at_response_free(p_response);
    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    return;
error:
    at_response_free(p_response);
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void requestOrSendDataCallList(RIL_Token *t);


static void onDataCallExit(void *param) {

    RIL_onUnsolicitedResponse(RIL_UNSOL_DATA_CALL_LIST_CHANGED, NULL, 0);
}

static void onDataCallListChanged(void *param)
{
    RIL_onUnsolicitedResponse(RIL_UNSOL_DATA_CALL_LIST_CHANGED,
                                  NULL, 0);
}

static void requestDataCallList(void *data, size_t datalen, RIL_Token t)
{
    requestOrSendDataCallList(&t);
}

#define BUF_SIZE 1024

static void get_local_ip(char *local_ip) {
       int sock_fd;
    struct ifconf conf;
    struct ifreq *ifr;
    char buff[BUF_SIZE] = {0};
    int num;
    int i;

    sock_fd = socket(PF_INET, SOCK_DGRAM, 0);
    if ( sock_fd < 0 )
    {
        strcpy(local_ip, "0.0.0.0");
	return ;
    }
    conf.ifc_len = BUF_SIZE;
    conf.ifc_buf = buff;

    if ( ioctl(sock_fd, SIOCGIFCONF, &conf) < 0 )
    {
        close(sock_fd);
        strcpy(local_ip, "0.0.0.0");
        return;
    }

    num = conf.ifc_len / sizeof(struct ifreq);
    ifr = conf.ifc_req;

    for(i = 0; i < num; i++)
    {
        struct sockaddr_in *sin = (struct sockaddr_in *)(&ifr->ifr_addr);

        if ( ioctl(sock_fd, SIOCGIFFLAGS, ifr) < 0 )
        {
            close(sock_fd);
            strcpy(local_ip, "0.0.0.0");
            return;
        }

        LOGD("get_local_ip status %s %d ", ifr->ifr_name,  (ifr->ifr_flags & IFF_UP));

        if ( (ifr->ifr_flags & IFF_UP) && strcmp("ppp0",ifr->ifr_name) == 0 )
        {
            char* strIP = inet_ntoa(sin->sin_addr);
            close(sock_fd);
            strcpy(local_ip, strIP);
            return ;
        }

        ifr++;
    }

	LOGD("get_local_ip ifr num %d ", num);
    strcpy(local_ip, "0.0.0.0");
    close(sock_fd);
}


static void requestDeactivateDataCall(void *data, size_t datalen, RIL_Token t)
{
	#if (ALLOW_CLOSE_PPP == 1)
		pppd_stop(SIGTERM);
	#endif

	if (t != NULL)
		RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
	else
		RIL_onUnsolicitedResponse(RIL_UNSOL_DATA_CALL_LIST_CHANGED, NULL, 0);
    return;
}

static int IsNewCGCONTRDP(char* out)
{
	bool ret = 0;
	int i = 0;
	char *out1;

	LOGD("## IsNewCGCONTRDP %s", out);

	out1 = out;

	for(i=0;i<6;i++)
	{

		out1 = strstr(out1, ",");

		if(out1)
		{

			out1 = out1 + 1;

			LOGD("## IsNewCGCONTRDP address out1 %s %d %d", out1,out1-out,i);
		}
		else
		{
			LOGD("## IsNewCGCONTRDP address out1 i %d", i);
			break;
		}
	}

	LOGD("## IsNewCGCONTRDP i %d", i);

	if(i>=6)
	{
		ret = 1;
		LOGD("## IsNewCGCONTRDP ret %d %d", ret,i);
	}

	return ret;

}

static void requestOrSendDataCallList(RIL_Token *t)
{
    ATResponse *p_response;
	ATResponse *p_response1;
    ATLine *p_cur;
    int err;
    int n = 1;
    char *out;
	char *out1;
	char *out2;
	char chackBear[20] = {'\0'};

    LOGD("requestOrSendDataCallList start");

    err = at_send_command_multiline ("AT+CGACT?", "+CGACT:", &p_response);
    if (err != 0 || p_response->success == 0) {
        if (t != NULL)
            RIL_onRequestComplete(*t, RIL_E_GENERIC_FAILURE, NULL, 0);
        else
            RIL_onUnsolicitedResponse(RIL_UNSOL_DATA_CALL_LIST_CHANGED,
                                      NULL, 0);
        return;
    }

#if 0
    for (p_cur = p_response->p_intermediates; p_cur != NULL;
         p_cur = p_cur->p_next)
        n++;
#endif

#if RIL_VERSION<=4
    RIL_Data_Call_Response  *response =
        alloca(sizeof(RIL_Data_Call_Response));
#elif RIL_VERSION< 12
    RIL_Data_Call_Response_v6 *response =
        alloca(sizeof(RIL_Data_Call_Response_v6));
#else
    RIL_Data_Call_Response_v11 *response =
        alloca(sizeof(RIL_Data_Call_Response_v11));

#endif

    response->cid = -1;
    response->active = -1;
    response->type = "IP";
#if RIL_VERSION>11
	response->pcscf = "";
	response->mtu = 0;

#endif


#if RIL_VERSION>4
    response->status = -1;
    response->suggestedRetryTime = -1;
    response->ifname = "";
	response->addresses = "";
    response->dnses = "";
    response->gateways = "";
#else
   response->address = "";
#endif

     for (p_cur = p_response->p_intermediates; p_cur != NULL;
         p_cur = p_cur->p_next) {
        char *line = p_cur->line;

        err = at_tok_start(&line);
        if (err < 0)
            goto error;

        err = at_tok_nextint(&line, &response->cid);
        if (err < 0)
            goto error;

        if(response->cid != 1)
        {
            continue;
        }

        err = at_tok_nextint(&line, &response->active);
        if (err < 0)
            goto error;

        break;
    }

    at_response_free(p_response);

	snprintf(chackBear, sizeof(chackBear), "AT+CGCONTRDP=%d", s_default_pdp);

    err = at_send_command_multiline (chackBear, "+CGCONTRDP:", &p_response);
    if (err != 0 || p_response->success == 0) {
        if (t != NULL)
            RIL_onRequestComplete(*t, RIL_E_GENERIC_FAILURE, NULL, 0);
        else
            RIL_onUnsolicitedResponse(RIL_UNSOL_DATA_CALL_LIST_CHANGED,
                                      NULL, 0);
        return;
    }


    for (p_cur = p_response->p_intermediates; p_cur != NULL;
         p_cur = p_cur->p_next) {
        char *line = p_cur->line;
        int cid;

		out2 = line;

        err = at_tok_start(&line);
        if (err < 0)
            goto error;

        err = at_tok_nextint(&line, &cid);
        if (err < 0)
            goto error;

        // Assume no error
    #if RIL_VERSION>4
         response->status = 0;
         response->active = 2;
    #endif

        //bearer id
        err = at_tok_nextint(&line, &cid);
        if (err < 0)
        {
            goto error;
        }

        response->type = "IP";

        // APN ignored for v5
        err = at_tok_nextstr(&line, &out);
        if (err < 0)
        {
            goto error;
        }


    #if RIL_VERSION<=4
        response->apn = alloca(strlen(out) + 1);
        strcpy(response->apn, out);
    #else
        response->ifname = alloca(strlen("ppp0") + 1);
        strcpy(response->ifname, "ppp0");
    #endif

        //local address
        err = at_tok_nextstr(&line, &out);
        if (err < 0)
            goto error;

LOGD("## MVersion %s",&MVersion);
//��ȡ�汾��ǰ��λ����
char ch = 'V';
char *ret;
int i,numz=0;
ret = memchr(MVersion, ch,strlen(MVersion));
    for(i = 0 ; i < 5 ; i++)
	{
		ret++;
		if((*ret >= '0')&&( *ret <= '9'))
		{
			numz = numz*10 + *ret - '0';
		} else {
            break;
        }
	}
	LOGD("version=%d\n",numz);
if(numz>=30)
{
	#if RIL_VERSION>4
        response->addresses = alloca(strlen(out) + 1);
        strcpy(response->addresses, out);
    #else
        response->address = alloca(strlen(out) + 1);
        strcpy(response->address, out);
    #endif


        //subnet mask
        err = at_tok_nextstr(&line, &out);
        if (err < 0)
            goto error;

        //gateway
        err = at_tok_nextstr(&line, &out);
        if (err < 0)
            goto error;


    #if RIL_VERSION>4
        response->gateways = alloca(strlen(out) + 1);
        strcpy(response->gateways, out);
    #endif

        //prim DNS
        err = at_tok_nextstr(&line, &out);
        if (err < 0)
            goto error;

    #if RIL_VERSION>4
        response->dnses = alloca(50 + 1);
        strcpy(response->dnses, out);
    #endif

        //sec DNS
        err = at_tok_nextstr(&line, &out);
        if (err < 0)
            goto error;

    #if RIL_VERSION>4
        if(out[0] != '\0')
        {
            strcat(response->dnses, " ");
            strcat(response->dnses, out);
        }
    #endif

    }
	else
	{
#if RIL_VERSION>4
				response->addresses = alloca(strlen(out) - 7);
				memset(response->addresses, '\0', strlen(out) - 7);
				strncpy(response->addresses, out,strlen(out) - 8);
    #else
				response->address = alloca(strlen(out) - 7);
				memset(response->address, '\0', strlen(out) - 7);
				strncpy(response->address, out,strlen(out) - 8);

    #endif


				//subnet mask



    #if RIL_VERSION>4
				response->gateways = alloca(strlen("192.168.0.1") + 1);
				strcpy(response->gateways, "192.168.0.1");

				snprintf(chackBear, sizeof(chackBear), "AT+CGPDNSADDR=%d", s_default_pdp);
				err = at_send_command_singleline (chackBear, "+CGPDNSADDR:", &p_response1);
				if (err < 0 || p_response1->success == 0) {
					goto error;
				}


				char *line1 = p_response1->p_intermediates->line;

				err = at_tok_start(&line1);
				if (err < 0)
				  goto error;


				err = at_tok_nextint(&line1, &cid);
				if (err < 0)
				  goto error;


				//DNS
				err = at_tok_nextstr(&line1, &out);
				if (err < 0)
				{
				  goto error;
				}
				char *	out1 =	strrchr(out,0x3A);

				response->dnses = alloca(strlen(out1)  );
				strcpy(response->dnses, out1+1);
	#endif

	}

        break;
    }
    at_response_free(p_response);

#if RIL_VERSION>11
    if (t != NULL)
    	{
			LOGD("requestOrSendDataCallList data status =%d  suggestedRetryTime = %d  cid= %d  active = %d  type = %s  ifname=%s  addresses= %s  dnses=%s  gateways=%s  pcscf = %s  mtu=%d",response->status,response->suggestedRetryTime,response->cid ,response->active,response->type,response->ifname,response->addresses,response->dnses,response->gateways,response->pcscf,response->mtu);
        	RIL_onRequestComplete(*t, RIL_E_SUCCESS, response,
                              1 * sizeof(RIL_Data_Call_Response_v11));
    	}
    else
		LOGD("requestOrSendDataCallList12");
        RIL_onUnsolicitedResponse(RIL_UNSOL_DATA_CALL_LIST_CHANGED,
                                  response,
                                  1 * sizeof(RIL_Data_Call_Response_v11));

#elif RIL_VERSION>4
    if (t != NULL)
        RIL_onRequestComplete(*t, RIL_E_SUCCESS, response,
                              1 * sizeof(RIL_Data_Call_Response_v6));
    else
        RIL_onUnsolicitedResponse(RIL_UNSOL_DATA_CALL_LIST_CHANGED,
                                  response,
                                  1 * sizeof(RIL_Data_Call_Response_v6));
#else
    if (t != NULL)
        RIL_onRequestComplete(*t, RIL_E_SUCCESS, response,
                              1 * sizeof(RIL_Data_Call_Response));
    else
        RIL_onUnsolicitedResponse(RIL_UNSOL_DATA_CALL_LIST_CHANGED,
                                  response,
                                  1 * sizeof(RIL_Data_Call_Response));
        LOGD("requestOrSendDataCallList6");
#endif

    return;

error:
    if (t != NULL)
        RIL_onRequestComplete(*t, RIL_E_GENERIC_FAILURE, NULL, 0);
    else
        RIL_onUnsolicitedResponse(RIL_UNSOL_DATA_CALL_LIST_CHANGED,
                                  NULL, 0);

    at_response_free(p_response);
}

static int airm2m_at_cops(int response[4]) {
    int err;
    ATResponse *p_response = NULL;
    char *line;
    char *oper;

    response[0] = response[1] = response[2] = response[3] = 0;

    err = at_send_command_singleline("AT+COPS=3,2;+COPS?", "+COPS:", &p_response);
    if ((err < 0) ||  (p_response == NULL) || (p_response->success == 0))
        goto error;

    line = p_response->p_intermediates->line;

    err = at_tok_start(&line);
    if (err < 0) goto error;

//+COPS:<mode>[,<format>[,<oper>][,<Act>]]
    err = at_tok_nextint(&line, &response[0]);
    if (err < 0) goto error;

    if (!at_tok_hasmore(&line)) goto error;

    err = at_tok_nextint(&line, &response[1]);
    if (err < 0) goto error;

    if (!at_tok_hasmore(&line)) goto error;

    err = at_tok_nextstr(&line, &oper);
    if (err < 0) goto error;
    response[2] = atoi(oper);

    if (!at_tok_hasmore(&line)) goto error;

    err = at_tok_nextint(&line, &response[3]);
    if (err < 0) goto error;

error:
     at_response_free(p_response);
     return response[3];
}


static void requestQueryNetworkSelectionMode(
                void *data, size_t datalen, RIL_Token t)
{
    int err;
    ATResponse *p_response = NULL;
    int response = 0;
    char *line;

    err = at_send_command_singleline("AT+COPS?", "+COPS:", &p_response);

    if (err < 0 || p_response->success == 0) {
	#if 1
        RIL_onRequestComplete(t, RIL_E_SUCCESS, &response, sizeof(int));
        at_response_free(p_response);
        return;
	#else
	    goto error;
	#endif
    }

    line = p_response->p_intermediates->line;

    err = at_tok_start(&line);

    if (err < 0) {
        goto error;
    }

    err = at_tok_nextint(&line, &response);

    if (err < 0) {
        goto error;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &response, sizeof(int));
    at_response_free(p_response);
    return;
error:
    at_response_free(p_response);
    LOGE("requestQueryNetworkSelectionMode must never return error when radio is on");
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void sendCallStateChanged(void *param)
{
    RIL_onUnsolicitedResponse (
        RIL_UNSOL_RESPONSE_CALL_STATE_CHANGED,
        NULL, 0);
}

static void requestGetCurrentCalls(void *data, size_t datalen, RIL_Token t)
{
    int err;
    ATResponse *p_response;
    ATLine *p_cur;
    int countCalls;
    int countValidCalls;
    RIL_Call *p_calls;
    RIL_Call **pp_calls;
    int i;
    int needRepoll = 0;

#ifdef WORKAROUND_ERRONEOUS_ANSWER
    int prevIncomingOrWaitingLine;

    prevIncomingOrWaitingLine = s_incomingOrWaitingLine;
    s_incomingOrWaitingLine = -1;
#endif /*WORKAROUND_ERRONEOUS_ANSWER*/

    err = at_send_command_multiline ("AT+CLCC", "+CLCC:", &p_response);

    if (err != 0 || p_response->success == 0) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return;
    }

    /* count the calls */
    for (countCalls = 0, p_cur = p_response->p_intermediates
            ; p_cur != NULL
            ; p_cur = p_cur->p_next
    ) {
        countCalls++;
    }

    /* yes, there's an array of pointers and then an array of structures */

    pp_calls = (RIL_Call **)alloca(countCalls * sizeof(RIL_Call *));
    p_calls = (RIL_Call *)alloca(countCalls * sizeof(RIL_Call));
    memset (p_calls, 0, countCalls * sizeof(RIL_Call));

    /* init the pointer array */
    for(i = 0; i < countCalls ; i++) {
        pp_calls[i] = &(p_calls[i]);
    }

    for (countValidCalls = 0, p_cur = p_response->p_intermediates
            ; p_cur != NULL
            ; p_cur = p_cur->p_next
    ) {
        err = callFromCLCCLine(p_cur->line, p_calls + countValidCalls);

        if (err != 0) {
            continue;
        }

#ifdef WORKAROUND_ERRONEOUS_ANSWER
        if (p_calls[countValidCalls].state == RIL_CALL_INCOMING
            || p_calls[countValidCalls].state == RIL_CALL_WAITING
        ) {
            s_incomingOrWaitingLine = p_calls[countValidCalls].index;
        }
#endif /*WORKAROUND_ERRONEOUS_ANSWER*/

        if (p_calls[countValidCalls].state != RIL_CALL_ACTIVE
            && p_calls[countValidCalls].state != RIL_CALL_HOLDING
        ) {
            needRepoll = 1;
        }

        countValidCalls++;
    }

#ifdef WORKAROUND_ERRONEOUS_ANSWER
    // Basically:
    // A call was incoming or waiting
    // Now it's marked as active
    // But we never answered it
    //
    // This is probably a bug, and the call will probably
    // disappear from the call list in the next poll
    if (prevIncomingOrWaitingLine >= 0
            && s_incomingOrWaitingLine < 0
            && s_expectAnswer == 0
    ) {
        for (i = 0; i < countValidCalls ; i++) {

            if (p_calls[i].index == prevIncomingOrWaitingLine
                    && p_calls[i].state == RIL_CALL_ACTIVE
                    && s_repollCallsCount < REPOLL_CALLS_COUNT_MAX
            ) {
                LOGI(
                    "Hit WORKAROUND_ERRONOUS_ANSWER case."
                    " Repoll count: %d\n", s_repollCallsCount);
                s_repollCallsCount++;
                goto error;
            }
        }
    }

    s_expectAnswer = 0;
    s_repollCallsCount = 0;
#endif /*WORKAROUND_ERRONEOUS_ANSWER*/

    RIL_onRequestComplete(t, RIL_E_SUCCESS, pp_calls,
            countValidCalls * sizeof (RIL_Call *));

    at_response_free(p_response);

#ifdef POLL_CALL_STATE
    if (countValidCalls) {  // We don't seem to get a "NO CARRIER" message from
                            // smd, so we're forced to poll until the call ends.
#else
    if (needRepoll) {
#endif
        RIL_requestTimedCallback (sendCallStateChanged, NULL, &TIMEVAL_CALLSTATEPOLL);
    }

    return;
error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

static void requestDial(void *data, size_t datalen, RIL_Token t)
{
    RIL_Dial *p_dial;
    char *cmd;
    const char *clir;
    int ret;

    p_dial = (RIL_Dial *)data;

    switch (p_dial->clir) {
        case 1: clir = "I"; break;  /*invocation*/
        case 2: clir = "i"; break;  /*suppression*/
        default:
        case 0: clir = ""; break;   /*subscription default*/
    }

    asprintf(&cmd, "ATD%s%s;", p_dial->address, clir);

    ret = at_send_command(cmd, NULL);

    free(cmd);

    /* success or failure is ignored by the upper layer here.
       it will call GET_CURRENT_CALLS and determine success that way */
    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
}

static void requestWriteSmsToSim(void *data, size_t datalen, RIL_Token t)
{
    RIL_SMS_WriteArgs *p_args;
    char *cmd;
    int length;
    int err;
    ATResponse *p_response = NULL;

    p_args = (RIL_SMS_WriteArgs *)data;

    length = strlen(p_args->pdu)/2;
    asprintf(&cmd, "AT+CMGW=%d,%d", length, p_args->status);

    err = at_send_command_sms(cmd, p_args->pdu, "+CMGW:", &p_response);

    if (err != 0 || p_response->success == 0) goto error;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    at_response_free(p_response);

    return;
error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

static void requestHangup(void *data, size_t datalen, RIL_Token t)
{
    int *p_line;

    int ret;
    char *cmd;

    p_line = (int *)data;

    // 3GPP 22.030 6.5.5
    // "Releases a specific active call X"
    asprintf(&cmd, "AT+CHLD=1%d", p_line[0]);

    ret = at_send_command(cmd, NULL);

    free(cmd);

    /* success or failure is ignored by the upper layer here.
       it will call GET_CURRENT_CALLS and determine success that way */
    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
}

static void requestSignalStrength(void *data, size_t datalen, RIL_Token t)
{
      ATResponse *p_response = NULL;
    int err;
    int response[11];
    char *line;

   err = at_send_command_singleline("AT+CSQ", "+CSQ:", &p_response);

    if (err < 0 || p_response->success == 0) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        goto error;
    }

   line = p_response->p_intermediates->line;

    err = at_tok_start(&line);
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &(response[0]));
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &(response[1]));
    if (err < 0) goto error;





   at_response_free(p_response);
   p_response = NULL;

    err = at_send_command_singleline("AT+CESQ", "+CESQ:", &p_response);

    if (err < 0 || p_response->success == 0) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        goto error;
    }

    line = p_response->p_intermediates->line;

    err = at_tok_start(&line);
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &(response[7])); /*Rssi*/
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &(response[7]));/*ber*/
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &(response[7]));/*RSCP*/
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &(response[7]));/*ECHO*/
    if (err < 0) goto error;


    err = at_tok_nextint(&line, &(response[8]));/*RSRq*/
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &(response[7]));/*RSRp*/
    if (err < 0) goto error;

    response[9] = 50; /*rssnr*/
    response[10] = 7;   /*cqi*/
	if(response[7] < 100)
	{
		response[2] = 100 - response[7]; /*EVDO DBM*/
		response[4] = 100 - response[7]; /*EVDO DBM*/
		response[3] = response[2] + 15;  /*CDMA ecio*/
		response[5] = response[2] + 15;  /*EVDO ecio*/
	}
	else
	{
		response[2] = 75; /*CDMA DBM*/
		response[3] = 125;  /*CDMA ecio*/
		response[4] = 75;
		response[5] = 125;  /*EVDO ecio*/
	}

    response[6] = 0;  /*EVDO signalNoiseRatio*/
    RIL_onRequestComplete(t, RIL_E_SUCCESS, response, sizeof(response));

    at_response_free(p_response);
		return;

error:
		LOGE("requestSignalStrength must never return an error when radio is on");
		RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
		at_response_free(p_response);

}



#ifdef AIRM2M_REPORT_SIGNAL_STRENGTH
	static void onReportSignalStrength(void *param)
	{
	    requestSignalStrength(NULL, 0, NULL);
	    RIL_requestTimedCallback(onReportSignalStrength, NULL, &TIMEVAL_20);
	}
#endif
#define REG_STATE_LEN 15
#define REG_DATA_STATE_LEN 6

static void requestRegistrationState(int request, void *data,
                                        size_t datalen, RIL_Token t)
{
    int err;
    int response[4];
    char * responseStr[4];
    ATResponse *p_response = NULL;
    const char *cmd;
    const char *prefix;
    char *line, *p;
    int commas;
    int skip;
    int count = 3,numElements = 0;
	int cops_response[4] = {0};

    int sysMode;
    int sysSubmode;

#if RIL_VERSION <=4
    if (request == RIL_REQUEST_REGISTRATION_STATE) {
    } else if (request == RIL_REQUEST_GPRS_REGISTRATION_STATE) {

#else

    if (request == RIL_REQUEST_VOICE_REGISTRATION_STATE) {
        cmd = "AT+CREG?";
        prefix = "+CREG:";
    } else if (request == RIL_REQUEST_DATA_REGISTRATION_STATE)
    {

#endif

#if RIL_VERSION > 11
	    if (request == RIL_REQUEST_VOICE_REGISTRATION_STATE) {
	        numElements = REG_STATE_LEN;
	    } else if (request == RIL_REQUEST_DATA_REGISTRATION_STATE) {
	        numElements = REG_DATA_STATE_LEN;
	    }
#endif
#if RIL_VERSION > 4
		{
		airm2m_at_cops(cops_response);

        response[3] = cops_response[3];


       at_response_free(p_response);
	   if(response[3] == 7){
        	cmd = "AT+CEREG?";
        	prefix = "+CEREG:";
        }else{
        	cmd = "AT+CREG?";
        	prefix = "+CREG:";
        }
    }
#else
        	cmd = "AT+CREG?";
        	prefix = "+CREG:";

#endif


    } else {
        assert(0);
        goto error;
    }

    err = at_send_command_singleline(cmd, prefix, &p_response);

    if (err != 0) goto error;

    line = p_response->p_intermediates->line;

    err = at_tok_start(&line);
    if (err < 0) goto error;

    /* Ok you have to be careful here
     * The solicited version of the CREG response is
     * +CREG: n, stat, [lac, cid]
     * and the unsolicited version is
     * +CREG: stat, [lac, cid]
     * The <n> parameter is basically "is unsolicited creg on?"
     * which it should always be
     *
     * Now we should normally get the solicited version here,
     * but the unsolicited version could have snuck in
     * so we have to handle both
     *
     * Also since the LAC and CID are only reported when registered,
     * we can have 1, 2, 3, or 4 arguments here
     *
     * finally, a +CGREG: answer may have a fifth value that corresponds
     * to the network type, as in;
     *
     *   +CGREG: n, stat [,lac, cid [,networkType]]
     */

    /* count number of commas */
    commas = 0;
    for (p = line ; *p != '\0' ;p++) {
        if (*p == ',') commas++;
    }

    switch (commas) {
        case 0: /* +CREG: <stat> */
            err = at_tok_nextint(&line, &response[0]);
            if (err < 0) goto error;
            response[1] = -1;
            response[2] = -1;
        break;

        case 1: /* +CREG: <n>, <stat> */
            err = at_tok_nextint(&line, &skip);
            if (err < 0) goto error;
            err = at_tok_nextint(&line, &response[0]);
            if (err < 0) goto error;
            response[1] = -1;
            response[2] = -1;
            if (err < 0) goto error;
        break;

        case 2: /* +CREG: <stat>, <lac>, <cid> */
            err = at_tok_nextint(&line, &response[0]);
            if (err < 0) goto error;
            err = at_tok_nexthexint(&line, &response[1]);
            if (err < 0) goto error;
            err = at_tok_nexthexint(&line, &response[2]);
            if (err < 0) goto error;
        break;
        case 3: /* +CREG: <n>, <stat>, <lac>, <cid> */
            err = at_tok_nextint(&line, &skip);
            if (err < 0) goto error;
            err = at_tok_nextint(&line, &response[0]);
            if (err < 0) goto error;
            err = at_tok_nexthexint(&line, &response[1]);
            if (err < 0) goto error;
            err = at_tok_nexthexint(&line, &response[2]);
            if (err < 0) goto error;
        break;
        /* special case for CGREG, there is a fourth parameter
         * that is the network type (unknown/gprs/edge/umts)
         */
        case 4: /* +CGREG: <n>, <stat>, <lac>, <cid>, <networkType> */
            err = at_tok_nextint(&line, &skip);
            if (err < 0) goto error;
            err = at_tok_nextint(&line, &response[0]);
            if (err < 0) goto error;
            err = at_tok_nexthexint(&line, &response[1]);
            if (err < 0) goto error;
            err = at_tok_nexthexint(&line, &response[2]);
            if (err < 0) goto error;
        #if RIL_VERSION <= 4
            err = at_tok_nexthexint(&line, &response[3]);
            if (err < 0) goto error;

            if(response[3] == 7)
                response[3] = 1;
        #endif

        break;
        default:
            goto error;
    }

#if RIL_VERSION >4
    if (request == RIL_REQUEST_DATA_REGISTRATION_STATE) {

        at_response_free(p_response);

        cmd = "AT^SYSINFO";
        prefix = "^SYSINFO:";

        err = at_send_command_singleline(cmd, prefix, &p_response);

        if (err != 0) goto error;

        count = 4;

        line = p_response->p_intermediates->line;

        err = at_tok_start(&line);
        if (err < 0) goto error;

        err = at_tok_nextint(&line, &skip);
        if (err < 0) goto error;

        err = at_tok_nextint(&line, &skip);
        if (err < 0) goto error;

        err = at_tok_nextint(&line, &skip);
        if (err < 0) goto error;

        err = at_tok_nextint(&line, &sysMode);
        if (err < 0) goto error;

        err = at_tok_nextint(&line, &skip);
        if (err < 0) goto error;

        err = at_tok_nextint(&line, &sysSubmode);
        if (err < 0) goto error;

        if(sysMode == 3)
        {
          if( sysSubmode == 3)
    	    response[3] = RADIO_TECH_EDGE;
    	  else
            response[3] = RADIO_TECH_GSM;

        }
        else if(sysMode == 4)
        {
       	  if(sysSubmode == 4)
       	  {
       		response[3] = RADIO_TECH_HSDPA;
       	  }
          else if(sysSubmode == 5)
    	  {
       		response[3] = RADIO_TECH_HSUPA;
    	  }
    	  else if(sysSubmode == 6)
    	  {
       	    response[3] = RADIO_TECH_HSPA;
    	  }
       }
#if RIL_VERSION > 9
       else if(sysMode == 5)
       {
    	 response[3] = RADIO_TECH_TD_SCDMA;
       }
#endif
       else if(sysMode == 17)
       {
    	 response[3] = RADIO_TECH_LTE;
       }
       else
       {
       	 count = 3;
       }
    }
#endif

    asprintf(&responseStr[0], "%d", response[0]);
    asprintf(&responseStr[1], "%x", response[1]);
    asprintf(&responseStr[2], "%x", response[2]);

    if (count > 3)
        asprintf(&responseStr[3], "%d", response[3]);

    RIL_onRequestComplete(t, RIL_E_SUCCESS, responseStr, count*sizeof(char*));
    at_response_free(p_response);

    return;
error:
    LOGE("requestRegistrationState must never return an error when radio is on");
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

static void requestOperator(void *data, size_t datalen, RIL_Token t)
{
    int err;
    int i;
    int skip;
    ATLine *p_cur;
    char *response[3];

    memset(response, 0, sizeof(response));

    ATResponse *p_response = NULL;

    err = at_send_command_multiline(
        "AT+COPS=3,0;+COPS?;+COPS=3,1;+COPS?;+COPS=3,2;+COPS?",
        "+COPS:", &p_response);

    /* we expect 3 lines here:
     * +COPS: 0,0,"T - Mobile"
     * +COPS: 0,1,"TMO"
     * +COPS: 0,2,"310170"
     */

    if (err != 0) goto error;

    for (i = 0, p_cur = p_response->p_intermediates
            ; p_cur != NULL
            ; p_cur = p_cur->p_next, i++
    ) {
        char *line = p_cur->line;

        err = at_tok_start(&line);
        if (err < 0) goto error;

        err = at_tok_nextint(&line, &skip);
        if (err < 0) goto error;

        // If we're unregistered, we may just get
        // a "+COPS: 0" response
        if (!at_tok_hasmore(&line)) {
            response[i] = NULL;
            continue;
        }

        err = at_tok_nextint(&line, &skip);
        if (err < 0) goto error;

        // a "+COPS: 0, n" response is also possible
        if (!at_tok_hasmore(&line)) {
            response[i] = NULL;
            continue;
        }

        err = at_tok_nextstr(&line, &(response[i]));
        if (err < 0) goto error;
    }

    if (i != 3) {
        /* expect 3 lines exactly */
        goto error;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, response, sizeof(response));
    at_response_free(p_response);

    return;
error:
    LOGE("requestOperator must not return error when radio is on");
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

static void requestSendSMS(void *data, size_t datalen, RIL_Token t)
{
    int err;
    const char *smsc;
    const char *pdu;
    int tpLayerLength;
    char *cmd1, *cmd2;
    RIL_SMS_Response response;
    ATResponse *p_response = NULL;

    smsc = ((const char **)data)[0];
    pdu = ((const char **)data)[1];

    tpLayerLength = strlen(pdu)/2;

    // "NULL for default SMSC"
    if (smsc == NULL) {
        smsc= "00";
    }

    asprintf(&cmd1, "AT+CMGS=%d", tpLayerLength);
    asprintf(&cmd2, "%s%s", smsc, pdu);

    err = at_send_command_sms(cmd1, cmd2, "+CMGS:", &p_response);

    if (err != 0 || p_response->success == 0) goto error;

    memset(&response, 0, sizeof(response));

    /* FIXME fill in messageRef and ackPDU */

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &response, sizeof(response));
    at_response_free(p_response);

    return;
error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}



int notifyDataCallProcessExit(void) {
#if 1
    if (bSetupDataCallCompelete) {
        RIL_requestTimedCallback (onDataCallExit, NULL, NULL);
        bSetupDataCallCompelete = 0;
        return 1;
    } else {
        nSetupDataCallFailTimes++;
        if (nSetupDataCallFailTimes > 3)
            return 1;
    }
#endif
    return 0;
}

static void requestSetupDataCall(void *data, size_t datalen, RIL_Token t)
{
    const char *apn;
    const char *user = NULL;
    const char *pass = NULL;
    const char *auth_type = NULL;
    const char *pdp_type = "IP";
    char ppp_number[20] = {'\0'};
    int cgreg_response[4];
    int cops_response[4];
    char *cmd;
    int err;
    int retry = 0;
    pid_t pppd_pid;
    ATResponse *p_response = NULL;
    ATResponse *p_pwd_response = NULL;
    char *line = NULL;
    char ppp_local_ip[PROPERTY_VALUE_MAX] = {'\0'};
    struct timeval begin_tv, end_tv;
    gettimeofday(&begin_tv, NULL);


    apn = ((const char **)data)[2];
    if (datalen > 3 * sizeof(char *))
        user = ((char **)data)[3];
    if (datalen > 4 * sizeof(char *))
        pass = ((char **)data)[4];
    if (datalen > 5 * sizeof(char *))
        auth_type = ((const char **)data)[5]; // 0 ~ NONE, 1 ~ PAP, 1 ~ CHAP, 3 ~ PAP / CHAP
    if (datalen > 6 * sizeof(char *))
        pdp_type = ((const char **)data)[6];

    LOGI("*************************************");
    LOGI("USER:%s",user);
    LOGI("PASS:%s",pass);
    LOGI("auth_type:%s",auth_type);
    LOGI("pdp_type:%s",pdp_type);
    LOGI("*************************************");

#if 1 // quectel Set DTR Function Mode
//ON->OFF on DTR: Disconnect data call, change to command mode. During state DTR = OFF, auto-answer function is disabled
    at_send_command("AT&D2", NULL);
#endif
#ifdef USB_HOST_USE_RESET_RESUME
        at_send_command("AT&D0", NULL);
#endif

    bSetupDataCallCompelete = 0;
    nSetupDataCallFailTimes = 0;

#if 0
     //Make sure there is no existing connection or pppd instance running
    if (!strncmp(PPP_TTY_PATH, "ppp", 3))
        pppd_stop(SIGTERM);
    else
        ndis_stop(SIGTERM);
#endif

    if (isRadioOn() != 1) {
        LOGE("raido is off!");
        goto error;
    }

    if(!netwrokDataReady())
        goto error;

    {
    	LOGD("requesting data connection in");

        if (!user )
            user = "";

        if (!pass )
            pass="";

        if (!auth_type || auth_type[0] == '0')
            auth_type = "2"; //chap

        s_default_pdp = CONFIG_DEFAULT_PDP;

    if (!ppp_number[0])
        snprintf(ppp_number, sizeof(ppp_number), "*99***%d#", s_default_pdp);

	if(strlen(apn) <=1)
		{

			LOGD("chinge ppp_number atd*99# ");
			s_default_pdp = 1;
			snprintf(ppp_number, sizeof(ppp_number), "*99#");
		}

    LOGD("requesting data connection to APN '%s'!\n", apn);


#if 0
        asprintf(&cmd, "AT+CGDCONT=%d,\"%s\",\"%s\"", s_default_pdp, pdp_type, apn);

        //FIXME check for error here
        err = at_send_command(cmd, NULL);
        free(cmd);
#endif

        /* start the gprs pppd */
        pppd_pid = pppd_start(s_default_pdp, (char*)pdp_type, (char*)apn, NULL, user, pass, auth_type, ppp_number);
        LOGD("pppd_start %d!\n", pppd_pid);
    }

    if (pppd_pid < 0)
        goto error;

    sleep(1);
    while (!s_closed && (retry++ < 50) && (nSetupDataCallFailTimes <= 3)) {

        if ((waitpid(pppd_pid, NULL, WNOHANG)) == pppd_pid)
            goto error;


	 if(!pppd_running())
		goto error;

        get_local_ip(ppp_local_ip);

        LOGD("[%d] trying to get_local_ip ... %s", retry, ppp_local_ip);
        if(strcmp(ppp_local_ip, "0.0.0.0"))
            break;
        sleep(1);
    }

    gettimeofday(&end_tv, NULL);
    LOGD("get_local_ip: %s, cost %ld sec", ppp_local_ip, (end_tv.tv_sec - begin_tv.tv_sec));

    if (!strcmp(ppp_local_ip, "0.0.0.0")) {
        goto error;
    }

    if (system("/system/bin/ip route add default dev ppp0 table ppp0") != 0)
    {
        LOGD("add route failed");
    }

    //Save the current active ip address
    strcpy(sKeepLocalip, ppp_local_ip);


#if RIL_VERSION<=4

    char* response[3];
    ATLine* p_cur;
    char *out;
	char chackBear[20] = {'\0'};

    at_response_free(p_response);
	snprintf(chackBear, sizeof(chackBear), "AT+CGCONTRDP=%d", s_default_pdp);

    err = at_send_command_multiline (chackBear, "+CGCONTRDP:", &p_response);
    if (err != 0 || p_response->success == 0) {
        goto error;
    }


    for (p_cur = p_response->p_intermediates; p_cur != NULL;
         p_cur = p_cur->p_next) {
        char *line = p_cur->line;
        int cid;

        err = at_tok_start(&line);
        if (err < 0)
            goto error;


        err = at_tok_nextint(&line, &cid);
        if (err < 0)
            goto error;

        response[0] =  alloca(10);
        sprintf(response[0], "%d", cid);
        // Assume no error

        response[1] = "ppp0";

        //bearer id
        err = at_tok_nextint(&line, &cid);
        if (err < 0)
        {
            goto error;
        }


        // APN ignored for v5
        err = at_tok_nextstr(&line, &out);
        if (err < 0)
        {
            goto error;
        }


        //local address
        err = at_tok_nextstr(&line, &out);
        if (err < 0)
            goto error;

        response[2] =  alloca(strlen(out) + 1);
        strcpy(response[2], out);

        break;
    }

#else
    requestOrSendDataCallList(&t);
#endif

    at_response_free(p_response);
    at_response_free(p_pwd_response);

    bSetupDataCallCompelete = 1;
    nSetupDataCallFailTimes = 0;

    return;

error:

#if 0
    if (!strncmp(PPP_TTY_PATH, "ppp", 3))
        pppd_stop(SIGTERM);
    else
        ndis_stop(SIGTERM);
#endif

    LOGE("Unable to setup PDP in %s\n", __func__);
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
    at_response_free(p_pwd_response);
}

static void requestSMSAcknowledge(void *data, size_t datalen, RIL_Token t)
{
    int ackSuccess;
    int err;

    ackSuccess = ((int *)data)[0];

    if (ackSuccess == 1) {
        err = at_send_command("AT+CNMA=1", NULL);
    } else if (ackSuccess == 0)  {
        err = at_send_command("AT+CNMA=2", NULL);
    } else {
        LOGE("unsupported arg to RIL_REQUEST_SMS_ACKNOWLEDGE\n");
        goto error;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);

}

extern void convertUSimFcp(char* fcp, int len, char* out);
char getResponseResult[31];
char* readBinaryResult;

#define READ_BINARY_BLOCK_LEN (255)

void requestReadBinary(void *data, size_t datalen, RIL_Token t)
{
    ATResponse *p_response = NULL;
    RIL_SIM_IO_Response sr;
    int err;
    char *cmd = NULL;
#if RIL_VERSION <= 4
    RIL_SIM_IO *p_args;
#else
	#if RIL_VERSION < 12
			RIL_SIM_IO_v5 *p_args;
	#else
			RIL_SIM_IO_v6 *p_args;
	#endif
#endif
    char *line;
    int read_full_binary_times;
    bool need_read_binary_part;
    int pos = 0;
    int length = 0;
    int remain = 0;

    memset(&sr, 0, sizeof(sr));

#if RIL_VERSION <= 4
    p_args = (RIL_SIM_IO *)data;
#else
	#if RIL_VERSION < 12
			p_args = (RIL_SIM_IO_v5 *)data;
	#else
			p_args = (RIL_SIM_IO_v6 *)data;
	#endif
#endif
    /* FIXME handle pin2 */

    if(readBinaryResult != NULL)
    {
        free(readBinaryResult);
    }

    readBinaryResult = (char*)malloc(p_args->p3 * 2 + 1);
    memset(readBinaryResult, 0, p_args->p3* 2 + 1);

    remain = p_args->p3;
    pos = ((p_args->p1 << 8) | p_args->p2);

    while(1)
    {
        length = (remain <= READ_BINARY_BLOCK_LEN) ? remain : READ_BINARY_BLOCK_LEN;

        remain -= length;

        asprintf(&cmd, "AT+CRSM=%d,%d,%d,%d,%d",
                    p_args->command, p_args->fileid,
                    (pos >> 8), (pos & 0xff), length);


        err = at_send_command_singleline(cmd, "+CRSM:", &p_response);

        if (err < 0 || p_response->success == 0) {
            goto error;
        }

        line = p_response->p_intermediates->line;

        err = at_tok_start(&line);
        if (err < 0) goto error;

        err = at_tok_nextint(&line, &(sr.sw1));
        if (err < 0) goto error;

        err = at_tok_nextint(&line, &(sr.sw2));
        if (err < 0) goto error;

        if (at_tok_hasmore(&line)) {
            err = at_tok_nextstr(&line, &(sr.simResponse));
            if (err < 0) goto error;
        }

        if(sr.simResponse != NULL)
        {
        	strcat(readBinaryResult, sr.simResponse);
        }

        pos += length;
        if(remain <= 0)
            break;
    }

    sr.simResponse = readBinaryResult;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &sr, sizeof(sr));
    at_response_free(p_response);
    free(cmd);

    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
    free(cmd);
    free(readBinaryResult);
    readBinaryResult = NULL;

}
static void  requestSIM_IO(void *data, size_t datalen, RIL_Token t)
{
    ATResponse *p_response = NULL;
    RIL_SIM_IO_Response sr;
    sr.simResponse = NULL;
    int err;
    char *cmd = NULL;

#if RIL_VERSION <= 4
    RIL_SIM_IO *p_args;
#else
	#if RIL_VERSION < 12
		RIL_SIM_IO_v5 *p_args;
	#else
		RIL_SIM_IO_v6 *p_args;
	#endif
#endif

    char *line;

    memset(&sr, 0, sizeof(sr));

#if RIL_VERSION <= 4
    p_args = (RIL_SIM_IO *)data;
#else
	#if RIL_VERSION < 12
		p_args = (RIL_SIM_IO_v5 *)data;
	#else
		p_args = (RIL_SIM_IO_v6 *)data;
	#endif
#endif

    /* FIXME handle pin2 */

    if(p_args->command == 0xB0)
    {
        requestReadBinary(data, datalen, t);
        return;
    }

    if(p_args->command == 0XC0)
        p_args->p3 = 255;

    if (p_args->data == NULL) {
        asprintf(&cmd, "AT+CRSM=%d,%d,%d,%d,%d",
                    p_args->command, p_args->fileid,
                    p_args->p1, p_args->p2, p_args->p3);
    } else {
        asprintf(&cmd, "AT+CRSM=%d,%d,%d,%d,%d,%s",
                    p_args->command, p_args->fileid,
                    p_args->p1, p_args->p2, p_args->p3, p_args->data);
    }

    err = at_send_command_singleline(cmd, "+CRSM:", &p_response);

    if (err < 0 || p_response->success == 0) {
        goto error;
    }

    line = p_response->p_intermediates->line;

    err = at_tok_start(&line);
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &(sr.sw1));
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &(sr.sw2));
    if (err < 0) goto error;

    if (at_tok_hasmore(&line)) {
        err = at_tok_nextstr(&line, &(sr.simResponse));
        if (err < 0) goto error;
    }
	#ifdef AIRM2M_REPORT_SIGNAL_STRENGTH
	    if ((p_args->fileid == 0x2fe2) && (p_args->command == 0xb0)) {
    	    requestSignalStrength(NULL, 0, NULL);
    	}
	#endif

#if RIL_VERSION > 6
    if(p_args->command == 0xc0 && sr.sw1 == 0x69 && sr.sw2 == 0x85 &&  p_args->fileid == 0x2fe2)   //���⿨�޷���ȡICCIC FILE �����޷�����
    {
		sr.sw1 =  0x90;
		sr.sw2 =  0x00;
        sr.simResponse = "62178202412183022FE28A01058B032F06038002000A880110";
    }


	if(p_args->command == 0XC0 && sr.sw1 == 0x90 && sr.sw2 == 0x00 )
	{
        if(sr.simResponse  == NULL)
            sr.simResponse = "621982054221001CFA83026F3A8A01058B036F060D80021B588800";
        else
        {
            convertUSimFcp(sr.simResponse, strlen(sr.simResponse), getResponseResult);
            sr.simResponse = getResponseResult;
        }
    }
#endif

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &sr, sizeof(sr));
    at_response_free(p_response);
    free(cmd);

    return;
error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
    free(cmd);

}

static void  requestEnterSimPin(void*  data, size_t  datalen, RIL_Token  t)
{
    ATResponse   *p_response = NULL;
    int           err;
    char*         cmd = NULL;
    const char**  strings = (const char**)data;;

    if ( datalen == sizeof(char*) ) {
        asprintf(&cmd, "AT+CPIN=%s", strings[0]);
    } else if ( datalen == 2*sizeof(char*) ) {
        asprintf(&cmd, "AT+CPIN=%s,%s", strings[0], strings[1]);
    } else
        goto error;

    err = at_send_command_singleline(cmd, "+CPIN:", &p_response);
    free(cmd);

	int remaining_times = 100;

    if (err < 0 || p_response->success == 0) {
error:
        RIL_onRequestComplete(t, RIL_E_PASSWORD_INCORRECT, &remaining_times, sizeof(remaining_times));
    } else {
        RIL_onRequestComplete(t, RIL_E_SUCCESS, &remaining_times, sizeof(remaining_times));
    }
    at_response_free(p_response);
}


static void  requestSendUSSD(void *data, size_t datalen, RIL_Token t)
{
    const char *ussdRequest;

    ussdRequest = (char *)(data);


    RIL_onRequestComplete(t, RIL_E_REQUEST_NOT_SUPPORTED, NULL, 0);

// @@@ TODO

}


static int32_t net2pmask[] = {
    4,                          // 0  - GSM / WCDMA Pref
    0,                                             // 1  - GSM only
    1,                                           // 2  - WCDMA only
    2,                                 // 3  - GSM / WCDMA Auto
    0,                                 // 4  - CDMA / EvDo Auto
    0,                                            // 5  - CDMA only
    0,                                            // 6  - EvDo only
    2,           // 7  - GSM/WCDMA, CDMA, EvDo
    9,                       // 8  - LTE, CDMA and EvDo
    15,                       // 9  - LTE, GSM/WCDMA
    15, // 10 - LTE, CDMA, EvDo, GSM/WCDMA
    5,                                             // 11 - LTE only
    11,  //12; /* LTE/WCDMA */
    1, //13; /* TD-SCDMA only */
     1,  //14; /* TD-SCDMA and WCDMA */
    11, //15/* TD-SCDMA and LTE */
    4, //16; /* TD-SCDMA and GSM */
    15, //17; /* TD-SCDMA,GSM and LTE */
    4, //18; /* TD-SCDMA, GSM/WCDMA */    11,//19; /* TD-SCDMA, WCDMA and LTE */
    11,//19; /* TD-SCDMA, WCDMA and LTE */
    15,//20; /* TD-SCDMA, GSM/WCDMA and LTE */
    4,//21; /*TD-SCDMA,EvDo,CDMA,GSM/WCDMA*/
    15,//22; /* TD-SCDMA/LTE/GSM/WCDMA, CDMA, and EvDo */
};

static void requestSetPreferredNetworkType(void *data, size_t datalen, RIL_Token t)
{
    int rat,err;
    int nwscanmode = 0; /* AUTO */
    char *cmd;
    char *line = NULL;
    int cur_scanmode = 0;
    int value = *(int *)data;
    ATResponse   *p_response = NULL;
    int mode ;
    int retry_count = 0;

    char networkValue[PROPERTY_VALUE_MAX];
    assert (datalen >= sizeof(int *));
    rat = ((int *)data)[0];


//    #if (RIL_VERSION < 6)
//    {
        if (t != NULL)
            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
        return;
//    }
//    #endif
    #if 0
    {
        int32_t preferred = net2pmask[value];
		if(preferred == 4 || preferred == 0 || preferred == 2 || preferred == 15 )
		{
			Include_GSM = 1;
		}
		else
		{
			Include_GSM = 0;
		}
	#if !defined(RIL_SET_PREFERRED_NETWORK_SUPPORTED)
		preferred = 15;
	#endif

        err = at_send_command_singleline("AT*BAND?", "*BAND:", &p_response);
        line = p_response->p_intermediates->line;

        err = at_tok_start(&line);

        if (err < 0 || p_response->success == 0) {
            at_response_free(p_response);
            RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            return;
        }

        at_tok_nextint(&line, &mode);

        at_response_free(p_response);

        if(mode == preferred)
        {
            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            return ;
        }

		#if (BAND_SET == 1)

	        while(retry_count < 3)
	        {
	            asprintf(&cmd, "AT*BAND=%d", preferred);
	            err = at_send_command(cmd, &p_response);
	            free(cmd);

	            pppd_stop(SIGTERM);
	            sleep(2);

	            if (err < 0 || p_response->success == 0) {
	                sleep(5);
	                at_response_free(p_response);
	            } else {
	                RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
	                at_response_free(p_response);
	                break;
	            }

	            retry_count++;
	        }
		#endif

        if(retry_count >= 3)
        {
            at_response_free(p_response);
            RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        }

#if 0
        sprintf(networkValue, "%d", value);
        property_set("ro.telephony.default_network", networkValue);
        property_get("ro.telephony.default_network", networkValue,"99");
        LOGD("ro.telephony.default_network %s", networkValue);
#endif
    }

    #endif

}


/*** Callback methods from the RIL library to us ***/

/**
 * Call from RIL to us to make a RIL_REQUEST
 *
 * Must be completed with a call to RIL_onRequestComplete()
 *
 * RIL_onRequestComplete() may be called from any thread, before or after
 * this function returns.
 *
 * Will always be called from the same thread, so returning here implies
 * that the radio is ready to process another command (whether or not
 * the previous command has completed).
 */
static void
onRequest (int request, void *data, size_t datalen, RIL_Token t)
{
    ATResponse *p_response;
    int err;

    LOGD("onRequest: %s %d", requestToString(request), sState);

    /* Ignore all requests except RIL_REQUEST_GET_SIM_STATUS
     * when RADIO_STATE_UNAVAILABLE.
     */
    if (sState == RADIO_STATE_UNAVAILABLE
        && request != RIL_REQUEST_GET_SIM_STATUS
        && request != RIL_REQUEST_SET_PREFERRED_NETWORK_TYPE
        && request != RIL_REQUEST_BASEBAND_VERSION
    #ifdef RIL_REQUEST_GET_RADIO_CAPABILITY
        && request != RIL_REQUEST_GET_RADIO_CAPABILITY
    #endif
        && request != RIL_REQUEST_GET_IMEI
#ifdef RIL_REQUEST_DEVICE_IDENTITY
       && request != RIL_REQUEST_DEVICE_IDENTITY
#endif
    ) {
        RIL_onRequestComplete(t, RIL_E_RADIO_NOT_AVAILABLE, NULL, 0);
        return;
    }
	#if 1
		onRequestCount++;
	#endif

    /* Ignore all non-power requests when RADIO_STATE_OFF
     * (except RIL_REQUEST_GET_SIM_STATUS)
     */
    if (sState == RADIO_STATE_OFF
        && !(request == RIL_REQUEST_RADIO_POWER
            || request == RIL_REQUEST_GET_SIM_STATUS
            || request == RIL_REQUEST_SET_PREFERRED_NETWORK_TYPE
            || request == RIL_REQUEST_BASEBAND_VERSION
    #ifdef RIL_REQUEST_GET_RADIO_CAPABILITY
            ||  request == RIL_REQUEST_GET_RADIO_CAPABILITY
    #endif
            || request == RIL_REQUEST_GET_IMEI
#ifdef RIL_REQUEST_DEVICE_IDENTITY
            || request == RIL_REQUEST_DEVICE_IDENTITY
#endif
        )
    ) {
        RIL_onRequestComplete(t, RIL_E_RADIO_NOT_AVAILABLE, NULL, 0);
        return;
    }

    switch (request) {
        case RIL_REQUEST_GET_SIM_STATUS: {

            char *p_buffer;
            int buffer_size;
#if (RIL_VERSION<=4)
             RIL_CardStatus *p_card_status;
            int result = getCardStatus(&p_card_status);
#else
			RIL_CardStatus_v6 *p_card_status;
			#if RIL_VERSION < 12
            	int result = getCardStatusV6(&p_card_status);
			#else
				int result = getCardStatusV12(&p_card_status);
			#endif


#endif

   	    if (result == RIL_E_SUCCESS) {
                p_buffer = (char *)p_card_status;
                buffer_size = sizeof(*p_card_status);
            } else {
                p_buffer = NULL;
                buffer_size = 0;
            }
            RIL_onRequestComplete(t, result, p_buffer, buffer_size);

#if (RIL_VERSION<=4)
	        freeCardStatus(p_card_status);
#else
			#if RIL_VERSION < 12
    	        freeCardStatusV6(p_card_status);
			#else
				freeCardStatusV6(p_card_status);
			#endif
#endif
            break;
        }
        case RIL_REQUEST_GET_CURRENT_CALLS:
            // no response to get current calls
            //requestGetCurrentCalls(data, datalen, t);
            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            break;
        case RIL_REQUEST_DIAL:
            requestDial(data, datalen, t);
            break;
        case RIL_REQUEST_HANGUP:
            requestHangup(data, datalen, t);
            break;
        case RIL_REQUEST_HANGUP_WAITING_OR_BACKGROUND:
            // 3GPP 22.030 6.5.5
            // "Releases all held calls or sets User Determined User Busy
            //  (UDUB) for a waiting call."
            at_send_command("AT+CHLD=0", NULL);

            /* success or failure is ignored by the upper layer here.
               it will call GET_CURRENT_CALLS and determine success that way */
            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            break;
        case RIL_REQUEST_HANGUP_FOREGROUND_RESUME_BACKGROUND:
            // 3GPP 22.030 6.5.5
            // "Releases all active calls (if any exist) and accepts
            //  the other (held or waiting) call."
            at_send_command("AT+CHLD=1", NULL);

            /* success or failure is ignored by the upper layer here.
               it will call GET_CURRENT_CALLS and determine success that way */
            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            break;
        case RIL_REQUEST_SWITCH_WAITING_OR_HOLDING_AND_ACTIVE:
            // 3GPP 22.030 6.5.5
            // "Places all active calls (if any exist) on hold and accepts
            //  the other (held or waiting) call."
            at_send_command("AT+CHLD=2", NULL);

#ifdef WORKAROUND_ERRONEOUS_ANSWER
            s_expectAnswer = 1;
#endif /* WORKAROUND_ERRONEOUS_ANSWER */

            /* success or failure is ignored by the upper layer here.
               it will call GET_CURRENT_CALLS and determine success that way */
            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            break;
        case RIL_REQUEST_ANSWER:
            at_send_command("ATA", NULL);

#ifdef WORKAROUND_ERRONEOUS_ANSWER
            s_expectAnswer = 1;
#endif /* WORKAROUND_ERRONEOUS_ANSWER */

            /* success or failure is ignored by the upper layer here.
               it will call GET_CURRENT_CALLS and determine success that way */
            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            break;
        case RIL_REQUEST_CONFERENCE:
            // 3GPP 22.030 6.5.5
            // "Adds a held call to the conversation"
            at_send_command("AT+CHLD=3", NULL);

            /* success or failure is ignored by the upper layer here.
               it will call GET_CURRENT_CALLS and determine success that way */
            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            break;
        case RIL_REQUEST_UDUB:
            /* user determined user busy */
            /* sometimes used: ATH */
            at_send_command("ATH", NULL);

            /* success or failure is ignored by the upper layer here.
               it will call GET_CURRENT_CALLS and determine success that way */
            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            break;

        case RIL_REQUEST_SEPARATE_CONNECTION:
            {
                char  cmd[12];
                int   party = ((int*)data)[0];

                // Make sure that party is in a valid range.
                // (Note: The Telephony middle layer imposes a range of 1 to 7.
                // It's sufficient for us to just make sure it's single digit.)
                if (party > 0 && party < 10) {
                    sprintf(cmd, "AT+CHLD=2%d", party);
                    at_send_command(cmd, NULL);
                    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
                } else {
                    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
                }
            }
            break;

        case RIL_REQUEST_SIGNAL_STRENGTH:
            requestSignalStrength(data, datalen, t);
            break;

		case RIL_REQUEST_GET_ACTIVITY_INFO:
			RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            break;

    #if RIL_VERSION <= 4
        case RIL_REQUEST_REGISTRATION_STATE:
        case RIL_REQUEST_GPRS_REGISTRATION_STATE:

    #else
        case RIL_REQUEST_VOICE_REGISTRATION_STATE:
        case RIL_REQUEST_DATA_REGISTRATION_STATE:
    #endif
            requestRegistrationState(request, data, datalen, t);
			#ifdef AIRM2M_REPORT_SIGNAL_STRENGTH
		        if (RIL_REQUEST_VOICE_REGISTRATION_STATE == request) {
    	            requestSignalStrength(NULL, 0, NULL);
        	    }
			#endif
            break;
        case RIL_REQUEST_OPERATOR:
            requestOperator(data, datalen, t);
            break;
        case RIL_REQUEST_RADIO_POWER:
			#if 1       //frameworks\base\telephony\java\com\android\internal\telephony/RIL.java ->processUnsolicited()
						//case RIL_UNSOL_RIL_CONNECTED : setRadioPower(false, null);
						//it it no need to power off radio when RIL.java connect
		        if ((onRequestCount < 4) && (((int *)data)[0] == 0)) {
		            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
		            break;
		        }
			#endif
            requestRadioPower(data, datalen, t);
            break;
        case RIL_REQUEST_DTMF: {
            char c = ((char *)data)[0];
            char *cmd;
            asprintf(&cmd, "AT+VTS=%c", (int)c);
            at_send_command(cmd, NULL);
            free(cmd);
            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            break;
        }
        case RIL_REQUEST_SEND_SMS:
            requestSendSMS(data, datalen, t);
            break;
        case RIL_REQUEST_SETUP_DATA_CALL:
            requestSetupDataCall(data, datalen, t);
            break;

	case RIL_REQUEST_DEACTIVATE_DATA_CALL:
		requestDeactivateDataCall(data, datalen, t);
	      break;

	case RIL_REQUEST_SMS_ACKNOWLEDGE:
            requestSMSAcknowledge(data, datalen, t);
            break;

        case RIL_REQUEST_GET_IMSI:
            p_response = NULL;
            err = at_send_command_numeric("AT+CIMI", &p_response);

            if (err < 0 || p_response->success == 0) {
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            } else {
                RIL_onRequestComplete(t, RIL_E_SUCCESS,
                    p_response->p_intermediates->line, sizeof(char *));
            }
            at_response_free(p_response);
            break;

        case RIL_REQUEST_GET_IMEI:

            p_response = NULL;
            err = at_send_command_numeric("AT+CGSN", &p_response);

            if (err < 0 || p_response->success == 0) {
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            } else {
                RIL_onRequestComplete(t, RIL_E_SUCCESS,
                    p_response->p_intermediates->line, sizeof(char *));
            }
            at_response_free(p_response);
            break;

#ifdef RIL_REQUEST_DEVICE_IDENTITY
        case RIL_REQUEST_DEVICE_IDENTITY:
            p_response = NULL;
            err = at_send_command_numeric("AT+CGSN", &p_response);

            if (err < 0 || p_response->success == 0) {
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            } else {
                char* response[4];
                response[0] = p_response->p_intermediates->line;
                response[1] = response[2] = response[3] = "";
                RIL_onRequestComplete(t, RIL_E_SUCCESS, response, 4 * sizeof(char *));
            }
            at_response_free(p_response);
            break;
#endif

        case RIL_REQUEST_BASEBAND_VERSION:
  	      requestBaseBandVersion(data, datalen, t);
  	     break;

        case RIL_REQUEST_SIM_IO:
            requestSIM_IO(data,datalen,t);
			#ifdef AIRM2M_REPORT_SIGNAL_STRENGTH
				requestSignalStrength(NULL, 0, NULL);
			#endif
            break;

        case RIL_REQUEST_SEND_USSD:
            requestSendUSSD(data, datalen, t);
            break;

        case RIL_REQUEST_CANCEL_USSD:
            p_response = NULL;
            err = at_send_command_numeric("AT+CUSD=2", &p_response);

            if (err < 0 || p_response->success == 0) {
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            } else {
                RIL_onRequestComplete(t, RIL_E_SUCCESS,
                    p_response->p_intermediates->line, sizeof(char *));
            }
            at_response_free(p_response);
            break;

        case RIL_REQUEST_SET_NETWORK_SELECTION_AUTOMATIC:
        #if 0
            at_send_command("AT+COPS=0", NULL);
        #endif

		        RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            break;

        case RIL_REQUEST_DATA_CALL_LIST:
            requestDataCallList(data, datalen, t);
            break;

        case RIL_REQUEST_SET_PREFERRED_NETWORK_TYPE:
            if (currentState() <= RADIO_STATE_UNAVAILABLE) {
                RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
                break;
            }
            requestSetPreferredNetworkType(data,datalen,t);
	        break;

        case RIL_REQUEST_REPORT_STK_SERVICE_IS_RUNNING:

    #if RIL_VERSION > 4
	case RIL_REQUEST_CDMA_SET_SUBSCRIPTION_SOURCE:
		RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
		break;
    #endif

#ifdef RIL_REQUEST_GET_RADIO_CAPABILITY
        case RIL_REQUEST_GET_RADIO_CAPABILITY: {
                RIL_RadioCapability RadioCapability = {
                    .version = RIL_RADIO_CAPABILITY_VERSION,    // Version of structure, RIL_RADIO_CAPABILITY_VERSION
                    .session = 0,                               // Unique session value defined by framework returned in all "responses/unsol"
                    .phase = RC_PHASE_CONFIGURED,               // CONFIGURED, START, APPLY, FINISH
                    .rat = 0,                                   // RIL_RadioAccessFamily for the radio
                    .logicalModemUuid = "com.airm2m.lm0",
                    .status =RC_STATUS_SUCCESS,             // Return status and an input parameter for RC_PHASE_FINISH
                };

                RadioCapability.rat |= RAF_GSM | RAF_GPRS |RAF_EDGE;
                RadioCapability.rat |= RAF_UMTS | RAF_HSDPA | RAF_HSUPA |RAF_HSPA | RAF_HSPAP; //3G
                RadioCapability.rat |= RAF_TD_SCDMA;
                RadioCapability.rat |= RAF_LTE; //4G

                RIL_onRequestComplete(t, RIL_E_SUCCESS, &RadioCapability, sizeof(RIL_RadioCapability));
            }
            break;
#endif

        case RIL_REQUEST_QUERY_NETWORK_SELECTION_MODE:
            requestQueryNetworkSelectionMode(data, datalen, t);
            break;

        case RIL_REQUEST_OEM_HOOK_RAW:
            // echo back data
            RIL_onRequestComplete(t, RIL_E_SUCCESS, data, datalen);
            break;
		#if RIL_VERSION > 9
			case RIL_REQUEST_SET_RADIO_CAPABILITY:
				RIL_onRequestComplete(t, RIL_E_SUCCESS, data, datalen);
	            break;
		#endif

        case RIL_REQUEST_OEM_HOOK_STRINGS: {
            int i;
            const char ** cur;

            LOGD("got OEM_HOOK_STRINGS: 0x%8p %lu", data, (long)datalen);


            for (i = (datalen / sizeof (char *)), cur = (const char **)data ;
                    i > 0 ; cur++, i --) {
                LOGD("> '%s'", *cur);
            }

            // echo back strings
            RIL_onRequestComplete(t, RIL_E_SUCCESS, data, datalen);
            break;
        }

        case RIL_REQUEST_WRITE_SMS_TO_SIM:
            requestWriteSmsToSim(data, datalen, t);
            break;

        case RIL_REQUEST_DELETE_SMS_ON_SIM: {
            char * cmd;
            p_response = NULL;
            asprintf(&cmd, "AT+CMGD=%d", ((int *)data)[0]);
            err = at_send_command(cmd, &p_response);
            free(cmd);
            if (err < 0 || p_response->success == 0) {
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            } else {
                RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            }
            at_response_free(p_response);
            break;
        }

        case RIL_REQUEST_ENTER_SIM_PIN:
        case RIL_REQUEST_ENTER_SIM_PUK:
        case RIL_REQUEST_ENTER_SIM_PIN2:
        case RIL_REQUEST_ENTER_SIM_PUK2:
        case RIL_REQUEST_CHANGE_SIM_PIN:
        case RIL_REQUEST_CHANGE_SIM_PIN2:
            requestEnterSimPin(data, datalen, t);
            break;

        default:
        LOGD("onRequest: %s not supported", requestToString(request));

            RIL_onRequestComplete(t, RIL_E_REQUEST_NOT_SUPPORTED, NULL, 0);
            break;
    }
}

/**
 * Synchronous call from the RIL to us to return current radio state.
 * RADIO_STATE_UNAVAILABLE should be the initial state.
 */
static RIL_RadioState
currentState()
{
    #if RIL_VERSION>4
		#if RIL_VERSION < 12
		    if(sState > RADIO_STATE_SIM_READY)
		        return RADIO_STATE_ON;
		#else
			if(sState == RADIO_STATE_ON)
		        return RADIO_STATE_ON;
		#endif
    #endif

    return sState;
}
/**
 * Call from RIL to us to find out whether a specific request code
 * is supported by this implementation.
 *
 * Return 1 for "supported" and 0 for "unsupported"
 */

static int
onSupports (int requestCode)
{
    //@@@ todo

    return 1;
}

static void onCancel (RIL_Token t)
{
    //@@@todo

}

static const char * getVersion(void)
{
#if 1
	onRequestCount = 0; //onNewCommandConnect will call this function, and RIL.java will send RIL_REQUEST_RADIO_POWER
#endif

    return "android reference-luat-ril 1.1";
}

static void
setRadioState(RIL_RadioState newState)
{
    RIL_RadioState oldState;

    pthread_mutex_lock(&s_state_mutex);

    LOGD("setRadioState %d", newState);
    oldState = sState;

    if (s_closed > 0) {
        // If we're closed, the only reasonable state is
        // RADIO_STATE_UNAVAILABLE
        // This is here because things on the main thread
        // may attempt to change the radio state after the closed
        // event happened in another thread
        newState = RADIO_STATE_UNAVAILABLE;
    }

    if (sState != newState || s_closed > 0) {
        sState = newState;

        pthread_cond_broadcast (&s_state_cond);
    }

    pthread_mutex_unlock(&s_state_mutex);


    /* do these outside of the mutex */
    if (sState != oldState) {
        RIL_onUnsolicitedResponse (RIL_UNSOL_RESPONSE_RADIO_STATE_CHANGED,
                                    NULL, 0);

        /* FIXME onSimReady() and onRadioPowerOn() cannot be called
         * from the AT reader thread
         * Currently, this doesn't happen, but if that changes then these
         * will need to be dispatched on the request thread
         */
        #if RIL_VERSION < 12
	        if (sState == RADIO_STATE_SIM_READY) {
	            onSIMReady();
	        }
		#else
		     RIL_onUnsolicitedResponse (RIL_UNSOL_RESPONSE_SIM_STATUS_CHANGED,
                                    NULL, 0);
			if (sState == RADIO_STATE_ON) {
	            onRadioPowerOn();
	        }
		#endif
    }
}

/** Returns SIM_NOT_READY on error */
static SIM_Status
getSIMStatus()
{
    ATResponse *p_response = NULL;
    int err;
    int ret;
    char *cpinLine;
    char *cpinResult;

    if (sState == RADIO_STATE_OFF || sState == RADIO_STATE_UNAVAILABLE) {
        ret = SIM_NOT_READY;
        goto done;
    }

    err = at_send_command_singleline("AT+CPIN?", "+CPIN:", &p_response);

    if (err != 0) {
		LOGI("getSIMStatus try aging\n");                                        //ĳЩ�豸�����ȽϿ�.��ʱ�п��ܵ�һ��û�л�ȡ��SIM���������º�����������
		err = at_send_command_singleline("AT+CPIN?", "+CPIN:", &p_response);
		if (err != 0) {
        ret = SIM_NOT_READY;
        goto done;
		}
    }

    switch (at_get_cme_error(p_response)) {
        case CME_SUCCESS:
            break;

        case CME_SIM_NOT_INSERTED:
            ret = SIM_ABSENT;
            goto done;

        default:
            ret = SIM_NOT_READY;
            goto done;
    }

    /* CPIN? has succeeded, now look at the result */

    cpinLine = p_response->p_intermediates->line;
    err = at_tok_start (&cpinLine);

    if (err < 0) {
        ret = SIM_NOT_READY;
        goto done;
    }

    err = at_tok_nextstr(&cpinLine, &cpinResult);

    if (err < 0) {
        ret = SIM_NOT_READY;
        goto done;
    }

    if (0 == strcmp (cpinResult, "SIM PIN")) {
        ret = SIM_PIN;
        goto done;
    } else if (0 == strcmp (cpinResult, "SIM PUK")) {
        ret = SIM_PUK;
        goto done;
    } else if (0 == strcmp (cpinResult, "PH-NET PIN")) {
        return SIM_NETWORK_PERSONALIZATION;
    } else if (0 != strcmp (cpinResult, "READY"))  {
        /* we're treating unsupported lock types as "sim absent" */
        ret = SIM_ABSENT;
        goto done;
    }

    at_response_free(p_response);
    p_response = NULL;
    cpinResult = NULL;

    ret = SIM_READY;

done:
    at_response_free(p_response);
    return ret;
}



#if RIL_VERSION<=4
static void freeCardStatus(RIL_CardStatus *p_card_status) {
    free(p_card_status);
}
static int getCardStatus(RIL_CardStatus **pp_card_status) {
    static RIL_AppStatus app_status_array[] = {
        // SIM_ABSENT = 0
        { RIL_APPTYPE_UNKNOWN, RIL_APPSTATE_UNKNOWN, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN },
        // SIM_NOT_READY = 1
        { RIL_APPTYPE_SIM, RIL_APPSTATE_DETECTED, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN },
        // SIM_READY = 2
        { RIL_APPTYPE_SIM, RIL_APPSTATE_READY, RIL_PERSOSUBSTATE_READY,
          NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN },
        // SIM_PIN = 3
        { RIL_APPTYPE_SIM, RIL_APPSTATE_PIN, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_ENABLED_NOT_VERIFIED, RIL_PINSTATE_UNKNOWN },
        // SIM_PUK = 4
        { RIL_APPTYPE_SIM, RIL_APPSTATE_PUK, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_ENABLED_BLOCKED, RIL_PINSTATE_UNKNOWN },
        // SIM_NETWORK_PERSONALIZATION = 5
        { RIL_APPTYPE_SIM, RIL_APPSTATE_SUBSCRIPTION_PERSO, RIL_PERSOSUBSTATE_SIM_NETWORK,
          NULL, NULL, 0, RIL_PINSTATE_ENABLED_NOT_VERIFIED, RIL_PINSTATE_UNKNOWN }
    };
    RIL_CardState card_state;
    int num_apps;

    int sim_status = getSIMStatus();
    if (sim_status == SIM_ABSENT) {
        card_state = RIL_CARDSTATE_ABSENT;
        num_apps = 0;
    } else {
        card_state = RIL_CARDSTATE_PRESENT;
        num_apps = 1;
    }

    // Allocate and initialize base card status.
    RIL_CardStatus *p_card_status = malloc(sizeof(RIL_CardStatus));
    p_card_status->card_state = card_state;
    p_card_status->universal_pin_state = RIL_PINSTATE_UNKNOWN;
    p_card_status->gsm_umts_subscription_app_index = RIL_CARD_MAX_APPS;
    p_card_status->cdma_subscription_app_index = RIL_CARD_MAX_APPS;
    p_card_status->num_applications = num_apps;

    // Initialize application status
    int i;
    for (i = 0; i < RIL_CARD_MAX_APPS; i++) {
        p_card_status->applications[i] = app_status_array[SIM_ABSENT];
    }

    // Pickup the appropriate application status
    // that reflects sim_status for gsm.
    if (num_apps != 0) {
        // Only support one app, gsm
        p_card_status->num_applications = 1;
        p_card_status->gsm_umts_subscription_app_index = 0;

        // Get the correct app status
        p_card_status->applications[0] = app_status_array[sim_status];
    }

    *pp_card_status = p_card_status;
    return RIL_E_SUCCESS;
}

#else


/**
 * Get the current card status.
 *
 * This must be freed using freeCardStatus.
 * @return: On success returns RIL_E_SUCCESS
 */
static int getCardStatusV5(RIL_CardStatus_v5 **pp_card_status) {
    static RIL_AppStatus app_status_array[] = {
        // SIM_ABSENT = 0
        { RIL_APPTYPE_UNKNOWN, RIL_APPSTATE_UNKNOWN, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN },
        // SIM_NOT_READY = 1
        { RIL_APPTYPE_SIM, RIL_APPSTATE_DETECTED, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN },
        // SIM_READY = 2
        { RIL_APPTYPE_SIM, RIL_APPSTATE_READY, RIL_PERSOSUBSTATE_READY,
          NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN },
        // SIM_PIN = 3
        { RIL_APPTYPE_SIM, RIL_APPSTATE_PIN, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_ENABLED_NOT_VERIFIED, RIL_PINSTATE_UNKNOWN },
        // SIM_PUK = 4
        { RIL_APPTYPE_SIM, RIL_APPSTATE_PUK, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_ENABLED_BLOCKED, RIL_PINSTATE_UNKNOWN },
        // SIM_NETWORK_PERSONALIZATION = 5
        { RIL_APPTYPE_SIM, RIL_APPSTATE_SUBSCRIPTION_PERSO, RIL_PERSOSUBSTATE_SIM_NETWORK,
          NULL, NULL, 0, RIL_PINSTATE_ENABLED_NOT_VERIFIED, RIL_PINSTATE_UNKNOWN }
    };
    RIL_CardState card_state;
    int num_apps;

    int sim_status = getSIMStatus();
    if (sim_status == SIM_ABSENT) {
        card_state = RIL_CARDSTATE_ABSENT;
        num_apps = 0;
    } else {
        card_state = RIL_CARDSTATE_PRESENT;
        num_apps = 1;
    }

    // Allocate and initialize base card status.
    RIL_CardStatus_v5 *p_card_status = malloc(sizeof(RIL_CardStatus_v5));
    p_card_status->card_state = card_state;
    p_card_status->universal_pin_state = RIL_PINSTATE_UNKNOWN;
    p_card_status->gsm_umts_subscription_app_index = RIL_CARD_MAX_APPS;
    p_card_status->cdma_subscription_app_index = RIL_CARD_MAX_APPS;
    p_card_status->num_applications = num_apps;

    // Initialize application status
    int i;
    for (i = 0; i < RIL_CARD_MAX_APPS; i++) {
        p_card_status->applications[i] = app_status_array[SIM_ABSENT];
    }

    // Pickup the appropriate application status
    // that reflects sim_status for gsm.
    if (num_apps != 0) {
        // Only support one app, gsm
        p_card_status->num_applications = 1;
        p_card_status->gsm_umts_subscription_app_index = 0;

        // Get the correct app status
        p_card_status->applications[0] = app_status_array[sim_status];
    }

    *pp_card_status = p_card_status;
    return RIL_E_SUCCESS;
}

/**
 * Free the card status returned by getCardStatus
 */
static void freeCardStatus(RIL_CardStatus_v5 *p_card_status) {
    free(p_card_status);
}
#if RIL_VERSION >11
static int getCardStatusV12(RIL_CardStatus_v6 **pp_card_status) {
    static RIL_AppStatus app_status_array[] = {
        // SIM_ABSENT = 0
        { RIL_APPTYPE_UNKNOWN, RIL_APPSTATE_UNKNOWN, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN },
        // SIM_NOT_READY = 1
        { RIL_APPTYPE_SIM, RIL_APPSTATE_DETECTED, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN },
        // SIM_READY = 2
        { RIL_APPTYPE_SIM, RIL_APPSTATE_READY, RIL_PERSOSUBSTATE_READY,
          NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN },
        // SIM_PIN = 3
        { RIL_APPTYPE_SIM, RIL_APPSTATE_PIN, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_ENABLED_NOT_VERIFIED, RIL_PINSTATE_UNKNOWN },
        // SIM_PUK = 4
        { RIL_APPTYPE_SIM, RIL_APPSTATE_PUK, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_ENABLED_BLOCKED, RIL_PINSTATE_UNKNOWN },
        // SIM_NETWORK_PERSONALIZATION = 5
        { RIL_APPTYPE_SIM, RIL_APPSTATE_SUBSCRIPTION_PERSO, RIL_PERSOSUBSTATE_SIM_NETWORK,
          NULL, NULL, 0, RIL_PINSTATE_ENABLED_NOT_VERIFIED, RIL_PINSTATE_UNKNOWN },
        // RUIM_ABSENT = 6
        { RIL_APPTYPE_UNKNOWN, RIL_APPSTATE_UNKNOWN, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN },
        // RUIM_NOT_READY = 7
        { RIL_APPTYPE_RUIM, RIL_APPSTATE_DETECTED, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN },
        // RUIM_READY = 8
        { RIL_APPTYPE_RUIM, RIL_APPSTATE_READY, RIL_PERSOSUBSTATE_READY,
          NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN },
        // RUIM_PIN = 9
        { RIL_APPTYPE_RUIM, RIL_APPSTATE_PIN, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_ENABLED_NOT_VERIFIED, RIL_PINSTATE_UNKNOWN },
        // RUIM_PUK = 10
        { RIL_APPTYPE_RUIM, RIL_APPSTATE_PUK, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_ENABLED_BLOCKED, RIL_PINSTATE_UNKNOWN },
        // RUIM_NETWORK_PERSONALIZATION = 11
        { RIL_APPTYPE_RUIM, RIL_APPSTATE_SUBSCRIPTION_PERSO, RIL_PERSOSUBSTATE_SIM_NETWORK,
           NULL, NULL, 0, RIL_PINSTATE_ENABLED_NOT_VERIFIED, RIL_PINSTATE_UNKNOWN },
        // ISIM_ABSENT = 12
        { RIL_APPTYPE_UNKNOWN, RIL_APPSTATE_UNKNOWN, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN },
        // ISIM_NOT_READY = 13
        { RIL_APPTYPE_ISIM, RIL_APPSTATE_DETECTED, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN },
        // ISIM_READY = 14
        { RIL_APPTYPE_ISIM, RIL_APPSTATE_READY, RIL_PERSOSUBSTATE_READY,
          NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN },
        // ISIM_PIN = 15
        { RIL_APPTYPE_ISIM, RIL_APPSTATE_PIN, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_ENABLED_NOT_VERIFIED, RIL_PINSTATE_UNKNOWN },
        // ISIM_PUK = 16
        { RIL_APPTYPE_ISIM, RIL_APPSTATE_PUK, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_ENABLED_BLOCKED, RIL_PINSTATE_UNKNOWN },
        // ISIM_NETWORK_PERSONALIZATION = 17
        { RIL_APPTYPE_ISIM, RIL_APPSTATE_SUBSCRIPTION_PERSO, RIL_PERSOSUBSTATE_SIM_NETWORK,
          NULL, NULL, 0, RIL_PINSTATE_ENABLED_NOT_VERIFIED, RIL_PINSTATE_UNKNOWN },

    };
    RIL_CardState card_state;
    int num_apps;

    int sim_status = getSIMStatus();
    if (sim_status == SIM_ABSENT) {
        card_state = RIL_CARDSTATE_ABSENT;
        num_apps = 0;
    } else {
        card_state = RIL_CARDSTATE_PRESENT;
        num_apps = 3;
    }

    // Allocate and initialize base card status.
    RIL_CardStatus_v6 *p_card_status = malloc(sizeof(RIL_CardStatus_v6));
    p_card_status->card_state = card_state;
    p_card_status->universal_pin_state = RIL_PINSTATE_UNKNOWN;
    p_card_status->gsm_umts_subscription_app_index = -1;
    p_card_status->cdma_subscription_app_index = -1;
    p_card_status->ims_subscription_app_index = -1;
    p_card_status->num_applications = num_apps;

    // Initialize application status
    int i;
    for (i = 0; i < RIL_CARD_MAX_APPS; i++) {
        p_card_status->applications[i] = app_status_array[SIM_ABSENT];
    }

    // Pickup the appropriate application status
    // that reflects sim_status for gsm.
    if (num_apps != 0) {
        p_card_status->num_applications = 3;
        p_card_status->gsm_umts_subscription_app_index = 0;
        p_card_status->cdma_subscription_app_index = 1;
        p_card_status->ims_subscription_app_index = 2;

        // Get the correct app status
        p_card_status->applications[0] = app_status_array[sim_status];
        p_card_status->applications[1] = app_status_array[sim_status + RUIM_ABSENT];
        p_card_status->applications[2] = app_status_array[sim_status + ISIM_ABSENT];
    }

    *pp_card_status = p_card_status;
    return RIL_E_SUCCESS;
}
#endif

/**
 * Get the current card status.
 *
 * This must be freed using freeCardStatus.
 * @return: On success returns RIL_E_SUCCESS
 */
static int getCardStatusV6(RIL_CardStatus_v6 **pp_card_status) {
    static RIL_AppStatus app_status_array[] = {
        // SIM_ABSENT = 0
        { RIL_APPTYPE_UNKNOWN, RIL_APPSTATE_UNKNOWN, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN },
        // SIM_NOT_READY = 1
        { RIL_APPTYPE_SIM, RIL_APPSTATE_DETECTED, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN },
        // SIM_READY = 2
        { RIL_APPTYPE_SIM, RIL_APPSTATE_READY, RIL_PERSOSUBSTATE_READY,
          NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN },
        // SIM_PIN = 3
        { RIL_APPTYPE_SIM, RIL_APPSTATE_PIN, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_ENABLED_NOT_VERIFIED, RIL_PINSTATE_UNKNOWN },
        // SIM_PUK = 4
        { RIL_APPTYPE_SIM, RIL_APPSTATE_PUK, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_ENABLED_BLOCKED, RIL_PINSTATE_UNKNOWN },
        // SIM_NETWORK_PERSONALIZATION = 5
        { RIL_APPTYPE_SIM, RIL_APPSTATE_SUBSCRIPTION_PERSO, RIL_PERSOSUBSTATE_SIM_NETWORK,
          NULL, NULL, 0, RIL_PINSTATE_ENABLED_NOT_VERIFIED, RIL_PINSTATE_UNKNOWN }
    };
    RIL_CardState card_state;
    int num_apps;

    int sim_status = getSIMStatus();
    if (sim_status == SIM_ABSENT) {
        card_state = RIL_CARDSTATE_ABSENT;
        num_apps = 0;
    } else {
        card_state = RIL_CARDSTATE_PRESENT;
        num_apps = 1;
    }

    // Allocate and initialize base card status.
    RIL_CardStatus_v6 *p_card_status = malloc(sizeof(RIL_CardStatus_v6));
    p_card_status->card_state = card_state;
    p_card_status->universal_pin_state = RIL_PINSTATE_UNKNOWN;
    p_card_status->gsm_umts_subscription_app_index = RIL_CARD_MAX_APPS;
    p_card_status->cdma_subscription_app_index = RIL_CARD_MAX_APPS;
    p_card_status->ims_subscription_app_index = RIL_CARD_MAX_APPS;
    p_card_status->num_applications = num_apps;

    // Initialize application status
    int i;
    for (i = 0; i < RIL_CARD_MAX_APPS; i++) {
        p_card_status->applications[i] = app_status_array[SIM_ABSENT];
    }

    // Pickup the appropriate application status
    // that reflects sim_status for gsm.
    if (num_apps != 0) {
        // Only support one app, gsm
        p_card_status->num_applications = 1;
        p_card_status->gsm_umts_subscription_app_index = 0;

        // Get the correct app status
        p_card_status->applications[0] = app_status_array[sim_status];
    }

    *pp_card_status = p_card_status;
    return RIL_E_SUCCESS;
}

/**
 * Free the card status returned by getCardStatus
 */
static void freeCardStatusV6(RIL_CardStatus_v6 *p_card_status) {
    free(p_card_status);
}
#endif
/**
 * SIM ready means any commands that access the SIM will work, including:
 *  AT+CPIN, AT+CSMS, AT+CNMI, AT+CRSM
 *  (all SMS-related commands)
 */

static void pollSIMState (void *param)
{
    ATResponse *p_response;
    int ret;
	#if  RIL_VERSION < 12
	    if (sState != RADIO_STATE_SIM_NOT_READY) {
	        // no longer valid to poll
	        return;
	    }
	#else
		if (sState != RADIO_STATE_UNAVAILABLE) {
	        // no longer valid to poll
	        return;
	    }
	#endif

    switch(getSIMStatus()) {
        case SIM_ABSENT:
        case SIM_PIN:
        case SIM_PUK:
        case SIM_NETWORK_PERSONALIZATION:
        default:
        	#if RIL_VERSION < 12
	            setRadioState(RADIO_STATE_SIM_LOCKED_OR_ABSENT);
			#else
			    RLOGI("SIM ABSENT or LOCKED");
	            RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_SIM_STATUS_CHANGED, NULL, 0);
			#endif
        return;

        case SIM_NOT_READY:
            RIL_requestTimedCallback (pollSIMState, NULL, &TIMEVAL_SIMPOLL);
        return;

        case SIM_READY:
			#if RIL_VERSION < 12
            	setRadioState(RADIO_STATE_SIM_READY);
			#else
				RLOGI("SIM_READY");
            	onSIMReady();
				RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_SIM_STATUS_CHANGED, NULL, 0);
			#endif
        return;
    }
}

/** returns 1 if on, 0 if off, and -1 on error */
static int isRadioOn()
{
    ATResponse *p_response = NULL;
    int err;
    char *line;
    char ret;

    err = at_send_command_singleline("AT+CFUN?", "+CFUN:", &p_response);

    if (err < 0 || p_response->success == 0) {
        // assume radio is off
        goto error;
    }

    line = p_response->p_intermediates->line;

    err = at_tok_start(&line);
    if (err < 0) goto error;

    err = at_tok_nextbool(&line, &ret);
    if (err < 0) goto error;

    at_response_free(p_response);

    return (int)ret;

error:

    at_response_free(p_response);
    return -1;
}

/**
 * Initialize everything that can be configured while we're still in
 * AT+CFUN=0
 */
static void initializeCallback(void *param)
{
    ATResponse *p_response = NULL;
    int err;
    char modemReadyValue[PROPERTY_VALUE_MAX];

	LOGI("AIRM2M VER 1.0.8\n");

    LOGI("AT at_handshake START\n");

    at_handshake();
    LOGI("AT at_handshake END\n");
	LOGI("AIRM2M VER 1.0.57_	4G\n");
/*
    property_get("telephony.modem,ready", modemReadyValue, "0");
    if(strcmp (modemReadyValue , "0")==0){
        LOGE("rebooting modem");
        err = at_send_command("AT+RESET", NULL);
        if(err >= 0) {
            property_set("telephony.modem.ready", "1");
            LOGE("reboot modem return success");
            //sleep(5):
        } else {
            LOGE ("reboot modem return failed");
        }
    }
*/
    /* note: we don't check errors here. Everything important will
       be handled in onATTimeout and onATReaderClosed */

    /*  atchannel is tolerant of echo but it must */
    /*  have verbose result codes */
    at_send_command("ATE0", NULL);
	at_send_command("AT*I", NULL);
    at_send_command("ATQ0", NULL);
    at_send_command("ATV1", NULL);
    /*  No auto-answer */
    at_send_command("ATS0=0", NULL);

    /*  Extended errors */
    at_send_command("AT+CMEE=1", NULL);

    /*  Network registration events */
    err = at_send_command("AT+CREG=2", &p_response);

    /* some handsets -- in tethered mode -- don't support CREG=2 */
    if (err < 0 || p_response->success == 0) {
        at_send_command("AT+CREG=1", NULL);
    }

    at_response_free(p_response);

    /*  GPRS registration events */
#if RIL_VERSION <= 4
    at_send_command("AT+CEREG=1", NULL);
#else
    at_send_command("AT+CEREG=2", NULL);
#endif
    /*  Call Waiting notifications */
    at_send_command("AT+CCWA=1", NULL);

    /*  Alternating voice/data off */
    at_send_command("AT+CMOD=0", NULL);

    /*  Not muted */
    at_send_command("AT+CMUT=0", NULL);

    /*  +CSSU unsolicited supp service notifications */
    at_send_command("AT+CSSN=0,1", NULL);

    /*  no connected line identification */
    at_send_command("AT+COLP=0", NULL);

    /*  HEX character set */
    at_send_command("AT+CSCS=\"HEX\"", NULL);

    /*  USSD unsolicited */
    at_send_command("AT+CUSD=1", NULL);

    /*  Enable +CGEV GPRS event notifications, but don't buffer */
    at_send_command("AT+CGEREP=1,0", NULL);

    /*  SMS PDU mode */
    at_send_command("AT+CMGF=0", NULL);
	// at_send_command("AT*BAND=15", NULL);

    at_send_command("AT+CSCLK=2", NULL);

#ifdef USE_TI_COMMANDS

    at_send_command("AT%CPI=3", NULL);

    /*  TI specific -- notifications when SMS is ready (currently ignored) */
    at_send_command("AT%CSTAT=1", NULL);

#endif /* USE_TI_COMMANDS */
#ifdef AIRM2M_REPORT_SIGNAL_STRENGTH
		if (0 == poll_signal_started) {
			RIL_requestTimedCallback(onReportSignalStrength, NULL, &TIMEVAL_0);
			poll_signal_started = 1;
		}
#endif


    /* assume radio is off on error */
    if (isRadioOn() > 0) {
		#if RIL_VERSION < 12
	        setRadioState (RADIO_STATE_SIM_READY);
		#else
			setRadioState (RADIO_STATE_ON);
		#endif
    }
    else
    {
        setRadioState (RADIO_STATE_OFF);
    }

}

static void waitForClose()
{
    pthread_mutex_lock(&s_state_mutex);

    while (s_closed == 0) {
        pthread_cond_wait(&s_state_cond, &s_state_mutex);
    }

    pthread_mutex_unlock(&s_state_mutex);
}

/**
 * Called by atchannel when an unsolicited line appears
 * This is called on atchannel's reader thread. AT commands may
 * not be issued here
 */
static void onUnsolicited (const char *s, const char *sms_pdu)
{
    char *line = NULL;
    int err;
	LOGI("onUnsolicited lj start %s ",s);

    /* Ignore unsolicited responses until we're initialized.
     * This is OK because the RIL library will poll for initial state
     */
    if (sState == RADIO_STATE_UNAVAILABLE) {
        return;
    }

    if (strStartsWith(s, "%CTZV:")) {
        /* TI specific -- NITZ time */
        char *response;

        line = strdup(s);
        at_tok_start(&line);

        err = at_tok_nextstr(&line, &response);

        if (err != 0) {
            LOGE("invalid NITZ line %s\n", s);
        } else {
            RIL_onUnsolicitedResponse (
                RIL_UNSOL_NITZ_TIME_RECEIVED,
                response, strlen(response));
        }
    } else if (strStartsWith(s,"+CRING:")
                || strStartsWith(s,"RING")
                || strStartsWith(s,"NO CARRIER")
                || strStartsWith(s,"+CCWA")
    ) {
        RIL_onUnsolicitedResponse (
            RIL_UNSOL_RESPONSE_CALL_STATE_CHANGED,
            NULL, 0);
#ifdef WORKAROUND_FAKE_CGEV
        RIL_requestTimedCallback (onDataCallListChanged, NULL, NULL); //TODO use new function
#endif /* WORKAROUND_FAKE_CGEV */
    } else if (strStartsWith(s,"+CREG:")
                || strStartsWith(s,"+CEREG:")
    ) {

    #if RIL_VERSION <= 4
        RIL_onUnsolicitedResponse (
            RIL_UNSOL_RESPONSE_NETWORK_STATE_CHANGED,
            NULL, 0);
    #else
        RIL_onUnsolicitedResponse (
            RIL_UNSOL_RESPONSE_VOICE_NETWORK_STATE_CHANGED,
            NULL, 0);
		LOGI(" pppd arg %d %d ",Include_GSM,LteInserve);



    #endif

#ifdef WORKAROUND_FAKE_CGEV
        RIL_requestTimedCallback (onDataCallListChanged, NULL, NULL);
#endif /* WORKAROUND_FAKE_CGEV */
    } else if (strStartsWith(s, "+CMT:")) {
        RIL_onUnsolicitedResponse (
            RIL_UNSOL_RESPONSE_NEW_SMS,
            sms_pdu, strlen(sms_pdu));
    } else if (strStartsWith(s, "+CDS:")) {
        RIL_onUnsolicitedResponse (
            RIL_UNSOL_RESPONSE_NEW_SMS_STATUS_REPORT,
            sms_pdu, strlen(sms_pdu));
    } else if (strStartsWith(s, "+CGEV:")) {
        /* Really, we can ignore NW CLASS and ME CLASS events here,
         * but right now we don't since extranous
         * RIL_UNSOL_DATA_CALL_LIST_CHANGED calls are tolerated
         */
        /* can't issue AT commands here -- call on main thread */
        RIL_requestTimedCallback (onDataCallListChanged, NULL, NULL);
#ifdef WORKAROUND_FAKE_CGEV
    } else if (strStartsWith(s, "+CME ERROR: 150")) {
        RIL_requestTimedCallback (onDataCallListChanged, NULL, NULL);
#endif /* WORKAROUND_FAKE_CGEV */
    }
}

/* Called on command or reader thread */
static void onATReaderClosed()
{
    LOGI("AT channel closed\n");
    at_close();
    s_closed = 1;

    setRadioState (RADIO_STATE_UNAVAILABLE);
}

/* Called on command thread */
static void onATTimeout()
{
    LOGI("AT channel timeout; closing\n");
    at_close();

    s_closed = 1;

    /* FIXME cause a radio reset here */

    setRadioState (RADIO_STATE_UNAVAILABLE);
}

static void usage(char *s)
{
#ifdef RIL_SHLIB
    fprintf(stderr, "reference-ril requires: -p <tcp port> or -d /dev/tty_device\n");
#else
    fprintf(stderr, "usage: %s [-p <tcp port>] [-d /dev/tty_device]\n", s);
    exit(-1);
#endif
}



static void *
mainLoop(void *param)
{
    int fd;
    int ret;

    AT_DUMP("== ", "entering mainLoop()", -1 );
    LOGI("RIL mainLoop\n");

    at_set_on_reader_closed(onATReaderClosed);
    at_set_on_timeout(onATTimeout);
    char* usb_device_name = NULL;

    for (;;) {
        if(usb_device_name == NULL)
        {
            usb_device_name = FindUsbDevice(AIRM2M_USB_DEVICE_AT_INTERFACE_ID);

            if(usb_device_name == NULL)
            {
                LOGI("mainLoop s_device_path null \n");
                sleep(1);
                continue;
            }
            else
            {
                sprintf(s_device_path, "/dev/%s", usb_device_name);
                //free(usb_device_name);
                LOGI("RIL_Init s_device_path %s \n", s_device_path);
            }
        }

        fd = -1;
        while  (fd < 0) {
            if (s_port > 0) {
				#if RIL_VERSION <12
	                fd = socket_loopback_client(s_port, SOCK_STREAM);
				#else
					fd = socket_network_client("localhost", s_port, SOCK_STREAM);
				#endif
            } else if (s_device_socket) {
                if (!strcmp(s_device_path, "/dev/socket/qemud")) {
                    /* Qemu-specific control socket */
                    fd = socket_local_client( "qemud",
                                              ANDROID_SOCKET_NAMESPACE_RESERVED,
                                              SOCK_STREAM );
                    if (fd >= 0 ) {
                        char  answer[2];

                        if ( write(fd, "gsm", 3) != 3 ||
                             read(fd, answer, 2) != 2 ||
                             memcmp(answer, "OK", 2) != 0)
                        {
                            close(fd);
                            fd = -1;
                        }
                   }
                }
                else
                    fd = socket_local_client( s_device_path,
                                            ANDROID_SOCKET_NAMESPACE_FILESYSTEM,
                                            SOCK_STREAM );
            } else if (s_device_path[0] != '\0') {
	          fd = open (s_device_path, O_RDWR);
              if ( fd >= 0) {
                    /* disable echo on serial ports */
                    struct termios  ios;
                    tcgetattr( fd, &ios );
                    ios.c_lflag = 0;  /* disable ECHO, ICANON, etc... */
                    tcsetattr( fd, TCSANOW, &ios );
                }
            }

            if (fd < 0) {
                perror ("opening AT interface. retrying...");
                sleep(10);
                /* never returns */
            }
        }

        s_closed = 0;
        ret = at_open(fd, onUnsolicited);

        if (ret < 0) {
            LOGE ("AT error %d on at_open\n", ret);
            return 0;
        }

        RIL_requestTimedCallback(initializeCallback, NULL, &TIMEVAL_0);

        // Give initializeCallback a chance to dispatched, since
        // we don't presently have a cancellation mechanism
        sleep(1);

        if(diag_need_save)
        {
            start_diagsaver();
        }

        waitForClose();

        usb_device_name = NULL;

        LOGI("Re-opening after close");

    }
}

#ifdef RIL_SHLIB
pthread_t s_tid_mainloop;

const RIL_RadioFunctions *RIL_Init(const struct RIL_Env *env, int argc, char **argv)
{
    int ret;
    int fd = -1;
    int opt;
    pthread_attr_t attr;
    char* usb_device_name;
    s_rilenv = env;

    LOGI("RIL_Init %d \n", s_port);

    pthread_attr_init (&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    ret = pthread_create(&s_tid_mainloop, &attr, mainLoop, NULL);

    return &s_callbacks;
}
#else /* RIL_SHLIB */

#endif /* RIL_SHLIB */
